package api

import (
	"net/http"
	"strconv"
	"strings"
	"unicode/utf8"

	"livehub/internal/store"
)

type playback struct {
	HLS    string `json:"hls"`
	FLV    string `json:"flv"`
	ABR    string `json:"abr,omitempty"`    // 多清晰度主播放列表（开启转码且就绪时）
	Edge   string `json:"edge,omitempty"`   // 分发该频道的边缘节点
	WebRTC string `json:"webrtc,omitempty"` // WHEP 地址（开启 WebRTC 超低延迟播放时）
}

type channelView struct {
	*store.Channel
	Viewers   int       `json:"viewers"`
	Playback  *playback `json:"playback,omitempty"`
	Thumbnail string    `json:"thumbnail,omitempty"`
}

func (s *Server) channelView(c *store.Channel) channelView {
	v := channelView{Channel: c, Viewers: s.Hub.Count(c.ID)}
	if c.IsLive {
		base := s.pickEdge(c.Slug) // 有健康边缘节点时由边缘分发（多清晰度仍由源站提供）
		v.Playback = &playback{HLS: base + "/live/" + c.Slug + ".m3u8", FLV: base + "/live/" + c.Slug + ".flv",
			ABR: s.abrURL(c.Slug), Edge: base}
		if s.Cfg.WebRTCCandidateIP != "" {
			v.Playback.WebRTC = "/whep/" + c.Slug
		}
		v.Thumbnail = s.thumbURL("live_" + c.Slug + ".jpg")
	}
	return v
}

func (s *Server) handleListChannels(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	limit, _ := strconv.Atoi(q.Get("limit"))
	offset, _ := strconv.Atoi(q.Get("offset"))
	list, err := s.Store.ListChannels(r.Context(), store.ChannelFilter{
		LiveOnly: q.Get("live") == "1" || q.Get("live") == "true",
		Query:    q.Get("q"),
		Category: q.Get("category"),
		Limit:    limit,
		Offset:   offset,
	})
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	out := make([]channelView, 0, len(list))
	for _, c := range list {
		out = append(out, s.channelView(c))
	}
	writeJSON(w, http.StatusOK, map[string]any{"channels": out})
}

func (s *Server) handleCategories(w http.ResponseWriter, r *http.Request) {
	cats, err := s.Store.Categories(r.Context())
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"categories": cats})
}

// channelFromPath 按路径参数 slug 查找频道；被封禁主播的频道视为不存在。
func (s *Server) channelFromPath(w http.ResponseWriter, r *http.Request) *store.Channel {
	c, err := s.Store.ChannelBySlug(r.Context(), r.PathValue("slug"))
	if isNotFound(err) || (err == nil && c.OwnerBanned) {
		writeError(w, http.StatusNotFound, "not_found", "频道不存在")
		return nil
	}
	if err != nil {
		s.internalError(w, r, err)
		return nil
	}
	return c
}

func (s *Server) handleGetChannel(w http.ResponseWriter, r *http.Request) {
	c := s.channelFromPath(w, r)
	if c == nil {
		return
	}
	owner, err := s.Store.UserByID(r.Context(), c.UserID)
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	following := false
	if u := currentUser(r); u != nil {
		following = s.Store.IsFollowing(r.Context(), u.ID, c.ID)
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"channel":   s.channelView(c),
		"followers": s.Store.FollowerCount(r.Context(), c.ID),
		"following": following,
		"owner": map[string]any{
			"id": owner.ID, "username": owner.Username, "display_name": owner.DisplayName,
			"bio": owner.Bio, "role": owner.Role, "created_at": owner.CreatedAt,
		},
	})
}

func (s *Server) handleUpdateMyChannel(w http.ResponseWriter, r *http.Request, u *store.User) {
	ch, err := s.Store.ChannelByUser(r.Context(), u.ID)
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	var body struct {
		Title         *string `json:"title"`
		Category      *string `json:"category"`
		Description   *string `json:"description"`
		RecordEnabled *bool   `json:"record_enabled"`
	}
	if !decode(w, r, &body) {
		return
	}
	title, cat, desc, rec := ch.Title, ch.Category, ch.Description, ch.RecordEnabled
	if body.Title != nil {
		title = strings.TrimSpace(*body.Title)
		if n := utf8.RuneCountInString(title); n < 1 || n > 80 {
			writeError(w, http.StatusBadRequest, "bad_request", "标题长度须为 1-80 字")
			return
		}
	}
	if body.Category != nil {
		cat = strings.TrimSpace(*body.Category)
		if utf8.RuneCountInString(cat) > 32 {
			writeError(w, http.StatusBadRequest, "bad_request", "分类不能超过 32 字")
			return
		}
	}
	if body.Description != nil {
		desc = strings.TrimSpace(*body.Description)
		if utf8.RuneCountInString(desc) > 2000 {
			writeError(w, http.StatusBadRequest, "bad_request", "简介不能超过 2000 字")
			return
		}
	}
	if body.RecordEnabled != nil {
		rec = *body.RecordEnabled
	}
	if err := s.Store.UpdateChannel(r.Context(), ch.ID, title, cat, desc, rec); err != nil {
		s.internalError(w, r, err)
		return
	}
	// 通知直播间（标题/分类实时更新）
	s.Hub.Broadcast(ch.ID, map[string]any{"type": "channel", "title": title, "category": cat})
	s.handleMe(w, r, u)
}

func (s *Server) handleResetKey(w http.ResponseWriter, r *http.Request, u *store.User) {
	ch, err := s.Store.ChannelByUser(r.Context(), u.ID)
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	if _, err := s.Store.ResetStreamKey(r.Context(), ch.ID); err != nil {
		s.internalError(w, r, err)
		return
	}
	s.Store.Audit(r.Context(), u.ID, "channel.reset_key", ch.Slug, "")
	s.handleMe(w, r, u)
}
