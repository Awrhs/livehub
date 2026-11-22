// Package auth 提供密码哈希与账号字段校验。
package auth

import (
	"errors"
	"regexp"
	"strings"
	"unicode/utf8"

	"golang.org/x/crypto/bcrypt"
)

var usernameRe = regexp.MustCompile(`^[A-Za-z][A-Za-z0-9_]{2,23}$`)

// 多清晰度转码的变体流名为 <频道>_<档位>（如 alice_720p），禁止注册此类用户名以免冲突
var variantRe = regexp.MustCompile(`_[0-9]+p$`)

// 与前端路由、平台路径冲突的保留名（频道页地址为 /<用户名>）
var reserved = map[string]bool{
	"admin": true, "api": true, "live": true, "login": true, "logout": true, "register": true,
	"settings": true, "dashboard": true, "directory": true, "following": true, "internal": true,
	"assets": true, "static": true, "about": true, "search": true, "vod": true, "videos": true,
	"notifications": true, "help": true, "whip": true, "whep": true, "metrics": true, "health": true,
	"healthz": true, "root": true, "system": true, "livehub": true, "favicon": true, "robots": true,
	"category": true, "categories": true, "browse": true, "moderator": true, "support": true,
}

var (
	ErrUsername    = errors.New("用户名须为 3-24 位字母、数字或下划线，且以字母开头")
	ErrReserved    = errors.New("该用户名为系统保留")
	ErrPassword    = errors.New("密码长度须为 8-72 个字符")
	ErrDisplayName = errors.New("昵称长度须为 1-32 个字符")
)

func ValidateUsername(u string) error {
	if !usernameRe.MatchString(u) {
		return ErrUsername
	}
	if reserved[strings.ToLower(u)] || variantRe.MatchString(strings.ToLower(u)) {
		return ErrReserved
	}
	return nil
}

func ValidatePassword(p string) error {
	// bcrypt 只使用前 72 字节
	if utf8.RuneCountInString(p) < 8 || len(p) > 72 {
		return ErrPassword
	}
	return nil
}

func ValidateDisplayName(n string) error {
	n = strings.TrimSpace(n)
	if c := utf8.RuneCountInString(n); c < 1 || c > 32 {
		return ErrDisplayName
	}
	return nil
}

func HashPassword(p string, cost int) (string, error) {
	if cost < bcrypt.MinCost {
		cost = bcrypt.DefaultCost
	}
	h, err := bcrypt.GenerateFromPassword([]byte(p), cost)
	return string(h), err
}

func CheckPassword(hash, p string) bool {
	return bcrypt.CompareHashAndPassword([]byte(hash), []byte(p)) == nil
}

// dummyHash 用户不存在时也执行一次比较，避免通过响应时间探测用户名是否存在
var dummyHash, _ = bcrypt.GenerateFromPassword([]byte("livehub-dummy-password"), bcrypt.DefaultCost)

func DummyCheck(p string) { _ = bcrypt.CompareHashAndPassword(dummyHash, []byte(p)) }
