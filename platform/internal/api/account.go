package api

import (
	"errors"
	"fmt"
	"net"
	"net/http"
	"net/url"
	"strings"
	"time"
	"unicode/utf8"

	"livehub/internal/auth"
	"livehub/internal/store"
)

// ---------- 站点信息 ----------

type ingestInfo struct {
	RTMP string `json:"rtmp"`
	SRT  string `json:"srt,omitempty"`
}

// publicHost 对外主机名（不含端口）：优先 LIVEHUB_PUBLIC_URL，否则取请求 Host。
func (s *Server) publicHost(r *http.Request) string {
	if s.Cfg.PublicURL != "" {
		if u, err := url.Parse(s.Cfg.PublicURL); err == nil && u.Hostname() != "" {
			return u.Hostname()
		}
	}
	host := r.Host
	if s.Cfg.TrustProxy {
		if xf := r.Header.Get("X-Forwarded-Host"); xf != "" {
			host = xf
		}
	}
	if h, _, err := net.SplitHostPort(host); err == nil {
		return h
	}
	return host
}

func (s *Server) ingest(r *http.Request) ingestInfo {
	var in ingestInfo
	host := s.publicHost(r)
	if strings.Contains(host, ":") { // IPv6
		host = "[" + host + "]"
	}
	if s.Cfg.RTMPURL != "" {
		in.RTMP = s.Cfg.RTMPURL
	} else if s.Cfg.MediaRTMPPort == 1935 {
		in.RTMP = "rtmp://" + host + "/live"
	} else {
		in.RTMP = fmt.Sprintf("rtmp://%s:%d/live", host, s.Cfg.MediaRTMPPort)
	}
	if s.Cfg.SRTURL != "" {
		in.SRT = s.Cfg.SRTURL
	} else if s.Cfg.MediaSRTPort > 0 {
		in.SRT = fmt.Sprintf("srt://%s:%d", host, s.Cfg.MediaSRTPort)
	}
	return in
}

func (s *Server) handleSite(w http.ResponseWriter, r *http.Request) {
	ctx := r.Context()
	set, err := s.Store.AllPublicSettings(ctx)
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	n, err := s.Store.CountUsers(ctx)
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"name":              set[store.SettingSiteName],
		"announcement":      set[store.SettingAnnouncement],
		"registration_mode": set[store.SettingRegistrationMode],
		"stream_approval":   set[store.SettingStreamApproval] == "true",
		"setup_required":    n == 0, // 尚无用户：首个注册者成为管理员
		"version":           s.Version,
		"ingest":            s.ingest(r),
		"media_enabled":     s.Media != nil,
	})
}

// ---------- 注册 / 登录 ----------

type credentials struct {
	Username    string `json:"username"`
	Password    string `json:"password"`
	DisplayName string `json:"display_name"`
	Invite      string `json:"invite"`
}

func (s *Server) handleRegister(w http.ResponseWriter, r *http.Request) {
	ctx := r.Context()
	if !s.registerLimiter.Allow(s.clientIP(r)) {
		writeError(w, http.StatusTooManyRequests, "rate_limited", "注册过于频繁，请稍后再试")
		return
	}
	var c credentials
	if !decode(w, r, &c) {
		return
	}
	c.Username = strings.TrimSpace(c.Username)
	c.DisplayName = strings.TrimSpace(c.DisplayName)
	c.Invite = strings.TrimSpace(c.Invite)
	if c.DisplayName == "" {
		c.DisplayName = c.Username
	}
	if err := auth.ValidateUsername(c.Username); err != nil {
		code := "invalid_username"
		if errors.Is(err, auth.ErrReserved) {
			code = "reserved_username"
		}
		writeError(w, http.StatusBadRequest, code, err.Error())
		return
	}
	if err := auth.ValidatePassword(c.Password); err != nil {
		writeError(w, http.StatusBadRequest, "invalid_password", err.Error())
		return
	}
	if err := auth.ValidateDisplayName(c.DisplayName); err != nil {
		writeError(w, http.StatusBadRequest, "invalid_display_name", err.Error())
		return
	}
	n, err := s.Store.CountUsers(ctx)
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	// 首个用户（管理员）不受注册模式限制
	invite := ""
	if n > 0 {
		mode, _ := s.Store.Setting(ctx, store.SettingRegistrationMode)
		switch mode {
		case "closed":
			writeError(w, http.StatusForbidden, "registration_closed", "本站已关闭注册")
			return
		case "invite":
			if c.Invite == "" {
				writeError(w, http.StatusForbidden, "invite_required", "本站需要邀请码才能注册")
				return
			}
			if !s.Store.InviteValid(ctx, c.Invite) {
				writeError(w, http.StatusForbidden, "invalid_invite", "邀请码无效或已被使用")
				return
			}
			invite = c.Invite
		}
	}
	hash, err := auth.HashPassword(c.Password, s.Cfg.BcryptCost)
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	u, err := s.Store.CreateUserWithChannel(ctx, c.Username, c.DisplayName, hash, invite)
	if errors.Is(err, store.ErrConflict) {
		writeError(w, http.StatusConflict, "username_taken", "用户名已被占用")
		return
	}
	if errors.Is(err, store.ErrNotFound) {
		writeError(w, http.StatusForbidden, "invalid_invite", "邀请码无效或已被使用")
		return
	}
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	if u.Role == store.RoleAdmin {
		s.Store.Audit(ctx, u.ID, "site.setup", u.Username, "首个用户成为管理员")
	}
	s.startSession(w, r, u, http.StatusCreated)
}

