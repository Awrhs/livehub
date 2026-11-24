// Package chat 管理直播间的 WebSocket 连接与消息广播（单实例内存实现）。
package chat

import (
	"encoding/json"
	"fmt"
	"sync"
)

// Client 一个 WebSocket 连接。Send 满时视为慢客户端并断开（不阻塞广播）。
type Client struct {
	UserID int64  // 0 = 游客
	Key    string // 观众去重键：登录用户为 "u:<ID>"，游客为 "ip:<地址>"（同一人多开标签页只计一次）
	Send   chan []byte
	// Kick 由 Hub 在客户端过慢或被管理员移出时调用（须幂等）
	Kick func(reason string)
}

func NewClient(userID int64, kick func(string)) *Client {
	return &Client{UserID: userID, Send: make(chan []byte, 64), Kick: kick}
}

type room struct {
	clients map[*Client]struct{}
}

type Hub struct {
	mu    sync.Mutex
	rooms map[int64]*room
}

func NewHub() *Hub { return &Hub{rooms: map[int64]*room{}} }

// Join 加入房间，返回当前连接数。
func (h *Hub) Join(channelID int64, c *Client) int {
	h.mu.Lock()
	defer h.mu.Unlock()
	r := h.rooms[channelID]
	if r == nil {
		r = &room{clients: map[*Client]struct{}{}}
		h.rooms[channelID] = r
	}
	r.clients[c] = struct{}{}
	return r.distinct()
}

// Leave 离开房间，返回剩余连接数。
func (h *Hub) Leave(channelID int64, c *Client) int {
	h.mu.Lock()
	defer h.mu.Unlock()
	r := h.rooms[channelID]
	if r == nil {
		return 0
	}
	delete(r.clients, c)
	n := r.distinct()
	if len(r.clients) == 0 {
		delete(h.rooms, channelID)
	}
	return n
}

// distinct 房间内去重后的观众数（调用方持锁）。
func (r *room) distinct() int {
	seen := make(map[string]struct{}, len(r.clients))
	for c := range r.clients {
		k := c.Key
		if k == "" {
			k = fmt.Sprintf("%p", c)
		}
		seen[k] = struct{}{}
	}
	return len(seen)
}

// Count 房间观众数（按观众去重）。
func (h *Hub) Count(channelID int64) int {
	h.mu.Lock()
	defer h.mu.Unlock()
	if r := h.rooms[channelID]; r != nil {
		return r.distinct()
	}
	return 0
}

// Counts 所有房间观众数快照。
func (h *Hub) Counts() map[int64]int {
	h.mu.Lock()
	defer h.mu.Unlock()
	out := make(map[int64]int, len(h.rooms))
	for id, r := range h.rooms {
		out[id] = r.distinct()
	}
	return out
}

// Broadcast 向房间所有连接发送消息（JSON 序列化 v）。
func (h *Hub) Broadcast(channelID int64, v any) {
	b, err := json.Marshal(v)
	if err != nil {
		return
	}
	h.mu.Lock()
	r := h.rooms[channelID]
	var slow []*Client
	if r != nil {
		for c := range r.clients {
			select {
			case c.Send <- b:
			default:
				slow = append(slow, c)
			}
		}
	}
	h.mu.Unlock()
	for _, c := range slow {
		c.Kick("too slow")
	}
}

// KickUser 断开某用户在某房间（channelID=0 表示所有房间）的全部连接。
func (h *Hub) KickUser(channelID, userID int64, reason string) {
	h.mu.Lock()
	var targets []*Client
	for id, r := range h.rooms {
		if channelID != 0 && id != channelID {
			continue
		}
		for c := range r.clients {
			if c.UserID == userID {
				targets = append(targets, c)
			}
		}
	}
	h.mu.Unlock()
	for _, c := range targets {
		c.Kick(reason)
	}
}
