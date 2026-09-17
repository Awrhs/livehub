#include "config/Vhost.hpp"
#include "test_framework.hpp"

#include <string>
#include <unordered_map>

using namespace streaming;

TEST(vhost_default_exists) {
    VhostManager mgr;
    auto def = mgr.defaultVhost();
    ASSERT_TRUE(def != nullptr);
    ASSERT_EQ(def->name, std::string("__default__"));
}

TEST(vhost_exact_match) {
    VhostManager mgr;
    auto vh = std::make_shared<Vhost>();
    vh->name = "live.example.com";
    mgr.add(vh);
    ASSERT_EQ(mgr.resolve("live.example.com")->name,
              std::string("live.example.com"));
}

TEST(vhost_wildcard_and_default) {
    VhostManager mgr;
    auto wild = std::make_shared<Vhost>();
    wild->name = "*.example.com";
    mgr.add(wild);

    // 子域名命中通配
    ASSERT_EQ(mgr.resolve("a.example.com")->name,
              std::string("*.example.com"));
    ASSERT_EQ(mgr.resolve("x.y.example.com")->name,
              std::string("*.example.com"));
    // 未匹配回退默认
    ASSERT_EQ(mgr.resolve("other.org")->name, std::string("__default__"));
    // 空 host 回退默认
    ASSERT_EQ(mgr.resolve("")->name, std::string("__default__"));
}

TEST(vhost_disabled_denies) {
    VhostManager mgr;
    auto vh = std::make_shared<Vhost>();
    vh->name = "disabled.test";
    vh->enabled = false;
    mgr.add(vh);
    ASSERT_FALSE(mgr.canPublish(mgr.resolve("disabled.test")));
    ASSERT_FALSE(mgr.canPlay(mgr.resolve("disabled.test")));
}

TEST(vhost_quota_enforcement) {
    VhostManager mgr;
    auto vh = std::make_shared<Vhost>();
    vh->name = "quota.test";
    vh->maxPublishers = 1;
    vh->maxViewers = 1;
    mgr.add(vh);
    auto r = mgr.resolve("quota.test");

    ASSERT_TRUE(mgr.canPublish(r));
    vh->publishers.fetch_add(1);
    ASSERT_FALSE(mgr.canPublish(r));  // 达到上限

    ASSERT_TRUE(mgr.canPlay(r));
    vh->viewers.fetch_add(1);
    ASSERT_FALSE(mgr.canPlay(r));
}

TEST(vhost_reload_replaces_table) {
    VhostManager mgr;
    auto a = std::make_shared<Vhost>();
    a->name = "a.test";
    mgr.add(a);
    ASSERT_EQ(mgr.resolve("a.test")->name, std::string("a.test"));

    // reload 一张全新的表，旧 vhost 消失；默认 vhost 自动补回
    std::unordered_map<std::string, VhostPtr> table;
    auto b = std::make_shared<Vhost>();
    b->name = "b.test";
    table["b.test"] = b;
    mgr.reload(table);

    ASSERT_EQ(mgr.resolve("a.test")->name, std::string("__default__"));
    ASSERT_EQ(mgr.resolve("b.test")->name, std::string("b.test"));
    ASSERT_TRUE(mgr.defaultVhost() != nullptr);
}
