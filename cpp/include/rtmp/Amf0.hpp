#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace streaming {

class Buffer;

/**
 * AMF0 类型标记
 */
enum Amf0Type : uint8_t {
    kAmf0Number = 0x00,
    kAmf0Boolean = 0x01,
    kAmf0String = 0x02,
    kAmf0Object = 0x03,
    kAmf0MovieClip = 0x04,
    kAmf0Null = 0x05,
    kAmf0Undefined = 0x06,
    kAmf0Reference = 0x07,
    kAmf0EcmaArray = 0x08,
    kAmf0ObjectEnd = 0x09,
    kAmf0StrictArray = 0x0A,
    kAmf0Date = 0x0B,
    kAmf0LongString = 0x0C,
    kAmf0Unsupported = 0x0D,
    kAmf0XmlDoc = 0x0F,
    kAmf0TypedObject = 0x10,
};

/**
 * Amf0Value: 一个 AMF0 值（变体类型）。
 *
 * 支持 Number/Boolean/String/Object/EcmaArray/StrictArray/Null。
 * Object 与 EcmaArray 都用 map 表示（键序在需要时不保留）。
 */
class Amf0Value {
public:
    using Object = std::map<std::string, Amf0Value>;
    using Array = std::vector<Amf0Value>;

    Amf0Value();

    static Amf0Value number(double v);
    static Amf0Value boolean(bool v);
    static Amf0Value str(std::string v);
    static Amf0Value object(Object v);
    static Amf0Value ecmaArray(Object v);
    static Amf0Value strictArray(Array v);
    static Amf0Value null();
    static Amf0Value undefined();

    Amf0Type type() const { return type_; }

    double asNumber(double def = 0.0) const;
    bool asBoolean(bool def = false) const;
    std::string asString(const std::string& def = "") const;
    const Object* asObject() const;
    const Array* asArray() const;

    bool isNumber() const { return type_ == kAmf0Number; }
    bool isString() const { return type_ == kAmf0String || type_ == kAmf0LongString; }
    bool isObject() const { return type_ == kAmf0Object; }
    bool isEcmaArray() const { return type_ == kAmf0EcmaArray; }

    /// 编码到 buffer
    void encode(Buffer& buf) const;

    /// 从 buffer 解码一个值，返回 true 成功
    bool decode(Buffer& buf);

    /// 调试用
    std::string inspect(int indent = 0) const;

private:
    void encodeString(Buffer& buf, const std::string& s) const;
    void encodeObject(Buffer& buf, const Object& obj) const;

    Amf0Type type_;
    double number_;
    bool bool_;
    std::string string_;
    Object object_;
    Array array_;
};

}  // namespace streaming
