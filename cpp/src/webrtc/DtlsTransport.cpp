#include "webrtc/DtlsTransport.hpp"
#include "core/Logger.hpp"

#include <cstring>
#include <memory>

#ifdef HAVE_OPENSSL
#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <cstdio>
#endif

namespace streaming {

#ifdef HAVE_OPENSSL
struct DtlsTransport::Impl {
    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;
    BIO* netBio = nullptr;  // 对端侧 BIO：app 写入密文供 SSL 读，读出 SSL 写出的密文
    EVP_PKEY* pkey = nullptr;
    X509* cert = nullptr;
};

namespace {
// 进程级共享的自签名证书：所有 DTLS 传输复用同一份，保证 SDP 中宣告的
// a=fingerprint 与握手实际使用的证书一致。按需生成，仅依赖 OpenSSL。
struct SharedCert {
    X509* cert = nullptr;
    EVP_PKEY* key = nullptr;
    std::string fingerprint;  // "sha-256 AA:BB:..."
};

SharedCert& sharedSelfSignedCert() {
    static SharedCert sc;
    return sc;
}

bool ensureSelfSignedCert(SharedCert& sc) {
    if (sc.cert && sc.key) return true;

    EVP_PKEY* key = EVP_RSA_gen(2048);
    if (!key) {
        LOG_ERROR("DTLS: EVP_RSA_gen failed");
        return false;
    }
    X509* cert = X509_new();
    if (!cert) {
        EVP_PKEY_free(key);
        return false;
    }

    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_set_version(cert, 2);  // X509v3
    X509_gmtime_adj(X509_getm_notBefore(cert), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert), 60L * 60 * 24 * 3650);  // ~10 年
    X509_set_pubkey(cert, key);

    X509_NAME* name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(
                                   "streaming-server-dtls"),
                               -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(
                                   "streaming-media-server"),
                               -1, -1, 0);
    X509_set_issuer_name(cert, name);

    if (X509_sign(cert, key, EVP_sha256()) <= 0) {
        LOG_ERROR("DTLS: X509_sign failed");
        X509_free(cert);
        EVP_PKEY_free(key);
        return false;
    }

    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    if (!X509_digest(cert, EVP_sha256(), md, &n)) {
        X509_free(cert);
        EVP_PKEY_free(key);
        return false;
    }
    std::string fp = "sha-256 ";
    char hex[4];
    for (unsigned int i = 0; i < n; ++i) {
        if (i) fp += ':';
        std::snprintf(hex, sizeof(hex), "%02X", md[i]);
        fp += hex;
    }

    sc.cert = cert;
    sc.key = key;
    sc.fingerprint = std::move(fp);
    return true;
}
}  // namespace
#else
struct DtlsTransport::Impl {};
#endif

DtlsTransport::DtlsTransport(Role role, std::string localFingerprint)
    : role_(role), localFingerprint_(std::move(localFingerprint)),
      impl_(new Impl()) {}

DtlsTransport::~DtlsTransport() {
#ifdef HAVE_OPENSSL
    if (impl_->netBio) BIO_free(impl_->netBio);
    if (impl_->ssl) SSL_free(impl_->ssl);
    if (impl_->ctx) SSL_CTX_free(impl_->ctx);
    if (impl_->cert) X509_free(impl_->cert);
    if (impl_->pkey) EVP_PKEY_free(impl_->pkey);
#endif
    delete impl_;
}

std::string DtlsTransport::localFingerprintSha256(const std::string& certPem) {
#ifdef HAVE_OPENSSL
    // 若显式传入 PEM 证书，直接计算其指纹；否则使用进程级自签证书的指纹。
    if (!certPem.empty()) {
        BIO* cbio = BIO_new_mem_buf(certPem.data(),
                                    static_cast<int>(certPem.size()));
        if (cbio) {
            X509* c = PEM_read_bio_X509(cbio, nullptr, nullptr, nullptr);
            BIO_free(cbio);
            if (c) {
                unsigned char md[EVP_MAX_MD_SIZE];
                unsigned int n = 0;
                std::string fp;
                if (X509_digest(c, EVP_sha256(), md, &n)) {
                    fp = "sha-256 ";
                    char hex[4];
                    for (unsigned int i = 0; i < n; ++i) {
                        if (i) fp += ':';
                        std::snprintf(hex, sizeof(hex), "%02X", md[i]);
                        fp += hex;
                    }
                }
                X509_free(c);
                if (!fp.empty()) return fp;
            }
        }
    }
    auto& sc = sharedSelfSignedCert();
    if (ensureSelfSignedCert(sc)) return sc.fingerprint;
    return "";
#else
    (void)certPem;
    return "";
#endif
}

