package dev.henge.probe;

import java.net.URLConnection;
import java.security.KeyStore;
import java.security.cert.CertificateException;
import java.security.cert.X509Certificate;

import javax.net.ssl.HttpsURLConnection;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLSocketFactory;
import javax.net.ssl.TrustManager;
import javax.net.ssl.TrustManagerFactory;
import javax.net.ssl.X509TrustManager;

/**
 * The platform's certificate verification, with one documented exception for Apple's CDN.
 *
 * Apple's download hosts present a chain containing a certificate signed with SHA-1. Android's
 * verifier rejects any chain like that on sight — `Signature uses an insecure hash function:
 * 1.2.840.113549.1.1.5` — even though the chain is properly anchored in a system root and the
 * alternative is not being able to download anything from Apple at all. That is what the fetch of
 * the boot ROM was failing on.
 *
 * So this wraps the system trust manager and tolerates exactly one failure: the weak-hash policy
 * check, which Conscrypt performs *after* it has anchored the chain to a trusted root. Nothing else
 * is forgiven — a chain that does not reach a system root still throws, a wrong hostname still
 * fails in HttpsURLConnection, and an expired certificate still throws.
 *
 * What makes that acceptable rather than reckless is that the content is checked separately:
 * the boot ROM is only written after its SHA-256 matches a constant in this app, and an IPSW is
 * verified by Apple's own signature while the restore runs. Transport security is not the thing
 * deciding whether these bytes are trustworthy.
 */
final class Tls {

    /** The marker Conscrypt uses for that one failure; the message is the only thing it gives. */
    private static final String WEAK_HASH = "insecure hash function";

    private static SSLSocketFactory factory;

    private Tls() {
    }

    /** Applies the shared settings to a connection, whatever its scheme. */
    static void apply(URLConnection connection) {
        if (connection instanceof HttpsURLConnection) {
            ((HttpsURLConnection) connection).setSSLSocketFactory(factory());
        }
    }

    private static synchronized SSLSocketFactory factory() {
        if (factory != null) return factory;
        try {
            TrustManagerFactory system =
                    TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
            system.init((KeyStore) null);
            X509TrustManager base = null;
            for (TrustManager tm : system.getTrustManagers()) {
                if (tm instanceof X509TrustManager) {
                    base = (X509TrustManager) tm;
                    break;
                }
            }
            if (base == null) throw new IllegalStateException("no system trust manager");

            SSLContext context = SSLContext.getInstance("TLS");
            context.init(null, new TrustManager[]{new Anchored(base)}, null);
            factory = context.getSocketFactory();
            return factory;
        } catch (Exception e) {
            // Falling back to the platform default is right: it fails loudly on the Apple chain
            // rather than quietly trusting everything.
            AppLog.w("TLS: 无法建立信任管理器", e);
            factory = (SSLSocketFactory) SSLSocketFactory.getDefault();
            return factory;
        }
    }

    /** The system verifier, minus its refusal to accept a SHA-1 signature inside an anchored chain. */
    private static final class Anchored implements X509TrustManager {
        private final X509TrustManager base;

        Anchored(X509TrustManager base) {
            this.base = base;
        }

        @Override
        public void checkClientTrusted(X509Certificate[] chain, String authType)
                throws CertificateException {
            base.checkClientTrusted(chain, authType);
        }

        @Override
        public void checkServerTrusted(X509Certificate[] chain, String authType)
                throws CertificateException {
            try {
                base.checkServerTrusted(chain, authType);
            } catch (CertificateException e) {
                if (!isWeakHashOnly(e)) throw e;
                AppLog.i("TLS: 接受链中的 SHA-1 签名（已锚定在系统根，Apple CDN 所需）");
            }
        }

        @Override
        public X509Certificate[] getAcceptedIssuers() {
            return base.getAcceptedIssuers();
        }

        private static boolean isWeakHashOnly(Throwable e) {
            for (Throwable t = e; t != null; t = t.getCause()) {
                String message = t.getMessage();
                if (message != null && message.contains(WEAK_HASH)) return true;
                if (t.getCause() == t) break;
            }
            return false;
        }
    }
}
