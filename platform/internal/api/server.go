// Package api 实现 LiveHub 的 HTTP 接口：REST API、聊天 WebSocket、媒体回调、播放反向代理。
package api

import (
	"context"
	"encoding/json"
	"errors"
	"log/slog"
	"net"
	"net/http"
	"net/url"
	"runtime/debug"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"livehub/internal/chat"
	"livehub/internal/config"
	"livehub/internal/mail"
	"livehub/internal/media"
	"livehub/internal/ratelimit"
	"livehub/internal/store"
	"livehub/internal/sysinfo"
)

const sessionCookie = "livehub_session"

type Server struct {
	Cfg        *config.Config
	Store      *store.Store
	Media      *media.Client // 未配置媒体服务器时为 nil
	Hub        *chat.Hub
	Log        *slog.Logger
	HookSecret string
	// Mailer 发信（未配置 SMTP 时为 nil，邮件相关功能关闭）
	Mailer mail.Sender
	// InternalToken 媒体服务器内部推流（多清晰度转码回推）携带的令牌
	InternalToken string
	Static        http.Handler
	Version       string

	authLimiter     *ratelimit.Limiter // 登录：按 IP
	registerLimiter *ratelimit.Limiter // 注册：按 IP
	chatLimiter     *ratelimit.Limiter // 聊天：按用户
	apiLimiter      *ratelimit.Limiter // 其余写操作：按 IP

	slowMu   sync.Mutex
	lastSent map[[2]int64]time.Time // (频道, 用户) → 上次发言时间（慢速模式）

	liveMu   sync.Mutex
	missing  map[int64]int // 对账：频道连续缺席次数
	chanLock sync.Map      // 频道 ID → *sync.Mutex，串行化同一频道的开播/下播处理

	// 账号级登录失败锁定（防止针对单个账号的分布式暴力破解；按 IP 的限流挡不住）
	loginMu       sync.Mutex
	loginFailures map[string][]time.Time

	// 每 IP 的聊天连接数上限
	wsMu    sync.Mutex
	wsPerIP map[string]int

	edges  []*edge
	events *notifier

	sys          *sysinfo.Sampler
	diskLow      atomic.Bool
	lastDiskWarn time.Time

	stateMu  sync.Mutex
	streams  map[string]*streamState // 频道 → 开播期间的录像/转码状态
	abrReady map[string]bool
	recLocks sync.Map // 频道 ID → *sync.Mutex：录像开始与归档串行

	proxy http.Handler
}

func (s *Server) recordMutex(id int64) *sync.Mutex {
	m, _ := s.recLocks.LoadOrStore(id, &sync.Mutex{})
	return m.(*sync.Mutex)
}

func New(cfg *config.Config, st *store.Store, mc *media.Client, hub *chat.Hub, log *slog.Logger,
	hookSecret string, static http.Handler, version string) *Server {
	s := &Server{
		Cfg: cfg, Store: st, Media: mc, Hub: hub, Log: log, HookSecret: hookSecret,
		Static: static, Version: version,
		authLimiter:     ratelimit.PerMinute(10, 10),
		registerLimiter: ratelimit.PerMinute(max(cfg.RegisterPerMin, 1), max(cfg.RegisterPerMin, 1)),
		chatLimiter:     ratelimit.New(1, 3),
		apiLimiter:      ratelimit.PerMinute(120, 60),
		lastSent:        map[[2]int64]time.Time{},
		missing:         map[int64]int{},
		streams:         map[string]*streamState{},
		loginFailures:   map[string][]time.Time{},
		wsPerIP:         map[string]int{},
		abrReady:        map[string]bool{},
	}
	if mc != nil {
		s.proxy = newPlaybackProxy(cfg.MediaURL, log)
	}
	s.initEdges()
	s.events = newNotifier()
	s.sys = &sysinfo.Sampler{Path: cfg.DataDir}
	return s
}

// Handler 构建完整路由。
func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	s.routes(mux)
	return s.recoverer(s.securityHeaders(s.withUser(s.csrf(mux))))
}

