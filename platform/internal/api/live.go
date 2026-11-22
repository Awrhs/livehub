package api

import (
	"context"
	"crypto/subtle"
	"encoding/json"
	"net"
	"net/http"
	"net/url"
	"regexp"
	"strings"
	"sync"
	"time"

	"livehub/internal/store"
)

// ---------- 媒体服务器回调 ----------

type hookEvent struct {
	Action string          `json:"action"`
	App    string          `json:"app"`
	Stream json.RawMessage `json:"stream"` // 媒体服务器会把纯数字流名序列化为数字
	Param  string          `json:"param"`
	Vhost  string          `json:"vhost"`
}

func (e *hookEvent) streamName() string {
	var s string
	if err := json.Unmarshal(e.Stream, &s); err == nil {
		return s
	}
	return strings.TrimSpace(string(e.Stream))
}

type hookReply struct {
	Code int    `json:"code"`
	Msg  string `json:"msg,omitempty"`
}

func (s *Server) handleMediaHook(w http.ResponseWriter, r *http.Request) {
	// 内嵌模式下媒体服务器与平台同机：只接受本机回调（密钥之外的纵深防御）
	if s.Cfg.Embedded() {
		host, _, _ := net.SplitHostPort(r.RemoteAddr)
		if ip := net.ParseIP(host); ip == nil || !ip.IsLoopback() {
			writeJSON(w, http.StatusForbidden, hookReply{Code: 403, Msg: "forbidden"})
			return
		}
	}
	secret := r.URL.Query().Get("secret")
	if s.HookSecret == "" || subtle.ConstantTimeCompare([]byte(secret), []byte(s.HookSecret)) != 1 {
		writeJSON(w, http.StatusForbidden, hookReply{Code: 403, Msg: "bad secret"})
		return
	}
	var ev hookEvent
	r.Body = http.MaxBytesReader(w, r.Body, 16<<10)
	if err := json.NewDecoder(r.Body).Decode(&ev); err != nil {
		writeJSON(w, http.StatusBadRequest, hookReply{Code: 400, Msg: "bad body"})
		return
	}
	name := ev.streamName()
	switch ev.Action {
	case "on_publish":
		// 媒体服务器内部推流（多清晰度转码结果回推）：校验内部令牌，不改变直播状态
		if s.isInternalPublish(r.Context(), name, ev.Param) {
			writeJSON(w, http.StatusOK, hookReply{Code: 0})
			return
		}
		ch, reason := s.authorizePublish(r, ev.App, name, ev.Param)
		if ch == nil {
			s.Log.Info("拒绝推流", "stream", name, "reason", reason)
			writeJSON(w, http.StatusOK, hookReply{Code: 403, Msg: reason})
			return
		}
		s.Log.Info("允许推流", "stream", name)
		s.goLive(context.WithoutCancel(r.Context()), ch)
		writeJSON(w, http.StatusOK, hookReply{Code: 0})
	case "on_unpublish":
		if ch, err := s.Store.ChannelBySlug(r.Context(), name); err == nil && ch.Slug == name {
			s.goOffline(context.WithoutCancel(r.Context()), ch)
		}
		writeJSON(w, http.StatusOK, hookReply{Code: 0})
	default:
		writeJSON(w, http.StatusOK, hookReply{Code: 0})
	}
}

var variantRe = regexp.MustCompile(`^(.+)_([0-9]+p)$`)

// isInternalPublish 是否为内部推流：<频道>_<档位>，携带正确的内部令牌，且频道正在直播。
func (s *Server) isInternalPublish(ctx context.Context, name, param string) bool {
	if s.InternalToken == "" {
		return false
	}
	m := variantRe.FindStringSubmatch(name)
	if m == nil {
		return false
	}
	q, _ := url.ParseQuery(strings.TrimPrefix(param, "?"))
	tok := q.Get("internal")
	if tok == "" || subtle.ConstantTimeCompare([]byte(tok), []byte(s.InternalToken)) != 1 {
		return false
	}
	ch, err := s.Store.ChannelBySlug(ctx, m[1])
	return err == nil && ch.Slug == m[1] && ch.IsLive
}

// authorizePublish 校验推流：频道存在（大小写一致）、主播有开播权限、密钥正确。
func (s *Server) authorizePublish(r *http.Request, app, name, param string) (*store.Channel, string) {
	if app != "live" {
		return nil, "app not allowed"
	}
	ch, err := s.Store.ChannelBySlug(r.Context(), name)
	if err != nil {
		return nil, "unknown channel"
	}
	// 流名须与频道名完全一致：播放地址按频道名生成，大小写不一致会导致无法播放
	if ch.Slug != name {
		return nil, "stream name must be " + ch.Slug
	}
	owner, err := s.Store.UserByID(r.Context(), ch.UserID)
	if err != nil {
		return nil, "unknown owner"
	}
	if owner.Banned {
		return nil, "banned"
	}
	if !s.canStream(r, owner) {
		return nil, "streaming not approved"
	}
	q, _ := url.ParseQuery(strings.TrimPrefix(param, "?"))
	key := q.Get("key")
	if key == "" || subtle.ConstantTimeCompare([]byte(key), []byte(ch.StreamKey)) != 1 {
		return nil, "invalid stream key"
	}
	return ch, ""
}

func (s *Server) channelMutex(id int64) *sync.Mutex {
	m, _ := s.chanLock.LoadOrStore(id, &sync.Mutex{})
	return m.(*sync.Mutex)
}

