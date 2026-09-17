#include "api/Json.hpp"
#include "core/Logger.hpp"
#include "middleware/AccessLog.hpp"
#include "test_framework.hpp"

#include <string>

using namespace streaming;

TEST(json_parse_string) {
    JsonValue v;
    ASSERT_TRUE(JsonValue::parse("\"hello\"", v));
    ASSERT_TRUE(v.isString());
    ASSERT_EQ(v.asString(), std::string("hello"));
}

TEST(json_parse_number) {
    JsonValue v;
    ASSERT_TRUE(JsonValue::parse("42", v));
    ASSERT_TRUE(v.isNumber());
    ASSERT_EQ(v.asNumber(), 42.0);
}

TEST(json_parse_bool_null) {
    JsonValue v;
    ASSERT_TRUE(JsonValue::parse("true", v));
    ASSERT_TRUE(v.isBool());
    ASSERT_EQ(v.asBool(), true);
    ASSERT_TRUE(JsonValue::parse("false", v));
    ASSERT_EQ(v.asBool(), false);
    ASSERT_TRUE(JsonValue::parse("null", v));
    ASSERT_TRUE(v.isNull());
}

TEST(json_parse_object) {
    JsonValue v;
    ASSERT_TRUE(JsonValue::parse("{\"a\":1,\"b\":\"x\"}", v));
    ASSERT_TRUE(v.isObject());
    const JsonValue* a = v.find("a");
    ASSERT_TRUE(a != nullptr);
    ASSERT_EQ(a->asNumber(), 1.0);
    const JsonValue* b = v.find("b");
    ASSERT_TRUE(b != nullptr);
    ASSERT_EQ(b->asString(), std::string("x"));
}

TEST(json_parse_array) {
    JsonValue v;
    ASSERT_TRUE(JsonValue::parse("[1,2,3]", v));
    ASSERT_TRUE(v.isArray());
    ASSERT_EQ(v.asArray().size(), size_t(3));
    ASSERT_EQ(v.asArray()[1].asNumber(), 2.0);
}

TEST(json_build_nested) {
    JsonValue root = JsonValue::object();
    JsonValue arr = JsonValue::array();
    arr.append(JsonValue::number(1));
    arr.append(JsonValue::number(2));
    root.set("items", arr);
    root.set("name", JsonValue::str("demo"));

    std::string s = root.dump();
    // 重新解析应当能还原
    JsonValue reparsed;
    ASSERT_TRUE(JsonValue::parse(s, reparsed));
    ASSERT_TRUE(reparsed.isObject());
    const JsonValue* items = reparsed.find("items");
    ASSERT_TRUE(items != nullptr);
    ASSERT_TRUE(items->isArray());
    ASSERT_EQ(items->asArray().size(), size_t(2));
    const JsonValue* name = reparsed.find("name");
    ASSERT_TRUE(name != nullptr);
    ASSERT_EQ(name->asString(), std::string("demo"));
}

TEST(json_parse_whitespace) {
    JsonValue v;
    ASSERT_TRUE(JsonValue::parse("  { \"k\" :  true }  ", v));
    ASSERT_TRUE(v.isObject());
    const JsonValue* k = v.find("k");
    ASSERT_TRUE(k != nullptr);
    ASSERT_TRUE(k->isBool());
    ASSERT_EQ(k->asBool(), true);
}

TEST(json_parse_invalid_returns_false) {
    JsonValue v;
    ASSERT_FALSE(JsonValue::parse("{bad}", v));
    ASSERT_FALSE(JsonValue::parse("'single'", v));
    ASSERT_FALSE(JsonValue::parse("", v));
}

// ---- 日志 JSON 转义（Logger）与访问日志 JSON 行 ----

TEST(logger_json_escape) {
    ASSERT_EQ(Logger::jsonEscape("plain"), std::string("plain"));
    ASSERT_EQ(Logger::jsonEscape("a\"b"), std::string("a\\\"b"));
    ASSERT_EQ(Logger::jsonEscape("a\\b"), std::string("a\\\\b"));
    ASSERT_EQ(Logger::jsonEscape("line1\nline2"),
              std::string("line1\\nline2"));
    ASSERT_EQ(Logger::jsonEscape("tab\there"), std::string("tab\\there"));
    // 控制字符 → \u00XX
    ASSERT_EQ(Logger::jsonEscape(std::string("\x01", 1)),
              std::string("\\u0001"));
    // UTF-8 多字节原样保留
    ASSERT_EQ(Logger::jsonEscape("中文"), std::string("中文"));
}

TEST(access_log_json_line) {
    std::string line = formatAccessLogJson(
        "2026-09-15 10:00:00.000001", "abc123", "127.0.0.1", "GET",
        "/live/test.m3u8", 200, 1024, 1.5, "curl/8.0 \"x\"");
    // 应能被 JSON 解析器还原
    JsonValue v;
    ASSERT_TRUE(JsonValue::parse(line, v));
    ASSERT_TRUE(v.isObject());
    const JsonValue* tid = v.find("trace_id");
    ASSERT_TRUE(tid != nullptr);
    ASSERT_EQ(tid->asString(), std::string("abc123"));
    const JsonValue* st = v.find("status");
    ASSERT_TRUE(st != nullptr);
    ASSERT_EQ(st->asNumber(), 200.0);
    // path/ua 中的引号必须被转义
    const JsonValue* ua = v.find("ua");
    ASSERT_TRUE(ua != nullptr);
    ASSERT_EQ(ua->asString(), std::string("curl/8.0 \"x\""));
}
