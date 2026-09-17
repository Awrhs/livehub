#pragma once

/**
 * NonCopyable: 禁止拷贝构造与拷贝赋值的基类。
 * 继承此类可使派生类默认不可拷贝，但仍可移动（除非也禁用移动）。
 * 用于 Socket、EventLoop、Buffer 等不应被拷贝的资源所有权类型。
 */
namespace streaming {

class NonCopyable {
public:
    NonCopyable(const NonCopyable&) = delete;
    NonCopyable& operator=(const NonCopyable&) = delete;

protected:
    NonCopyable() = default;
    ~NonCopyable() = default;
};

}  // namespace streaming
