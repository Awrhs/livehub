// TcpConnection::sendFile / 共享缓冲发送：socket 写满后靠 EPOLLOUT 续传、与前后数据保持顺序、
// 对端中途断开时释放文件并关闭连接。
#include "net/EventLoop.hpp"
#include "net/TcpConnection.hpp"
#include "test_framework.hpp"

#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>

using namespace streaming;

namespace {

// 在独立线程运行 EventLoop，并在该线程中为 fd 创建 TcpConnection（与 TcpServer 的用法一致）
struct LoopConn {
    std::thread th;
    EventLoop* loop = nullptr;
    std::shared_ptr<TcpConnection> conn;
    std::promise<void> ready;  // 成员：保证线程中 set_value 期间 promise 仍存活

    explicit LoopConn(int fd) {
        auto fut = ready.get_future();
        th = std::thread([this, fd] {
            EventLoop lp;
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            conn = std::make_shared<TcpConnection>(&lp, "sendfile-test", fd, addr, addr);
            conn->connectEstablished();
            loop = &lp;
            ready.set_value();
            lp.loop();
        });
        fut.wait();
    }

    // 断言失败抛异常时也要结束 loop 线程（否则 std::thread 析构直接 terminate）
    ~LoopConn() {
        if (th.joinable()) stop();
    }

    void stop() {
        std::promise<void> done;
        loop->runInLoop([this, &done] {
            conn->connectDestroyed();
            conn.reset();
            done.set_value();
        });
        done.get_future().wait();
        loop->quit();
        th.join();
    }
};

// 写入一个已 unlink 的临时文件，返回 fd（内容为确定的伪随机字节）
int makeTempFile(size_t size, std::string* content) {
    char path[] = "/tmp/sendfile_testXXXXXX";
    int fd = ::mkstemp(path);
    if (fd < 0) return -1;
    ::unlink(path);
    content->resize(size);
    for (size_t i = 0; i < size; ++i) (*content)[i] = static_cast<char>((i * 131 + i / 7) & 0xFF);
    size_t off = 0;
    while (off < size) {
        ssize_t n = ::write(fd, content->data() + off, size - off);
        if (n <= 0) return -1;
        off += static_cast<size_t>(n);
    }
    return fd;
}

void makePair(int sv[2]) {
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) std::abort();
    ::fcntl(sv[0], F_SETFL, ::fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    int small = 16 * 1024;  // 小发送缓冲：保证 sendfile 很快 EAGAIN，走 EPOLLOUT 续传路径
    ::setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
}

std::string readExactly(int fd, size_t want, int timeoutMs) {
    std::string out;
    char buf[65536];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (out.size() < want && std::chrono::steady_clock::now() < deadline) {
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, 100) <= 0) continue;
        size_t room = std::min(sizeof(buf), want - out.size());  // 恰好读 want 字节
        ssize_t n = ::read(fd, buf, room);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    return out;
}

bool waitFor(const std::function<bool()>& cond, int timeoutMs) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return cond();
}

std::shared_ptr<const std::string> shared(const std::string& s) {
    return std::make_shared<std::string>(s);
}

}  // namespace

TEST(tcp_sendfile_cross_thread_resumes_on_epollout_in_order) {
    int sv[2];
    makePair(sv);
    std::string content;
    int f = makeTempFile(3 * 1024 * 1024 + 123, &content);
    ASSERT_TRUE(f >= 0);
    LoopConn lc(sv[0]);

    // 跨线程依次投递：头部、文件区间（偏移 100）、共享缓冲
    lc.conn->send(std::string("HEAD\r\n"));
    lc.conn->sendFile(f, 100, content.size() - 100);
    lc.conn->send(shared("TAIL"));

    // 对端暂不读取：socket 写满，剩余部分应排队等待 EPOLLOUT
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_GT(lc.conn->pendingOutputBytes(), static_cast<size_t>(0));

    std::string expect = "HEAD\r\n" + content.substr(100) + "TAIL";
    std::string got = readExactly(sv[1], expect.size(), 10000);
    ASSERT_EQ(got.size(), expect.size());
    ASSERT_TRUE(got == expect);
    ASSERT_TRUE(waitFor([&] { return lc.conn->pendingOutputBytes() == 0; }, 2000));

    // 文件发完后普通发送恢复直接写
    lc.conn->send(std::string("NEXT"));
    ASSERT_TRUE(readExactly(sv[1], 4, 2000) == "NEXT");
    lc.stop();
    ::close(sv[1]);
}

TEST(tcp_sendfile_in_loop_queues_later_data_behind_file) {
    int sv[2];
    makePair(sv);
    std::string content;
    int f = makeTempFile(1024 * 1024, &content);
    ASSERT_TRUE(f >= 0);
    LoopConn lc(sv[0]);

    // 在 loop 线程内连续发送（HTTP 响应的实际调用方式）：两个文件之间夹着数据
    int f2 = ::dup(f);
    std::promise<void> sent;
    lc.loop->runInLoop([&] {
        lc.conn->send(std::string("A"));
        lc.conn->sendFile(f, 0, content.size());
        lc.conn->send(std::string("B"));
        lc.conn->sendFile(f2, 10, 20);  // Range 区间
        lc.conn->send(shared("C"));
        sent.set_value();
    });
    sent.get_future().wait();

    std::string expect = "A" + content + "B" + content.substr(10, 20) + "C";
    std::string got = readExactly(sv[1], expect.size(), 10000);
    ASSERT_EQ(got.size(), expect.size());
    ASSERT_TRUE(got == expect);
    lc.stop();
    ::close(sv[1]);
}

TEST(tcp_sendfile_peer_close_releases_file_and_closes) {
    ::signal(SIGPIPE, SIG_IGN);  // 与 main.cpp 一致：写已关闭的 socket 返回 EPIPE 而不是杀进程
    int sv[2];
    makePair(sv);
    std::string content;
    int f = makeTempFile(4 * 1024 * 1024, &content);
    ASSERT_TRUE(f >= 0);
    LoopConn lc(sv[0]);

    lc.conn->sendFile(f, 0, content.size());
    std::string head = readExactly(sv[1], 1000, 2000);
    ASSERT_EQ(head.size(), static_cast<size_t>(1000));
    ::close(sv[1]);  // 观众中途离开

    ASSERT_TRUE(waitFor(
        [&] { return lc.conn->disconnected() && lc.conn->pendingOutputBytes() == 0; }, 5000));
    // 文件 fd 已由连接关闭
    ASSERT_TRUE(waitFor([&] { return ::fcntl(f, F_GETFD) == -1; }, 1000));
    lc.stop();
}
