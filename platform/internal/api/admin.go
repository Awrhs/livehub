package api

import (
	"fmt"
	"net/http"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"

	"livehub/internal/store"
)

func (s *Server) handleAdminStats(w http.ResponseWriter, r *http.Request, _ *store.User) {
	ctx := r.Context()
	users, err := s.Store.CountUsers(ctx)
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	live, err := s.Store.LiveChannels(ctx)
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	_, pendingTotal, err := s.Store.ListUsers(ctx, store.UserFilter{StreamStatus: store.StreamPending, Limit: 1})
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	viewers := 0
	for _, n := range s.Hub.Counts() {
		viewers += n
	}
	mediaOK := false
	if s.Media != nil {
		mediaOK = s.Media.Healthy(ctx)
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"users": users, "live_channels": len(live), "viewers": viewers,
		"pending_requests": pendingTotal, "media_healthy": mediaOK, "media_enabled": s.Media != nil,
		"open_reports": s.Store.OpenReportCount(ctx),
	})
}

func (s *Server) handleAdminUsers(w http.ResponseWriter, r *http.Request, _ *store.User) {
	q := r.URL.Query()
	f := store.UserFilter{Query: q.Get("q"), StreamStatus: q.Get("stream_status"), Role: q.Get("role")}
	f.Limit, _ = strconv.Atoi(q.Get("limit"))
	f.Offset, _ = strconv.Atoi(q.Get("offset"))
	if b := q.Get("banned"); b != "" {
		v := b == "1" || b == "true"
		f.Banned = &v
	}
	users, total, err := s.Store.ListUsers(r.Context(), f)
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	if users == nil {
		users = []*store.User{}
	}
	writeJSON(w, http.StatusOK, map[string]any{"users": users, "total": total})
}

func (s *Server) handleAdminPatchUser(w http.ResponseWriter, r *http.Request, admin *store.User) {
	ctx := r.Context()
	id, err := strconv.ParseInt(r.PathValue("id"), 10, 64)
	if err != nil {
		writeError(w, http.StatusBadRequest, "bad_request", "用户 ID 不合法")
		return
	}
	target, err := s.Store.UserByID(ctx, id)
	if isNotFound(err) {
		writeError(w, http.StatusNotFound, "not_found", "用户不存在")
		return
	}
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	var body struct {
		Role         *string `json:"role"`
		StreamStatus *string `json:"stream_status"`
		Banned       *bool   `json:"banned"`
		BanReason    *string `json:"ban_reason"`
	}
	if !decode(w, r, &body) {
		return
	}
	if body.Role != nil {
		switch *body.Role {
		case store.RoleAdmin, store.RoleModerator, store.RoleUser:
		default:
			writeError(w, http.StatusBadRequest, "bad_request", "角色不合法")
			return
		}
	}
	if body.StreamStatus != nil {
		switch *body.StreamStatus {
		case store.StreamNone, store.StreamPending, store.StreamApproved, store.StreamRejected:
		default:
			writeError(w, http.StatusBadRequest, "bad_request", "开播状态不合法")
			return
		}
	}
	if body.BanReason != nil && utf8.RuneCountInString(*body.BanReason) > 200 {
		writeError(w, http.StatusBadRequest, "bad_request", "封禁原因不能超过 200 字")
		return
	}
	// 不能封禁自己或撤销自己的管理员身份（避免站点失去管理员）
	if target.ID == admin.ID {
		if (body.Banned != nil && *body.Banned) || (body.Role != nil && *body.Role != store.RoleAdmin) {
			writeError(w, http.StatusBadRequest, "self_demote", "不能封禁自己或撤销自己的管理员身份")
			return
		}
	}
	if err := s.Store.PatchUser(ctx, id, store.UserPatch{
		Role: body.Role, StreamStatus: body.StreamStatus, Banned: body.Banned, BanReason: body.BanReason,
	}); err != nil {
		s.internalError(w, r, err)
		return
	}
	var changes []string
	if body.Role != nil && *body.Role != target.Role {
		changes = append(changes, "role="+*body.Role)
	}
	if body.StreamStatus != nil && *body.StreamStatus != target.StreamStatus {
		changes = append(changes, "stream_status="+*body.StreamStatus)
		switch *body.StreamStatus {
		case store.StreamApproved:
			s.notifyUser(r, target.ID, "stream_approved", nil, "")
		case store.StreamRejected:
			s.notifyUser(r, target.ID, "stream_rejected", nil, "")
		}
	}
	if body.Banned != nil && *body.Banned != target.Banned {
		changes = append(changes, fmt.Sprintf("banned=%v", *body.Banned))
	}
	if len(changes) > 0 {
		s.Store.Audit(ctx, admin.ID, "user.update", target.Username, strings.Join(changes, " "))
	}
	// 封禁或撤销开播权限：断开推流；封禁还要注销会话、踢出聊天
	banned := body.Banned != nil && *body.Banned && !target.Banned
	revoked := body.StreamStatus != nil && *body.StreamStatus != store.StreamApproved &&
		target.StreamStatus == store.StreamApproved
	if banned || revoked {
		if ch, err := s.Store.ChannelByUser(ctx, id); err == nil && ch.IsLive {
			updated, _ := s.Store.UserByID(ctx, id)
			if updated == nil || !s.canStream(r, updated) {
				s.endStream(ctx, ch)
			}
		}
	}
	if banned {
		_ = s.Store.DeleteUserSessions(ctx, id, "")
		s.Hub.KickUser(0, id, "banned")
	}
	u, err := s.Store.UserByID(ctx, id)
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, u)
}

