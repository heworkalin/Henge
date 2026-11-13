/*
 * The part of libcurl the restore tools use, over the app's own HTTPS.
 *
 * The installer's C side — libtatsu's signing requests, idevicerestore's downloads, the restore
 * OS's URL fetches — is written against libcurl's easy API. Apple's platforms come with it; iOS
 * does not, which is why `host/curl_shim.m` implements the same eleven functions over
 * NSURLSession. Android is in the same position, and the same shape applies: the platform already
 * has a maintained TLS stack and trust store, so the sensible thing is to use it rather than
 * build curl *and* a TLS library for a program that makes a handful of requests.
 *
 * So this file is a shim in the literal sense: it stores the options, and at
 * `curl_easy_perform` it calls into `dev.henge.probe.Http`, which does the request with
 * `HttpsURLConnection` and streams the body back through `sinkWrite`. The URL never reaches a
 * certificate check implemented here, because there is none here.
 *
 * What it supports is exactly what the restore stack asks for (the option list below was taken
 * from the vendored sources, not from libcurl's headers): GET and POST with a body, request
 * headers, a user agent, following redirects, header and body write callbacks, a progress
 * callback, an error buffer, and the response code. An option outside that set is refused loudly
 * at `setopt` rather than silently ignored — a download that goes to the wrong place because an
 * option was dropped is worse than one that does not start.
 *
 * Threading: `curl_easy_perform` may be called from any thread, including ones this library
 * started, so the shim attaches to the JVM for the call and detaches after. The Java call is
 * synchronous, and the sink callbacks run on the same thread inside it.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jni.h>

#include "curl/curl.h"

/* `struct curl_slist` comes from the vendored curl header (it is a subset of libcurl's, cut down
 * to what the restore stack uses). Only four CURLE_* values exist there too, so the shim reports
 * failures with the ones that are meaningful to a caller: a malformed URL, a refused connection,
 * or a write the sink would not take. */

typedef struct Easy {
    char *url;
    char *useragent;
    void *postfields;          /* borrowed for the call, as libcurl treats it without COPYPOSTFIELDS */
    long postsize;
    int is_post;
    int follow;
    int noprogress;
    int verbose;
    int ssl_verifypeer;
    curl_write_callback write_cb;
    void *write_data;
    curl_write_callback header_cb;
    void *header_data;
    curl_xferinfo_callback xferinfo;
    void *xferinfo_data;
    curl_progress_callback progress;
    void *progress_data;
    struct curl_slist *headers;
    char *errorbuffer;
    long response_code;
} Easy;

/* The JVM, cached by JNI_OnLoad, and the class/method ids the shim needs. */
static JavaVM *g_vm;
static jclass g_http;
static jmethodID g_fetch;

/* Java calls these back, with the token the shim passed down. */
JNIEXPORT jlong JNICALL
Java_dev_henge_probe_Http_nativeSinkWrite(JNIEnv *env, jclass clazz, jlong token, jbyteArray data,
                                          jint length);
JNIEXPORT jlong JNICALL
Java_dev_henge_probe_Http_nativeSinkHeader(JNIEnv *env, jclass clazz, jlong token, jbyteArray data,
                                           jint length);

/*
 * Declared in stderr_to_logcat.c. Turned on here because this is the first thing the library does:
 * the restore stack's own diagnostics go to stdout and stderr, which Android discards, and they are
 * the only account of why a restore failed.
 */
int hv_android_capture_output(void);

/* Declared in compat_android.c: the page the JIT's inline counter path reads. */
int hv_android_map_commpage(void);

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    hv_android_capture_output();
    // Before any guest code is translated: the inline counter path loads from that address.
    hv_android_map_commpage();
    g_vm = vm;
    JNIEnv *env = NULL;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass local = (*env)->FindClass(env, "dev/henge/probe/Http");
    if (!local) return JNI_ERR;
    g_http = (jclass)(*env)->NewGlobalRef(env, local);
    (*env)->DeleteLocalRef(env, local);
    if (!g_http) return JNI_ERR;
    g_fetch = (*env)->GetStaticMethodID(env, g_http, "fetch",
                                        "(Ljava/lang/String;[BZLjava/lang/String;[Ljava/lang/String;ZJ)I");
    if (!g_fetch) return JNI_ERR;
    return JNI_VERSION_1_6;
}

/* Attaches the calling thread if it is native, and says whether it has to detach afterwards. */
static JNIEnv *attach(int *must_detach) {
    *must_detach = 0;
    if (!g_vm) return NULL;
    JNIEnv *env = NULL;
    jint r = (*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6);
    if (r == JNI_EDETACHED) {
        if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) return NULL;
        *must_detach = 1;
    }
    return env;
}

static void detach(int must_detach) {
    if (must_detach && g_vm) (*g_vm)->DetachCurrentThread(g_vm);
}