bool DtlsTransport::init(const std::string& certPem,
                         const std::string& keyPem) {
#ifdef HAVE_OPENSSL
    impl_->ctx = SSL_CTX_new(DTLS_method());
    if (!impl_->ctx) {
        LOG_ERROR("DTLS: SSL_CTX_new failed");
        return false;
    }
    SSL_CTX_set_options(impl_->ctx, SSL_OP_NO_QUERY_MTU);

    if (!certPem.empty() && !keyPem.empty()) {
        BIO* cbio = BIO_new_mem_buf(certPem.data(),
                                    static_cast<int>(certPem.size()));
        BIO* kbio = BIO_new_mem_buf(keyPem.data(),
                                    static_cast<int>(keyPem.size()));
        impl_->cert = PEM_read_bio_X509(cbio, nullptr, nullptr, nullptr);
        impl_->pkey = PEM_read_bio_PrivateKey(kbio, nullptr, nullptr, nullptr);
        BIO_free(cbio);
        BIO_free(kbio);
    } else {
        // 未提供证书：使用进程级自签名证书。复制一份引用，使析构函数
        // 可以无条件 X509_free/EVP_PKEY_free 而不影响其他传输实例。
        auto& sc = sharedSelfSignedCert();
        if (ensureSelfSignedCert(sc)) {
            impl_->cert = X509_dup(sc.cert);
            if (EVP_PKEY_up_ref(sc.key) > 0) impl_->pkey = sc.key;
        }
    }
    if (impl_->cert && impl_->pkey) {
        SSL_CTX_use_certificate(impl_->ctx, impl_->cert);
        SSL_CTX_use_PrivateKey(impl_->ctx, impl_->pkey);
    } else {
        LOG_ERROR("DTLS: no certificate/key available, handshake will fail");
    }

    // 仅启用 SRTP_AEAD_AES_128_GCM / SRTP_AES128_CM_HMAC_SHA1_80
    SSL_CTX_set_tlsext_use_srtp(
        impl_->ctx,
        "SRTP_AEAD_AES_128_GCM:SRTP_AES128_CM_SHA1_80");

    impl_->ssl = SSL_new(impl_->ctx);
    // 内存 BIO 对：internal 交给 SSL，netBio 由本类持有用于收发
    BIO* internal = nullptr;
    BIO_new_bio_pair(&internal, 0, &impl_->netBio, 0);
    SSL_set_bio(impl_->ssl, internal, internal);
    // internal 所有权已转移给 SSL；netBio 由本类持有

    if (role_ == Role::kServer) {
        SSL_set_accept_state(impl_->ssl);
    } else {
        SSL_set_connect_state(impl_->ssl);
    }
    return true;
#else
    (void)certPem;
    (void)keyPem;
    LOG_WARN("DTLS: OpenSSL unavailable, running in passthrough/stub mode");
    // stub 模式：握手直接标记完成，key 为空
    established_ = true;
    if (readyCb_) readyCb_({}, {});
    return true;
#endif
}

void DtlsTransport::startHandshake() {
#ifdef HAVE_OPENSSL
    doHandshakeStep();
#else
    established_ = true;
    if (readyCb_) readyCb_({}, {});
#endif
}

void DtlsTransport::doHandshakeStep() {
#ifdef HAVE_OPENSSL
    int rc = SSL_do_handshake(impl_->ssl);
    sendPending();
    if (rc == 1) {
        established_ = true;
        // 导出 SRTP key/salt（RFC 5764）
        unsigned char buf[60];
        if (SSL_export_keying_material(impl_->ssl, buf, sizeof(buf),
                                       "EXTRACTOR-dtls_srtp", 19, nullptr, 0,
                                       0) == 1) {
            // SRTP_AEAD_AES_128_GCM:
            //   client_write_SRTP_master_key(16) | server_write_...key(16) |
            //   client_write_SRTP_master_salt(12) | server_write_...salt(12)
            std::vector<uint8_t> key(buf, buf + 16);
            std::vector<uint8_t> salt(buf + 32, buf + 32 + 12);
            if (readyCb_) readyCb_(key, salt);
        } else if (readyCb_) {
            readyCb_({}, {});
        }
        return;
    }
    int err = SSL_get_error(impl_->ssl, rc);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        // 等待对端数据；feed() 后会继续
        return;
    }
    LOG_WARN("DTLS handshake error: %d", err);
#else
    established_ = true;
#endif
}

void DtlsTransport::sendPending() {
#ifdef HAVE_OPENSSL
    if (!impl_->netBio || !impl_->ssl) return;
    char buf[2048];
    while (true) {
        int n = BIO_read(impl_->netBio, buf, sizeof(buf));
        if (n <= 0) break;
        if (sendCb_) sendCb_(reinterpret_cast<const uint8_t*>(buf),
                             static_cast<size_t>(n));
    }
#endif
}

void DtlsTransport::feed(const uint8_t* data, size_t len) {
#ifdef HAVE_OPENSSL
    if (established_) return;
    int w = BIO_write(impl_->netBio, data, static_cast<int>(len));
    if (w <= 0) return;
    if (!established_) doHandshakeStep();
#else
    (void)data;
    (void)len;
#endif
}

}  // namespace streaming
