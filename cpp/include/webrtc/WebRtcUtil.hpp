#pragma once

#include <cstdint>
#include <string>

namespace streaming {

/// 构造 SDP host 候选行（含 "a=" 前缀）；ip 为空则自动探测首个非回环 IPv4
std::string webrtcCandidateLine(const std::string& ip, uint16_t port);

/// 从请求中提取流 token：Authorization: Bearer <t> 优先，其次 ?token=
class HttpRequest;
std::string webrtcRequestToken(const HttpRequest& req);

}  // namespace streaming