/* The body callback, called from Java with a chunk. Returns the bytes taken. */
JNIEXPORT jlong JNICALL
Java_dev_henge_probe_Http_nativeSinkWrite(JNIEnv *env, jclass clazz, jlong token, jbyteArray data,
                                          jint length) {
    (void)clazz;
    Easy *e = (Easy *)(intptr_t)token;
    if (!e) return 0;
    jbyte *bytes = (*env)->GetByteArrayElements(env, data, NULL);
    if (!bytes) return 0;
    size_t taken = (size_t)length;
    if (e->write_cb) {
        taken = e->write_cb((char *)bytes, 1, (size_t)length, e->write_data);
    } else if (e->write_data) {
        /* No callback but a destination: libcurl's default writes to the FILE* here. The restore
         * stack always sets a callback, so this is a guard rather than a feature. */
        taken = fwrite(bytes, 1, (size_t)length, (FILE *)e->write_data);
    }
    (*env)->ReleaseByteArrayElements(env, data, bytes, JNI_ABORT);
    return (jlong)taken;
}

JNIEXPORT jlong JNICALL
Java_dev_henge_probe_Http_nativeSinkHeader(JNIEnv *env, jclass clazz, jlong token, jbyteArray data,
                                           jint length) {
    (void)clazz;
    Easy *e = (Easy *)(intptr_t)token;
    if (!e) return 0;
    jbyte *bytes = (*env)->GetByteArrayElements(env, data, NULL);
    if (!bytes) return 0;
    size_t taken = (size_t)length;
    if (e->header_cb) taken = e->header_cb((char *)bytes, 1, (size_t)length, e->header_data);
    (*env)->ReleaseByteArrayElements(env, data, bytes, JNI_ABORT);
    return (jlong)taken;
}

/* ---- the curl API ------------------------------------------------------- */

CURLcode curl_global_init(long flags) {
    (void)flags;
    return CURLE_OK;
}

void curl_global_cleanup(void) {
}

CURL *curl_easy_init(void) {
    Easy *e = calloc(1, sizeof(Easy));
    if (!e) return NULL;
    e->ssl_verifypeer = 1;
    e->follow = 1;
    return (CURL *)e;
}

void curl_easy_cleanup(CURL *handle) {
    Easy *e = (Easy *)handle;
    if (!e) return;
    free(e->url);
    free(e->useragent);
    /* The callers own their slists (curl_slist_free_all); the option only borrowed it. */
    free(e);
}

CURLcode curl_easy_setopt(CURL *handle, CURLoption option, ...) {
    Easy *e = (Easy *)handle;
    if (!e) return CURLE_COULDNT_CONNECT;

    va_list ap;
    va_start(ap, option);
    CURLcode result = CURLE_OK;

    switch (option) {
    case CURLOPT_URL: {
        const char *url = va_arg(ap, const char *);
        free(e->url);
        e->url = url ? strdup(url) : NULL;
        break;
    }
    case CURLOPT_USERAGENT: {
        const char *ua = va_arg(ap, const char *);
        free(e->useragent);
        e->useragent = ua ? strdup(ua) : NULL;
        break;
    }
    case CURLOPT_HTTPGET:
        e->is_post = 0;
        break;
    case CURLOPT_POSTFIELDS:
        e->postfields = va_arg(ap, void *);
        e->is_post = 1;
        break;
    case CURLOPT_POSTFIELDSIZE:
        e->postsize = va_arg(ap, long);
        break;
    case CURLOPT_HTTPHEADER:
        e->headers = va_arg(ap, struct curl_slist *);
        break;
    case CURLOPT_FOLLOWLOCATION:
        e->follow = va_arg(ap, long) != 0;
        break;
    case CURLOPT_NOPROGRESS:
        e->noprogress = va_arg(ap, long) != 0;
        break;
    case CURLOPT_VERBOSE:
        e->verbose = va_arg(ap, long) != 0;
        break;
    case CURLOPT_SSL_VERIFYPEER:
        e->ssl_verifypeer = va_arg(ap, long) != 0;
        break;
    case CURLOPT_WRITEFUNCTION:
        e->write_cb = va_arg(ap, curl_write_callback);
        break;
    case CURLOPT_WRITEDATA:
        e->write_data = va_arg(ap, void *);
        break;
    case CURLOPT_HEADERFUNCTION:
        e->header_cb = va_arg(ap, curl_write_callback);
        break;
    case CURLOPT_HEADERDATA:
        e->header_data = va_arg(ap, void *);
        break;
    case CURLOPT_XFERINFOFUNCTION:
        e->xferinfo = va_arg(ap, curl_xferinfo_callback);
        break;
    case CURLOPT_XFERINFODATA:
        e->xferinfo_data = va_arg(ap, void *);
        break;
    case CURLOPT_PROGRESSFUNCTION:
        e->progress = va_arg(ap, curl_progress_callback);
        break;
    case CURLOPT_ERRORBUFFER:
        e->errorbuffer = va_arg(ap, char *);
        break;
    default:
        /* Refused rather than ignored: see the note at the top. */
        fprintf(stderr, "curl_shim: CURLOPT %d is not implemented\n", (int)option);
        result = CURLE_URL_MALFORMAT;
        break;
    }
    va_end(ap);
    return result;
}

