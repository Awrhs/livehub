#include "cluster/ProxyHandler.hpp"

#include "api/HttpRequest.hpp"
#include "api/HttpResponse.hpp"
#include "api/HttpServer.hpp"
#include "core/Buffer.hpp"
#include "core/Logger.hpp"
#include "net/TcpConnection.hpp"

namespace streaming {

struct ProxyHandler::Bridge
    : public std::enable_shared_from_this<Bridge> {
    EventLoop* loop;
    TcpConnectionPtr client;        // 下游（浏览器）
    std::unique_ptr<TcpClient> upstream;
    std::string host;
    uint16_t port = 0;
    std::string requestText;
    bool headerSent = false;

    Bridge(EventLoop* l, TcpConnectionPtr c, std::string h, uint16_t p,
           std::string req)
        : loop(l), client(std::move(c)), host(std::move(h)), port(p),
          requestText(std::move(req)) {
        upstream = std::make_unique<TcpClient>(loop, host, port, "proxy");
        upstream->enableRetry(false);
        std::weak_ptr<Bridge> w = shared_from_this();

        upstream->setConnectionCallback(
            [w](const TcpConnectionPtr& up) {
                auto self = w.lock();
                if (!self) return;
                if (up->connected()) {
                    up->send(self->requestText);
                } else {
                    if (self->client && self->client->connected())
                        self->client->shutdown();
                }
            });
        upstream->setMessageCallback(
            [w](const TcpConnectionPtr&, Buffer* buf, Timestamp) {
                auto self = w.lock();
                if (!self) return;
                if (self->client && self->client->connected()) {
                    self->client->send(buf);
                }
            });
    }

    void start() { upstream->connect(); }
};

ProxyHandler::ProxyHandler(EventLoop* loop) : loop_(loop) {}

void ProxyHandler::registerRoutes(HttpServer& server) {
    server.router().get(
        "/proxy/:key",
        [this](const HttpRequest& req, HttpResponse& resp) {
            std::string key = req.param("key");
            if (!selector_) {
                resp.setStatusCode(HttpStatusCode::k502BadGateway);
                resp.setContentType("application/json");
                resp.setBody("{\"error\":\"no upstream selector\"}");
                return;
            }
            auto target = selector_(key);
            std::string host = target.first;   // 结构化绑定不能被 lambda 捕获（C++20 起才支持）
            auto port = target.second;
            if (host.empty() || port == 0) {
                resp.setStatusCode(HttpStatusCode::k502BadGateway);
                resp.setBody("{\"error\":\"no upstream\"}");
                return;
            }

            if (redirectMode_) {
                resp.setStatusCode(HttpStatusCode::k302Found);
                resp.addHeader("Location",
                               "http://" + host + ":" +
                                   std::to_string(port) + req.path());
                resp.setBody("");
                return;
            }

            resp.setStatusCode(HttpStatusCode::k200Ok);
            resp.setContentType("application/octet-stream");
            resp.addHeader("Cache-Control", "no-cache");
            resp.setCloseConnection(false);

            // 重建简单 HTTP/1.0 请求转发到上游
            std::string fwd =
                "GET " + req.path() + " HTTP/1.0\r\nHost: " + host + ":" +
                std::to_string(port) + "\r\nConnection: close\r\n\r\n";

            resp.setStreamCallback(
                [this, host, port, fwd](
                    const std::shared_ptr<TcpConnection>& client) {
                    auto bridge = std::make_shared<Bridge>(
                        loop_, client, host, port, fwd);
                    {
                        std::lock_guard<std::mutex> lk(mu_);
                        bridges_[client->name()] = bridge;
                    }
                    bridge->start();
                });
        });
}

}  // namespace streaming
