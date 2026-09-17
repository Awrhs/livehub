#include "core/Buffer.hpp"
#include "api/HttpRequest.hpp"
#include "test_framework.hpp"

using namespace streaming;

TEST(http_get_request) {
    Buffer buf;
    buf.append(std::string("GET /index.html HTTP/1.1\r\n"
                           "Host: example.com\r\n"
                           "User-Agent: test\r\n"
                           "\r\n"));
    HttpRequest req;
    ASSERT_TRUE(req.parse(&buf));
    ASSERT_TRUE(req.done());
    ASSERT_TRUE(req.method() == HttpMethod::GET);
    ASSERT_EQ(req.path(), std::string("/index.html"));
    ASSERT_EQ(req.header("Host"), std::string("example.com"));
}

TEST(http_post_with_body) {
    std::string body = "hello world";
    Buffer buf;
    buf.append("POST /api/x HTTP/1.1\r\n"
               "Content-Length: " + std::to_string(body.size()) + "\r\n"
               "\r\n" + body);
    HttpRequest req;
    ASSERT_TRUE(req.parse(&buf));
    ASSERT_TRUE(req.method() == HttpMethod::POST);
    ASSERT_EQ(req.body(), body);
}

TEST(http_partial_arrival) {
    HttpRequest req;
    Buffer b1;
    b1.append("GET /path HTTP/1.1\r");  // 缺 \n
    ASSERT_FALSE(req.parse(&b1));
    ASSERT_FALSE(req.done());

    Buffer b2;
    b2.append("\nHost: h\r\n");  // 补全请求行 + 半个头
    ASSERT_FALSE(req.parse(&b2));

    Buffer b3;
    b3.append("\r\n");  // 头结束
    ASSERT_TRUE(req.parse(&b3));
    ASSERT_EQ(req.path(), std::string("/path"));
}

TEST(http_query_params) {
    Buffer buf;
    buf.append("GET /search?q=hi&page=2 HTTP/1.1\r\n\r\n");
    HttpRequest req;
    ASSERT_TRUE(req.parse(&buf));
    ASSERT_EQ(req.param("q"), std::string("hi"));
    ASSERT_EQ(req.param("page"), std::string("2"));
}

TEST(http_pipelined_requests) {
    Buffer buf;
    buf.append("GET /a HTTP/1.1\r\n\r\nGET /b HTTP/1.1\r\n\r\n");
    HttpRequest r1;
    ASSERT_TRUE(r1.parse(&buf));
    ASSERT_EQ(r1.path(), std::string("/a"));
    HttpRequest r2;
    ASSERT_TRUE(r2.parse(&buf));
    ASSERT_EQ(r2.path(), std::string("/b"));
}
