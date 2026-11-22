#include "config/Config.hpp"
#include "core/Logger.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace streaming {

namespace {
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return s;
}
std::string getEnv(const char* name, const std::string& def = "") {
    const char* v = std::getenv(name);
    return v ? std::string(v) : def;
}
int getEnvInt(const char* name, int def) {
    const char* v = std::getenv(name);
    if (!v) return def;
    try { return std::stoi(v); } catch (...) { return def; }
}
}  // namespace

Config& Config::instance() {
    static Config c;
    return c;
}

bool Config::load(const std::string& file) {
    if (!file.empty()) {
        loadFromFile(file);
    }
    loadFromEnv();
    return true;
}

void Config::reloadVhosts(const std::string& path) { loadFromFile(path, true); }

void Config::loadFromFile(const std::string& path, bool vhostsOnly) {
    std::ifstream f(path);
    if (!f) {
        LOG_WARN("config file %s not readable, using env/defaults", path.c_str());
        return;
    }

    // vhost 先解析到局部表，最后在锁内整体替换（读者通过 vhosts() 加锁拿快照）
    std::unordered_map<std::string, VhostPtr> newVhosts;
    VhostPtr currentVhost;
    int ignoredScalars = 0;

    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        // section 头：[vhost:<name>]
        if (line.front() == '[' && line.back() == ']') {
            std::string sec = trim(line.substr(1, line.size() - 2));
            currentVhost.reset();
            std::string low = sec;
            std::transform(low.begin(), low.end(), low.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            const std::string prefix = "vhost:";
            if (low.compare(0, prefix.size(), prefix) == 0) {
                std::string name = trim(sec.substr(prefix.size()));
                if (!name.empty()) {
                    auto vh = std::make_shared<Vhost>();
                    vh->name = name;
                    currentVhost = vh;
                    newVhosts[name] = vh;
                }
            }
            continue;
        }

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        // 去掉首尾引号
        if (val.size() >= 2 && (val.front() == '"' || val.front() == '\'') &&
            val.back() == val.front()) {
            val = val.substr(1, val.size() - 2);
        }
        if (currentVhost) {
            applyVhostKv(currentVhost, key, val);
        } else if (vhostsOnly) {
            ++ignoredScalars;
        } else {
            applyKv(key, val);
        }
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        vhosts_.swap(newVhosts);
    }
    if (vhostsOnly && ignoredScalars > 0)
        LOG_INFO("config reload: %d top-level settings ignored (restart required)",
                 ignoredScalars);
}

std::unordered_map<std::string, VhostPtr> Config::vhosts() const {
    std::lock_guard<std::mutex> lk(mu_);
    return vhosts_;
}

