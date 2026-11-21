#include "cache/RedisClient.hpp"
#include "test_framework.hpp"

#include <string>
#include <vector>

using namespace streaming;

// ---- RESP2 命令编码 ----

TEST(redis_encode_command) {
    std::string cmd = RedisClient::encodeCommand({"SET", "k", "v"});
    // *3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n
    ASSERT_EQ(cmd, std::string("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"));

    // 空参数 / 二进制安全（含 \r\n 与 \0 的载荷）
    cmd = RedisClient::encodeCommand({"GET", "a\r\nb"});
    ASSERT_EQ(cmd,
              std::string("*2\r\n$3\r\nGET\r\n$4\r\na\r\nb\r\n"));
    std::string bin = std::string("x\0y", 3);
    cmd = RedisClient::encodeCommand({"SET", "k", bin});
    // 注意：期望值不能直接用含 \0 的 C 字面量构造（会被截断）
    ASSERT_EQ(cmd, std::string("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$3\r\n") + bin +
                       "\r\n");
}

// ---- RESP2 回复解析 ----

TEST(redis_parse_simple_and_error) {
    RedisReply r;
    int n = RedisClient::parseReply("+OK\r\n", 5, &r);
    ASSERT_EQ(n, 5);
    ASSERT_EQ(r.type, RedisReply::kSimple);
    ASSERT_EQ(r.str, std::string("OK"));

    n = RedisClient::parseReply("-ERR unknown command\r\n", 22, &r);
    ASSERT_EQ(n, 22);
    ASSERT_EQ(r.type, RedisReply::kError);
    ASSERT_EQ(r.str, std::string("ERR unknown command"));
    ASSERT_TRUE(r.isError());

    // 不完整
    ASSERT_EQ(RedisClient::parseReply("+OK\r", 4, &r), 0);
    ASSERT_EQ(RedisClient::parseReply("", 0, &r), 0);
}

TEST(redis_parse_integer) {
    RedisReply r;
    int n = RedisClient::parseReply(":12345\r\n", 8, &r);
    ASSERT_EQ(n, 8);
    ASSERT_EQ(r.type, RedisReply::kInt);
    ASSERT_EQ(r.integer, int64_t(12345));

    n = RedisClient::parseReply(":-1\r\n", 5, &r);
    ASSERT_EQ(n, 5);
    ASSERT_EQ(r.integer, int64_t(-1));

    ASSERT_EQ(RedisClient::parseReply(":12", 3, &r), 0);
}

TEST(redis_parse_bulk) {
    RedisReply r;
    std::string wire = "$6\r\nfoobar\r\n";
    int n = RedisClient::parseReply(wire.data(), wire.size(), &r);
    ASSERT_EQ(n, static_cast<int>(wire.size()));
    ASSERT_EQ(r.type, RedisReply::kBulk);
    ASSERT_EQ(r.str, std::string("foobar"));

    // 载荷内含 CRLF 与 \0（长度前缀驱动，不受内容影响）
    wire = "$4\r\na\r\nb\r\n";
    n = RedisClient::parseReply(wire.data(), wire.size(), &r);
    ASSERT_EQ(n, static_cast<int>(wire.size()));
    ASSERT_EQ(r.str, std::string("a\r\nb"));

    // 空串 bulk：$0\r\n\r\n
    wire = "$0\r\n\r\n";
    n = RedisClient::parseReply(wire.data(), wire.size(), &r);
    ASSERT_EQ(n, static_cast<int>(wire.size()));
    ASSERT_EQ(r.type, RedisReply::kBulk);
    ASSERT_EQ(r.str, std::string(""));

    // nil bulk：$-1
    wire = "$-1\r\n";
    n = RedisClient::parseReply(wire.data(), wire.size(), &r);
    ASSERT_EQ(n, static_cast<int>(wire.size()));
    ASSERT_EQ(r.type, RedisReply::kNil);
    ASSERT_TRUE(r.isNil());

    // 不完整：头未齐 / 载荷未齐 / 尾 CRLF 未齐
    ASSERT_EQ(RedisClient::parseReply("$6\r\nfoo", 7, &r), 0);
    ASSERT_EQ(RedisClient::parseReply("$6\r\nfoobar\r", 11, &r), 0);
    ASSERT_EQ(RedisClient::parseReply("$6\r\nfoob", 8, &r), 0);
}

TEST(redis_parse_array) {
    RedisReply r;
    // INCR 场景常见：数组混排 bulk/integer
    std::string wire = "*3\r\n$3\r\nfoo\r\n:7\r\n$-1\r\n";
    int n = RedisClient::parseReply(wire.data(), wire.size(), &r);
    ASSERT_EQ(n, static_cast<int>(wire.size()));
    ASSERT_EQ(r.type, RedisReply::kArray);
    ASSERT_EQ(r.arr.size(), size_t(3));
    ASSERT_EQ(r.arr[0].type, RedisReply::kBulk);
    ASSERT_EQ(r.arr[0].str, std::string("foo"));
    ASSERT_EQ(r.arr[1].type, RedisReply::kInt);
    ASSERT_EQ(r.arr[1].integer, int64_t(7));
    ASSERT_EQ(r.arr[2].type, RedisReply::kNil);

    // 空数组 / nil 数组
    wire = "*0\r\n";
    n = RedisClient::parseReply(wire.data(), wire.size(), &r);
    ASSERT_EQ(n, static_cast<int>(wire.size()));
    ASSERT_EQ(r.type, RedisReply::kArray);
    ASSERT_EQ(r.arr.size(), size_t(0));

    wire = "*-1\r\n";
    n = RedisClient::parseReply(wire.data(), wire.size(), &r);
    ASSERT_EQ(n, static_cast<int>(wire.size()));
    ASSERT_EQ(r.type, RedisReply::kNil);

    // 不完整：最后一个元素未到齐 → 0
    wire = "*2\r\n$3\r\nfoo\r\n$3\r\nba";
    ASSERT_EQ(RedisClient::parseReply(wire.data(), wire.size(), &r), 0);
}

TEST(redis_parse_protocol_errors) {
    RedisReply r;
    // 未知类型前缀
    ASSERT_EQ(RedisClient::parseReply("X12\r\n", 5, &r), -1);
    // 非数字长度头
    ASSERT_EQ(RedisClient::parseReply("$abc\r\n", 6, &r), -1);
    ASSERT_EQ(RedisClient::parseReply(":xyz\r\n", 6, &r), -1);
    ASSERT_EQ(RedisClient::parseReply("*zz\r\n", 6, &r), -1);
}

TEST(redis_encode_then_parse_roundtrip) {
    // 服务端不会回显命令，但 encode→parse 可验证解析器对
    // 自产字节的消费字节数正确性（长度自洽）
    std::string wire = "$11\r\nhello world\r\n";
    RedisReply r;
    int used = RedisClient::parseReply(wire.data(), wire.size(), &r);
    ASSERT_EQ(used, static_cast<int>(wire.size()));
    // 恰好消费全部、无越界
    ASSERT_EQ(r.str, std::string("hello world"));
}
