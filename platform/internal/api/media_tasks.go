package api

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"livehub/internal/store"
)

// 多清晰度转码档位（与媒体服务器 TRANSCODE_PROFILES 一致）
const abrProfiles = "480p,720p"

// streamState 开播期间的媒体侧状态（录像、转码）。
type streamState struct {
	recording bool
	abr       bool
}

func (s *Server) setStreamState(slug string, fn func(st *streamState)) {
	s.stateMu.Lock()
	defer s.stateMu.Unlock()
	st := s.streams[slug]
	if st == nil {
		st = &streamState{}
		s.streams[slug] = st
	}
	fn(st)
}

func (s *Server) takeStreamState(slug string) streamState {
	s.stateMu.Lock()
	defer s.stateMu.Unlock()
	st := s.streams[slug]
	delete(s.streams, slug)
	if st == nil {
		return streamState{}
	}
	return *st
}

// abrURL 多清晰度主播放列表地址：仅在各档位流都已在媒体服务器上出现后返回，
// 否则播放器会因子播放列表 404 而失败。
func (s *Server) abrURL(slug string) string {
	s.stateMu.Lock()
	ready := s.abrReady[slug]
	s.stateMu.Unlock()
	if !ready {
		return ""
	}
	return "/live/" + slug + "/master.m3u8"
}

// waitStream 等待媒体服务器上出现该流的推流（最多 timeout）。
func (s *Server) waitStream(ctx context.Context, key string, timeout time.Duration) bool {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if streams, err := s.Media.ListStreams(ctx); err == nil {
			for _, st := range streams {
				if st.Key == key && st.Publisher {
					return true
				}
			}
		}
		select {
		case <-ctx.Done():
			return false
		case <-time.After(500 * time.Millisecond):
		}
	}
	return false
}

// afterLive 开播后的异步任务：开播通知、录像、多清晰度转码、首张封面。
// 与 afterOffline 共用频道录像锁，保证上一场录像归档完成后才开始新录像。
func (s *Server) afterLive(ch *store.Channel) {
	ctx := context.Background()
	if ids, err := s.Store.NotifyFollowers(ctx, ch.ID, ch.Title); err == nil && len(ids) > 0 {
		for _, id := range ids {
			s.events.publish(id, map[string]string{"type": "notification", "kind": "live", "channel": ch.Slug})
		}
		s.Log.Info("已发送开播通知", "channel", ch.Slug, "count", len(ids))
	}
	if s.Media == nil {
		return
	}
	mu := s.recordMutex(ch.ID)
	mu.Lock()
	defer mu.Unlock()
	if !s.waitStream(ctx, ch.Slug, 15*time.Second) {
		s.Log.Warn("开播后未在媒体服务器找到推流", "channel", ch.Slug)
		return
	}
	if s.Store.SettingBool(ctx, store.SettingRecordingEnabled) && ch.RecordEnabled {
		if ok, err := s.Media.StartRecord(ctx, ch.Slug); err != nil || !ok {
			s.Log.Warn("开始录像失败", "channel", ch.Slug, "err", err)
		} else {
			s.setStreamState(ch.Slug, func(st *streamState) { st.recording = true })
			s.Log.Info("开始录像", "channel", ch.Slug)
		}
	}
	if s.Store.SettingBool(ctx, store.SettingTranscodeEnabled) {
		if err := s.Media.StartABR(ctx, ch.Slug, abrProfiles); err != nil {
			s.Log.Warn("开启多清晰度转码失败", "channel", ch.Slug, "err", err)
		} else {
			s.setStreamState(ch.Slug, func(st *streamState) { st.abr = true })
		}
	}
	// 首张封面：等几秒让画面稳定
	go func() {
		time.Sleep(3 * time.Second)
		s.captureLiveThumb(ctx, ch.Slug)
	}()
}

