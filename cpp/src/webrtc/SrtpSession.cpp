#include "webrtc/SrtpSession.hpp"
#include "core/Logger.hpp"

#include <cstring>

#ifdef HAVE_SRTP
#include <srtp2/srtp.h>
#endif

namespace streaming {

#ifdef HAVE_SRTP
struct SrtpSession::Impl {
    srtp_t sendSession = nullptr;
    srtp_t recvSession = nullptr;
    static bool initialized;
};
bool SrtpSession::Impl::initialized = false;
#else
struct SrtpSession::Impl {};
#endif

SrtpSession::SrtpSession() : impl_(new Impl()) {}

SrtpSession::~SrtpSession() {
#ifdef HAVE_SRTP
    if (impl_->sendSession) srtp_dealloc(impl_->sendSession);
    if (impl_->recvSession) srtp_dealloc(impl_->recvSession);
#endif
    delete impl_;
}

#ifdef HAVE_SRTP
static srtp_t createSession(int profile, const std::vector<uint8_t>& key,
                            const std::vector<uint8_t>& salt,
                            ssrc_type_t direction) {
    if (!Impl::initialized) {
        srtp_init();
        Impl::initialized = true;
    }
    srtp_policy_t policy;
    std::memset(&policy, 0, sizeof(policy));
    // 按协商的 profile 选择加密套件（原实现固定 GCM）
    if (profile == SrtpSession::kAeadAes128Gcm) {
        srtp_crypto_policy_set_aes_gcm_128_16_auth(&policy.rtp);
        srtp_crypto_policy_set_aes_gcm_128_16_auth(&policy.rtcp);
    } else {
        srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtp);
        srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtcp);
    }
    policy.ssrc.type = direction;
    policy.ssrc.value = 0;
    policy.window_size = 1024;
    policy.allow_repeat_tags = 0;
    // master key + salt 拼接：key(16) + salt(12)。
    // srtp_create 内部会拷贝密钥，因此本地 ks 在函数返回后释放是安全的。
    std::vector<uint8_t> ks = key;
    ks.insert(ks.end(), salt.begin(), salt.end());
    policy.key = ks.data();
    srtp_t session = nullptr;
    if (srtp_create(&session, &policy) != srtp_err_status_ok) {
        return nullptr;
    }
    return session;
}
#endif

bool SrtpSession::initSend(int profile, const std::vector<uint8_t>& key,
                           const std::vector<uint8_t>& salt) {
#ifdef HAVE_SRTP
    impl_->sendSession = createSession(profile, key, salt, SSRC_ANY_OUTBOUND);
    return impl_->sendSession != nullptr;
#else
    (void)profile;
    (void)key;
    (void)salt;
    LOG_WARN("SRTP: libsrtp2 unavailable, running in passthrough mode");
    return true;
#endif
}

bool SrtpSession::initRecv(int profile, const std::vector<uint8_t>& key,
                           const std::vector<uint8_t>& salt) {
#ifdef HAVE_SRTP
    impl_->recvSession = createSession(profile, key, salt, SSRC_ANY_INBOUND);
    return impl_->recvSession != nullptr;
#else
    (void)profile;
    (void)key;
    (void)salt;
    return true;
#endif
}

int SrtpSession::protectRtp(uint8_t* data, int len, int cap) {
#ifdef HAVE_SRTP
    int outLen = cap;
    if (srtp_protect(impl_->sendSession, data, &outLen) !=
        srtp_err_status_ok) {
        return -1;
    }
    return outLen;
#else
    (void)data;
    (void)cap;
    return len;
#endif
}

int SrtpSession::unprotectRtp(uint8_t* data, int len) {
#ifdef HAVE_SRTP
    int outLen = len;
    if (srtp_unprotect(impl_->recvSession, data, &outLen) !=
        srtp_err_status_ok) {
        return -1;
    }
    return outLen;
#else
    (void)data;
    return len;
#endif
}

int SrtpSession::protectRtcp(uint8_t* data, int len, int cap) {
#ifdef HAVE_SRTP
    int outLen = cap;
    if (srtp_protect_rtcp(impl_->sendSession, data, &outLen) !=
        srtp_err_status_ok) {
        return -1;
    }
    return outLen;
#else
    (void)data;
    (void)cap;
    return len;
#endif
}

int SrtpSession::unprotectRtcp(uint8_t* data, int len) {
#ifdef HAVE_SRTP
    int outLen = len;
    if (srtp_unprotect_rtcp(impl_->recvSession, data, &outLen) !=
        srtp_err_status_ok) {
        return -1;
    }
    return outLen;
#else
    (void)data;
    return len;
#endif
}

}  // namespace streaming
