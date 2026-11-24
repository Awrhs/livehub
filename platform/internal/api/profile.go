package api

import (
	"bytes"
	"image"
	"image/jpeg"
	_ "image/gif" // 注册解码器
	_ "image/png"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strconv"

	"golang.org/x/image/draw"
	_ "golang.org/x/image/webp"

	"livehub/internal/auth"
	"livehub/internal/store"
)

const (
	avatarMaxBytes = 4 << 20
	avatarSize     = 256
)

func (s *Server) avatarDir() string { return filepath.Join(s.Cfg.DataDir, "avatars") }

// handleUploadAvatar 上传头像：任意常见格式（PNG/JPEG/GIF/WebP），居中裁成正方形、缩放到 256×256 后存为 JPEG。
// 重新编码可去除图片中的元数据（如 GPS 位置），也避免把用户上传的原始字节直接提供给浏览器。
func (s *Server) handleUploadAvatar(w http.ResponseWriter, r *http.Request, u *store.User) {
	r.Body = http.MaxBytesReader(w, r.Body, avatarMaxBytes+1024)
	var src io.Reader = r.Body
	if ct := r.Header.Get("Content-Type"); len(ct) >= 19 && ct[:19] == "multipart/form-data" {
		f, _, err := r.FormFile("file")
		if err != nil {
			writeError(w, http.StatusBadRequest, "bad_request", "缺少图片文件")
			return
		}
		defer f.Close()
		src = f
	}
	data, err := io.ReadAll(io.LimitReader(src, avatarMaxBytes+1))
	if err != nil || len(data) > avatarMaxBytes {
		writeError(w, http.StatusRequestEntityTooLarge, "too_large", "图片不能超过 4MB")
		return
	}
	cfg, _, err := image.DecodeConfig(bytes.NewReader(data))
	if err != nil {
		writeError(w, http.StatusBadRequest, "bad_image", "无法识别的图片格式")
		return
	}
	if cfg.Width*cfg.Height > 40_000_000 { // 防止解压炸弹
		writeError(w, http.StatusBadRequest, "bad_image", "图片尺寸过大")
		return
	}
	img, _, err := image.Decode(bytes.NewReader(data))
	if err != nil {
		writeError(w, http.StatusBadRequest, "bad_image", "无法识别的图片格式")
		return
	}
	b := img.Bounds()
	side := min(b.Dx(), b.Dy())
	crop := image.Rect(b.Min.X+(b.Dx()-side)/2, b.Min.Y+(b.Dy()-side)/2, 0, 0)
	crop.Max = crop.Min.Add(image.Pt(side, side))
	dst := image.NewRGBA(image.Rect(0, 0, avatarSize, avatarSize))
	draw.CatmullRom.Scale(dst, dst.Bounds(), img, crop, draw.Src, nil)
	var out bytes.Buffer
	if err := jpeg.Encode(&out, dst, &jpeg.Options{Quality: 88}); err != nil {
		s.internalError(w, r, err)
		return
	}
	if err := os.MkdirAll(s.avatarDir(), 0o755); err != nil {
		s.internalError(w, r, err)
		return
	}
	path := filepath.Join(s.avatarDir(), strconv.FormatInt(u.ID, 10)+".jpg")
	if err := os.WriteFile(path+".tmp", out.Bytes(), 0o644); err != nil {
		s.internalError(w, r, err)
		return
	}
	if err := os.Rename(path+".tmp", path); err != nil {
		s.internalError(w, r, err)
		return
	}
	v, err := s.Store.BumpAvatar(r.Context(), u.ID, false)
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	u.AvatarVersion, u.AvatarURL = v, store.AvatarURL(u.ID, v)
	s.handleMe(w, r, u)
}

func (s *Server) handleDeleteAvatar(w http.ResponseWriter, r *http.Request, u *store.User) {
	_ = os.Remove(filepath.Join(s.avatarDir(), strconv.FormatInt(u.ID, 10)+".jpg"))
	if _, err := s.Store.BumpAvatar(r.Context(), u.ID, true); err != nil {
		s.internalError(w, r, err)
		return
	}
	u.AvatarVersion, u.AvatarURL = 0, ""
	s.handleMe(w, r, u)
}

var avatarNameRe = regexp.MustCompile(`^[0-9]+\.jpg$`)

func (s *Server) handleAvatarFile(w http.ResponseWriter, r *http.Request) {
	name := r.PathValue("name")
	if !avatarNameRe.MatchString(name) {
		http.NotFound(w, r)
		return
	}
	// 地址带版本号，内容不变，可长期缓存
	w.Header().Set("Cache-Control", "public, max-age=31536000, immutable")
	http.ServeFile(w, r, filepath.Join(s.avatarDir(), name))
}

// handleAdminResetPassword 管理员为用户生成临时密码（无邮件或用户无法收信时使用），
// 同时注销该用户全部会话。临时密码只在本次响应中返回。
func (s *Server) handleAdminResetPassword(w http.ResponseWriter, r *http.Request, admin *store.User) {
	id, err := strconv.ParseInt(r.PathValue("id"), 10, 64)
	if err != nil {
		writeError(w, http.StatusBadRequest, "bad_request", "用户 ID 不合法")
		return
	}
	target, err := s.Store.UserByID(r.Context(), id)
	if err != nil {
		writeError(w, http.StatusNotFound, "not_found", "用户不存在")
		return
	}
	pw := store.RandomToken(6) // 12 位十六进制
	hash, err := auth.HashPassword(pw, s.Cfg.BcryptCost)
	if err != nil {
		s.internalError(w, r, err)
		return
	}
	if err := s.Store.UpdatePassword(r.Context(), id, hash); err != nil {
		s.internalError(w, r, err)
		return
	}
	_ = s.Store.DeleteUserSessions(r.Context(), id, "")
	s.Store.Audit(r.Context(), admin.ID, "user.reset_password", target.Username, "")
	writeJSON(w, http.StatusOK, map[string]string{"password": pw})
}
