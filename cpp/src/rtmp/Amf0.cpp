#include "rtmp/Amf0.hpp"
#include "core/Buffer.hpp"

#include <cstring>
#include <sstream>

namespace streaming {

Amf0Value::Amf0Value()
    : type_(kAmf0Undefined), number_(0), bool_(false) {}

Amf0Value Amf0Value::number(double v) {
    Amf0Value val;
    val.type_ = kAmf0Number;
    val.number_ = v;
    return val;
}
Amf0Value Amf0Value::boolean(bool v) {
    Amf0Value val;
    val.type_ = kAmf0Boolean;
    val.bool_ = v;
    return val;
}
Amf0Value Amf0Value::str(std::string v) {
    Amf0Value val;
    val.type_ = kAmf0String;
    val.string_ = std::move(v);
    return val;
}
Amf0Value Amf0Value::object(Object v) {
    Amf0Value val;
    val.type_ = kAmf0Object;
    val.object_ = std::move(v);
    return val;
}
Amf0Value Amf0Value::ecmaArray(Object v) {
    Amf0Value val;
    val.type_ = kAmf0EcmaArray;
    val.object_ = std::move(v);
    return val;
}
Amf0Value Amf0Value::strictArray(Array v) {
    Amf0Value val;
    val.type_ = kAmf0StrictArray;
    val.array_ = std::move(v);
    return val;
}
Amf0Value Amf0Value::null() {
    Amf0Value val;
    val.type_ = kAmf0Null;
    return val;
}
Amf0Value Amf0Value::undefined() {
    Amf0Value val;
    val.type_ = kAmf0Undefined;
    return val;
}

double Amf0Value::asNumber(double def) const {
    if (type_ == kAmf0Number) return number_;
    return def;
}
bool Amf0Value::asBoolean(bool def) const {
    if (type_ == kAmf0Boolean) return bool_;
    return def;
}
std::string Amf0Value::asString(const std::string& def) const {
    if (type_ == kAmf0String || type_ == kAmf0LongString) return string_;
    return def;
}
const Amf0Value::Object* Amf0Value::asObject() const {
    if (type_ == kAmf0Object || type_ == kAmf0EcmaArray) return &object_;
    return nullptr;
}
const Amf0Value::Array* Amf0Value::asArray() const {
    if (type_ == kAmf0StrictArray) return &array_;
    return nullptr;
}

// ---- 编码 ----
void Amf0Value::encodeString(Buffer& buf, const std::string& s) const {
    if (s.size() < 0xFFFF) {
        buf.appendInt16(static_cast<int16_t>(s.size()));
    } else {
        buf.appendInt8(static_cast<int8_t>(kAmf0LongString));
        buf.appendInt32(static_cast<int32_t>(s.size()));
    }
    buf.append(s);
}

void Amf0Value::encodeObject(Buffer& buf, const Object& obj) const {
    for (const auto& [k, v] : obj) {
        buf.appendInt16(static_cast<int16_t>(k.size()));
        buf.append(k);
        v.encode(buf);
    }
    // object end: 0x00 0x00 0x09
    buf.appendInt16(0);
    buf.appendInt8(static_cast<int8_t>(kAmf0ObjectEnd));
}

void Amf0Value::encode(Buffer& buf) const {
    buf.appendInt8(static_cast<int8_t>(type_));
    switch (type_) {
        case kAmf0Number: {
            // 大端 double
            uint64_t bits;
            std::memcpy(&bits, &number_, 8);
            for (int i = 7; i >= 0; --i) {
                buf.appendInt8(static_cast<int8_t>((bits >> (i * 8)) & 0xFF));
            }
            break;
        }
        case kAmf0Boolean:
            buf.appendInt8(bool_ ? 1 : 0);
            break;
        case kAmf0String:
            encodeString(buf, string_);
            break;
        case kAmf0LongString:
            buf.appendInt32(static_cast<int32_t>(string_.size()));
            buf.append(string_);
            break;
        case kAmf0Object:
            encodeObject(buf, object_);
            break;
        case kAmf0EcmaArray:
            buf.appendInt32(static_cast<int32_t>(object_.size()));
            encodeObject(buf, object_);
            break;
        case kAmf0StrictArray:
            buf.appendInt32(static_cast<int32_t>(array_.size()));
            for (const auto& v : array_) v.encode(buf);
            break;
        case kAmf0Null:
        case kAmf0Undefined:
            break;
        default:
            break;
    }
}

// ---- 解码 ----
static std::string readUtf8(Buffer& buf, size_t len) {
    if (buf.readableBytes() < len) return "";
    std::string s(buf.peek(), len);
    buf.retrieve(len);
    return s;
}

bool Amf0Value::decode(Buffer& buf) {
    if (buf.readableBytes() < 1) return false;
    type_ = static_cast<Amf0Type>(buf.readInt8());

    switch (type_) {
        case kAmf0Number: {
            if (buf.readableBytes() < 8) return false;
            uint64_t bits = 0;
            for (int i = 7; i >= 0; --i) {
                bits |= static_cast<uint64_t>(static_cast<uint8_t>(buf.readInt8()))
                        << (i * 8);
            }
            std::memcpy(&number_, &bits, 8);
            break;
        }
        case kAmf0Boolean:
            if (buf.readableBytes() < 1) return false;
            bool_ = buf.readInt8() != 0;
            break;
        case kAmf0String: {
            if (buf.readableBytes() < 2) return false;
            uint16_t len = static_cast<uint16_t>(buf.readInt16());
            string_ = readUtf8(buf, len);
            if (string_.size() != len) return false;
            break;
        }
        case kAmf0LongString: {
            if (buf.readableBytes() < 4) return false;
            uint32_t len = static_cast<uint32_t>(buf.readInt32());
            string_ = readUtf8(buf, len);
            if (string_.size() != len) return false;
            break;
        }
        case kAmf0Object:
            object_.clear();
            for (;;) {
                if (buf.readableBytes() < 2) return false;
                uint16_t klen = static_cast<uint16_t>(buf.readInt16());
                if (klen == 0) {
                    if (buf.readableBytes() < 1) return false;
                    uint8_t marker = static_cast<uint8_t>(buf.readInt8());
                    if (marker == kAmf0ObjectEnd) break;
                    continue;
                }
                std::string key = readUtf8(buf, klen);
                Amf0Value v;
                if (!v.decode(buf)) return false;
                object_[key] = std::move(v);
            }
            break;
        case kAmf0EcmaArray:
            if (buf.readableBytes() < 4) return false;
            buf.readInt32();  // count，忽略，按 object-end 结束
            object_.clear();
            for (;;) {
                if (buf.readableBytes() < 2) return false;
                uint16_t klen = static_cast<uint16_t>(buf.readInt16());
                if (klen == 0) {
                    if (buf.readableBytes() < 1) return false;
                    uint8_t marker = static_cast<uint8_t>(buf.readInt8());
                    if (marker == kAmf0ObjectEnd) break;
                    continue;
                }
                std::string key = readUtf8(buf, klen);
                Amf0Value v;
                if (!v.decode(buf)) return false;
                object_[key] = std::move(v);
            }
            break;
        case kAmf0StrictArray: {
            if (buf.readableBytes() < 4) return false;
            uint32_t count = static_cast<uint32_t>(buf.readInt32());
            array_.clear();
            array_.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                Amf0Value v;
                if (!v.decode(buf)) return false;
                array_.push_back(std::move(v));
            }
            break;
        }
        case kAmf0Null:
        case kAmf0Undefined:
            break;
        default:
            return false;
    }
    return true;
}

std::string Amf0Value::inspect(int indent) const {
    std::ostringstream os;
    std::string pad(indent * 2, ' ');
    switch (type_) {
        case kAmf0Number: os << number_; break;
        case kAmf0Boolean: os << (bool_ ? "true" : "false"); break;
        case kAmf0String:
        case kAmf0LongString:
            os << "\"" << string_ << "\""; break;
        case kAmf0Null: os << "null"; break;
        case kAmf0Undefined: os << "undefined"; break;
        case kAmf0Object:
        case kAmf0EcmaArray: {
            os << "{\n";
            for (const auto& [k, v] : object_) {
                os << pad << "  \"" << k << "\": "
                   << v.inspect(indent + 1) << "\n";
            }
            os << pad << "}";
            break;
        }
        case kAmf0StrictArray: {
            os << "[";
            for (size_t i = 0; i < array_.size(); ++i) {
                if (i) os << ", ";
                os << array_[i].inspect(indent);
            }
            os << "]";
            break;
        }
        default: os << "?type=" << static_cast<int>(type_);
    }
    return os.str();
}

}  // namespace streaming
