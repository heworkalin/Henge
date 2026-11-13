package dev.henge.probe;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;

import javax.net.ssl.HttpsURLConnection;

/**
 * The app's HTTP, for the installer's C side.
 *
 * The restore tools are written against libcurl's easy API. iOS does not have libcurl, so this
 * project implements that API over NSURLSession there (`host/curl_shim.m`); Android does not have
 * it either, and has the same answer available: the platform's own TLS and trust store, used from
 * Java, with the C side calling in through {@code android/curl_jni.c}.
 *
 * The body is streamed: each chunk read from the connection is handed straight to the native
 * write callback, which is what writes it to a file. An IPSW is measured in gigabytes, so
 * buffering a response — even the ones this is used for — is not an option.
 *
 * Everything here runs on the calling thread, which is a native one. Nothing touches the UI.
 */
public final class Http {

    private Http() {
    }

    /**
     * Performs one request and streams the response into the C sink.
     *
     * @param token the C side's handle, passed back to {@link #nativeSinkWrite}
     * @return the HTTP status code, or -1 when the request could not be made at all
     */
    static int fetch(String url, byte[] post, boolean isPost, String userAgent, String[] headers,
                     boolean follow, long token) {
        HttpURLConnection connection = null;
        try {
            connection = (HttpURLConnection) new URL(url).openConnection();
            // The platform's verification, through Tls: Apple's signing and download hosts are
            // rejected by the default policy because their chains contain a SHA-1 signature, and
            // that policy is the one thing Tls relaxes — see its comment for why that is bounded.
            Tls.apply(connection);
            connection.setRequestMethod(isPost ? "POST" : "GET");
            connection.setInstanceFollowRedirects(follow);
            connection.setConnectTimeout(30000);
            connection.setReadTimeout(120000);
            if (userAgent != null && !userAgent.isEmpty()) {
                connection.setRequestProperty("User-Agent", userAgent);
            }
            if (headers != null) {
                for (String header : headers) {
                    if (header == null) continue;
                    int colon = header.indexOf(':');
                    if (colon <= 0) continue;
                    connection.setRequestProperty(header.substring(0, colon).trim(),
                            header.substring(colon + 1).trim());
                }
            }
            if (isPost) {
                connection.setDoOutput(true);
                byte[] body = post == null ? new byte[0] : post;
                connection.setFixedLengthStreamingMode(body.length);
                try (OutputStream out = connection.getOutputStream()) {
                    out.write(body);
                }
            }

            int status = connection.getResponseCode();

            // The headers the C side asked for, as raw lines: that is what a curl header callback
            // receives, and the restore stack parses them that way.
            for (int i = 0; ; i++) {
                String name = connection.getHeaderFieldKey(i);
                String value = connection.getHeaderField(i);
                if (name == null && value == null) break;
                String line = (name == null ? "" : name + ": " + value + "\r\n");
                if (pump(line.getBytes("ISO-8859-1"), token, true) < 0) return -1;
            }
            if (pump("\r\n".getBytes("ISO-8859-1"), token, true) < 0) return -1;

            // The body, in chunks, straight to the sink.
            InputStream in = status >= 400 ? connection.getErrorStream() : connection.getInputStream();
            if (in != null) {
                byte[] buffer = new byte[64 * 1024];
                int n;
                while ((n = in.read(buffer)) > 0) {
                    byte[] chunk = new byte[n];
                    System.arraycopy(buffer, 0, chunk, 0, n);
                    if (pump(chunk, token, false) < 0) break;
                }
                in.close();
            }
            return status;
        } catch (IOException | RuntimeException e) {
            AppLog.w("Http: " + url + " 失败", e);
            return -1;
        } finally {
            if (connection != null) connection.disconnect();
        }
    }

    /** Hands one chunk to C; returns -1 when the sink refused it, which ends the transfer. */
    private static int pump(byte[] chunk, long token, boolean header) {
        long taken = header
                ? nativeSinkHeader(token, chunk, chunk.length)
                : nativeSinkWrite(token, chunk, chunk.length);
        return taken == chunk.length ? 0 : -1;
    }

    private static native long nativeSinkWrite(long token, byte[] data, int length);

    private static native long nativeSinkHeader(long token, byte[] data, int length);
}
