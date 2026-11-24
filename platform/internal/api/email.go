package api

import (
	"context"
	"errors"
	"net/http"
	"net/mail"
	"net/url"
	"strings"
	"time"

	"livehub/internal/auth"
	"livehub/internal/store"
)

const (
	verifyTokenTTL = 48 * time.Hour
	resetTokenTTL  = time.Hour
	mailCooldown   = 60 // 同一用户同类邮件最短间隔（秒）
)

// normalizeEmail 校验邮箱格式，返回规范化地址；不合法返回空串。
func normalizeEmail(s string) string {
	s = strings.TrimSpace(s)
	if s == "" || len(s) > 254 || strings.ContainsAny(s, "\r\n<>") {
		return ""
	}
	a, err := mail.ParseAddress(s)
	if err != nil || a.Address != s || !strings.Contains(s[strings.LastIndex(s, "@")+1:], ".") {
		return ""
	}
	return s
}

// baseURL 邮件中链接的站点地址：优先 LIVEHUB_PUBLIC_URL，否则按请求推断。
func (s *Server) baseURL(r *http.Request) string {
	if s.Cfg.PublicURL != "" {
		return s.Cfg.PublicURL
	}
	scheme := "http"
	if r.TLS != nil || (s.Cfg.TrustProxy && r.Header.Get("X-Forwarded-Proto") == "https") {
		scheme = "https"
	}
	host := r.Host
	if s.Cfg.TrustProxy && r.Header.Get("X-Forwarded-Host") != "" {
		host = r.Header.Get("X-Forwarded-Host")
	}
	return scheme + "://" + host
}

// sendMailAsync 异步发信（不阻塞请求；失败只记录日志）。
func (s *Server) sendMailAsync(to, subject, body string) {
	if s.Mailer == nil {
		return
	}
	go func() {
		ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
		defer cancel()
		if err := s.Mailer.Send(ctx, to, subject, body); err != nil {
			s.Log.Warn("发送邮件失败", "to", to, "subject", subject, "err", err)
		} else {
			s.Log.Info("已发送邮件", "to", to, "subject", subject)
		}
	}()
}

func (s *Server) siteName(r *http.Request) string {
	n, _ := s.Store.Setting(r.Context(), store.SettingSiteName)
	return n
}

// sendVerification 生成验证令牌并发送验证邮件。
func (s *Server) sendVerification(r *http.Request, u *store.User, email string) error {
	if s.Mailer == nil {
		return nil
	}
	tok, err := s.Store.CreateToken(r.Context(), u.ID, store.TokenVerifyEmail, email, verifyTokenTTL)
	if err != nil {
		return err
	}
	link := s.baseURL(r) + "/verify-email?token=" + url.QueryEscape(tok)
	site := s.siteName(r)
	s.sendMailAsync(email, "["+site+"] 请验证你的邮箱 / Verify your email",
		u.DisplayName+"，你好：\n\n请点击下面的链接验证你在「"+site+"」绑定的邮箱（48 小时内有效）：\n\n"+link+
			"\n\n验证后可以用此邮箱找回密码。如果这不是你本人的操作，请忽略本邮件。\n\n"+
			"Hi "+u.DisplayName+", please verify your email for "+site+" (valid for 48 hours):\n"+link+"\n")
	return nil
}

// handleSetEmail POST /api/v1/me/email {email}：绑定或更换邮箱（空字符串表示解绑）。
func (s *Server) handleSetEmail(w http.ResponseWriter, r *http.Request, u *store.User) {
	var body struct {
		Email string `json:"email"`
	}
	if !decode(w, r, &body) {
		return
	}
	email := ""
	if strings.TrimSpace(body.Email) != "" {
		if email = normalizeEmail(body.Email); email == "" {
			writeError(w, http.StatusBadRequest, "invalid_email", "邮箱格式不正确")
			return
		}
	}
	if strings.EqualFold(email, u.Email) && u.EmailVerified {
		s.handleMe(w, r, u)
		return
	}
	if err := s.Store.SetEmail(r.Context(), u.ID, email); err != nil {
		if errors.Is(err, store.ErrConflict) {
			writeError(w, http.StatusConflict, "email_taken", "该邮箱已被其他账号使用")
			return
		}
		s.internalError(w, r, err)
		return
	}
	u.Email, u.EmailVerified = email, false
	if email != "" {
		if err := s.sendVerification(r, u, email); err != nil {
			s.internalError(w, r, err)
			return
		}
	}
	s.handleMe(w, r, u)
}

