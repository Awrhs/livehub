/**
 * rtmp_push: 把一个 FLV 文件以 RTMP 推流到服务器（命令行工具样例）。
 *
 * 用法:
 *   rtmp_push <flv-file> <rtmp-url>
 *   例: rtmp_push test.flv rtmp://127.0.0.1:1935/live/test
 *
 * 实现：
 *  - 阻塞式 TCP socket（示例工具，不依赖 net/ 反应堆）
 *  - RTMP 简单握手：C0+C1 → S0+S1+S2 → C2
 *  - AMF0 命令：connect / releaseStream / FCPublish / createStream / publish
 *  - 读取 FLV（FlvDemuxer），按 timestamp 实时节奏，用 ChunkWriter 发送音视频
 *
 * 简历点：手写 RTMP 客户端协议、AMF0 命令交互、FLV 解封装、实时发送节奏控制。
 */
#include "core/Logger.hpp"
#include "protocol/FlvDemuxer.hpp"
#include "rtmp/Amf0.hpp"
#include "rtmp/Chunk.hpp"
#include "rtmp/RtmpMessage.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace streaming;

namespace {

// ---- URL 解析 ----
struct RtmpUrl {
    std::string host;
    uint16_t port = 1935;
    std::string app;     // e.g. live
    std::string stream;  // e.g. test
};

bool parseRtmpUrl(const std::string& url, RtmpUrl& out) {
    // rtmp://host[:port]/app/stream
    auto pos = url.find("://");
    std::string rest = (pos == std::string::npos) ? url : url.substr(pos + 3);
    auto slash = rest.find('/');
    std::string hostport = rest.substr(0, slash);
    std::string path = (slash == std::string::npos) ? "" : rest.substr(slash + 1);
    auto colon = hostport.find(':');
    if (colon == std::string::npos) {
        out.host = hostport;
    } else {
        out.host = hostport.substr(0, colon);
        out.port = static_cast<uint16_t>(std::stoi(hostport.substr(colon + 1)));
    }
    auto s = path.find('/');
    if (s == std::string::npos) {
        out.app = path;
    } else {
        out.app = path.substr(0, s);
        out.stream = path.substr(s + 1);
        auto q = out.stream.find('?');
        if (q != std::string::npos) out.stream = out.stream.substr(0, q);
    }
    return !out.app.empty() && !out.stream.empty();
}

int connectTcp(const std::string& host, uint16_t port) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0)
        return -1;
    int fd = -1;
    for (auto* p = res; p; p = p->ai_next) {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    return fd;
}

