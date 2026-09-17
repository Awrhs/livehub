#pragma once

/**
 * 最小化自研测试框架：无外部依赖。
 *
 *   TEST(mytest) { ASSERT_EQ(1+1, 2); }
 *   int main() { return RUN_ALL_TESTS(); }
 *
 * 用静态注册把测试收集到全局链表，RUN_ALL_TESTS 依次执行并统计 pass/fail。
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <string>
#include <vector>

namespace tftest {

struct TestCase {
    const char* name;
    const char* file;
    int line;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Registrar {
    Registrar(const char* name, const char* file, int line,
              std::function<void()> fn) {
        registry().push_back({name, file, line, std::move(fn)});
    }
};

struct AssertionError {
    std::string expr;
    std::string file;
    int line;
};

inline void fail(const char* expr, const char* file, int line) {
    throw AssertionError{expr, file, line};
}

}  // namespace tftest

#define TEST(name)                                                      \
    static void test_##name();                                          \
    static tftest::Registrar registrar_##name(#name, __FILE__,          \
                                               __LINE__, test_##name);   \
    static void test_##name()

#define ASSERT_TRUE(x)                                                  \
    do {                                                                \
        if (!(x)) tftest::fail(#x, __FILE__, __LINE__);                 \
    } while (0)

#define ASSERT_FALSE(x) ASSERT_TRUE(!(x))

#define ASSERT_EQ(a, b)                                                 \
    do {                                                                \
        auto _va = (a);                                                 \
        auto _vb = (b);                                                 \
        if (!(_va == _vb)) {                                            \
            char _buf[256];                                             \
            std::snprintf(_buf, sizeof(_buf),                           \
                          "%s == %s (got values not equal)", #a, #b);    \
            tftest::fail(_buf, __FILE__, __LINE__);                     \
        }                                                               \
    } while (0)

#define ASSERT_NE(a, b)                                                 \
    do {                                                                \
        if ((a) == (b))                                                 \
            tftest::fail(#a " != " #b, __FILE__, __LINE__);             \
    } while (0)

#define ASSERT_GT(a, b)                                                 \
    do {                                                                \
        if (!((a) > (b)))                                               \
            tftest::fail(#a " > " #b, __FILE__, __LINE__);              \
    } while (0)

#define ASSERT_LT(a, b)                                                 \
    do {                                                                \
        if (!((a) < (b)))                                               \
            tftest::fail(#a " < " #b, __FILE__, __LINE__);              \
    } while (0)

#define ASSERT_GE(a, b)                                                 \
    do {                                                                \
        if (!((a) >= (b)))                                              \
            tftest::fail(#a " >= " #b, __FILE__, __LINE__);             \
    } while (0)

inline int RUN_ALL_TESTS() {
    int pass = 0, fail = 0;
    for (const auto& tc : tftest::registry()) {
        try {
            tc.fn();
            ++pass;
            std::printf("  [PASS] %s\n", tc.name);
        } catch (const tftest::AssertionError& e) {
            ++fail;
            std::printf("  [FAIL] %s at %s:%d: %s\n", tc.name,
                        e.file.c_str(), e.line, e.expr.c_str());
        } catch (const std::exception& e) {
            ++fail;
            std::printf("  [FAIL] %s: exception: %s\n", tc.name, e.what());
        } catch (...) {
            ++fail;
            std::printf("  [FAIL] %s: unknown exception\n", tc.name);
        }
    }
    std::printf("\n%d tests, %d passed, %d failed\n",
                pass + fail, pass, fail);
    return fail == 0 ? 0 : 1;
}
