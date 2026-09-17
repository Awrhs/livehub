#include "api/Jwt.hpp"
#include "test_framework.hpp"

#include <map>
#include <string>

using namespace streaming;

TEST(jwt_sign_verify_roundtrip) {
    Jwt jwt("my-secret");
    std::map<std::string, std::string> payload = {
        {"user", "alice"}, {"role", "admin"}};
    std::string token = jwt.sign(payload, 3600);
    ASSERT_FALSE(token.empty());

    auto out = jwt.verify(token);
    ASSERT_FALSE(out.empty());
    ASSERT_EQ(out.at("user"), std::string("alice"));
    ASSERT_EQ(out.at("role"), std::string("admin"));
}

TEST(jwt_tampered_signature_rejected) {
    Jwt jwt("secret");
    std::string token = jwt.sign({{"user", "bob"}}, 3600);
    // 翻转签名最后一个字符
    char& c = token.back();
    c = (c == 'A') ? 'B' : 'A';
    auto out = jwt.verify(token);
    ASSERT_TRUE(out.empty());
}

TEST(jwt_wrong_secret_rejected) {
    Jwt a("secret-a");
    std::string token = a.sign({{"user", "x"}}, 3600);
    Jwt b("secret-b");
    ASSERT_TRUE(b.verify(token).empty());
}

TEST(jwt_expired_rejected) {
    Jwt jwt("s");
    std::string token = jwt.sign({{"user", "x"}}, -10);  // 已过期
    ASSERT_TRUE(jwt.verify(token).empty());
}

TEST(jwt_three_dots_format) {
    Jwt jwt("s");
    std::string token = jwt.sign({{"k", "v"}}, 100);
    size_t dots = 0;
    for (char c : token)
        if (c == '.') ++dots;
    ASSERT_EQ(dots, size_t(2));  // header.payload.sig
}