// afterOffline 下播后的异步任务：停止转码、结束录像并归档为回放。
func (s *Server) afterOffline(ch *store.Channel, sessionID int64) {
	ctx := context.Background()
	st := s.takeStreamState(ch.Slug)
	s.stateMu.Lock()
	delete(s.abrReady, ch.Slug)
	s.stateMu.Unlock()
	_ = os.Remove(filepath.Join(s.Cfg.ThumbDir(), "live_"+ch.Slug+".jpg"))
	if s.Media == nil {
		return
	}
	if st.abr {
		if err := s.Media.StopABR(ctx, ch.Slug); err != nil {
			s.Log.Warn("关闭多清晰度转码失败", "channel", ch.Slug, "err", err)
		}
	}
	if !st.recording {
		return
	}
	mu := s.recordMutex(ch.ID)
	mu.Lock()
	defer mu.Unlock()
	// 踢流时媒体服务器已自行停止录像，此处返回 false 属正常；以文件是否存在为准
	if _, err := s.Media.StopRecord(ctx, ch.Slug); err != nil {
		s.Log.Warn("停止录像失败", "channel", ch.Slug, "err", err)
	}
	if err := s.archiveRecording(ctx, ch, sessionID); err != nil {
		s.Log.Warn("录像归档失败", "channel", ch.Slug, "err", err)
	}
}

// archiveRecording 把媒体服务器的录像文件移入平台 VOD 目录并登记。
func (s *Server) archiveRecording(ctx context.Context, ch *store.Channel, sessionID int64) error {
	if s.Cfg.MediaDVRDir == "" {
		return fmt.Errorf("未配置媒体服务器录像目录（LIVEHUB_MEDIA_DVR_DIR）")
	}
	src := filepath.Join(s.Cfg.MediaDVRDir, "recordings", ch.Slug+".mp4")
	info, err := os.Stat(src)
	if err != nil {
		return fmt.Errorf("录像文件不存在: %w", err)
	}
	if info.Size() < 32<<10 {
		_ = os.Remove(src)
		return fmt.Errorf("录像过短（%d 字节），已丢弃", info.Size())
	}
	name := fmt.Sprintf("%s_%d_%s.mp4", ch.Slug, time.Now().Unix(), store.RandomToken(4))
	dst := filepath.Join(s.Cfg.VODDir(), name)
	if err := moveFile(src, dst); err != nil {
		return err
	}
	dur := s.probeDuration(ctx, dst)
	if dur > 0 && dur < 3 {
		_ = os.Remove(dst)
		return fmt.Errorf("录像时长过短（%.1fs），已丢弃", dur)
	}
	title, category := ch.Title, ch.Category
	if sessionID != 0 {
		if t, c, err := s.Store.SessionInfo(ctx, sessionID); err == nil {
			title, category = t, c
		}
	}
	id, err := s.Store.CreateVOD(ctx, ch.ID, sessionID, title, category, name, dur, info.Size())
	if err != nil {
		_ = os.Remove(dst)
		return err
	}
	at := 5.0
	if dur > 0 && dur < 10 {
		at = dur / 2
	}
	s.captureThumb(ctx, dst, filepath.Join(s.Cfg.ThumbDir(), "vod_"+strconv.FormatInt(id, 10)+".jpg"), at)
	s.Log.Info("录像已归档", "channel", ch.Slug, "vod", id, "duration", dur)
	return nil
}

// moveFile 先尝试 rename，跨文件系统时退回复制后删除。
func moveFile(src, dst string) error {
	if err := os.Rename(src, dst); err == nil {
		return nil
	}
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.Create(dst + ".tmp")
	if err != nil {
		return err
	}
	if _, err := io.Copy(out, in); err != nil {
		out.Close()
		os.Remove(dst + ".tmp")
		return err
	}
	if err := out.Close(); err != nil {
		return err
	}
	if err := os.Rename(dst+".tmp", dst); err != nil {
		return err
	}
	return os.Remove(src)
}