func (s *Server) routes(mux *http.ServeMux) {
	// 站点与账号
	mux.HandleFunc("GET /api/v1/site", s.handleSite)
	mux.HandleFunc("POST /api/v1/auth/register", s.handleRegister)
	mux.HandleFunc("POST /api/v1/auth/login", s.handleLogin)
	mux.HandleFunc("POST /api/v1/auth/logout", s.handleLogout)
	mux.HandleFunc("POST /api/v1/auth/verify-email", s.handleVerifyEmail)
	mux.HandleFunc("POST /api/v1/auth/forgot", s.handleForgotPassword)
	mux.HandleFunc("POST /api/v1/auth/reset", s.handleResetPassword)
	mux.HandleFunc("POST /api/v1/me/email", s.requireUser(s.handleSetEmail))
	mux.HandleFunc("POST /api/v1/me/email/resend", s.requireUser(s.handleResendVerification))
	mux.HandleFunc("POST /api/v1/admin/mail/test", s.requireAdmin(s.handleAdminTestMail))
	mux.HandleFunc("GET /api/v1/me", s.requireUser(s.handleMe))
	mux.HandleFunc("PATCH /api/v1/me", s.requireUser(s.handleUpdateMe))
	mux.HandleFunc("POST /api/v1/me/password", s.requireUser(s.handleChangePassword))
	mux.HandleFunc("PATCH /api/v1/me/channel", s.requireUser(s.handleUpdateMyChannel))
	mux.HandleFunc("POST /api/v1/me/channel/key", s.requireUser(s.handleResetKey))
	mux.HandleFunc("POST /api/v1/me/stream-request", s.requireUser(s.handleStreamRequest))

	// 频道与聊天
	mux.HandleFunc("GET /api/v1/channels", s.handleListChannels)
	mux.HandleFunc("GET /api/v1/categories", s.handleCategories)
	mux.HandleFunc("GET /api/v1/channels/{slug}", s.handleGetChannel)
	mux.HandleFunc("GET /api/v1/channels/{slug}/chat", s.handleChatHistory)
	mux.HandleFunc("GET /api/v1/channels/{slug}/chat/ws", s.handleChatWS)
	mux.HandleFunc("DELETE /api/v1/chat/messages/{id}", s.requireUser(s.handleDeleteChat))

	// 管理后台
	mux.HandleFunc("GET /api/v1/admin/stats", s.requireAdmin(s.handleAdminStats))
	mux.HandleFunc("GET /api/v1/admin/users", s.requireAdmin(s.handleAdminUsers))
	mux.HandleFunc("PATCH /api/v1/admin/users/{id}", s.requireAdmin(s.handleAdminPatchUser))
	mux.HandleFunc("GET /api/v1/admin/settings", s.requireAdmin(s.handleAdminGetSettings))
	mux.HandleFunc("PUT /api/v1/admin/settings", s.requireAdmin(s.handleAdminPutSettings))
	mux.HandleFunc("GET /api/v1/admin/audit", s.requireAdmin(s.handleAdminAudit))
	mux.HandleFunc("GET /api/v1/admin/invites", s.requireAdmin(s.handleAdminInvites))
	mux.HandleFunc("POST /api/v1/admin/invites", s.requireAdmin(s.handleAdminCreateInvite))
	mux.HandleFunc("DELETE /api/v1/admin/invites/{code}", s.requireAdmin(s.handleAdminDeleteInvite))
	mux.HandleFunc("POST /api/v1/admin/channels/{slug}/kick", s.requireAdmin(s.handleAdminKick))

	// 关注、通知、举报
	mux.HandleFunc("POST /api/v1/channels/{slug}/follow", s.requireUser(s.handleFollow))
	mux.HandleFunc("DELETE /api/v1/channels/{slug}/follow", s.requireUser(s.handleFollow))
	mux.HandleFunc("GET /api/v1/me/following", s.requireUser(s.handleFollowing))
	mux.HandleFunc("GET /api/v1/me/notifications", s.requireUser(s.handleNotifications))
	mux.HandleFunc("GET /api/v1/me/events", s.requireUser(s.handleEvents))
	mux.HandleFunc("POST /api/v1/me/avatar", s.requireUser(s.handleUploadAvatar))
	mux.HandleFunc("DELETE /api/v1/me/avatar", s.requireUser(s.handleDeleteAvatar))
	mux.HandleFunc("GET /avatars/{name}", s.handleAvatarFile)
	mux.HandleFunc("POST /api/v1/admin/users/{id}/password", s.requireAdmin(s.handleAdminResetPassword))
	mux.HandleFunc("POST /api/v1/me/notifications/read", s.requireUser(s.handleNotificationsRead))
	mux.HandleFunc("POST /api/v1/channels/{slug}/report", s.requireUser(s.handleReport))
	mux.HandleFunc("GET /api/v1/admin/reports", s.requireAdmin(s.handleAdminReports))
	mux.HandleFunc("POST /api/v1/admin/reports/{id}", s.requireAdmin(s.handleAdminResolveReport))

	// 频道聊天管理
	mux.HandleFunc("POST /api/v1/channels/{slug}/bans", s.requireUser(s.handleChannelBan))
	mux.HandleFunc("DELETE /api/v1/channels/{slug}/bans/{uid}", s.requireUser(s.handleChannelUnban))
	mux.HandleFunc("GET /api/v1/channels/{slug}/moderators", s.requireUser(s.handleListModerators))
	mux.HandleFunc("POST /api/v1/channels/{slug}/moderators", s.requireUser(s.handleAddModerator))
	mux.HandleFunc("DELETE /api/v1/channels/{slug}/moderators/{uid}", s.requireUser(s.handleRemoveModerator))

	// 录像回放
	mux.HandleFunc("GET /api/v1/vods", s.handleRecentVODs)
	mux.HandleFunc("GET /api/v1/channels/{slug}/vods", s.handleChannelVODs)
	mux.HandleFunc("GET /api/v1/vods/{id}", s.handleGetVOD)
	mux.HandleFunc("PATCH /api/v1/vods/{id}", s.requireUser(s.handlePatchVOD))
	mux.HandleFunc("DELETE /api/v1/vods/{id}", s.requireUser(s.handleDeleteVOD))
	mux.HandleFunc("GET /vods/{id}", s.handleVODFile)
	mux.HandleFunc("GET /thumbs/{name}", s.handleThumb)

	// API 未匹配的路径返回 JSON 404（不落到前端页面）
	mux.HandleFunc("/api/", func(w http.ResponseWriter, r *http.Request) {
		writeError(w, http.StatusNotFound, "not_found", "接口不存在")
	})

	// 媒体服务器回调
	mux.HandleFunc("POST /internal/media/hook", s.handleMediaHook)

	// 健康检查
	mux.HandleFunc("GET /healthz", s.handleHealth)

	// 播放反向代理（HLS / HTTP-FLV / WS-FLV）
	mux.HandleFunc("/live/", s.handlePlayback)
	mux.HandleFunc("POST /whep/{slug}", s.handleWHEP)
	mux.HandleFunc("DELETE /whep/{slug}/{session}", s.handleWHEPDelete)

	// 前端单页应用
	if s.Static != nil {
		mux.Handle("/", s.Static)
	}
}

