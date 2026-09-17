#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace streaming {

/**
 * Node: 集群节点信息。
 */
struct Node {
    std::string id;
    std::string ip;
    uint16_t httpPort = 8080;
    uint16_t rtmpPort = 1935;
    int weight = 1;

    std::atomic<int> load{0};             // 当前连接/负载数
    std::atomic<bool> healthy{true};
    std::atomic<int> consecutiveFailures{0};
    std::atomic<int64_t> lastCheckMs{0};

    Node() = default;
    Node(std::string id_, std::string ip_, uint16_t http = 8080,
         uint16_t rtmp = 1935, int w = 1)
        : id(std::move(id_)), ip(std::move(ip_)), httpPort(http),
          rtmpPort(rtmp), weight(w) {}

    std::string httpBase() const {
        return "http://" + ip + ":" + std::to_string(httpPort);
    }
    std::string rtmpUrl() const {
        return "rtmp://" + ip + ":" + std::to_string(rtmpPort);
    }
};

using NodePtr = std::shared_ptr<Node>;

}  // namespace streaming