func (s *Server) probeDuration(ctx context.Context, path string) float64 {
	cctx, cancel := context.WithTimeout(ctx, 20*time.Second)
	defer cancel()
	out, err := exec.CommandContext(cctx, s.Cfg.FFprobe, "-v", "error", "-show_entries", "format=duration",
		"-of", "json", path).Output()
	if err != nil {
		return 0
	}
	var r struct {
		Format struct {
			Duration string `json:"duration"`
		} `json:"format"`
	}
	if json.Unmarshal(out, &r) != nil {
		return 0
	}
	d, _ := strconv.ParseFloat(r.Format.Duration, 64)
	return d
}

// captureThumb 从 input 第 at 秒截一帧缩放为 480 宽的 JPEG（先写临时文件再改名，避免读到半张图）。
func (s *Server) captureThumb(ctx context.Context, input, out string, at float64) bool {
	cctx, cancel := context.WithTimeout(ctx, 20*time.Second)
	defer cancel()
	tmp := out + ".tmp.jpg"
	args := []string{"-hide_banner", "-loglevel", "error", "-y"}
	if at > 0 {
		args = append(args, "-ss", strconv.FormatFloat(at, 'f', 2, 64))
	}
	args = append(args, "-i", input, "-frames:v", "1", "-vf", "scale=480:-2", "-q:v", "5", tmp)
	if err := exec.CommandContext(cctx, s.Cfg.FFmpeg, args...).Run(); err != nil {
		_ = os.Remove(tmp)
		return false
	}
	return os.Rename(tmp, out) == nil
}

func (s *Server) captureLiveThumb(ctx context.Context, slug string) {
	if s.Media == nil {
		return
	}
	s.captureThumb(ctx, s.Cfg.MediaURL+"/live/"+slug+".flv", filepath.Join(s.Cfg.ThumbDir(), "live_"+slug+".jpg"), 0)
}

// refreshThumbs 周期刷新直播封面（逐个串行，避免同时拉起大量 ffmpeg）。
func (s *Server) refreshThumbs(ctx context.Context) {
	live, err := s.Store.LiveChannels(ctx)
	if err != nil {
		return
	}
	for _, ch := range live {
		if ctx.Err() != nil {
			return
		}
		s.captureLiveThumb(ctx, ch.Slug)
	}
}

// updateABRReady 根据媒体服务器流列表判断各频道多清晰度是否就绪（所有档位流都已出现）。
func (s *Server) updateABRReady(active map[string]bool) {
	profiles := strings.Split(abrProfiles, ",")
	s.stateMu.Lock()
	defer s.stateMu.Unlock()
	for slug, st := range s.streams {
		if !st.abr {
			continue
		}
		ready := active[slug]
		for _, p := range profiles {
			if !active[slug+"_"+p] {
				ready = false
			}
		}
		s.abrReady[slug] = ready
	}
}

// thumbURL 文件存在时返回带版本号（修改时间）的地址，便于浏览器缓存与刷新。
func (s *Server) thumbURL(name string) string {
	info, err := os.Stat(filepath.Join(s.Cfg.ThumbDir(), name))
	if err != nil {
		return ""
	}
	return "/thumbs/" + name + "?v=" + strconv.FormatInt(info.ModTime().Unix(), 10)
}

// pruneVODs 按保留天数清理过期录像。
func (s *Server) pruneVODs(ctx context.Context) {
	days := s.Store.SettingInt(ctx, store.SettingVODRetentionDays)
	if days <= 0 {
		return
	}
	cutoff := time.Now().Add(-time.Duration(days) * 24 * time.Hour).Unix()
	vods, err := s.Store.VODsBefore(ctx, cutoff)
	if err != nil {
		return
	}
	for _, v := range vods {
		s.removeVOD(ctx, v)
	}
}

func (s *Server) removeVOD(ctx context.Context, v *store.VOD) {
	_ = os.Remove(filepath.Join(s.Cfg.VODDir(), v.File))
	_ = os.Remove(filepath.Join(s.Cfg.ThumbDir(), "vod_"+strconv.FormatInt(v.ID, 10)+".jpg"))
	_ = s.Store.DeleteVOD(ctx, v.ID)
}
