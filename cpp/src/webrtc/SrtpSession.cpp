#include "webrtc/SrtpSession.hpp"
#include "core/Logger.hpp"

#include <cstring>
#include <mutex>

#ifdef HAVE_SRTP
#include <srtp2/srtp.h>
#endif

namespace streaming {

#ifdef HAVE_SRTP
struct SrtpSession::Impl {
    srtp_t sendSession = nullptr;
    srtp_t recvSession = nullptr;
};
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

// 注：HAVE_SRTP 路径此前从未被编译过（CMake 未定义该宏），API 用法与 libsrtp2 不符
#ifdef HAVE_SRTP
static srtp_t createSession(int profile, const std::vector<uint8_t>& key,
                            const std::vector<uint8_t>& salt,
                            srtp_ssrc_type_t direction) {
    static std::once_flag once;
    std::call_once(once, [] { srtp_init(); });
    srtp_policy_t policy;
    std::memset(&policy, 0, sizeof(policy));
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
    policy.allow_repeat_tx = 1;  // 允许重传包（NACK 重发）使用相同序号
    // master key || master salt（srtp_create 内部拷贝）
    std::vector<uint8_t> ks = key;
    ks.insert(ks.end(), salt.begin(), salt.end());
    policy.key = ks.data();
    srtp_t session = nullptr;
    srtp_err_status_t st = srtp_create(&session, &policy);
    if (st != srtp_err_status_ok) {
        LOG_WARN("SRTP: srtp_create failed: %d", static_cast<int>(st));
        return nullptr;
    }
    return session;
}
#endif

bool SrtpSession::initSend(int profile, const std::vector<uint8_t>& key,
                           const std::vector<uint8_t>& salt) {
#ifdef HAVE_SRTP
    if (key.empty()) return false;
    impl_->sendSession = createSession(profile, key, salt, ssrc_any_outbound);
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
    if (key.empty()) return false;
    impl_->recvSession = createSession(profile, key, salt, ssrc_any_inbound);
    return impl_->recvSession != nullptr;
#else
    (void)profile;
    (void)key;
    (void)salt;
    return true;
#endif
}

// srtp_protect 的长度参数：输入明文长度，输出密文长度（缓冲区需预留尾部认证标签空间）
int SrtpSession::protectRtp(uint8_t* data, int len, int cap) {
#ifdef HAVE_SRTP
    if (!impl_->sendSession || cap < len + SRTP_MAX_TRAILER_LEN) return -1;
    int outLen = len;
    if (srtp_protect(impl_->sendSession, data, &outLen) != srtp_err_status_ok) return -1;
    return outLen;
#else
    (void)data;
    (void)cap;
    return len;
#endif
}

int SrtpSession::unprotectRtp(uint8_t* data, int len) {
#ifdef HAVE_SRTP
    if (!impl_->recvSession) return -1;
    int outLen = len;
    if (srtp_unprotect(impl_->recvSession, data, &outLen) != srtp_err_status_ok) return -1;
    return outLen;
#else
    (void)data;
    return len;
#endif
}

int SrtpSession::protectRtcp(uint8_t* data, int len, int cap) {
#ifdef HAVE_SRTP
    if (!impl_->sendSession || cap < len + SRTP_MAX_TRAILER_LEN) return -1;
    int outLen = len;
    if (srtp_protect_rtcp(impl_->sendSession, data, &outLen) != srtp_err_status_ok) return -1;
    return outLen;
#else
    (void)data;
    (void)cap;
    return len;
#endif
}

int SrtpSession::unprotectRtcp(uint8_t* data, int len) {
#ifdef HAVE_SRTP
    if (!impl_->recvSession) return -1;
    int outLen = len;
    if (srtp_unprotect_rtcp(impl_->recvSession, data, &outLen) != srtp_err_status_ok)
        return -1;
    return outLen;
#else
    (void)data;
    return len;
#endif
}

}  // namespace streaming