bool sendAll(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        ssize_t n = ::send(fd, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool recvAll(int fd, void* data, size_t len) {
    char* p = static_cast<char*>(data);
    while (len > 0) {
        ssize_t n = ::recv(fd, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

// ---- 简单 RTMP 客户端握手 ----
bool handshake(int fd) {
    // C0 + C1
    uint8_t c0c1[1 + 1536];
    c0c1[0] = 0x03;
    for (int i = 1; i < 1537; ++i) c0c1[i] = static_cast<uint8_t>(i & 0xFF);
    // time(4)=0, zero(4)=0, random(1528)
    std::memset(c0c1 + 1, 0, 8);
    if (!sendAll(fd, c0c1, sizeof(c0c1))) return false;

    // S0 + S1 + S2
    uint8_t s0s1s2[1 + 1536 + 1536];
    if (!recvAll(fd, s0s1s2, sizeof(s0s1s2))) return false;

    // C2 = 回显 S1（简单握手）
    return sendAll(fd, s0s1s2 + 1, 1536);
}

// ---- 命令构造辅助 ----
uint32_t nextTxnId() {
    static uint32_t id = 1;
    return id++;
}

std::vector<uint8_t> takeBuffer(Buffer& b) {
    const char* p = b.peek();
    return std::vector<uint8_t>(p, p + b.readableBytes());
}

void writeCommand(Buffer& out, ChunkWriter& w, const std::string& cmd,
                  const std::vector<Amf0Value>& args,
                  uint32_t streamId = 0, uint32_t chunkStreamId = 3) {
    Buffer payload;
    Amf0Value::str(cmd).encode(payload);
    Amf0Value::number(static_cast<double>(nextTxnId())).encode(payload);
    for (auto& a : args) a.encode(payload);

    RtmpMessage msg;
    msg.typeId = 20;  // AMF0 command
    msg.chunkStreamId = chunkStreamId;
    msg.streamId = streamId;
    msg.payload = takeBuffer(payload);
    w.write(out, msg);
}

// ---- 读取一条服务端命令（简化：只读 chunk 重组后的消息）----
bool readResponse(int fd, Buffer& in, ChunkParser& parser,
                  RtmpMessage& outMsg, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (parser.parseMessage(in, outMsg)) return true;
        if (std::chrono::steady_clock::now() > deadline) return false;
        char tmp[4096];
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        in.append(tmp, static_cast<size_t>(n));
    }
}

// 等待服务端窗口确认 / set peer bandwidth / _result(chunkSize, createStream)
// 这里做一个简单的 drain：读到 createStream 的 _result(含 stream id) 即返回。
bool waitForCreateStreamResult(int fd, Buffer& in, ChunkParser& parser,
                               double& outStreamId) {
    RtmpMessage msg;
    for (int i = 0; i < 50; ++i) {
        if (!readResponse(fd, in, parser, msg, std::chrono::seconds(5)))
            return false;
        if (msg.typeId == 20) {
            // AMF0 command: [cmd(string), txnId(number), props, info...]
            Buffer p = msg.payload;
            Amf0Value cmd;
            if (!cmd.decode(p)) continue;
            std::string name = cmd.asString();
            if (name == "_result") {
                Amf0Value txn, props, info;
                txn.decode(p);
                props.decode(p);
                if (!info.decode(p)) continue;
                if (info.isNumber()) {
                    outStreamId = info.asNumber();
                    return true;
                }
            }
        } else if (msg.typeId == 1) {
            // Set Chunk Size
            if (msg.payload.readableBytes() >= 4) {
                const uint8_t* d =
                    reinterpret_cast<const uint8_t*>(msg.payload.peek());
                uint32_t sz = (uint32_t(d[0]) << 24) |
                              (uint32_t(d[1]) << 16) |
                              (uint32_t(d[2]) << 8) | d[3];
                parser.setChunkSize(sz & 0x7FFFFFFF);
            }
        }
    }
    return false;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <flv-file> <rtmp-url>\n", argv[0]);
        return 1;
    }
    std::string flvPath = argv[1];
    std::string urlStr = argv[2];

    RtmpUrl url;
    if (!parseRtmpUrl(urlStr, url)) {
        std::fprintf(stderr, "bad rtmp url: %s\n", urlStr.c_str());
        return 1;
    }

    int fd = connectTcp(url.host, url.port);
    if (fd < 0) {
        std::fprintf(stderr, "connect failed: %s:%u\n", url.host.c_str(),
                     url.port);
        return 1;
    }
    if (!handshake(fd)) {
        std::fprintf(stderr, "handshake failed\n");
        return 1;
    }

    Buffer in;
    ChunkParser parser;
    ChunkWriter writer;
    Buffer out;

    // connect 命令
    Amf0Value::Object cmdObj;
    cmdObj["app"] = Amf0Value::str(url.app);
    cmdObj["type"] = Amf0Value::str("nonprivate");
    cmdObj["flashVer"] = Amf0Value::str("FMLE/3.0 (compatible; lavf)");
    cmdObj["tcUrl"] = Amf0Value::str("rtmp://" + url.host + ":" +
                                     std::to_string(url.port) + "/" +
                                     url.app);
    std::vector<Amf0Value> connectArgs = {
        Amf0Value::object(cmdObj),
    };
    writeCommand(out, writer, "connect", connectArgs, 0, 3);

    // 先发 connect，等待 createStream 之前需要先读 window ack / set peer bw
    // 这些通常在 _result(connect) 之后由服务端发来；我们一并 drain。
    // 简单实现：直接发送后续命令序列，再统一读 createStream 结果。
    // releaseStream / FCPublish / createStream on chunk stream 3
    auto writeNullCmd = [&](const std::string& name) {
        std::vector<Amf0Value> a = {Amf0Value::null(),
                                    Amf0Value::str(url.stream)};
        writeCommand(out, writer, name, a, 0, 3);
    };
    writeNullCmd("releaseStream");
    writeNullCmd("FCPublish");

    std::vector<Amf0Value> csArgs = {Amf0Value::null()};
    writeCommand(out, writer, "createStream", csArgs, 0, 3);

    if (!sendAll(fd, out.peek(), out.readableBytes())) {
        std::fprintf(stderr, "send commands failed\n");
        return 1;
    }
    out.retrieveAll();

    double streamId = 0;
    if (!waitForCreateStreamResult(fd, in, parser, streamId)) {
        std::fprintf(stderr, "waiting createStream result failed\n");
        return 1;
    }

    // publish 命令（在返回的 streamId 上）
    std::vector<Amf0Value> pubArgs = {Amf0Value::null(),
                                      Amf0Value::str(url.stream),
                                      Amf0Value::str("live")};
    writeCommand(out, writer, "publish", pubArgs,
                 static_cast<uint32_t>(streamId), 8);
    // @setDataFrame onMetaData
    {
        Buffer meta;
        Amf0Value::str("@setDataFrame").encode(meta);
        Amf0Value::str("onMetaData").encode(meta);
        Amf0Value::Object md;
        md["duration"] = Amf0Value::number(0);
        md["width"] = Amf0Value::number(1280);
        md["height"] = Amf0Value::number(720);
        md["framerate"] = Amf0Value::number(30);
        md["videocodecid"] = Amf0Value::number(7);
        md["audiocodecid"] = Amf0Value::number(10);
        Amf0Value::ecmaArray(md).encode(meta);
        RtmpMessage m;
        m.typeId = 18;
        m.chunkStreamId = 5;
        m.streamId = static_cast<uint32_t>(streamId);
        m.payload = takeBuffer(meta);
        writer.write(out, m);
    }
    if (!sendAll(fd, out.peek(), out.readableBytes())) {
        std::fprintf(stderr, "send publish failed\n");
        return 1;
    }
    out.retrieveAll();

    // ---- 读 FLV 并按 timestamp 发送 ----
    FILE* flv = std::fopen(flvPath.c_str(), "rb");
    if (!flv) {
        std::fprintf(stderr, "open flv failed: %s\n", flvPath.c_str());
        return 1;
    }
    std::fseek(flv, 0, SEEK_END);
    long fsz = std::ftell(flv);
    std::fseek(flv, 0, SEEK_SET);
    std::vector<uint8_t> flvData(static_cast<size_t>(fsz));
    if (std::fread(flvData.data(), 1, flvData.size(), flv) != flvData.size()) {
        std::fprintf(stderr, "read flv failed\n");
        return 1;
    }
    std::fclose(flv);

    FlvDemuxer demux;
    auto startTime = std::chrono::steady_clock::now();
    uint32_t firstTs = UINT32_MAX;
    bool failed = false;
    demux.setTagCallback(
        [&](const MediaPacket& tag) {
            if (failed) return;
            if (tag.type != MediaPacket::kAudio &&
                tag.type != MediaPacket::kVideo)
                return;
            if (firstTs == UINT32_MAX) firstTs = tag.timestamp;
            uint32_t rel = tag.timestamp - firstTs;

            // 实时节奏
            auto target = startTime + std::chrono::milliseconds(rel);
            std::this_thread::sleep_until(target);

            RtmpMessage m;
            m.typeId = static_cast<uint8_t>(tag.type);
            m.chunkStreamId = (tag.type == MediaPacket::kVideo) ? 6 : 4;
            m.streamId = static_cast<uint32_t>(streamId);
            m.timestamp = rel;
            m.payload = tag.data;
            writer.write(out, m);
            if (!sendAll(fd, out.peek(), out.readableBytes())) {
                std::fprintf(stderr, "send media failed at ts=%u\n", rel);
                failed = true;
            }
            out.retrieveAll();
        });
    demux.feed(flvData.data(), flvData.size());
    if (failed) return 1;

    // deleteStream / close
    std::fprintf(stdout, "push complete: %s\n", url.stream.c_str());
    ::close(fd);
    return 0;
}