func (s *Server) startSession(w http.ResponseWriter, r *http.Request, u *store.User, status int) {
	tok, err := s.Store.CreateSession(r.Context(), u.ID, s.clientIP(r), r.UserAgent())
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	_ = s.Store.TouchLogin(r.Context(), u.ID)
	s.setSessionCookie(w, r, tok, int(store.SessionTTL.Seconds()))
	me, err := s.meView(r, u)
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	writeJSON(w, status, me)
}

func (s *Server) handleLogin(w http.ResponseWriter, r *http.Request) {
	if !s.authLimiter.Allow(s.clientIP(r)) {
		writeError(w, http.StatusTooManyRequests, "rate_limited", "登录尝试过于频繁，请稍后再试")
		return
	}
	var c credentials
	if !decode(w, r, &c) {
		return
	}
	name := strings.ToLower(strings.TrimSpace(c.Username))
	if s.loginLocked(name) {
		writeError(w, http.StatusTooManyRequests, "account_locked", "该账号登录失败次数过多，请 15 分钟后再试")
		return
	}
	u, err := s.Store.UserByName(r.Context(), strings.TrimSpace(c.Username))
	if err != nil && !isNotFound(err) {
		s.internalError(w, r, err)
		return
	}
	if u == nil {
		auth.DummyCheck(c.Password)
		writeError(w, http.StatusUnauthorized, "invalid_credentials", "用户名或密码错误")
		return
	}
	if !auth.CheckPassword(u.PasswordHash, c.Password) {
		s.recordLoginFailure(name)
		writeError(w, http.StatusUnauthorized, "invalid_credentials", "用户名或密码错误")
		return
	}
	s.clearLoginFailures(name)
	if u.Banned {
		msg := "账号已被封禁"
		if u.BanReason != "" {
			msg += "：" + u.BanReason
		}
		writeError(w, http.StatusForbidden, "banned", msg)
		return
	}
	s.startSession(w, r, u, http.StatusOK)
}

const (
	loginMaxFailures = 8
	loginWindow      = 15 * time.Minute
)

func (s *Server) recentFailures(name string, now time.Time) []time.Time {
	var kept []time.Time
	for _, t := range s.loginFailures[name] {
		if now.Sub(t) < loginWindow {
			kept = append(kept, t)
		}
	}
	return kept
}

func (s *Server) loginLocked(name string) bool {
	s.loginMu.Lock()
	defer s.loginMu.Unlock()
	f := s.recentFailures(name, time.Now())
	if len(f) == 0 {
		delete(s.loginFailures, name)
	} else {
		s.loginFailures[name] = f
	}
	return len(f) >= loginMaxFailures
}

func (s *Server) recordLoginFailure(name string) {
	s.loginMu.Lock()
	defer s.loginMu.Unlock()
	now := time.Now()
	s.loginFailures[name] = append(s.recentFailures(name, now), now)
}

func (s *Server) clearLoginFailures(name string) {
	s.loginMu.Lock()
	defer s.loginMu.Unlock()
	delete(s.loginFailures, name)
}

