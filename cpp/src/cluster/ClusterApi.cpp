#include "cluster/ClusterApi.hpp"

#include "api/HttpRequest.hpp"
#include "api/HttpResponse.hpp"
#include "api/HttpServer.hpp"
#include "cluster/HealthChecker.hpp"
#include "cluster/LoadBalancer.hpp"
#include "cluster/NodeRegistry.hpp"
#include "core/Logger.hpp"

#include <map>
#include <sstream>
#include <string>

namespace streaming {

namespace {

// 解析最简单的 application/x-www-form-urlencoded body
void parseForm(const std::string& body,
               std::map<std::string, std::string>& out) {
    size_t i = 0;
    while (i < body.size()) {
        size_t amp = body.find('&', i);
        std::string pair = body.substr(
            i, amp == std::string::npos ? std::string::npos : amp - i);
        auto eq = pair.find('=');
        if (eq != std::string::npos) {
            out[pair.substr(0, eq)] = pair.substr(eq + 1);
        }
        if (amp == std::string::npos) break;
        i = amp + 1;
    }
}

}  // namespace

void ClusterApi::registerRoutes(HttpServer& server) {
    server.router().get(
        "/api/nodes",
        [this](const HttpRequest&, HttpResponse& resp) {
            auto nodes = reg_->all();
            std::ostringstream os;
            os << "{\"nodes\":[";
            for (size_t i = 0; i < nodes.size(); ++i) {
                const auto& n = nodes[i];
                if (i) os << ",";
                os << "{\"id\":\"" << n->id << "\",\"ip\":\"" << n->ip
                   << "\",\"httpPort\":" << n->httpPort
                   << ",\"rtmpPort\":" << n->rtmpPort
                   << ",\"weight\":" << n->weight
                   << ",\"load\":" << n->load.load()
                   << ",\"healthy\":"
                   << (n->healthy.load() ? "true" : "false") << "}";
            }
            os << "]}";
            resp.setStatusCode(HttpStatusCode::k200Ok);
            resp.setContentType("application/json");
            resp.setBody(os.str());
        });

    server.router().post(
        "/api/nodes",
        [this](const HttpRequest& req, HttpResponse& resp) {
            std::map<std::string, std::string> form;
            parseForm(req.body(), form);
            if (form["id"].empty() || form["ip"].empty()) {
                resp.setStatusCode(HttpStatusCode::k400BadRequest);
                resp.setContentType("application/json");
                resp.setBody("{\"error\":\"id and ip required\"}");
                return;
            }
            auto node = std::make_shared<Node>(
                form["id"], form["ip"],
                static_cast<uint16_t>(
                    std::stoi(form.count("httpPort") ? form["httpPort"] : "8080")),
                static_cast<uint16_t>(
                    std::stoi(form.count("rtmpPort") ? form["rtmpPort"] : "1935")),
                std::stoi(form.count("weight") ? form["weight"] : "1"));
            reg_->add(node);
            lb_->rebuildRing();
            resp.setStatusCode(HttpStatusCode::k200Ok);
            resp.setContentType("application/json");
            resp.setBody("{\"status\":\"added\"}");
        });

    server.router().add(
        HttpMethod::DELETE,
        "/api/nodes/:id",
        [this](const HttpRequest& req, HttpResponse& resp) {
            std::string id = req.param("id");
            bool removed = reg_->remove(id);
            lb_->rebuildRing();
            resp.setStatusCode(removed ? HttpStatusCode::k200Ok
                                        : HttpStatusCode::k404NotFound);
            resp.setContentType("application/json");
            resp.setBody(removed ? "{\"status\":\"removed\"}"
                                  : "{\"error\":\"not found\"}");
        });

    server.router().get(
        "/api/nodes/:id/health",
        [this](const HttpRequest& req, HttpResponse& resp) {
            std::string id = req.param("id");
            auto st = hc_ ? hc_->status(id) : CircuitStatus{};
            const char* stateStr = "CLOSED";
            switch (st.state) {
                case CircuitState::kOpen: stateStr = "OPEN"; break;
                case CircuitState::kHalfOpen: stateStr = "HALF_OPEN"; break;
                default: stateStr = "CLOSED"; break;
            }
            std::ostringstream os;
            os << "{\"id\":\"" << id << "\",\"state\":\"" << stateStr
               << "\",\"failures\":" << st.failures
               << ",\"successes\":" << st.successes << "}";
            resp.setStatusCode(HttpStatusCode::k200Ok);
            resp.setContentType("application/json");
            resp.setBody(os.str());
        });

    server.router().get(
        "/api/balance",
        [this](const HttpRequest& req, HttpResponse& resp) {
            std::string key = req.param("key");
            if (key.empty()) key = "default";
            auto node = lb_->selectNode(key);
            if (!node) {
                resp.setStatusCode(HttpStatusCode::k503ServiceUnavailable);
                resp.setContentType("application/json");
                resp.setBody("{\"error\":\"no healthy node\"}");
                return;
            }
            lb_->reportLoad(node);
            std::ostringstream os;
            os << "{\"id\":\"" << node->id << "\",\"ip\":\"" << node->ip
               << "\",\"httpPort\":" << node->httpPort
               << ",\"rtmpPort\":" << node->rtmpPort << "}";
            resp.setStatusCode(HttpStatusCode::k200Ok);
            resp.setContentType("application/json");
            resp.setBody(os.str());
        });

    server.router().get(
        "/api/cluster/status",
        [this](const HttpRequest&, HttpResponse& resp) {
            auto all = reg_->all();
            int healthy = 0;
            for (const auto& n : all)
                if (n->healthy.load()) ++healthy;
            std::ostringstream os;
            os << "{\"totalNodes\":" << all.size()
               << ",\"healthyNodes\":" << healthy << "}";
            resp.setStatusCode(HttpStatusCode::k200Ok);
            resp.setContentType("application/json");
            resp.setBody(os.str());
        });
}

}  // namespace streaming
