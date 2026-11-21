#include "api/Router.hpp"
#include "core/Buffer.hpp"
#include "test_framework.hpp"

using namespace streaming;

static HttpRequest makeGet(const std::string& path) {
    Buffer buf;
    buf.append("GET " + path + " HTTP/1.1\r\n\r\n");
    HttpRequest req;
    req.parse(&buf);
    return req;
}

// Router::dispatch 需要左值请求，测试中用临时对象时经由此辅助函数
static bool dispatchReq(Router& r, HttpRequest req, HttpResponse& resp) {
    return r.dispatch(req, resp);
}

TEST(router_exact_match) {
    Router r;
    bool hit = false;
    r.get("/health", [&](const HttpRequest&, HttpResponse&) { hit = true; });
    HttpResponse resp;
    ASSERT_TRUE(dispatchReq(r, makeGet("/health"), resp));
    ASSERT_TRUE(hit);
}

TEST(router_param_match) {
    Router r;
    std::string id;
    r.get("/users/:id", [&](const HttpRequest& req, HttpResponse&) {
        id = req.param("id");
    });
    HttpResponse resp;
    ASSERT_TRUE(dispatchReq(r, makeGet("/users/42"), resp));
    ASSERT_EQ(id, std::string("42"));
}

TEST(router_not_found) {
    Router r;
    r.get("/a", [](const HttpRequest&, HttpResponse&) {});
    HttpResponse resp;
    ASSERT_FALSE(dispatchReq(r, makeGet("/missing"), resp));
}

TEST(router_method_dispatch) {
    Router r;
    bool getHit = false, postHit = false;
    r.get("/x", [&](const HttpRequest&, HttpResponse&) { getHit = true; });
    r.post("/x", [&](const HttpRequest&, HttpResponse&) { postHit = true; });

    Buffer gb;
    gb.append("GET /x HTTP/1.1\r\n\r\n");
    HttpRequest greq;
    greq.parse(&gb);
    HttpResponse gresp;
    ASSERT_TRUE(r.dispatch(greq, gresp));
    ASSERT_TRUE(getHit);
    ASSERT_FALSE(postHit);

    Buffer pb;
    pb.append("POST /x HTTP/1.1\r\nContent-Length: 0\r\n\r\n");
    HttpRequest preq;
    preq.parse(&pb);
    HttpResponse presp;
    ASSERT_TRUE(r.dispatch(preq, presp));
    ASSERT_TRUE(postHit);
}

TEST(router_multi_params) {
    Router r;
    std::string a, b;
    r.get("/s/:stream/seq/:seq",
          [&](const HttpRequest& req, HttpResponse&) {
              a = req.param("stream");
              b = req.param("seq");
          });
    HttpResponse resp;
    ASSERT_TRUE(dispatchReq(r, makeGet("/s/live/seq/10"), resp));
    ASSERT_EQ(a, std::string("live"));
    ASSERT_EQ(b, std::string("10"));
}