// goLive 标记开播并通知直播间。
func (s *Server) goLive(ctx context.Context, ch *store.Channel) {
	mu := s.channelMutex(ch.ID)
	mu.Lock()
	defer mu.Unlock()
	changed, err := s.Store.SetLive(ctx, ch.ID)
	if err != nil {
		s.Log.Error("标记开播失败", "channel", ch.Slug, "err", err)
		return
	}
	s.liveMu.Lock()
	delete(s.missing, ch.ID)
	s.liveMu.Unlock()
	if changed {
		s.Log.Info("开播", "channel", ch.Slug)
		s.Hub.Broadcast(ch.ID, map[string]any{"type": "live", "is_live": true})
		go s.afterLive(ch)
	}
}

// goOffline 标记下播并通知直播间。
func (s *Server) goOffline(ctx context.Context, ch *store.Channel) {
	mu := s.channelMutex(ch.ID)
	mu.Lock()
	defer mu.Unlock()
	sid, changed, err := s.Store.SetOffline(ctx, ch.ID)
	if err != nil {
		s.Log.Error("标记下播失败", "channel", ch.Slug, "err", err)
		return
	}
	if changed {
		s.Log.Info("下播", "channel", ch.Slug)
		s.Hub.Broadcast(ch.ID, map[string]any{"type": "live", "is_live": false})
		go s.afterOffline(ch, sid)
	}
}

// endStream 主动断开推流并标记下播（封禁、管理员断流）。
func (s *Server) endStream(ctx context.Context, ch *store.Channel) {
	if s.Media != nil {
		if _, err := s.Media.Kick(ctx, ch.Slug); err != nil {
			s.Log.Warn("断开推流失败", "channel", ch.Slug, "err", err)
		}
	}
	s.goOffline(ctx, ch)
}

// ---------- 对账与后台任务 ----------

// RunBackground 周期任务：直播状态对账、观众数推送、峰值统计、清理过期数据。阻塞直到 ctx 取消。
func (s *Server) RunBackground(ctx context.Context) {
	reconcile := time.NewTicker(10 * time.Second)
	thumbs := time.NewTicker(60 * time.Second)
	defer thumbs.Stop()
	viewers := time.NewTicker(5 * time.Second)
	cleanup := time.NewTicker(time.Hour)
	defer reconcile.Stop()
	defer viewers.Stop()
	defer cleanup.Stop()
	last := map[int64]int{}
	go s.runEdgeChecks(ctx)
	for {
		select {
		case <-ctx.Done():
			return
		case <-reconcile.C:
			s.reconcile(ctx)
		case <-viewers.C:
			last = s.pushViewerCounts(ctx, last)
		case <-thumbs.C:
			s.refreshThumbs(ctx)
		case <-cleanup.C:
			_ = s.Store.PurgeExpiredSessions(ctx)
			_ = s.Store.PruneChat(ctx, 1000)
			_ = s.Store.PruneNotifications(ctx)
			s.pruneVODs(ctx)
		}
	}
}

// reconcile 以媒体服务器的实际推流为准修正直播状态（回调可能丢失：平台重启、网络抖动）。
func (s *Server) reconcile(ctx context.Context) {
	if s.Media == nil {
		return
	}
	cctx, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()
	streams, err := s.Media.ListStreams(cctx)
	if err != nil {
		// 媒体服务器不可达时不改状态；若其已重启，由 Supervisor.OnRestart 统一重置
		s.Log.Debug("对账：获取流列表失败", "err", err)
		return
	}
	active := map[string]bool{}
	for _, st := range streams {
		if st.Publisher {
			active[st.Key] = true
		}
	}
	s.updateABRReady(active)
	live, err := s.Store.LiveChannels(ctx)
	if err != nil {
		return
	}
	for _, ch := range live {
		if active[ch.Slug] {
			s.liveMu.Lock()
			delete(s.missing, ch.ID)
			s.liveMu.Unlock()
			continue
		}
		// 连续两次缺席才判定下播（避免与刚开播的回调竞争）
		s.liveMu.Lock()
		s.missing[ch.ID]++
		n := s.missing[ch.ID]
		s.liveMu.Unlock()
		if n >= 2 {
			s.Log.Info("对账：频道已无推流，标记下播", "channel", ch.Slug)
			s.goOffline(ctx, ch)
		}
	}
	for key := range active {
		ch, err := s.Store.ChannelBySlug(ctx, key)
		if err != nil || ch.Slug != key || ch.IsLive {
			continue
		}
		owner, err := s.Store.UserByID(ctx, ch.UserID)
		if err != nil {
			continue
		}
		if owner.Banned {
			s.endStream(ctx, ch)
			continue
		}
		s.Log.Info("对账：发现未登记的推流，标记开播", "channel", ch.Slug)
		s.goLive(ctx, ch)
	}
}

// pushViewerCounts 观众数变化时推送到直播间，并更新场次峰值。
func (s *Server) pushViewerCounts(ctx context.Context, last map[int64]int) map[int64]int {
	counts := s.Hub.Counts()
	for id, n := range counts {
		if last[id] != n {
			s.Hub.Broadcast(id, map[string]any{"type": "viewers", "count": n})
		}
	}
	live, err := s.Store.LiveChannels(ctx)
	if err == nil {
		for _, ch := range live {
			if ch.CurrentSessionID != nil {
				_ = s.Store.UpdatePeakViewers(ctx, *ch.CurrentSessionID, counts[ch.ID])
			}
		}
	}
	return counts
}

// OnMediaRestart 媒体服务器（重新）启动：此前推流全部断开，重置所有直播状态。
func (s *Server) OnMediaRestart() {
	ctx := context.Background()
	live, err := s.Store.LiveChannels(ctx)
	if err != nil {
		return
	}
	for _, ch := range live {
		s.goOffline(ctx, ch)
	}
}
