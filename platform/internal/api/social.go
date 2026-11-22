package api

import (
	"errors"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"

	"livehub/internal/store"
)

// ---------- 关注 ----------

func (s *Server) handleFollow(w http.ResponseWriter, r *http.Request, u *store.User) {
	ch := s.channelFromPath(w, r)
	if ch == nil {
		return
	}
	if ch.UserID == u.ID {
		writeError(w, http.StatusBadRequest, "bad_request", "不能关注自己")
		return
	}
	var err error
	if r.Method == http.MethodDelete {
		err = s.Store.Unfollow(r.Context(), u.ID, ch.ID)
	} else {
		err = s.Store.Follow(r.Context(), u.ID, ch.ID)
	}
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"following": r.Method != http.MethodDelete, "followers": s.Store.FollowerCount(r.Context(), ch.ID),
	})
}

func (s *Server) handleFollowing(w http.ResponseWriter, r *http.Request, u *store.User) {
	list, err := s.Store.FollowedChannels(r.Context(), u.ID)
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

// ---------- 通知 ----------

func (s *Server) handleNotifications(w http.ResponseWriter, r *http.Request, u *store.User) {
	limit, _ := strconv.Atoi(r.URL.Query().Get("limit"))
	list, unread, err := s.Store.ListNotifications(r.Context(), u.ID, limit)
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"notifications": list, "unread": unread})
}

func (s *Server) handleNotificationsRead(w http.ResponseWriter, r *http.Request, u *store.User) {
	if err := s.Store.MarkNotificationsRead(r.Context(), u.ID); err != nil {
		s.internalError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

// ---------- 举报 ----------

func (s *Server) handleReport(w http.ResponseWriter, r *http.Request, u *store.User) {
	ch := s.channelFromPath(w, r)
	if ch == nil {
		return
	}
	var body struct {
		Reason string `json:"reason"`
	}
	if !decode(w, r, &body) {
		return
	}
	reason := strings.TrimSpace(body.Reason)
	if n := utf8.RuneCountInString(reason); n < 1 || n > 500 {
		writeError(w, http.StatusBadRequest, "bad_request", "举报理由长度须为 1-500 字")
		return
	}
	if err := s.Store.CreateReport(r.Context(), u.ID, ch.ID, reason); err != nil {
		if errors.Is(err, store.ErrConflict) {
			writeError(w, http.StatusConflict, "already_reported", "你已举报过该频道，管理员处理中")
			return
		}
		s.internalError(w, r, err)
		return
	}
	writeJSON(w, http.StatusCreated, map[string]bool{"ok": true})
}

func (s *Server) handleAdminReports(w http.ResponseWriter, r *http.Request, _ *store.User) {
	list, err := s.Store.ListReports(r.Context(), r.URL.Query().Get("status"))
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"reports": list})
}

