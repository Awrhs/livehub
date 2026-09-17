#include "core/Buffer.hpp"
#include "rtmp/Amf0.hpp"
#include "test_framework.hpp"

using namespace streaming;

TEST(amf_number_roundtrip) {
    Buffer buf;
    Amf0Value::number(3.14).encode(buf);
    Amf0Value v;
    ASSERT_TRUE(v.decode(buf));
    ASSERT_TRUE(v.isNumber());
    ASSERT_GT(v.asNumber(), 3.13);
    ASSERT_LT(v.asNumber(), 3.15);
}

TEST(amf_string_roundtrip) {
    Buffer buf;
    Amf0Value::str("hello").encode(buf);
    Amf0Value v;
    ASSERT_TRUE(v.decode(buf));
    ASSERT_EQ(v.asString(), std::string("hello"));
}

TEST(amf_boolean) {
    Buffer buf;
    Amf0Value::boolean(true).encode(buf);
    Amf0Value v;
    ASSERT_TRUE(v.decode(buf));
    ASSERT_TRUE(v.asBoolean());
}

TEST(amf_object) {
    Amf0Value::Object obj;
    obj["name"] = Amf0Value::str("test");
    obj["count"] = Amf0Value::number(42);
    Buffer buf;
    Amf0Value::object(obj).encode(buf);
    Amf0Value v;
    ASSERT_TRUE(v.decode(buf));
    ASSERT_TRUE(v.asObject() != nullptr);
    ASSERT_EQ(v.asObject()->at("name").asString(), std::string("test"));
    ASSERT_EQ(v.asObject()->at("count").asNumber(), 42.0);
}

TEST(amf_strict_array) {
    Amf0Value::Array arr;
    arr.push_back(Amf0Value::number(1));
    arr.push_back(Amf0Value::number(2));
    Buffer buf;
    Amf0Value::strictArray(arr).encode(buf);
    Amf0Value v;
    ASSERT_TRUE(v.decode(buf));
    ASSERT_TRUE(v.asArray() != nullptr);
    ASSERT_EQ(v.asArray()->size(), size_t(2));
}
