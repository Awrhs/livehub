#include "api/Json.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iomanip>

namespace streaming {

static std::string escapeString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    ::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

void JsonValue::dumpTo(std::ostringstream& os) const {
    switch (type_) {
        case Type::Null:
            os << "null";
            break;
        case Type::Bool:
            os << (bool_ ? "true" : "false");
            break;
        case Type::Number: {
            // 整数则不带小数点
            if (num_ == static_cast<long long>(num_)) {
                os << static_cast<long long>(num_);
            } else {
                os << num_;
            }
            break;
        }
        case Type::String:
            os << escapeString(str_);
            break;
        case Type::Array: {
            os << '[';
            for (size_t i = 0; i < array_.size(); ++i) {
                if (i) os << ',';
                array_[i].dumpTo(os);
            }
            os << ']';
            break;
        }
        case Type::Object: {
            os << '{';
            bool first = true;
            for (const auto& kv : object_) {
                if (!first) os << ',';
                first = false;
                os << escapeString(kv.first);
                os << ':';
                kv.second.dumpTo(os);
            }
            os << '}';
            break;
        }
    }
}

std::string JsonValue::dump(int /*indent*/) const {
    std::ostringstream os;
    dumpTo(os);
    return os.str();
}

namespace {
// 极简递归下降 JSON 解析器（够用即可，非严格校验）
struct Parser {
    const std::string& s;
    size_t i = 0;
    explicit Parser(const std::string& str) : s(str) {}
    void skipWs() {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    }
    bool parseValue(JsonValue& out) {
        skipWs();
        if (i >= s.size()) return false;
        char c = s[i];
        if (c == '{') return parseObject(out);
        if (c == '[') return parseArray(out);
        if (c == '"') return parseString(out);
        if (c == 't' || c == 'f') return parseBool(out);
        if (c == 'n') return parseNull(out);
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c)))
            return parseNumber(out);
        return false;
    }
    bool parseObject(JsonValue& out) {
        out = JsonValue::object();
        ++i;  // {
        skipWs();
        if (i < s.size() && s[i] == '}') { ++i; return true; }
        while (i < s.size()) {
            skipWs();
            JsonValue key;
            if (!parseString(key)) return false;
            skipWs();
            if (i >= s.size() || s[i] != ':') return false;
            ++i;
            JsonValue val;
            if (!parseValue(val)) return false;
            out.set(key.asString(), val);
            skipWs();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            return false;
        }
        return false;
    }
    bool parseArray(JsonValue& out) {
        out = JsonValue::array();
        ++i;  // [
        skipWs();
        if (i < s.size() && s[i] == ']') { ++i; return true; }
        while (i < s.size()) {
            JsonValue val;
            if (!parseValue(val)) return false;
            out.append(val);
            skipWs();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == ']') { ++i; return true; }
            return false;
        }
        return false;
    }
    bool parseString(JsonValue& out) {
        if (s[i] != '"') return false;
        ++i;
        std::string str;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') { out = JsonValue::str(str); return true; }
            if (c == '\\' && i < s.size()) {
                char e = s[i++];
                switch (e) {
                    case '"': str += '"'; break;
                    case '\\': str += '\\'; break;
                    case '/': str += '/'; break;
                    case 'b': str += '\b'; break;
                    case 'f': str += '\f'; break;
                    case 'n': str += '\n'; break;
                    case 'r': str += '\r'; break;
                    case 't': str += '\t'; break;
                    case 'u': {
                        if (i + 4 > s.size()) return false;
                        unsigned cp = 0;
                        for (int k = 0; k < 4; ++k) {
                            char h = s[i++];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= h - '0';
                            else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                            else return false;
                        }
                        // 简化：仅支持 BMP，直接按 UTF-8 编码
                        if (cp < 0x80) str += static_cast<char>(cp);
                        else if (cp < 0x800) {
                            str += static_cast<char>(0xC0 | (cp >> 6));
                            str += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            str += static_cast<char>(0xE0 | (cp >> 12));
                            str += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            str += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: str += e;
                }
            } else {
                str += c;
            }
        }
        return false;
    }
    bool parseBool(JsonValue& out) {
        if (s.compare(i, 4, "true") == 0) { out = JsonValue::boolean(true); i += 4; return true; }
        if (s.compare(i, 5, "false") == 0) { out = JsonValue::boolean(false); i += 5; return true; }
        return false;
    }
    bool parseNull(JsonValue& out) {
        if (s.compare(i, 4, "null") == 0) { out = JsonValue(); i += 4; return true; }
        return false;
    }
    bool parseNumber(JsonValue& out) {
        size_t start = i;
        if (s[i] == '-') ++i;
        while (i < s.size() &&
               (std::isdigit(static_cast<unsigned char>(s[i])) ||
                s[i] == '.' || s[i] == 'e' || s[i] == 'E' || s[i] == '+' ||
                s[i] == '-'))
            ++i;
        std::string num = s.substr(start, i - start);
        out = JsonValue::number(std::strtod(num.c_str(), nullptr));
        return true;
    }
};
}  // namespace

bool JsonValue::parse(const std::string& text, JsonValue& out) {
    Parser p(text);
    return p.parseValue(out);
}

}  // namespace streaming