/** Copies `text` into the error buffer when the caller gave one. */
static void set_error(Easy *e, const char *text) {
    if (e->errorbuffer) {
        snprintf(e->errorbuffer, CURL_ERROR_SIZE, "%s", text);
    }
}

CURLcode curl_easy_perform(CURL *handle) {
    Easy *e = (Easy *)handle;
    if (!e) return CURLE_COULDNT_CONNECT;
    if (!e->url) {
        set_error(e, "no URL set");
        return CURLE_URL_MALFORMAT;
    }
    JNIEnv *env = NULL;
    if (g_vm) {
        int must_detach = 0;
        env = attach(&must_detach);
        if (!env) {
            set_error(e, "cannot attach to the JVM");
            return CURLE_COULDNT_CONNECT;
        }
        if (e->progress && !e->noprogress) {
            e->progress(e->progress_data, 0.0, 0.0, 0.0, 0.0);
        }

        jstring url = (*env)->NewStringUTF(env, e->url);
        jstring ua = e->useragent ? (*env)->NewStringUTF(env, e->useragent) : NULL;

        /* The request headers, as the slist holds them. */
        int count = 0;
        for (struct curl_slist *n = e->headers; n; n = n->next) count++;
        jobjectArray headers = (*env)->NewObjectArray(env, count,
                (*env)->FindClass(env, "java/lang/String"), NULL);
        int i = 0;
        for (struct curl_slist *n = e->headers; n; n = n->next) {
            jstring h = (*env)->NewStringUTF(env, n->data);
            (*env)->SetObjectArrayElement(env, headers, i++, h);
            (*env)->DeleteLocalRef(env, h);
        }

        jbyteArray body = NULL;
        if (e->is_post && e->postfields && e->postsize > 0) {
            body = (*env)->NewByteArray(env, (jsize)e->postsize);
            (*env)->SetByteArrayRegion(env, body, 0, (jsize)e->postsize,
                                       (const jbyte *)e->postfields);
        }

        jint status = (*env)->CallStaticIntMethod(env, g_http, g_fetch, url, body,
                                                  (jboolean)e->is_post, ua, headers,
                                                  (jboolean)e->follow, (jlong)(intptr_t)e);
        CURLcode result = CURLE_OK;
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            set_error(e, "the request threw");
            result = CURLE_WRITE_ERROR;
        } else if (status < 0) {
            set_error(e, "the request failed");
            result = CURLE_COULDNT_CONNECT;
        } else {
            e->response_code = (long)status;
        }

        if (body) (*env)->DeleteLocalRef(env, body);
        if (headers) (*env)->DeleteLocalRef(env, headers);
        if (ua) (*env)->DeleteLocalRef(env, ua);
        if (url) (*env)->DeleteLocalRef(env, url);
        detach(must_detach);
        return result;
    }
    set_error(e, "the JVM was never reached: is libhenge loaded by the app?");
    return CURLE_COULDNT_CONNECT;
}

CURLcode curl_easy_getinfo(CURL *handle, CURLINFO info, ...) {
    Easy *e = (Easy *)handle;
    if (!e) return CURLE_COULDNT_CONNECT;
    va_list ap;
    va_start(ap, info);
    CURLcode result = CURLE_OK;
    switch (info) {
    case CURLINFO_RESPONSE_CODE: {
        long *out = va_arg(ap, long *);
        if (out) *out = e->response_code;
        break;
    }
    default:
        result = CURLE_URL_MALFORMAT;
        break;
    }
    va_end(ap);
    return result;
}

const char *curl_easy_strerror(CURLcode code) {
    /* The vendored curl header defines four codes, not libcurl's sixty-odd: a request that could
     * not be made, a malformed URL or an option the shim refuses, and a write the sink refused. */
    switch (code) {
    case CURLE_OK: return "no error";
    case CURLE_WRITE_ERROR: return "the response could not be written";
    default: return "the request could not be made";
    }
}

struct curl_slist *curl_slist_append(struct curl_slist *list, const char *data) {
    struct curl_slist *node = calloc(1, sizeof *node);
    if (!node) return list;
    node->data = strdup(data);
    if (!list) return node;
    struct curl_slist *tail = list;
    while (tail->next) tail = tail->next;
    tail->next = node;
    return list;
}

void curl_slist_free_all(struct curl_slist *list) {
    while (list) {
        struct curl_slist *next = list->next;
        free(list->data);
        free(list);
        list = next;
    }
}