// ---------- 中间件 ----------

type ctxKey int

const userKey ctxKey = 1

func (s *Server) recoverer(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		defer func() {
			if v := recover(); v != nil {
				if v == http.ErrAbortHandler {
					panic(v)
				}
				s.Log.Error("panic", "path", r.URL.Path, "err", v, "stack", string(debug.Stack()))
				writeError(w, http.StatusInternalServerError, "internal", "服务器内部错误")
			}
		}()
		next.ServeHTTP(w, r)
	})
}

// withUser 从会话 Cookie 解析当前用户放入 context（API 与 WebSocket 路径）。
func (s *Server) withUser(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if strings.HasPrefix(r.URL.Path, "/api/") {
			if c, err := r.Cookie(sessionCookie); err == nil && c.Value != "" {
				if u, err := s.Store.UserBySession(r.Context(), c.Value); err == nil && !u.Banned {
					r = r.WithContext(context.WithValue(r.Context(), userKey, u))
				}
			}
		}
		next.ServeHTTP(w, r)
	})
}

// csrf 写操作要求自定义请求头（跨站请求无法在不经 CORS 预检的情况下携带），
// 且 Origin（若有）必须与 Host 一致。
func (s *Server) csrf(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if strings.HasPrefix(r.URL.Path, "/api/") {
			switch r.Method {
			case http.MethodGet, http.MethodHead, http.MethodOptions:
			default:
				if r.Header.Get("X-Requested-With") != "livehub" {
					writeError(w, http.StatusForbidden, "csrf", "缺少 X-Requested-With 请求头")
					return
				}
				if o := r.Header.Get("Origin"); o != "" {
					u, err := url.Parse(o)
					if err != nil || !strings.EqualFold(u.Host, r.Host) {
						writeError(w, http.StatusForbidden, "csrf", "来源不匹配")
						return
					}
				}
				if !s.apiLimiter.Allow("api:" + s.clientIP(r)) {
					writeError(w, http.StatusTooManyRequests, "rate_limited", "操作过于频繁，请稍后再试")
					return
				}
			}
		}
		next.ServeHTTP(w, r)
	})
}

