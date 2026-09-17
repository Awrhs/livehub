#include "test_framework.hpp"

TEST(framework_self_check) {
    ASSERT_EQ(1 + 1, 2);
    ASSERT_TRUE(true);
    ASSERT_FALSE(false);
    ASSERT_GT(5, 3);
}

int main() {
    return RUN_ALL_TESTS();
}
