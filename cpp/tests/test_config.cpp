#include "config/Config.hpp"
#include "test_framework.hpp"

#include <cstdio>
#include <fstream>
#include <string>

using namespace streaming;

namespace {
// 写入临时配置文件，返回路径
std::string writeTemp(const std::string& body) {
    std::string path = "/tmp/streaming_cfg_test.conf";
    std::ofstream f(path, std::ios::trunc);
    f << body;
    f.close();
    return path;
}
}  // namespace

TEST(config_scalar_keys) {
    // 重置：单例在进程内复用，通过新文件覆盖
    std::string p = writeTemp(
        "HTTP_PORT=18080\nRTMP_PORT=11935\nIO_THREADS=8\nSRT_PORT=19999\n");
    Config::instance().load(p);
    ASSERT_EQ(Config::instance().httpPort(), uint16_t(18080));
    ASSERT_EQ(Config::instance().rtmpPort(), uint16_t(11935));
    ASSERT_EQ(Config::instance().ioThreads(), 8);
    ASSERT_EQ(Config::instance().srtPort(), uint16_t(19999));
    std::remove(p.c_str());
}

TEST(config_vhost_sections) {
    std::string p = writeTemp(
        "[vhost:live.example.com]\n"
        "enable_auth=true\n"
        "jwt_secret=topsecret\n"
        "max_publishers=10\n"
        "webhook_on_publish=http://auth.local/pub\n"
        "webhook_on_play=http://auth.local/play\n"
        "hls_segment_duration=4\n"
        "enable_dvr=true\n"
        "dvr_dir=/data/dvr\n"
        "\n"
        "[vhost:__default__]\n"
        "webhook_on_publish=http://default.local/pub\n");
    Config::instance().load(p);

    auto vhosts = Config::instance().vhosts();
    ASSERT_TRUE(vhosts.find("live.example.com") != vhosts.end());
    auto vh = vhosts["live.example.com"];
    ASSERT_TRUE(vh->enableAuth);
    ASSERT_EQ(vh->jwtSecret, std::string("topsecret"));
    ASSERT_EQ(vh->maxPublishers, 10);
    ASSERT_EQ(vh->webhooks.onPublish, std::string("http://auth.local/pub"));
    ASSERT_EQ(vh->webhooks.onPlay, std::string("http://auth.local/play"));
    ASSERT_EQ(vh->hlsSegmentDuration, 4);
    ASSERT_TRUE(vh->enableDvr);
    ASSERT_EQ(vh->dvrDir, std::string("/data/dvr"));

    ASSERT_TRUE(vhosts.find("__default__") != vhosts.end());
    ASSERT_EQ(vhosts["__default__"]->webhooks.onPublish,
              std::string("http://default.local/pub"));
    std::remove(p.c_str());
}

TEST(config_comments_and_quoted_values) {
    std::string p = writeTemp(
        "# a comment\n"
        "JWT_SECRET=\"quoted secret\"\n"
        "HLS_OUTPUT_DIR='./my hls'\n");
    Config::instance().load(p);
    ASSERT_EQ(Config::instance().jwtSecret(), std::string("quoted secret"));
    ASSERT_EQ(Config::instance().hlsOutputDir(), std::string("./my hls"));
    std::remove(p.c_str());
}

TEST(config_vhost_transcode_profiles) {
    std::string p = writeTemp(
        "[vhost:tenant-a]\n"
        "enable_transcode=true\n"
        "transcode_profiles=360p,480p\n"
        "\n"
        "[vhost:tenant-b]\n"
        "transcode_profiles=1080p\n");
    Config::instance().load(p);
    auto vhosts = Config::instance().vhosts();
    ASSERT_TRUE(vhosts.find("tenant-a") != vhosts.end());
    ASSERT_TRUE(vhosts["tenant-a"]->enableTranscode);
    ASSERT_EQ(vhosts["tenant-a"]->transcodeProfiles, std::string("360p,480p"));
    ASSERT_EQ(vhosts["tenant-b"]->transcodeProfiles, std::string("1080p"));
    std::remove(p.c_str());
}
