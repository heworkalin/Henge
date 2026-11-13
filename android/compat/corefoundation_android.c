/*
 * Android/Linux compatibility: CFRetain / CFRelease, as a token counter.
 *
 * See include/CoreFoundation/CoreFoundation.h for why this is a counter and not
 * an object system. The only thing that matters here is that a retain is matched
 * by a release, because devices/pvg_lite.c and host/vm.c both hold the display
 * layer across an arbitrary number of replacements and release the previous one.
 *
 * The table is small on purpose: a process has one display layer, and a nil layer
 * is the normal Android case. Twelve slots cover the display plus anything a test
 * hands in. Overflow means a leak of the *count* only — no memory is owned here —
 * so it is reported and the retain is dropped rather than aborting a running VM.
 */
#include <CoreFoundation/CoreFoundation.h>

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#define HV_CF_SLOTS 12

static pthread_mutex_t g_cf_mu = PTHREAD_MUTEX_INITIALIZER;
static CFTypeRef g_cf_obj[HV_CF_SLOTS];
static uint32_t g_cf_count[HV_CF_SLOTS];

CFTypeRef CFRetain(CFTypeRef cf) {
    if (!cf) return NULL; /* CoreFoundation's contract: retaining NULL is NULL */
    pthread_mutex_lock(&g_cf_mu);
    int free_slot = -1;
    for (int i = 0; i < HV_CF_SLOTS; i++) {
        if (g_cf_obj[i] == cf) {
            g_cf_count[i]++;
            pthread_mutex_unlock(&g_cf_mu);
            return cf;
        }
        if (!g_cf_obj[i] && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) {
        g_cf_obj[free_slot] = cf;
        g_cf_count[free_slot] = 1;
    } else {
        fprintf(stderr, "hengevm: CFRetain: %d tokens tracked, cannot record another\n", HV_CF_SLOTS);
    }
    pthread_mutex_unlock(&g_cf_mu);
    return cf;
}

void CFRelease(CFTypeRef cf) {
    if (!cf) return;
    pthread_mutex_lock(&g_cf_mu);
    for (int i = 0; i < HV_CF_SLOTS; i++) {
        if (g_cf_obj[i] == cf) {
            if (--g_cf_count[i] == 0) g_cf_obj[i] = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&g_cf_mu);
}