func (s *Server) handleAdminGetSettings(w http.ResponseWriter, r *http.Request, _ *store.User) {
	set, err := s.Store.AllPublicSettings(r.Context())
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, set)
}

func (s *Server) handleAdminPutSettings(w http.ResponseWriter, r *http.Request, admin *store.User) {
	var body map[string]string
	if !decode(w, r, &body) {
		return
	}
	allowed := map[string]bool{}
	for _, k := range store.PublicSettingKeys {
		allowed[k] = true
	}
	for k, v := range body {
		if !allowed[k] {
			writeError(w, http.StatusBadRequest, "bad_request", "未知设置项："+k)
			return
		}
		if msg := validateSetting(k, v); msg != "" {
			writeError(w, http.StatusBadRequest, "bad_request", msg)
			return
		}
	}
	var changed []string
	for k, v := range body {
		if err := s.Store.SetSetting(r.Context(), k, strings.TrimSpace(v)); err != nil {
			s.internalError(w, r, err)
			return
		}
		changed = append(changed, k)
	}
	if len(changed) > 0 {
		s.Store.Audit(r.Context(), admin.ID, "settings.update", "", strings.Join(changed, ","))
	}
	s.handleAdminGetSettings(w, r, admin)
}

func validateSetting(k, v string) string {
	v = strings.TrimSpace(v)
	switch k {
	case store.SettingSiteName:
		if n := utf8.RuneCountInString(v); n < 1 || n > 40 {
			return "站点名称长度须为 1-40 字"
		}
	case store.SettingRegistrationMode:
		if v != "open" && v != "invite" && v != "closed" {
			return "注册模式只能是 open / invite / closed"
		}
	case store.SettingStreamApproval, store.SettingRecordingEnabled, store.SettingTranscodeEnabled:
		if v != "true" && v != "false" {
			return k + " 只能是 true / false"
		}
	case store.SettingChatSlowModeSec:
		n, err := strconv.Atoi(v)
		if err != nil || n < 0 || n > 3600 {
			return "慢速模式秒数须为 0-3600"
		}
	case store.SettingAnnouncement:
		if utf8.RuneCountInString(v) > 500 {
			return "公告不能超过 500 字"
		}
	case store.SettingVODRetentionDays:
		n, err := strconv.Atoi(v)
		if err != nil || n < 0 || n > 3650 {
			return "录像保留天数须为 0-3650"
		}
	case store.SettingChatBlockedWords:
		if utf8.RuneCountInString(v) > 10000 {
			return "屏蔽词列表过长"
		}
	}
	return ""
}

func (s *Server) handleAdminAudit(w http.ResponseWriter, r *http.Request, _ *store.User) {
	limit, _ := strconv.Atoi(r.URL.Query().Get("limit"))
	offset, _ := strconv.Atoi(r.URL.Query().Get("offset"))
	entries, err := s.Store.ListAudit(r.Context(), limit, offset)
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"entries": entries})
}

func (s *Server) handleAdminInvites(w http.ResponseWriter, r *http.Request, _ *store.User) {
	inv, err := s.Store.ListInvites(r.Context())
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"invites": inv})
}

func (s *Server) handleAdminCreateInvite(w http.ResponseWriter, r *http.Request, admin *store.User) {
	var body struct {
		ExpiresInHours int `json:"expires_in_hours"` // 0 = 永不过期
	}
	if !decode(w, r, &body) {
		return
	}
	var exp *int64
	if body.ExpiresInHours > 0 {
		t := time.Now().Add(time.Duration(body.ExpiresInHours) * time.Hour).Unix()
		exp = &t
	}
	inv, err := s.Store.CreateInvite(r.Context(), admin.ID, exp)
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	s.Store.Audit(r.Context(), admin.ID, "invite.create", inv.Code, "")
	writeJSON(w, http.StatusCreated, inv)
}

func (s *Server) handleAdminDeleteInvite(w http.ResponseWriter, r *http.Request, admin *store.User) {
	code := r.PathValue("code")
	if err := s.Store.DeleteInvite(r.Context(), code); err != nil {
		if isNotFound(err) {
			writeError(w, http.StatusNotFound, "not_found", "邀请码不存在或已被使用")
			return
		}
		s.internalError(w, r, err)
		return
	}
	s.Store.Audit(r.Context(), admin.ID, "invite.delete", code, "")
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

func (s *Server) handleAdminKick(w http.ResponseWriter, r *http.Request, admin *store.User) {
	ch, err := s.Store.ChannelBySlug(r.Context(), r.PathValue("slug"))
	if isNotFound(err) {
		writeError(w, http.StatusNotFound, "not_found", "频道不存在")
		return
	}
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	s.endStream(r.Context(), ch)
	s.Store.Audit(r.Context(), admin.ID, "channel.kick", ch.Slug, "")
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}
