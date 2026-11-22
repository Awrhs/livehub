package media

import (
	"bufio"
	"context"
	"fmt"
	"io"
	"log/slog"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"time"
)

// ServerConfig 内嵌模式下生成的媒体服务器配置。
type ServerConfig struct {
	WorkDir       string
	HTTPPort      int
	RTMPPort      int
	SRTPort       int // 0 = 关闭
	AdminToken    string
	JWTSecret     string
	HookBaseURL   string // 平台地址，如 http://127.0.0.1:8000
	HookSecret    string
	DVRDir        string
	CandidateIP   string
	InternalToken string // 转码回推等内部推流携带的令牌（INTERNAL_PUBLISH_TOKEN）
}

// Render 生成 streaming.conf 内容。
//
// 要点：
//   - 播放请求全部经平台反向代理而来（来源 IP 都是 127.0.0.1），关闭媒体侧按 IP 限流/限连接，
//     由平台负责限流；
//   - 推流鉴权完全交给平台 webhook，fail-closed：平台不可用时拒绝推流；
//   - 只允许 live 应用。
func (c ServerConfig) Render() string {
	var b strings.Builder
	w := func(format string, a ...any) { fmt.Fprintf(&b, format+"\n", a...) }
	w("# 由 LiveHub 自动生成，请勿手工修改（重启时会被覆盖）")
	w("HTTP_PORT=%d", c.HTTPPort)
	w("RTMP_PORT=%d", c.RTMPPort)
	if c.SRTPort > 0 {
		w("SRT_PORT=%d", c.SRTPort)
	} else {
		w("SRT_PORT=0")
	}
	w("RTSP_PORT=0")
	w("IO_THREADS=2")
	w("IDLE_TIMEOUT=30")
	w("MAX_CONNECTIONS=10000")
	w("MAX_CONNECTIONS_PER_IP=0")
	w("ENABLE_RATE_LIMIT=false")
	w("HLS_OUTPUT_DIR=%s", filepath.Join(c.WorkDir, "hls"))
	w("HLS_SEGMENT_DURATION=2")
	w("HLS_WINDOW_SIZE=6")
	w("DVR_DIR=%s", c.DVRDir)
	w("JWT_SECRET=%s", c.JWTSecret)
	w("ADMIN_TOKEN=%s", c.AdminToken)
	w("LOG_LEVEL=INFO")
	w("TRANSCODE_PROFILES=480p,720p")
	w("TRANSCODE_CONCURRENCY=8")
	if c.InternalToken != "" {
		w("INTERNAL_PUBLISH_TOKEN=%s", c.InternalToken)
	}
	if c.CandidateIP != "" {
		w("WEBRTC_CANDIDATE_IP=%s", c.CandidateIP)
	}
	w("")
	w("[vhost:__default__]")
	w("allow_apps=live")
	hook := c.HookBaseURL + "/internal/media/hook?secret=" + c.HookSecret
	w("webhook_on_publish=%s", hook)
	w("webhook_on_unpublish=%s", hook)
	w("webhook_fail_closed=true")
	w("webhook_timeout_ms=3000")
	w("webhook_max_retries=5")
	return b.String()
}

// Supervisor 拉起并守护媒体服务器子进程：异常退出后按退避重启。
type Supervisor struct {
	Bin    string
	Config ServerConfig
	Log    *slog.Logger
	// OnRestart 子进程（重新）启动后回调：此前的推流都已断开，平台需重置直播状态
	OnRestart func()
}

// Run 阻塞运行直到 ctx 取消；取消时向子进程发送 SIGTERM 并等待退出（最多 10s）。
func (s *Supervisor) Run(ctx context.Context) error {
	if err := os.MkdirAll(s.Config.WorkDir, 0o755); err != nil {
		return err
	}
	if err := os.MkdirAll(s.Config.DVRDir, 0o755); err != nil {
		return err
	}
	confPath := filepath.Join(s.Config.WorkDir, "streaming.conf")
	if err := os.WriteFile(confPath, []byte(s.Config.Render()), 0o600); err != nil {
		return err
	}
	backoff := time.Second
	first := true
	for {
		if ctx.Err() != nil {
			return nil
		}
		start := time.Now()
		cmd := exec.Command(s.Bin, confPath)
		cmd.Dir = s.Config.WorkDir
		// 子进程不继承平台的 LIVEHUB_* 之外的 STREAM_* 环境变量，避免覆盖生成的配置
		env := []string{}
		for _, e := range os.Environ() {
			if !strings.HasPrefix(e, "STREAM_") {
				env = append(env, e)
			}
		}
		cmd.Env = env
		cmd.SysProcAttr = &syscall.SysProcAttr{Pdeathsig: syscall.SIGTERM}
		stdout, _ := cmd.StdoutPipe()
		stderr, _ := cmd.StderrPipe()
		if err := cmd.Start(); err != nil {
			s.Log.Error("媒体服务器启动失败", "err", err)
		} else {
			s.Log.Info("媒体服务器已启动", "pid", cmd.Process.Pid)
			go s.pipe(stdout)
			go s.pipe(stderr)
			if !first && s.OnRestart != nil {
				s.OnRestart()
			}
			first = false
			done := make(chan error, 1)
			go func() { done <- cmd.Wait() }()
			select {
			case err := <-done:
				s.Log.Warn("媒体服务器退出", "err", err)
				if s.OnRestart != nil {
					s.OnRestart()
				}
			case <-ctx.Done():
				_ = cmd.Process.Signal(syscall.SIGTERM)
				select {
				case <-done:
				case <-time.After(10 * time.Second):
					_ = cmd.Process.Kill()
					<-done
				}
				s.Log.Info("媒体服务器已停止")
				return nil
			}
		}
		// 稳定运行超过 1 分钟则重置退避
		if time.Since(start) > time.Minute {
			backoff = time.Second
		}
		select {
		case <-ctx.Done():
			return nil
		case <-time.After(backoff):
		}
		if backoff < 30*time.Second {
			backoff *= 2
		}
	}
}

func (s *Supervisor) pipe(r io.Reader) {
	sc := bufio.NewScanner(r)
	sc.Buffer(make([]byte, 64*1024), 1024*1024)
	for sc.Scan() {
		s.Log.Info(sc.Text(), "src", "media")
	}
}
