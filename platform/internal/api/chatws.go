package api

import (
	"context"
	"encoding/json"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode/utf8"

	"github.com/coder/websocket"

	"livehub/internal/chat"
	"livehub/internal/store"
)

const (
	chatMaxRunes   = 300
	chatHistoryLen = 50
)

type chatMe struct {
	ID          int64  `json:"id"`
	Username    string `json:"username"`
	DisplayName string `json:"display_name"`
	Role        string `json:"role"`
	CanModerate bool   `json:"can_moderate"`
}

// canModerate 管理员、站务与频道主可管理该频道聊天。
func canModerate(u *store.User, ch *store.Channel) bool {
	return u != nil && (u.IsStaff() || u.ID == ch.UserID)
}

func (s *Server) handleChatHistory(w http.ResponseWriter, r *http.Request) {
	ch := s.channelFromPath(w, r)
	if ch == nil {
		return
	}
	msgs, err := s.Store.RecentChat(r.Context(), ch.ID, chatHistoryLen)
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	if msgs == nil {
		msgs = []*store.ChatMessage{}
	}
	writeJSON(w, http.StatusOK, map[string]any{"messages": msgs})
}

func (s *Server) handleChatWS(w http.ResponseWriter, r *http.Request) {
	ch := s.channelFromPath(w, r)
	if ch == nil {
		return
	}
	u := currentUser(r)
	conn, err := websocket.Accept(w, r, nil) // 默认校验 Origin 与 Host 一致
	if err != nil {
		return
	}
	conn.SetReadLimit(4096)
	ctx, cancel := context.WithCancel(context.WithoutCancel(r.Context()))
	defer cancel()

	var once sync.Once
	var uid int64
	if u != nil {
		uid = u.ID
	}
	client := chat.NewClient(uid, func(reason string) {
		// 可能在广播协程中调用：关闭握手会等待对端，放到独立协程避免阻塞广播
		once.Do(func() {
			cancel()
			go conn.Close(websocket.StatusPolicyViolation, reason)
		})
	})
	viewers := s.Hub.Join(ch.ID, client)
	defer s.Hub.Leave(ch.ID, client)

	history, _ := s.Store.RecentChat(ctx, ch.ID, chatHistoryLen)
	if history == nil {
		history = []*store.ChatMessage{}
	}
	hello := map[string]any{
		"type": "hello", "viewers": viewers, "is_live": ch.IsLive, "history": history,
		"slow_mode": s.Store.SettingInt(ctx, store.SettingChatSlowModeSec),
	}
	if u != nil {
		hello["me"] = chatMe{ID: u.ID, Username: u.Username, DisplayName: u.DisplayName, Role: u.Role,
			CanModerate: canModerate(u, ch)}
	}
	if b, err := json.Marshal(hello); err == nil {
		client.Send <- b
	}

	// 写协程：转发广播消息并定期 ping
	go func() {
		ping := time.NewTicker(30 * time.Second)
		defer ping.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case b := <-client.Send:
				wctx, wcancel := context.WithTimeout(ctx, 10*time.Second)
				err := conn.Write(wctx, websocket.MessageText, b)
				wcancel()
				if err != nil {
					cancel()
					return
				}
			case <-ping.C:
				pctx, pcancel := context.WithTimeout(ctx, 10*time.Second)
				err := conn.Ping(pctx)
				pcancel()
				if err != nil {
					cancel()
					return
				}
			}
		}
	}()

	for {
		_, data, err := conn.Read(ctx)
		if err != nil {
			break
		}
		var in struct {
			Type string `json:"type"`
			Body string `json:"body"`
		}
		if json.Unmarshal(data, &in) != nil || in.Type != "send" {
			continue
		}
		if code, msg := s.postChat(ctx, ch, u, in.Body); code != "" {
			if b, err := json.Marshal(map[string]string{"type": "error", "code": code, "message": msg}); err == nil {
				select {
				case client.Send <- b:
				default:
				}
			}
		}
	}
	_ = conn.Close(websocket.StatusNormalClosure, "")
}

