#include "test_framework.hpp"
#include "transcode/TranscodeManager.hpp"

using namespace streaming;

// 不 start() 调度线程：任务全部停留在排队态，可稳定断言队列顺序与状态机。

TEST(transcode_priority_queue_order) {
    TranscodeManager tm(nullptr, 1);
    std::string idA = tm.submit("in", "out", {Profile::p480()}, true, 0);
    std::string idB = tm.submit("in", "out", {Profile::p480()}, true, 5);
    std::string idC = tm.submit("in", "out", {Profile::p480()}, true, 3);
    std::string idD = tm.submit("in", "out", {Profile::p480()}, true, 5);
    auto tasks = tm.list();
    ASSERT_EQ(tasks.size(), size_t(4));
    // 期望顺序：priority 降序，同优先级按提交序 FIFO
    //   B(5, 先提交) -> D(5, 后提交) -> C(3) -> A(0)
    ASSERT_EQ(tasks[0].id, idB);
    ASSERT_EQ(tasks[1].id, idD);
    ASSERT_EQ(tasks[2].id, idC);
    ASSERT_EQ(tasks[3].id, idA);
    ASSERT_EQ(tasks[0].state, TaskState::kPending);
    ASSERT_EQ(tasks[0].priority, 5);
    ASSERT_GT(tasks[0].submittedAtMs, int64_t(0));
}

TEST(transcode_cancel_pending_task) {
    TranscodeManager tm(nullptr, 1);
    std::string id = tm.submit("in", "out", {Profile::p480()}, true, 0);
    ASSERT_TRUE(tm.cancel(id));

    auto tasks = tm.list();
    ASSERT_EQ(tasks.size(), size_t(1));
    ASSERT_EQ(tasks[0].state, TaskState::kCancelled);
    ASSERT_GT(tasks[0].endedAtMs, int64_t(0));

    // 重复取消 / 未知 id 均返回 false
    ASSERT_FALSE(tm.cancel(id));
    ASSERT_FALSE(tm.cancel("tc-does-not-exist"));

    auto st = tm.stats();
    ASSERT_EQ(st.cancelled, size_t(1));
    ASSERT_EQ(st.pending, size_t(0));
    ASSERT_EQ(st.running, size_t(0));
}

TEST(transcode_sweep_finished_ttl) {
    TranscodeManager tm(nullptr, 1);
    std::string id1 = tm.submit("in", "out", {Profile::p480()}, true, 0);
    std::string id2 = tm.submit("in", "out", {Profile::p480()}, true, 0);
    tm.cancel(id1);
    tm.cancel(id2);
    ASSERT_EQ(tm.list().size(), size_t(2));

    // ttl<=0：全部清理
    ASSERT_EQ(tm.sweepFinished(0), size_t(2));
    ASSERT_EQ(tm.list().size(), size_t(0));

    // 大 ttl：不清理
    std::string id3 = tm.submit("in", "out", {Profile::p480()}, true, 0);
    tm.cancel(id3);
    ASSERT_EQ(tm.sweepFinished(3600), size_t(0));
    ASSERT_EQ(tm.list().size(), size_t(1));
}

TEST(transcode_stats_basic_fields) {
    TranscodeManager tm(nullptr, 1);
    tm.submit("in", "out", {Profile::p480()}, true, 1);
    tm.submit("in", "out", {Profile::p720()}, true, 2);
    auto st = tm.stats();
    ASSERT_EQ(st.pending, size_t(2));
    ASSERT_EQ(st.running, size_t(0));
    ASSERT_EQ(st.completed, size_t(0));
    ASSERT_EQ(st.failed, size_t(0));
    ASSERT_EQ(st.cancelled, size_t(0));
    // 尚无已启动任务，平均排队时长无样本
    ASSERT_EQ(st.avgWaitMs, 0.0);
}

TEST(transcode_task_state_names) {
    ASSERT_EQ(std::string(taskStateName(TaskState::kPending)),
              std::string("pending"));
    ASSERT_EQ(std::string(taskStateName(TaskState::kRunning)),
              std::string("running"));
    ASSERT_EQ(std::string(taskStateName(TaskState::kCompleted)),
              std::string("completed"));
    ASSERT_EQ(std::string(taskStateName(TaskState::kFailed)),
              std::string("failed"));
    ASSERT_EQ(std::string(taskStateName(TaskState::kCancelled)),
              std::string("cancelled"));
}
