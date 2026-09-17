#include "config/Vhost.hpp"

#include "core/Logger.hpp"

#include <algorithm>

namespace streaming {

static const std::string kDefaultVhost = "__default__";

VhostManager::VhostManager() {
    auto def = std::make_shared<Vhost>();
    def->name = kDefaultVhost;
    def->webhooks.enabled = false;
    vhosts_[kDefaultVhost] = def;
}

void VhostManager::reload(
    std::unordered_map<std::string, VhostPtr> vhosts) {
    std::lock_guard<std::mutex> lk(mu_);
    // 始终保留一个默认 Vhost
    if (vhosts.find(kDefaultVhost) == vhosts.end()) {
        auto def = std::make_shared<Vhost>();
        def->name = kDefaultVhost;
        vhosts[kDefaultVhost] = def;
    }
    vhosts_ = std::move(vhosts);
    LOG_INFO("VhostManager reloaded, %zu vhosts", vhosts_.size());
}

void VhostManager::add(const VhostPtr& vh) {
    if (!vh) return;
    std::lock_guard<std::mutex> lk(mu_);
    vhosts_[vh->name] = vh;
}

void VhostManager::remove(const std::string& name) {
    if (name == kDefaultVhost) return;
    std::lock_guard<std::mutex> lk(mu_);
    vhosts_.erase(name);
}

VhostPtr VhostManager::defaultVhost() const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = vhosts_.find(kDefaultVhost);
    if (it != vhosts_.end()) return it->second;
    return nullptr;
}

VhostPtr VhostManager::resolve(const std::string& host) const {
    if (host.empty()) return defaultVhost();
    // 去掉端口
    std::string h = host;
    auto colon = h.find(':');
    if (colon != std::string::npos) h = h.substr(0, colon);

    std::lock_guard<std::mutex> lk(mu_);
    auto it = vhosts_.find(h);
    if (it != vhosts_.end()) return it->second;

    // 通配：*.example.com
    auto dot = h.find('.');
    if (dot != std::string::npos) {
        std::string wildcard = "*" + h.substr(dot);
        it = vhosts_.find(wildcard);
        if (it != vhosts_.end()) return it->second;
    }

    auto def = vhosts_.find(kDefaultVhost);
    return def != vhosts_.end() ? def->second : nullptr;
}

std::vector<std::string> VhostManager::listNames() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    out.reserve(vhosts_.size());
    for (const auto& kv : vhosts_) out.push_back(kv.first);
    std::sort(out.begin(), out.end());
    return out;
}

bool VhostManager::canPublish(const VhostPtr& vh) const {
    if (!vh) return true;
    if (!vh->enabled) return false;
    if (vh->maxPublishers > 0 &&
        vh->publishers.load() >= vh->maxPublishers)
        return false;
    return true;
}

bool VhostManager::canPlay(const VhostPtr& vh) const {
    if (!vh) return true;
    if (!vh->enabled) return false;
    if (vh->maxViewers > 0 &&
        vh->viewers.load() >= vh->maxViewers)
        return false;
    return true;
}

}  // namespace streaming