// securityHeaders 安全响应头：内容安全策略（CSP）、禁止被嵌入、禁止 MIME 嗅探等。
func (s *Server) securityHeaders(next http.Handler) http.Handler {
	// 边缘节点的播放请求跨域，须加入 connect-src / media-src
	extra := ""
	for _, e := range s.Cfg.EdgeURLs {
		extra += " " + e
	}
	csp := "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; " +
		"img-src 'self' data: blob:; font-src 'self' data:; " +
		"media-src 'self' blob:" + extra + "; connect-src 'self' ws: wss:" + extra + "; " +
		"worker-src 'self' blob:; object-src 'none'; base-uri 'self'; form-action 'self'; frame-ancestors 'none'"
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		h := w.Header()
		h.Set("X-Content-Type-Options", "nosniff")
		h.Set("Referrer-Policy", "strict-origin-when-cross-origin")
		h.Set("Permissions-Policy", "camera=(), microphone=(), geolocation=()")
		if !strings.HasPrefix(r.URL.Path, "/live/") && !strings.HasPrefix(r.URL.Path, "/whep/") {
			h.Set("X-Frame-Options", "DENY")
			h.Set("Content-Security-Policy", csp)
		}
		next.ServeHTTP(w, r)
	})
}

func currentUser(r *http.Request) *store.User {
	u, _ := r.Context().Value(userKey).(*store.User)
	return u
}

func (s *Server) requireUser(h func(http.ResponseWriter, *http.Request, *store.User)) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		u := currentUser(r)
		if u == nil {
			writeError(w, http.StatusUnauthorized, "unauthorized", "请先登录")
			return
		}
		h(w, r, u)
	}
}

func (s *Server) requireAdmin(h func(http.ResponseWriter, *http.Request, *store.User)) http.HandlerFunc {
	return s.requireUser(func(w http.ResponseWriter, r *http.Request, u *store.User) {
		if u.Role != store.RoleAdmin {
			writeError(w, http.StatusForbidden, "forbidden", "需要管理员权限")
			return
		}
		h(w, r, u)
	})
}

// ---------- 工具函数 ----------

type apiError struct {
	Code    string `json:"code"`
	Message string `json:"message"`
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func writeError(w http.ResponseWriter, status int, code, msg string) {
	writeJSON(w, status, map[string]apiError{"error": {Code: code, Message: msg}})
}

func (s *Server) internalError(w http.ResponseWriter, r *http.Request, err error) {
	s.Log.Error("请求处理失败", "path", r.URL.Path, "err", err)
	writeError(w, http.StatusInternalServerError, "internal", "服务器内部错误")
}

// decode 解析 JSON 请求体（上限 64KB）。
func decode(w http.ResponseWriter, r *http.Request, v any) bool {
	r.Body = http.MaxBytesReader(w, r.Body, 64<<10)
	if err := json.NewDecoder(r.Body).Decode(v); err != nil {
		writeError(w, http.StatusBadRequest, "bad_request", "请求体不是合法的 JSON")
		return false
	}
	return true
}

func (s *Server) clientIP(r *http.Request) string {
	if s.Cfg.TrustProxy {
		if xff := r.Header.Get("X-Forwarded-For"); xff != "" {
			return strings.TrimSpace(strings.Split(xff, ",")[0])
		}
		if xr := r.Header.Get("X-Real-IP"); xr != "" {
			return strings.TrimSpace(xr)
		}
	}
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		return r.RemoteAddr
	}
	return host
}

func (s *Server) secureCookie(r *http.Request) bool {
	switch s.Cfg.SecureCookies {
	case "true":
		return true
	case "false":
		return false
	}
	if strings.HasPrefix(s.Cfg.PublicURL, "https://") || r.TLS != nil {
		return true
	}
	return s.Cfg.TrustProxy && r.Header.Get("X-Forwarded-Proto") == "https"
}

func (s *Server) setSessionCookie(w http.ResponseWriter, r *http.Request, token string, maxAge int) {
	http.SetCookie(w, &http.Cookie{
		Name: sessionCookie, Value: token, Path: "/", MaxAge: maxAge,
		HttpOnly: true, Secure: s.secureCookie(r), SameSite: http.SameSiteLaxMode,
	})
}

func (s *Server) handleHealth(w http.ResponseWriter, r *http.Request) {
	mediaOK := false
	if s.Media != nil {
		ctx, cancel := context.WithTimeout(r.Context(), 2*time.Second)
		mediaOK = s.Media.Healthy(ctx)
		cancel()
	}
	if err := s.Store.DB.PingContext(r.Context()); err != nil {
		writeJSON(w, http.StatusServiceUnavailable, map[string]any{"status": "db_error"})
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"status": "ok", "media": mediaOK, "version": s.Version})
}

func isNotFound(err error) bool { return errors.Is(err, store.ErrNotFound) }