// postChat 校验并发布一条聊天消息；失败返回错误码与提示。
func (s *Server) postChat(ctx context.Context, ch *store.Channel, u *store.User, body string) (string, string) {
	if u == nil {
		return "unauthorized", "登录后才能发言"
	}
	// 会话期间可能被封禁：以数据库最新状态为准
	fresh, err := s.Store.UserByID(ctx, u.ID)
	if err != nil || fresh.Banned {
		return "banned", "账号已被封禁"
	}
	body = strings.TrimSpace(body)
	n := utf8.RuneCountInString(body)
	if n == 0 {
		return "empty", "消息不能为空"
	}
	if n > chatMaxRunes {
		return "too_long", "消息不能超过 " + strconv.Itoa(chatMaxRunes) + " 字"
	}
	mod := canModerate(fresh, ch)
	if !mod {
		if !s.chatLimiter.Allow(strconv.FormatInt(u.ID, 10)) {
			return "rate_limited", "发言太快了"
		}
		if slow := s.Store.SettingInt(ctx, store.SettingChatSlowModeSec); slow > 0 {
			k := [2]int64{ch.ID, u.ID}
			s.slowMu.Lock()
			last, ok := s.lastSent[k]
			if ok && time.Since(last) < time.Duration(slow)*time.Second {
				s.slowMu.Unlock()
				return "slow_mode", "慢速模式：每 " + strconv.Itoa(slow) + " 秒只能发言一次"
			}
			s.lastSent[k] = time.Now()
			s.slowMu.Unlock()
		}
		if word := s.blockedWord(ctx, body); word != "" {
			return "blocked_word", "消息包含被屏蔽的词语"
		}
	}
	m, err := s.Store.InsertChat(ctx, ch.ID, store.ChatUser{
		ID: fresh.ID, Username: fresh.Username, DisplayName: fresh.DisplayName, Role: fresh.Role,
	}, body)
	if err != nil {
		s.Log.Error("保存聊天失败", "err", err)
		return "internal", "发送失败"
	}
	s.Hub.Broadcast(ch.ID, map[string]any{"type": "msg", "message": m})
	return "", ""
}

// blockedWord 返回消息命中的屏蔽词（不区分大小写），未命中返回空串。
func (s *Server) blockedWord(ctx context.Context, body string) string {
	words, _ := s.Store.Setting(ctx, store.SettingChatBlockedWords)
	if words == "" {
		return ""
	}
	lb := strings.ToLower(body)
	for _, w := range strings.Split(words, "\n") {
		w = strings.ToLower(strings.TrimSpace(w))
		if w != "" && strings.Contains(lb, w) {
			return w
		}
	}
	return ""
}

func (s *Server) handleDeleteChat(w http.ResponseWriter, r *http.Request, u *store.User) {
	id, err := strconv.ParseInt(r.PathValue("id"), 10, 64)
	if err != nil {
		writeError(w, http.StatusBadRequest, "bad_request", "消息 ID 不合法")
		return
	}
	var chID int64
	if err := s.Store.DB.QueryRowContext(r.Context(), `SELECT channel_id FROM chat_messages WHERE id = ?`, id).
		Scan(&chID); err != nil {
		writeError(w, http.StatusNotFound, "not_found", "消息不存在")
		return
	}
	ch, err := s.Store.ChannelByID(r.Context(), chID)
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	if !canModerate(u, ch) {
		writeError(w, http.StatusForbidden, "forbidden", "无权删除该消息")
		return
	}
	if _, err := s.Store.DeleteChat(r.Context(), id); err != nil {
		s.internalError(w, r, err)
		return
	}
	s.Hub.Broadcast(ch.ID, map[string]any{"type": "delete", "id": id})
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}