// handleResendVerification POST /api/v1/me/email/resend
func (s *Server) handleResendVerification(w http.ResponseWriter, r *http.Request, u *store.User) {
	if s.Mailer == nil {
		writeError(w, http.StatusServiceUnavailable, "mail_disabled", "本站未配置邮件服务")
		return
	}
	if u.Email == "" || u.EmailVerified {
		writeError(w, http.StatusBadRequest, "bad_request", "没有待验证的邮箱")
		return
	}
	if time.Now().Unix()-s.Store.LastTokenAt(r.Context(), u.ID, store.TokenVerifyEmail) < mailCooldown {
		writeError(w, http.StatusTooManyRequests, "rate_limited", "发送过于频繁，请稍后再试")
		return
	}
	if err := s.sendVerification(r, u, u.Email); err != nil {
		s.internalError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

// handleVerifyEmail POST /api/v1/auth/verify-email {token}（无需登录：用户可能在另一台设备上打开邮件）
func (s *Server) handleVerifyEmail(w http.ResponseWriter, r *http.Request) {
	var body struct {
		Token string `json:"token"`
	}
	if !decode(w, r, &body) {
		return
	}
	uid, email, err := s.Store.ConsumeToken(r.Context(), body.Token, store.TokenVerifyEmail)
	if err != nil {
		writeError(w, http.StatusBadRequest, "invalid_token", "链接无效或已过期")
		return
	}
	ok, err := s.Store.MarkEmailVerified(r.Context(), uid, email)
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	if !ok {
		writeError(w, http.StatusBadRequest, "invalid_token", "该邮箱已被更换，请使用最新的验证邮件")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"ok": true, "email": email})
}

// handleForgotPassword POST /api/v1/auth/forgot {login}：用户名或邮箱。
// 无论账号是否存在都返回相同结果，避免被用来探测注册信息。
func (s *Server) handleForgotPassword(w http.ResponseWriter, r *http.Request) {
	if !s.authLimiter.Allow("forgot:" + s.clientIP(r)) {
		writeError(w, http.StatusTooManyRequests, "rate_limited", "操作过于频繁，请稍后再试")
		return
	}
	if s.Mailer == nil {
		writeError(w, http.StatusServiceUnavailable, "mail_disabled", "本站未配置邮件服务，请联系管理员重置密码")
		return
	}
	var body struct {
		Login string `json:"login"`
	}
	if !decode(w, r, &body) {
		return
	}
	login := strings.TrimSpace(body.Login)
	var u *store.User
	if strings.Contains(login, "@") {
		u, _ = s.Store.UserByEmail(r.Context(), login)
	} else {
		u, _ = s.Store.UserByName(r.Context(), login)
	}
	if u != nil && u.Email != "" && u.EmailVerified && !u.Banned &&
		time.Now().Unix()-s.Store.LastTokenAt(r.Context(), u.ID, store.TokenResetPassword) >= mailCooldown {
		tok, err := s.Store.CreateToken(r.Context(), u.ID, store.TokenResetPassword, u.Email, resetTokenTTL)
		if err != nil {
			s.internalError(w, r, err)
			return
		}
		link := s.baseURL(r) + "/reset-password?token=" + url.QueryEscape(tok)
		site := s.siteName(r)
		s.sendMailAsync(u.Email, "["+site+"] 重置密码 / Reset your password",
			u.DisplayName+"，你好：\n\n有人（希望是你）请求重置「"+site+"」账号 "+u.Username+" 的密码。\n"+
				"点击下面的链接设置新密码（1 小时内有效，只能使用一次）：\n\n"+link+
				"\n\n如果不是你本人的操作，请忽略本邮件，你的密码不会改变。\n\n"+
				"Someone requested a password reset for "+u.Username+" on "+site+". Use this link within 1 hour:\n"+link+"\n")
	}
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

// handleResetPassword POST /api/v1/auth/reset {token, password}
func (s *Server) handleResetPassword(w http.ResponseWriter, r *http.Request) {
	var body struct {
		Token    string `json:"token"`
		Password string `json:"password"`
	}
	if !decode(w, r, &body) {
		return
	}
	if err := auth.ValidatePassword(body.Password); err != nil {
		writeError(w, http.StatusBadRequest, "invalid_password", err.Error())
		return
	}
	uid, _, err := s.Store.ConsumeToken(r.Context(), body.Token, store.TokenResetPassword)
	if err != nil {
		writeError(w, http.StatusBadRequest, "invalid_token", "链接无效或已过期")
		return
	}
	hash, err := auth.HashPassword(body.Password, s.Cfg.BcryptCost)
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	if err := s.Store.UpdatePassword(r.Context(), uid, hash); err != nil {
		s.internalError(w, r, err)
		return
	}
	_ = s.Store.DeleteUserSessions(r.Context(), uid, "")
	s.Store.Audit(r.Context(), uid, "user.reset_password_email", "", "")
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

// handleAdminTestMail POST /api/v1/admin/mail/test：给管理员自己的邮箱发一封测试邮件（同步发送以便返回错误）。
func (s *Server) handleAdminTestMail(w http.ResponseWriter, r *http.Request, admin *store.User) {
	if s.Mailer == nil {
		writeError(w, http.StatusServiceUnavailable, "mail_disabled", "本站未配置邮件服务（LIVEHUB_SMTP_*）")
		return
	}
	if admin.Email == "" {
		writeError(w, http.StatusBadRequest, "no_email", "请先在账号设置中填写你的邮箱")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 30*time.Second)
	defer cancel()
	if err := s.Mailer.Send(ctx, admin.Email, "["+s.siteName(r)+"] 测试邮件 / Test email", "邮件服务配置正常。\nEmail is working.\n"); err != nil {
		writeError(w, http.StatusBadGateway, "mail_failed", "发送失败："+err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}
