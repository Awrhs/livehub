// 多 Reactor 并发 echo 压测：
//   启动一个 setThreadNum(N) 的 TcpServer echo 服务，
//   再用 M 条裸 socket 连接并发收发，统计总 QPS/吞吐。
//
// 用法：./bench_concurrent [ioThreads] [connections] [seconds]
#include "core/Logger.hpp"
#include "net/EventLoop.hpp"
#include "net/EventLoopThreadPool.hpp"
#include "net/TcpServer.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace streaming;

static int connectOne(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    return fd;
}

int main(int argc, char** argv) {
    int ioThreads = argc > 1 ? std::atoi(argv[1]) : 4;
    int connections = argc > 2 ? std::atoi(argv[2]) : 200;
    int seconds = argc > 3 ? std::atoi(argv[3]) : 5;

    // 找一个空闲端口：bind(0) 后取端口
    int tmpFd = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    ::setsockopt(tmpFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in tmp{};
    tmp.sin_family = AF_INET;
    tmp.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    tmp.sin_port = 0;
    ::bind(tmpFd, reinterpret_cast<sockaddr*>(&tmp), sizeof(tmp));
    ::listen(tmpFd, 128);
    socklen_t slen = sizeof(tmp);
    ::getsockname(tmpFd, reinterpret_cast<sockaddr*>(&tmp), &slen);
    uint16_t port = ntohs(tmp.sin_port);
    ::close(tmpFd);

    EventLoop loop;
    TcpServer server(&loop, port, "127.0.0.1");
    server.setThreadNum(ioThreads);
    std::atomic<long long> bytesEchoed{0};
    server.setMessageCallback(
        [&](const TcpConnectionPtr& conn, Buffer* buf, Timestamp) {
            size_t n = buf->readableBytes();
            bytesEchoed += static_cast<long long>(n);
            conn->send(buf->retrieveAllAsString());
        });
    server.start();

    std::thread serverThread([&] { loop.loop(); });

    // 给服务器一点时间
    ::usleep(200 * 1000);

    std::vector<int> fds;
    for (int i = 0; i < connections; ++i) {
        int fd = connectOne(port);
        // 等待连接完成（简单重试）
        for (int r = 0; r < 100; ++r) {
            int err = 0;
            socklen_t elen = sizeof(err);
            ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen);
            if (err == 0) break;
            ::usleep(1000);
        }
        fds.push_back(fd);
    }

    const char* msg = "BENCH_PAYLOAD_1234567890";
    const size_t msgLen = std::strlen(msg);

    auto t0 = std::chrono::steady_clock::now();
    auto deadline = t0 + std::chrono::seconds(seconds);
    std::atomic<long long> sent{0}, recv{0};

    unsigned ncpu = std::thread::hardware_concurrency();
    if (ncpu == 0) ncpu = 4;
    int workers = static_cast<int>(ncpu);
    std::vector<std::thread> clients;
    for (int w = 0; w < workers; ++w) {
        clients.emplace_back([&, w] {
            for (int i = w; i < connections; i += workers) {
                int fd = fds[i];
                char rbuf[256];
                while (std::chrono::steady_clock::now() < deadline) {
                    ::send(fd, msg, msgLen, MSG_NOSIGNAL);
                    sent += msgLen;
                    ssize_t n = ::recv(fd, rbuf, sizeof(rbuf), 0);
                    if (n > 0) recv += n;
                }
            }
        });
    }
    for (auto& t : clients) t.join();
    auto t1 = std::chrono::high_resolution_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();

    for (int fd : fds) ::close(fd);
    loop.quit();
    serverThread.join();

    std::printf(
        "Concurrent: ioThreads=%d conns=%d time=%.1fs | sent=%.2f MB/s "
        "recv=%.2f MB/s echoed=%.2f MB/s\n",
        ioThreads, connections, sec, sent / sec / 1e6, recv / sec / 1e6,
        bytesEchoed / sec / 1e6);
    return 0;
}
