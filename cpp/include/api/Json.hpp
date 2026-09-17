#pragma once

#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace streaming {

/**
 * 轻量 JSON 值（不依赖第三方库）。
 * 支持 string/number/bool/object/array，用于 API 响应序列化。
 */
class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    JsonValue() : type_(Type::Null) {}
    static JsonValue boolean(bool b) {
        JsonValue v; v.type_ = Type::Bool; v.bool_ = b; return v;
    }
    static JsonValue number(double n) {
        JsonValue v; v.type_ = Type::Number; v.num_ = n; return v;
    }
    static JsonValue str(const std::string& s) {
        JsonValue v; v.type_ = Type::String; v.str_ = s; return v;
    }
    static JsonValue array() {
        JsonValue v; v.type_ = Type::Array; return v;
    }
    static JsonValue object() {
        JsonValue v; v.type_ = Type::Object; return v;
    }

    Type type() const { return type_; }

    void append(const JsonValue& v) { array_.push_back(v); }
    void set(const std::string& key, const JsonValue& v) {
        object_[key] = v;
    }

    /// 序列化为字符串
    std::string dump(int indent = -1) const;

    /// 极简 JSON 解析（仅支持 webhook 响应所需的 object/string/number/bool/null）。
    static bool parse(const std::string& text, JsonValue& out);

    // ---- 访问器 ----
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool asBool() const { return bool_; }
    double asNumber() const { return num_; }
    const std::string& asString() const { return str_; }
    const std::vector<JsonValue>& asArray() const { return array_; }
    const std::map<std::string, JsonValue>& asObject() const { return object_; }

    const JsonValue* find(const std::string& key) const {
        if (type_ != Type::Object) return nullptr;
        auto it = object_.find(key);
        return it == object_.end() ? nullptr : &it->second;
    }

private:
    void dumpTo(std::ostringstream& os) const;

    Type type_;
    bool bool_ = false;
    double num_ = 0;
    std::string str_;
    std::vector<JsonValue> array_;
    std::map<std::string, JsonValue> object_;
};

}  // namespace streaming
