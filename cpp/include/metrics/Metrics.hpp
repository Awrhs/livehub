#pragma once

#include "metrics/Counter.hpp"
#include "metrics/Gauge.hpp"
#include "metrics/Histogram.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace streaming {

/**
 * Metrics: 全局指标注册表，输出 Prometheus 文本格式。
 *
 * 用法：
 *   auto& m = Metrics::instance();
 *   m.counter("http_requests_total")->inc();
 *   m.gauge("connections")->set(n);
 *   m.histogram("http_duration_seconds")->observe(dt);
 *
 *   GET /metrics -> m.render()
 */
class Metrics {
public:
    static Metrics& instance();

    Counter* counter(const std::string& name, const std::string& help = "");
    Gauge* gauge(const std::string& name, const std::string& help = "");
    Histogram* histogram(const std::string& name, const std::string& help = "");

    /// 渲染 Prometheus exposition format
    std::string render() const;

private:
    Metrics() = default;
    mutable std::mutex mu_;
    std::map<std::string, std::unique_ptr<Counter>> counters_;
    std::map<std::string, std::unique_ptr<Gauge>> gauges_;
    std::map<std::string, std::unique_ptr<Histogram>> histograms_;
};

}  // namespace streaming
