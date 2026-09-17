// HTTP 请求解析与路由分发基准。
#include "api/HttpRequest.hpp"
#include "api/HttpResponse.hpp"
#include "api/Router.hpp"
#include "core/Buffer.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace streaming;

int main(int argc, char** argv) {
    int iterations = argc > 1 ? std::atoi(argv[1]) : 1'000'000;

    const std::string reqText =
        "GET /live/stream123/456.ts HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "User-Agent: bench\r\n"
        "Accept: */*\r\n"
        "Range: bytes=0-1023\r\n"
        "\r\n";

    Router router;
    int matched = 0;
    router.get("/live/:stream/:seq",
               [&](const HttpRequest&, HttpResponse&) { ++matched; });

    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        Buffer buf;
        buf.append(reqText);
        HttpRequest req;
        req.parse(&buf);
        HttpResponse resp;
        router.dispatch(req, resp);
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();
    std::printf("HTTP parse+route: %d iters, %.2f Mreq/s, matched=%d, %.3f sec\n",
                iterations, iterations / sec / 1e6, matched, sec);
    return 0;
}
