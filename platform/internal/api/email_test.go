package api

import (
	"net/url"
	"regexp"
	"strings"
	"testing"
	"time"

	"livehub/internal/mail"
)

var tokenRe = regexp.MustCompile(`token=([A-Za-z0-9%]+)`)

// waitMail 等待异步发出的邮件
func waitMail(t *testing.T, m *mail.Memory, to, subjectPart string) mail.Message {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if msg, ok := m.Last(to); ok && strings.Contains(msg.Subject, subjectPart) {
			return msg
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatalf("没有收到发给 %s 的「%s」邮件", to, subjectPart)
	return mail.Message{}
}

func tokenOf(t *testing.T, body string) string {
	t.Helper()
	m := tokenRe.FindStringSubmatch(body)
	if m == nil {
		t.Fatalf("邮件中没有令牌链接: %s", body)
	}
	tok, _ := url.QueryUnescape(m[1])
	return tok
}

func TestEmailVerificationAndPasswordReset(t *testing.T) {
	e := newEnv(t)
	mem := &mail.Memory{}
	e.srv.Mailer = mem
	e.client().register("alice")

	// 注册时填写邮箱 → 收到验证邮件
	bob := e.client()
	var me map[string]any
	if code := bob.do("POST", "/api/v1/auth/register", map[string]string{"username": "bob", "password": "password123", "email": "bob@example.com"}, &me); code != 201 {
		t.Fatalf("注册失败: %d", code)
	}
	if me["email"] != "bob@example.com" || me["email_verified"] != false {
		t.Fatalf("注册后邮箱状态错误: %v", me)
	}
	verify := waitMail(t, mem, "bob@example.com", "验证")
	// 未验证的邮箱不能用于找回密码
	e.client().do("POST", "/api/v1/auth/forgot", map[string]string{"login": "bob"}, nil)
	time.Sleep(200 * time.Millisecond)
	if msg, _ := mem.Last("bob@example.com"); strings.Contains(msg.Subject, "重置") {
		t.Fatal("未验证邮箱不应收到重置邮件")
	}
	// 验证
	if code := e.client().do("POST", "/api/v1/auth/verify-email", map[string]string{"token": tokenOf(t, verify.Body)}, nil); code != 200 {
		t.Fatalf("验证邮箱失败: %d", code)
	}
	if code := e.client().do("POST", "/api/v1/auth/verify-email", map[string]string{"token": tokenOf(t, verify.Body)}, nil); code != 400 {
		t.Fatalf("验证链接应只能用一次，得到 %d", code)
	}
	bob.do("GET", "/api/v1/me", nil, &me)
	if me["email_verified"] != true {
		t.Fatalf("邮箱应已验证: %v", me)
	}
	// 同一邮箱不能被其他账号使用
	if code := e.client().do("POST", "/api/v1/auth/register", map[string]string{"username": "carol", "password": "password123", "email": "BOB@example.com"}, nil); code != 409 {
		t.Fatalf("重复邮箱应 409，得到 %d", code)
	}
	// 不存在的账号：同样返回 200（不泄露账号是否存在）
	if code := e.client().do("POST", "/api/v1/auth/forgot", map[string]string{"login": "nobody"}, nil); code != 200 {
		t.Fatalf("找回密码应总是 200，得到 %d", code)
	}
	// 用邮箱找回密码
	if code := e.client().do("POST", "/api/v1/auth/forgot", map[string]string{"login": "bob@example.com"}, nil); code != 200 {
		t.Fatalf("找回密码失败: %d", code)
	}
	reset := waitMail(t, mem, "bob@example.com", "重置")
	tok := tokenOf(t, reset.Body)
	if code := e.client().do("POST", "/api/v1/auth/reset", map[string]string{"token": tok, "password": "short"}, nil); code != 400 {
		t.Fatalf("弱密码应 400，得到 %d", code)
	}
	if code := e.client().do("POST", "/api/v1/auth/reset", map[string]string{"token": tok, "password": "newpassword456"}, nil); code != 200 {
		t.Fatalf("重置密码失败: %d", code)
	}
	if code := bob.do("GET", "/api/v1/me", nil, nil); code != 401 {
		t.Fatalf("重置密码后旧会话应失效，得到 %d", code)
	}
	if code := e.client().do("POST", "/api/v1/auth/login", map[string]string{"username": "bob", "password": "newpassword456"}, nil); code != 200 {
		t.Fatalf("新密码应能登录，得到 %d", code)
	}
	if code := e.client().do("POST", "/api/v1/auth/reset", map[string]string{"token": tok, "password": "another789xyz"}, nil); code != 400 {
		t.Fatalf("重置链接应只能用一次，得到 %d", code)
	}
	// 更换邮箱：状态回到未验证，旧验证链接作废
	c := e.client()
	c.do("POST", "/api/v1/auth/login", map[string]string{"username": "bob", "password": "newpassword456"}, nil)
	c.do("POST", "/api/v1/me/email", map[string]string{"email": "bob2@example.com"}, &me)
	if me["email"] != "bob2@example.com" || me["email_verified"] != false {
		t.Fatalf("更换邮箱后状态错误: %v", me)
	}
	if code := c.do("POST", "/api/v1/me/email", map[string]string{"email": "not-an-email"}, nil); code != 400 {
		t.Fatalf("非法邮箱应 400，得到 %d", code)
	}
}

func TestMailDisabled(t *testing.T) {
	e := newEnv(t)
	var site map[string]any
	e.client().do("GET", "/api/v1/site", nil, &site)
	if site["mail_enabled"] != false {
		t.Fatalf("未配置邮件时 mail_enabled 应为 false: %v", site)
	}
	if code := e.client().do("POST", "/api/v1/auth/forgot", map[string]string{"login": "x"}, nil); code != 503 {
		t.Fatalf("未配置邮件时找回密码应 503，得到 %d", code)
	}
}