void Config::loadFromEnv() {
    httpPort_ = static_cast<uint16_t>(
        getEnvInt("STREAM_HTTP_PORT", httpPort_));
    rtmpPort_ = static_cast<uint16_t>(
        getEnvInt("STREAM_RTMP_PORT", rtmpPort_));
    ioThreads_ = getEnvInt("STREAM_IO_THREADS", ioThreads_);
    idleTimeoutSec_ = getEnvInt("STREAM_IDLE_TIMEOUT", idleTimeoutSec_);
    maxConnections_ = getEnvInt("STREAM_MAX_CONNECTIONS", maxConnections_);
    if (!getEnv("STREAM_WEBRTC_CANDIDATE_IP", "").empty())
        webrtcCandidateIp_ = getEnv("STREAM_WEBRTC_CANDIDATE_IP", "");
    maxConnectionsPerIp_ = getEnvInt("STREAM_MAX_CONNECTIONS_PER_IP", maxConnectionsPerIp_);
    hlsSegmentDuration_ =
        getEnvInt("STREAM_HLS_SEGMENT_DURATION", hlsSegmentDuration_);
    hlsWindowSize_ = getEnvInt("STREAM_HLS_WINDOW_SIZE", hlsWindowSize_);
    cacheSizeMB_ = getEnvInt("STREAM_CACHE_SIZE_MB",
                              static_cast<int>(cacheSizeMB_));

    std::string v;
    v = getEnv("STREAM_HLS_OUTPUT_DIR", "");
    if (!v.empty()) hlsOutputDir_ = v;
    v = getEnv("STREAM_JWT_SECRET", "");
    if (!v.empty()) jwtSecret_ = v;
    v = getEnv("STREAM_ADMIN_TOKEN", "");
    if (!v.empty()) adminToken_ = v;
    v = getEnv("STREAM_LOG_LEVEL", "");
    if (!v.empty()) logLevel_ = v;
    v = getEnv("STREAM_LOG_JSON", "");
    if (!v.empty()) logJson_ = (v != "0" && v != "false");
    v = getEnv("STREAM_NODE_ID", "");
    if (!v.empty()) nodeId_ = v;
    v = getEnv("STREAM_ORIGIN_URL", "");
    if (!v.empty()) originUrl_ = v;
    v = getEnv("STREAM_RATE_LIMIT", "");
    if (!v.empty()) enableRateLimit_ = (v != "0" && v != "false");
    rateLimitPerSec_ =
        getEnvInt("STREAM_RATE_LIMIT_PER_SEC", rateLimitPerSec_);
    rateLimitBurst_ = getEnvInt("STREAM_RATE_LIMIT_BURST", rateLimitBurst_);
    ratePlaybackPerMin_ =
        getEnvInt("STREAM_RATE_PLAYBACK_PER_MIN", ratePlaybackPerMin_);
    rateApiPerMin_ = getEnvInt("STREAM_RATE_API_PER_MIN", rateApiPerMin_);
    rateAuthPerMin_ = getEnvInt("STREAM_RATE_AUTH_PER_MIN", rateAuthPerMin_);
    rateTranscodePerMin_ =
        getEnvInt("STREAM_RATE_TRANSCODE_PER_MIN", rateTranscodePerMin_);
    rateAdminPerMin_ =
        getEnvInt("STREAM_RATE_ADMIN_PER_MIN", rateAdminPerMin_);
    v = getEnv("STREAM_CLUSTER_NODES", "");
    if (!v.empty()) {
        clusterNodes_.clear();
        std::stringstream ss(v);
        std::string item;
        while (std::getline(ss, item, ',')) {
            item = trim(item);
            if (!item.empty()) clusterNodes_.push_back(item);
        }
    }
    v = getEnv("STREAM_TRANSCODE_PROFILES", "");
    if (!v.empty()) transcodeProfiles_ = v;
    transcodeConcurrency_ =
        getEnvInt("STREAM_TRANSCODE_CONCURRENCY", transcodeConcurrency_);
    transcodeTaskTtlSec_ =
        getEnvInt("STREAM_TRANSCODE_TASK_TTL_SEC", transcodeTaskTtlSec_);
    srtPort_ = static_cast<uint16_t>(getEnvInt("STREAM_SRT_PORT", srtPort_));
    rtspPort_ = static_cast<uint16_t>(getEnvInt("STREAM_RTSP_PORT", rtspPort_));
    v = getEnv("STREAM_REDIS_HOST", "");
    if (!v.empty()) redisHost_ = v;
    redisPort_ =
        static_cast<uint16_t>(getEnvInt("STREAM_REDIS_PORT", redisPort_));
    redisTimeoutMs_ = getEnvInt("STREAM_REDIS_TIMEOUT_MS", redisTimeoutMs_);
    v = getEnv("STREAM_REDIS_PASSWORD", "");
    if (!v.empty()) redisPassword_ = v;
}

