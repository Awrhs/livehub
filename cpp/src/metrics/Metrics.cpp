#include "metrics/Metrics.hpp"

#include <sstream>

namespace streaming {

Metrics& Metrics::instance() {
    static Metrics m;
    return m;
}

Counter* Metrics::counter(const std::string& name, const std::string& help) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = counters_.find(name);
    if (it != counters_.end()) return it->second.get();
    auto c = std::make_unique<Counter>(name, help);
    auto* p = c.get();
    counters_[name] = std::move(c);
    return p;
}

Gauge* Metrics::gauge(const std::string& name, const std::string& help) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = gauges_.find(name);
    if (it != gauges_.end()) return it->second.get();
    auto g = std::make_unique<Gauge>(name, help);
    auto* p = g.get();
    gauges_[name] = std::move(g);
    return p;
}

Histogram* Metrics::histogram(const std::string& name,
                               const std::string& help) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = histograms_.find(name);
    if (it != histograms_.end()) return it->second.get();
    auto h = std::make_unique<Histogram>(name, help);
    auto* p = h.get();
    histograms_[name] = std::move(h);
    return p;
}

std::string Metrics::render() const {
    std::ostringstream os;
    std::lock_guard<std::mutex> lk(mu_);

    for (const auto& [name, c] : counters_) {
        if (!c->help().empty())
            os << "# HELP " << name << " " << c->help() << "\n";
        os << "# TYPE " << name << " counter\n";
        os << name << " " << c->value() << "\n";
    }
    for (const auto& [name, g] : gauges_) {
        if (!g->help().empty())
            os << "# HELP " << name << " " << g->help() << "\n";
        os << "# TYPE " << name << " gauge\n";
        os << name << " " << g->value() << "\n";
    }
    for (const auto& [name, h] : histograms_) {
        if (!h->help().empty())
            os << "# HELP " << name << " " << h->help() << "\n";
        os << "# TYPE " << name << " histogram\n";
        uint64_t cumulative = 0;
        for (size_t i = 0; i < h->buckets().size(); ++i) {
            cumulative += h->bucketCount(i);
            os << name << "_bucket{le=\"" << h->buckets()[i] << "\"} "
               << cumulative << "\n";
        }
        cumulative += h->bucketCount(h->buckets().size());
        os << name << "_bucket{le=\"+Inf\"} " << cumulative << "\n";
        os << name << "_sum " << h->sum() << "\n";
        os << name << "_count " << h->count() << "\n";
    }
    return os.str();
}

}  // namespace streaming