func (s *Server) handleAdminResolveReport(w http.ResponseWriter, r *http.Request, admin *store.User) {
	id, err := strconv.ParseInt(r.PathValue("id"), 10, 64)
	if err != nil {
		writeError(w, http.StatusBadRequest, "bad_request", "举报 ID 不合法")
		return
	}
	var body struct {
		Status string `json:"status"`
	}
	if !decode(w, r, &body) {
		return
	}
	if body.Status != "resolved" && body.Status != "dismissed" {
		writeError(w, http.StatusBadRequest, "bad_request", "状态只能是 resolved / dismissed")
		return
	}
	if err := s.Store.ResolveReport(r.Context(), id, admin.ID, body.Status); err != nil {
		if isNotFound(err) {
			writeError(w, http.StatusNotFound, "not_found", "举报不存在或已处理")
			return
		}
		s.internalError(w, r, err)
		return
	}
	s.Store.Audit(r.Context(), admin.ID, "report."+body.Status, strconv.FormatInt(id, 10), "")
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

// ---------- 频道聊天管理：禁言与房管 ----------

// canModerateChannel 管理员、站务、频道主与该频道房管可管理聊天。
func (s *Server) canModerateChannel(r *http.Request, u *store.User, ch *store.Channel) bool {
	return s.isChannelMod(r.Context(), u, ch)
}

func (s *Server) handleChannelBan(w http.ResponseWriter, r *http.Request, u *store.User) {
	ch := s.channelFromPath(w, r)
	if ch == nil {
		return
	}
	if !s.canModerateChannel(r, u, ch) {
		writeError(w, http.StatusForbidden, "forbidden", "无权管理该频道聊天")
		return
	}
	var body struct {
		UserID      int64 `json:"user_id"`
		DurationSec int64 `json:"duration_sec"` // 0 = 永久
	}
	if !decode(w, r, &body) {
		return
	}
	target, err := s.Store.UserByID(r.Context(), body.UserID)
	if err != nil {
		writeError(w, http.StatusNotFound, "not_found", "用户不存在")
		return
	}
	// 不能禁言频道主、站务人员与自己
	if target.ID == ch.UserID || target.IsStaff() || target.ID == u.ID {
		writeError(w, http.StatusForbidden, "forbidden", "不能禁言该用户")
		return
	}
	var until *int64
	if body.DurationSec > 0 {
		t := time.Now().Unix() + body.DurationSec
		until = &t
	}
	if err := s.Store.ChannelBan(r.Context(), ch.ID, target.ID, u.ID, until); err != nil {
		s.internalError(w, r, err)
		return
	}
	s.Store.Audit(r.Context(), u.ID, "chat.ban", ch.Slug+"/"+target.Username, strconv.FormatInt(body.DurationSec, 10))
	s.Hub.Broadcast(ch.ID, map[string]any{"type": "ban", "user_id": target.ID, "duration_sec": body.DurationSec})
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

func (s *Server) handleChannelUnban(w http.ResponseWriter, r *http.Request, u *store.User) {
	ch := s.channelFromPath(w, r)
	if ch == nil {
		return
	}
	if !s.canModerateChannel(r, u, ch) {
		writeError(w, http.StatusForbidden, "forbidden", "无权管理该频道聊天")
		return
	}
	uid, err := strconv.ParseInt(r.PathValue("uid"), 10, 64)
	if err != nil {
		writeError(w, http.StatusBadRequest, "bad_request", "用户 ID 不合法")
		return
	}
	if err := s.Store.ChannelUnban(r.Context(), ch.ID, uid); err != nil {
		s.internalError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

// 房管只能由频道主或管理员任免
func (s *Server) canManageModerators(u *store.User, ch *store.Channel) bool {
	return u.ID == ch.UserID || u.Role == store.RoleAdmin
}

func (s *Server) handleListModerators(w http.ResponseWriter, r *http.Request, u *store.User) {
	ch := s.channelFromPath(w, r)
	if ch == nil {
		return
	}
	if !s.canManageModerators(u, ch) {
		writeError(w, http.StatusForbidden, "forbidden", "无权查看")
		return
	}
	list, err := s.Store.ListModerators(r.Context(), ch.ID)
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"moderators": list})
}

func (s *Server) handleAddModerator(w http.ResponseWriter, r *http.Request, u *store.User) {
	ch := s.channelFromPath(w, r)
	if ch == nil {
		return
	}
	if !s.canManageModerators(u, ch) {
		writeError(w, http.StatusForbidden, "forbidden", "无权任命房管")
		return
	}
	var body struct {
		Username string `json:"username"`
	}
	if !decode(w, r, &body) {
		return
	}
	target, err := s.Store.UserByName(r.Context(), strings.TrimSpace(body.Username))
	if err != nil {
		writeError(w, http.StatusNotFound, "not_found", "用户不存在")
		return
	}
	if target.ID == ch.UserID {
		writeError(w, http.StatusBadRequest, "bad_request", "频道主无需设为房管")
		return
	}
	if err := s.Store.AddModerator(r.Context(), ch.ID, target.ID); err != nil {
		s.internalError(w, r, err)
		return
	}
	s.Store.Audit(r.Context(), u.ID, "channel.add_moderator", ch.Slug+"/"+target.Username, "")
	s.handleListModerators(w, r, u)
}

func (s *Server) handleRemoveModerator(w http.ResponseWriter, r *http.Request, u *store.User) {
	ch := s.channelFromPath(w, r)
	if ch == nil {
		return
	}
	if !s.canManageModerators(u, ch) {
		writeError(w, http.StatusForbidden, "forbidden", "无权任免房管")
		return
	}
	uid, err := strconv.ParseInt(r.PathValue("uid"), 10, 64)
	if err != nil {
		writeError(w, http.StatusBadRequest, "bad_request", "用户 ID 不合法")
		return
	}
	if err := s.Store.RemoveModerator(r.Context(), ch.ID, uid); err != nil {
		s.internalError(w, r, err)
		return
	}
	s.handleListModerators(w, r, u)
}

// ---------- 录像回放 ----------

type vodView struct {
	*store.VOD
	URL       string `json:"url"`
	Thumbnail string `json:"thumbnail,omitempty"`
}

func (s *Server) vodView(v *store.VOD) vodView {
	return vodView{VOD: v, URL: "/vods/" + strconv.FormatInt(v.ID, 10) + ".mp4",
		Thumbnail: s.thumbURL("vod_" + strconv.FormatInt(v.ID, 10) + ".jpg")}
}

func (s *Server) listVODs(w http.ResponseWriter, r *http.Request, channelID int64) {
	limit, _ := strconv.Atoi(r.URL.Query().Get("limit"))
	offset, _ := strconv.Atoi(r.URL.Query().Get("offset"))
	list, err := s.Store.ListVODs(r.Context(), channelID, limit, offset)
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	out := make([]vodView, 0, len(list))
	for _, v := range list {
		out = append(out, s.vodView(v))
	}
	writeJSON(w, http.StatusOK, map[string]any{"vods": out})
}

func (s *Server) handleChannelVODs(w http.ResponseWriter, r *http.Request) {
	ch := s.channelFromPath(w, r)
	if ch == nil {
		return
	}
	s.listVODs(w, r, ch.ID)
}

func (s *Server) handleRecentVODs(w http.ResponseWriter, r *http.Request) { s.listVODs(w, r, 0) }

// vodFromPath 按路径参数 id 查找录像；主播被封禁时视为不存在。
func (s *Server) vodFromPath(w http.ResponseWriter, r *http.Request) *store.VOD {
	id, err := strconv.ParseInt(strings.TrimSuffix(r.PathValue("id"), ".mp4"), 10, 64)
	if err != nil {
		writeError(w, http.StatusNotFound, "not_found", "录像不存在")
		return nil
	}
	v, err := s.Store.VODByID(r.Context(), id)
	if err != nil {
		if isNotFound(err) {
			writeError(w, http.StatusNotFound, "not_found", "录像不存在")
		} else {
			s.internalError(w, r, err)
		}
		return nil
	}
	if owner, err := s.Store.UserByID(r.Context(), v.OwnerID); err != nil || owner.Banned {
		writeError(w, http.StatusNotFound, "not_found", "录像不存在")
		return nil
	}
	return v
}

func (s *Server) handleGetVOD(w http.ResponseWriter, r *http.Request) {
	v := s.vodFromPath(w, r)
	if v == nil {
		return
	}
	s.Store.IncVODViews(r.Context(), v.ID)
	v.Views++
	writeJSON(w, http.StatusOK, s.vodView(v))
}

func (s *Server) canEditVOD(u *store.User, v *store.VOD) bool {
	return u.ID == v.OwnerID || u.Role == store.RoleAdmin
}

func (s *Server) handlePatchVOD(w http.ResponseWriter, r *http.Request, u *store.User) {
	v := s.vodFromPath(w, r)
	if v == nil {
		return
	}
	if !s.canEditVOD(u, v) {
		writeError(w, http.StatusForbidden, "forbidden", "无权修改该录像")
		return
	}
	var body struct {
		Title string `json:"title"`
	}
	if !decode(w, r, &body) {
		return
	}
	title := strings.TrimSpace(body.Title)
	if n := utf8.RuneCountInString(title); n < 1 || n > 80 {
		writeError(w, http.StatusBadRequest, "bad_request", "标题长度须为 1-80 字")
		return
	}
	if err := s.Store.RenameVOD(r.Context(), v.ID, title); err != nil {
		s.internalError(w, r, err)
		return
	}
	v.Title = title
	writeJSON(w, http.StatusOK, s.vodView(v))
}

func (s *Server) handleDeleteVOD(w http.ResponseWriter, r *http.Request, u *store.User) {
	v := s.vodFromPath(w, r)
	if v == nil {
		return
	}
	if !s.canEditVOD(u, v) {
		writeError(w, http.StatusForbidden, "forbidden", "无权删除该录像")
		return
	}
	s.removeVOD(r.Context(), v)
	s.Store.Audit(r.Context(), u.ID, "vod.delete", strconv.FormatInt(v.ID, 10), v.Title)
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

// handleVODFile 提供录像文件（支持 Range，可拖动进度）。
func (s *Server) handleVODFile(w http.ResponseWriter, r *http.Request) {
	v := s.vodFromPath(w, r)
	if v == nil {
		return
	}
	f, err := os.Open(filepath.Join(s.Cfg.VODDir(), filepath.Base(v.File)))
	if err != nil {
		http.NotFound(w, r)
		return
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil {
		http.NotFound(w, r)
		return
	}
	w.Header().Set("Content-Type", "video/mp4")
	w.Header().Set("Cache-Control", "public, max-age=86400")
	http.ServeContent(w, r, "", st.ModTime(), f)
}

var thumbNameRe = regexp.MustCompile(`^(live_[A-Za-z0-9_]+|vod_[0-9]+)\.jpg$`)

func (s *Server) handleThumb(w http.ResponseWriter, r *http.Request) {
	name := r.PathValue("name")
	if !thumbNameRe.MatchString(name) {
		http.NotFound(w, r)
		return
	}
	w.Header().Set("Cache-Control", "public, max-age=60")
	http.ServeFile(w, r, filepath.Join(s.Cfg.ThumbDir(), name))
}
