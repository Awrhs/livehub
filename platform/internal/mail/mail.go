// Package mail 通过 SMTP 发送纯文本邮件（邮箱验证、找回密码）。
package mail

import (
	"context"
	"crypto/tls"
	"encoding/base64"
	"fmt"
	"mime"
	"net"
	"net/smtp"
	"strings"
	"sync"
	"time"
)

// Sender 发送邮件；测试中可替换为内存实现。
type Sender interface {
	Send(ctx context.Context, to, subject, body string) error
}

// Config SMTP 配置。TLS：starttls（默认，587 端口）| tls（隐式 TLS，465 端口）| none（仅限内网/测试）。
type Config struct {
	Host     string
	Port     int
	User     string
	Password string
	From     string
	TLS      string
}

type SMTP struct{ cfg Config }

func NewSMTP(cfg Config) *SMTP {
	if cfg.TLS == "" {
		cfg.TLS = "starttls"
	}
	if cfg.Port == 0 {
		switch cfg.TLS {
		case "tls":
			cfg.Port = 465
		case "none":
			cfg.Port = 25
		default:
			cfg.Port = 587
		}
	}
	return &SMTP{cfg: cfg}
}

// Build 构造 MIME 邮件（UTF-8 正文 base64 编码，主题按 RFC 2047 编码）。
func Build(from, to, subject, body string) []byte {
	var b strings.Builder
	w := func(k, v string) { fmt.Fprintf(&b, "%s: %s\r\n", k, v) }
	w("From", from)
	w("To", to)
	w("Subject", mime.QEncoding.Encode("utf-8", subject))
	w("Date", time.Now().Format(time.RFC1123Z))
	domain := "livehub.local"
	if i := strings.LastIndex(from, "@"); i >= 0 {
		domain = strings.Trim(from[i+1:], "> ")
	}
	w("Message-ID", fmt.Sprintf("<%d.%d@%s>", time.Now().UnixNano(), len(to), domain))
	w("MIME-Version", "1.0")
	w("Content-Type", "text/plain; charset=utf-8")
	w("Content-Transfer-Encoding", "base64")
	b.WriteString("\r\n")
	enc := base64.StdEncoding.EncodeToString([]byte(body))
	for len(enc) > 76 {
		b.WriteString(enc[:76] + "\r\n")
		enc = enc[76:]
	}
	b.WriteString(enc + "\r\n")
	return []byte(b.String())
}

func (s *SMTP) Send(ctx context.Context, to, subject, body string) error {
	if strings.ContainsAny(to, "\r\n") || strings.ContainsAny(subject, "\r\n") {
		return fmt.Errorf("非法的邮件头")
	}
	addr := net.JoinHostPort(s.cfg.Host, fmt.Sprint(s.cfg.Port))
	d := net.Dialer{Timeout: 15 * time.Second}
	var conn net.Conn
	var err error
	tlsCfg := &tls.Config{ServerName: s.cfg.Host, MinVersion: tls.VersionTLS12}
	if s.cfg.TLS == "tls" {
		conn, err = tls.DialWithDialer(&d, "tcp", addr, tlsCfg)
	} else {
		conn, err = d.DialContext(ctx, "tcp", addr)
	}
	if err != nil {
		return err
	}
	_ = conn.SetDeadline(time.Now().Add(30 * time.Second))
	c, err := smtp.NewClient(conn, s.cfg.Host)
	if err != nil {
		conn.Close()
		return err
	}
	defer c.Close()
	if s.cfg.TLS == "starttls" {
		if err := c.StartTLS(tlsCfg); err != nil {
			return fmt.Errorf("STARTTLS 失败: %w", err)
		}
	}
	if s.cfg.User != "" {
		if err := c.Auth(smtp.PlainAuth("", s.cfg.User, s.cfg.Password, s.cfg.Host)); err != nil {
			return fmt.Errorf("SMTP 认证失败: %w", err)
		}
	}
	envFrom := s.cfg.From
	if i := strings.Index(envFrom, "<"); i >= 0 {
		envFrom = strings.Trim(envFrom[i:], "<> ")
	}
	if err := c.Mail(envFrom); err != nil {
		return err
	}
	if err := c.Rcpt(to); err != nil {
		return err
	}
	wc, err := c.Data()
	if err != nil {
		return err
	}
	if _, err := wc.Write(Build(s.cfg.From, to, subject, body)); err != nil {
		return err
	}
	if err := wc.Close(); err != nil {
		return err
	}
	return c.Quit()
}

// Memory 内存实现（测试用）。
type Memory struct {
	mu   sync.Mutex
	Sent []Message
}

type Message struct{ To, Subject, Body string }

func (m *Memory) Send(_ context.Context, to, subject, body string) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.Sent = append(m.Sent, Message{to, subject, body})
	return nil
}

// Last 最近一封发给 to 的邮件。
func (m *Memory) Last(to string) (Message, bool) {
	m.mu.Lock()
	defer m.mu.Unlock()
	for i := len(m.Sent) - 1; i >= 0; i-- {
		if strings.EqualFold(m.Sent[i].To, to) {
			return m.Sent[i], true
		}
	}
	return Message{}, false
}