void Config::applyKv(const std::string& keyIn, const std::string& value) {
    std::string key = upper(keyIn);
    auto asInt = [&value]() {
        try { return std::stoi(value); } catch (...) { return 0; }
    };
    if (key == "HTTP_PORT") httpPort_ = static_cast<uint16_t>(asInt());
    else if (key == "RTMP_PORT") rtmpPort_ = static_cast<uint16_t>(asInt());
    else if (key == "IO_THREADS") ioThreads_ = asInt();
    else if (key == "IDLE_TIMEOUT") idleTimeoutSec_ = asInt();
    else if (key == "MAX_CONNECTIONS") maxConnections_ = asInt();
    else if (key == "WEBRTC_CANDIDATE_IP") webrtcCandidateIp_ = value;
    else if (key == "MAX_CONNECTIONS_PER_IP") maxConnectionsPerIp_ = asInt();
    else if (key == "HLS_SEGMENT_DURATION") hlsSegmentDuration_ = asInt();
    else if (key == "HLS_WINDOW_SIZE") hlsWindowSize_ = asInt();
    else if (key == "HLS_OUTPUT_DIR") hlsOutputDir_ = value;
    else if (key == "CACHE_SIZE_MB") cacheSizeMB_ = asInt();
    else if (key == "JWT_SECRET") jwtSecret_ = value;
    else if (key == "ADMIN_TOKEN") adminToken_ = value;
    else if (key == "LOG_LEVEL") logLevel_ = value;
    else if (key == "LOG_JSON") logJson_ = (value != "0" && value != "false");
    else if (key == "ENABLE_RATE_LIMIT")
        enableRateLimit_ = (value != "0" && value != "false");
    else if (key == "RATE_LIMIT_PER_SEC") rateLimitPerSec_ = asInt();
    else if (key == "RATE_LIMIT_BURST") rateLimitBurst_ = asInt();
    else if (key == "RATE_PLAYBACK_PER_MIN") ratePlaybackPerMin_ = asInt();
    else if (key == "RATE_API_PER_MIN") rateApiPerMin_ = asInt();
    else if (key == "RATE_AUTH_PER_MIN") rateAuthPerMin_ = asInt();
    else if (key == "RATE_TRANSCODE_PER_MIN") rateTranscodePerMin_ = asInt();
    else if (key == "RATE_ADMIN_PER_MIN") rateAdminPerMin_ = asInt();
    else if (key == "NODE_ID") nodeId_ = value;
    else if (key == "ORIGIN_URL") originUrl_ = value;
    else if (key == "TRANSCODE_PROFILES") transcodeProfiles_ = value;
    else if (key == "TRANSCODE_CONCURRENCY") transcodeConcurrency_ = asInt();
    else if (key == "TRANSCODE_TASK_TTL_SEC") transcodeTaskTtlSec_ = asInt();
    else if (key == "SRT_PORT") srtPort_ = static_cast<uint16_t>(asInt());
    else if (key == "RTSP_PORT") rtspPort_ = static_cast<uint16_t>(asInt());
    else if (key == "REDIS_HOST") redisHost_ = value;
    else if (key == "REDIS_PORT")
        redisPort_ = static_cast<uint16_t>(asInt());
    else if (key == "REDIS_TIMEOUT_MS") redisTimeoutMs_ = asInt();
    else if (key == "REDIS_PASSWORD") redisPassword_ = value;
}

void Config::applyVhostKv(const VhostPtr& vh, const std::string& keyIn,
                          const std::string& value) {
    std::string key = keyIn;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    auto asBool = [&]() { return value != "0" && value != "false" &&
                                     value != "no" && !value.empty(); };
    auto asInt = [&]() {
        try { return std::stoi(value); } catch (...) { return 0; }
    };
    auto asI64 = [&]() {
        try { return static_cast<int64_t>(std::stoll(value)); } catch (...) {
            return static_cast<int64_t>(0);
        }
    };

    auto& w = vh->webhooks;
    if (key == "enabled") vh->enabled = asBool();
    else if (key == "default_app") vh->defaultApp = value;
    else if (key == "enable_auth") vh->enableAuth = asBool();
    else if (key == "jwt_secret") vh->jwtSecret = value;
    else if (key == "allow_apps") {
        vh->allowApps.clear();
        std::stringstream ss(value);
        std::string item;
        while (std::getline(ss, item, ',')) {
            item = trim(item);
            if (!item.empty()) vh->allowApps.push_back(item);
        }
    }
    else if (key == "enable_hls") vh->enableHls = asBool();
    else if (key == "hls_segment_duration") vh->hlsSegmentDuration = asInt();
    else if (key == "hls_window_size") vh->hlsWindowSize = asInt();
    else if (key == "enable_dvr") vh->enableDvr = asBool();
    else if (key == "dvr_dir") vh->dvrDir = value;
    else if (key == "enable_transcode") vh->enableTranscode = asBool();
    else if (key == "transcode_profiles") vh->transcodeProfiles = value;
    else if (key == "max_publishers") vh->maxPublishers = asInt();
    else if (key == "max_viewers") vh->maxViewers = asInt();
    else if (key == "max_bandwidth_bps") vh->maxBandwidthBps = asI64();
    // webhook URLs
    else if (key == "webhook_on_connect") w.onConnect = value;
    else if (key == "webhook_on_close") w.onClose = value;
    else if (key == "webhook_on_publish") w.onPublish = value;
    else if (key == "webhook_on_unpublish") w.onUnpublish = value;
    else if (key == "webhook_on_play") w.onPlay = value;
    else if (key == "webhook_on_stop") w.onStop = value;
    else if (key == "webhook_on_dvr") w.onDvr = value;
    else if (key == "webhook_on_hls") w.onHls = value;
    else if (key == "webhook_enabled") w.enabled = asBool();
    else if (key == "webhook_timeout_ms") w.timeoutMs = asInt();
    else if (key == "webhook_max_retries") w.maxRetries = asInt();
}

}  // namespace streaming
