#include "cluster/HealthChecker.hpp"
#include "cluster/NodeRegistry.hpp"
#include "test_framework.hpp"

using namespace streaming;

// 不启动 EventLoop/定时器，仅测试被动熔断器状态机
// （recordFailure/recordSuccess/status/allowRequest 不依赖 loop）。

TEST(circuit_closed_to_open) {
    NodeRegistry reg;
    reg.add(std::make_shared<Node>("n1", "127.0.0.1", 8080, 1935));
    HealthChecker hc(nullptr, &reg, 5, /*threshold=*/3,
                     /*cooldown=*/15, 2000);
    ASSERT_TRUE(hc.allowRequest("n1"));
    hc.recordFailure("n1");
    hc.recordFailure("n1");
    ASSERT_EQ(hc.status("n1").state, CircuitState::kClosed);
    hc.recordFailure("n1");  // 达到阈值
    ASSERT_EQ(hc.status("n1").state, CircuitState::kOpen);
    ASSERT_FALSE(hc.allowRequest("n1"));
}

TEST(circuit_half_open_then_closed_on_success) {
    NodeRegistry reg;
    reg.add(std::make_shared<Node>("n2", "127.0.0.1", 8080, 1935));
    // cooldown=0：OPEN 后下一次失败即转 HALF_OPEN
    HealthChecker hc(nullptr, &reg, 5, /*threshold=*/2,
                     /*cooldown=*/0, 2000);
    hc.recordFailure("n2");
    hc.recordFailure("n2");  // -> OPEN
    ASSERT_EQ(hc.status("n2").state, CircuitState::kOpen);
    hc.recordFailure("n2");  // 冷却已过 -> HALF_OPEN
    ASSERT_EQ(hc.status("n2").state, CircuitState::kHalfOpen);
    hc.recordSuccess("n2");  // 试探成功 -> CLOSED
    ASSERT_EQ(hc.status("n2").state, CircuitState::kClosed);
    ASSERT_TRUE(hc.allowRequest("n2"));
}

TEST(circuit_half_open_failure_reopens) {
    NodeRegistry reg;
    reg.add(std::make_shared<Node>("n3", "127.0.0.1", 8080, 1935));
    HealthChecker hc(nullptr, &reg, 5, /*threshold=*/2,
                     /*cooldown=*/0, 2000);
    hc.recordFailure("n3");
    hc.recordFailure("n3");  // OPEN
    hc.recordFailure("n3");  // HALF_OPEN
    hc.recordFailure("n3");  // HALF_OPEN 失败 -> 重新 OPEN
    ASSERT_EQ(hc.status("n3").state, CircuitState::kOpen);
    ASSERT_FALSE(hc.allowRequest("n3"));
}

TEST(circuit_success_resets_failures) {
    NodeRegistry reg;
    reg.add(std::make_shared<Node>("n4", "127.0.0.1", 8080, 1935));
    HealthChecker hc(nullptr, &reg, 5, /*threshold=*/3, 15, 2000);
    hc.recordFailure("n4");
    hc.recordFailure("n4");
    hc.recordSuccess("n4");  // 重置
    hc.recordFailure("n4");
    hc.recordFailure("n4");
    ASSERT_EQ(hc.status("n4").state, CircuitState::kClosed);  // 未达阈值
}