func (s *Server) handleLogout(w http.ResponseWriter, r *http.Request) {
	if c, err := r.Cookie(sessionCookie); err == nil {
		_ = s.Store.DeleteSession(r.Context(), c.Value)
	}
	s.setSessionCookie(w, r, "", -1)
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

// ---------- 当前用户 ----------

// canStream 用户是否有开播权限。
func (s *Server) canStream(r *http.Request, u *store.User) bool {
	if u.Banned {
		return false
	}
	if u.Role == store.RoleAdmin || u.StreamStatus == store.StreamApproved {
		return true
	}
	return !s.Store.SettingBool(r.Context(), store.SettingStreamApproval)
}

type myChannel struct {
	*store.Channel
	HasStreamKey bool `json:"has_stream_key"`
}

// revealedKey 新生成的推流码：只在生成的这一次响应中返回（库中只存摘要）。
type revealedKey struct {
	StreamKey  string `json:"stream_key"`
	PublishKey string `json:"publish_key"` // OBS「推流码」：<频道>?key=<密钥>
	SRTURL     string `json:"srt_url,omitempty"`
}

type meResponse struct {
	*store.User
	CanStream bool         `json:"can_stream"`
	Channel   *myChannel   `json:"channel"`
	Ingest    ingestInfo   `json:"ingest"`
	NewKey    *revealedKey `json:"new_key,omitempty"`
}

func (s *Server) meView(r *http.Request, u *store.User) (*meResponse, error) {
	ch, err := s.Store.ChannelByUser(r.Context(), u.ID)
	if err != nil {
		return nil, err
	}
	in := s.ingest(r)
	mc := &myChannel{Channel: ch, HasStreamKey: ch.HasStreamKey()}
	return &meResponse{User: u, CanStream: s.canStream(r, u), Channel: mc, Ingest: in}, nil
}

func (s *Server) handleMe(w http.ResponseWriter, r *http.Request, u *store.User) {
	me, err := s.meView(r, u)
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, me)
}

func (s *Server) handleUpdateMe(w http.ResponseWriter, r *http.Request, u *store.User) {
	var body struct {
		DisplayName *string `json:"display_name"`
		Bio         *string `json:"bio"`
	}
	if !decode(w, r, &body) {
		return
	}
	dn, bio := u.DisplayName, u.Bio
	if body.DisplayName != nil {
		dn = strings.TrimSpace(*body.DisplayName)
		if err := auth.ValidateDisplayName(dn); err != nil {
			writeError(w, http.StatusBadRequest, "invalid_display_name", err.Error())
			return
		}
	}
	if body.Bio != nil {
		bio = strings.TrimSpace(*body.Bio)
		if utf8.RuneCountInString(bio) > 500 {
			writeError(w, http.StatusBadRequest, "bad_request", "简介不能超过 500 字")
			return
		}
	}
	if err := s.Store.UpdateProfile(r.Context(), u.ID, dn, bio); err != nil {
		s.internalError(w, r, err)
		return
	}
	u.DisplayName, u.Bio = dn, bio
	s.handleMe(w, r, u)
}

func (s *Server) handleChangePassword(w http.ResponseWriter, r *http.Request, u *store.User) {
	var body struct {
		Old string `json:"old_password"`
		New string `json:"new_password"`
	}
	if !decode(w, r, &body) {
		return
	}
	if !auth.CheckPassword(u.PasswordHash, body.Old) {
		writeError(w, http.StatusBadRequest, "invalid_credentials", "原密码错误")
		return
	}
	if err := auth.ValidatePassword(body.New); err != nil {
		writeError(w, http.StatusBadRequest, "invalid_password", err.Error())
		return
	}
	hash, err := auth.HashPassword(body.New, s.Cfg.BcryptCost)
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	if err := s.Store.UpdatePassword(r.Context(), u.ID, hash); err != nil {
		s.internalError(w, r, err)
		return
	}
	// 注销其他设备上的会话
	keep := ""
	if c, err := r.Cookie(sessionCookie); err == nil {
		keep = c.Value
	}
	_ = s.Store.DeleteUserSessions(r.Context(), u.ID, keep)
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

func (s *Server) handleStreamRequest(w http.ResponseWriter, r *http.Request, u *store.User) {
	var body struct {
		Note string `json:"note"`
	}
	if !decode(w, r, &body) {
		return
	}
	note := strings.TrimSpace(body.Note)
	if utf8.RuneCountInString(note) > 500 {
		writeError(w, http.StatusBadRequest, "bad_request", "申请说明不能超过 500 字")
		return
	}
	if s.canStream(r, u) {
		writeError(w, http.StatusConflict, "already_allowed", "你已经可以开播")
		return
	}
	if err := s.Store.RequestStream(r.Context(), u.ID, note); err != nil {
		s.internalError(w, r, err)
		return
	}
	s.Store.Audit(r.Context(), u.ID, "stream.request", u.Username, note)
	u.StreamStatus, u.StreamRequestNote = store.StreamPending, note
	s.handleMe(w, r, u)
}
