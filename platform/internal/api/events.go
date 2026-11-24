package api

import (
	"encoding/json"
	"fmt"
	"net/http"
	"sync"
	"time"

	"livehub/internal/store"
)

// notifier 用户级实时事件（SSE）：新通知到达时推送，前端据此即时刷新并弹出桌面通知。
type notifier struct {
	mu   sync.Mutex
	subs map[int64]map[chan []byte]struct{}
}

func newNotifier() *notifier { return &notifier{subs: map[int64]map[chan []byte]struct{}{}} }

func (n *notifier) subscribe(uid int64) chan []byte {
	ch := make(chan []byte, 16)
	n.mu.Lock()
	defer n.mu.Unlock()
	if n.subs[uid] == nil {
		n.subs[uid] = map[chan []byte]struct{}{}
	}
	n.subs[uid][ch] = struct{}{}
	return ch
}

func (n *notifier) unsubscribe(uid int64, ch chan []byte) {
	n.mu.Lock()
	defer n.mu.Unlock()
	delete(n.subs[uid], ch)
	if len(n.subs[uid]) == 0 {
		delete(n.subs, uid)
	}
}

// publish 非阻塞推送；接收方过慢时丢弃（前端收到任一事件都会整体刷新通知列表）。
func (n *notifier) publish(uid int64, v any) {
	b, err := json.Marshal(v)
	if err != nil {
		return
	}
	n.mu.Lock()
	defer n.mu.Unlock()
	for ch := range n.subs[uid] {
		select {
		case ch <- b:
		default:
		}
	}
}

// handleEvents GET /api/v1/me/events：Server-Sent Events 长连接。
func (s *Server) handleEvents(w http.ResponseWriter, r *http.Request, u *store.User) {
	fl, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "streaming unsupported", http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("X-Accel-Buffering", "no") // Nginx 不缓冲
	w.WriteHeader(http.StatusOK)
	fmt.Fprint(w, ": ok\n\n")
	fl.Flush()
	ch := s.events.subscribe(u.ID)
	defer s.events.unsubscribe(u.ID, ch)
	hb := time.NewTicker(25 * time.Second)
	defer hb.Stop()
	for {
		select {
		case <-r.Context().Done():
			return
		case b := <-ch:
			fmt.Fprintf(w, "data: %s\n\n", b)
			fl.Flush()
		case <-hb.C:
			fmt.Fprint(w, ": ping\n\n") // 心跳，防止代理断开空闲连接
			fl.Flush()
		}
	}
}

// notifyUser 写入站内通知并实时推送。
func (s *Server) notifyUser(r *http.Request, uid int64, kind string, channelID *int64, body string) {
	if err := s.Store.Notify(r.Context(), uid, kind, channelID, body); err == nil {
		s.events.publish(uid, map[string]string{"type": "notification", "kind": kind})
	}
}
