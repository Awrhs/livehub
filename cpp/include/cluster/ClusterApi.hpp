#pragma once

#include "cluster/HealthChecker.hpp"
#include "cluster/LoadBalancer.hpp"
#include "cluster/NodeRegistry.hpp"
#include "core/NonCopyable.hpp"

namespace streaming {

class HttpServer;

/**
 * ClusterApi: 集群管理 HTTP 端点（实现在 ClusterApi.cpp）。
 */
class ClusterApi : NonCopyable {
public:
    ClusterApi(NodeRegistry* reg, LoadBalancer* lb, HealthChecker* hc)
        : reg_(reg), lb_(lb), hc_(hc) {}

    void registerRoutes(HttpServer& server);

private:
    NodeRegistry* reg_;
    LoadBalancer* lb_;
    HealthChecker* hc_;
};

}  // namespace streaming
