// LiveHub：社区自部署的多主播直播平台。
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"sync"
	"syscall"
	"time"

	"livehub/internal/api"
	"livehub/internal/chat"
	"livehub/internal/config"
	"livehub/internal/db"
	"livehub/internal/media"
	"livehub/internal/store"
	"livehub/internal/webui"
)

// version 构建时通过 -ldflags "-X main.version=..." 注入
var version = "dev"

func main() {
	showVersion := flag.Bool("version", false, "打印版本并退出")
	flag.Parse()
	if *showVersion {
		fmt.Println(version)
		return
	}
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "livehub:", err)
		os.Exit(1)
	}
}

func newLogger(level string) *slog.Logger {
	var lv slog.Level
	switch strings.ToLower(level) {
	case "debug":
		lv = slog.LevelDebug
	case "warn":
		lv = slog.LevelWarn
	case "error":
		lv = slog.LevelError
	default:
		lv = slog.LevelInfo
	}
	return slog.New(slog.NewTextHandler(os.Stdout, &slog.HandlerOptions{Level: lv}))
}

func run() error {
	cfg, err := config.Load()
	if err != nil {
		return err
	}
	log := newLogger(cfg.LogLevel)
	for _, d := range []string{cfg.DataDir, cfg.VODDir(), cfg.ThumbDir()} {
		if err := os.MkdirAll(d, 0o755); err != nil {
			return err
		}
	}
	sqlDB, err := db.Open(cfg.DBPath())
	if err != nil {
		return err
	}
	defer sqlDB.Close()
	st := store.New(sqlDB)

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	// 平台重启后，直播状态以媒体服务器为准：先全部置为下播，由对账恢复仍在推的流
	if err := st.ResetAllLive(ctx); err != nil {
		return err
	}

	// 内嵌模式：密钥自动生成并持久化；外部模式：取自环境变量
	adminToken, hookSecret := cfg.MediaAdminToken, cfg.HookSecret
	if cfg.Embedded() {
		if adminToken, err = st.EnsureSecret(ctx, store.SettingMediaAdminToken); err != nil {
			return err
		}
		if hookSecret, err = st.EnsureSecret(ctx, store.SettingHookSecret); err != nil {
			return err
		}
	}
	internalToken := cfg.InternalToken
	if cfg.Embedded() {
		if internalToken, err = st.EnsureSecret(ctx, store.SettingInternalToken); err != nil {
			return err
		}
	}

	var mc *media.Client
	if cfg.HasMedia() {
		mc = media.NewClient(cfg.MediaURL, adminToken)
	}
	hub := chat.NewHub()
	srv := api.New(cfg, st, mc, hub, log, hookSecret, webui.Handler(), version)
	srv.InternalToken = internalToken

	var wg sync.WaitGroup
	if cfg.Embedded() {
		jwtSecret, err := st.EnsureSecret(ctx, store.SettingMediaJWTSecret)
		if err != nil {
			return err
		}
		sup := &media.Supervisor{
			Bin: cfg.MediaBin,
			Config: media.ServerConfig{
				WorkDir: cfg.MediaWorkDir(), HTTPPort: cfg.MediaHTTPPort, RTMPPort: cfg.MediaRTMPPort,
				SRTPort: cfg.MediaSRTPort, AdminToken: adminToken, JWTSecret: jwtSecret,
				HookBaseURL: cfg.HookURL, HookSecret: hookSecret, DVRDir: cfg.MediaDVRDir,
				CandidateIP: os.Getenv("LIVEHUB_WEBRTC_CANDIDATE_IP"), InternalToken: internalToken,
			},
			Log:       log.With("component", "supervisor"),
			OnRestart: srv.OnMediaRestart,
		}
		wg.Add(1)
		go func() {
			defer wg.Done()
			if err := sup.Run(ctx); err != nil {
				log.Error("媒体服务器托管失败", "err", err)
				stop()
			}
		}()
	}

	wg.Add(1)
	go func() {
		defer wg.Done()
		srv.RunBackground(ctx)
	}()

	httpSrv := &http.Server{
		Addr:              cfg.Addr,
		Handler:           srv.Handler(),
		ReadHeaderTimeout: 10 * time.Second,
		IdleTimeout:       120 * time.Second,
	}
	errCh := make(chan error, 1)
	go func() {
		log.Info("LiveHub 启动", "addr", cfg.Addr, "version", version, "data", cfg.DataDir,
			"media", cfg.MediaURL, "embedded", cfg.Embedded())
		if err := httpSrv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			errCh <- err
		}
		close(errCh)
	}()

	select {
	case err := <-errCh:
		if err != nil {
			stop()
			wg.Wait()
			return err
		}
	case <-ctx.Done():
	}
	log.Info("正在关闭")
	shutdownCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	_ = httpSrv.Shutdown(shutdownCtx)
	wg.Wait()
	return nil
}
