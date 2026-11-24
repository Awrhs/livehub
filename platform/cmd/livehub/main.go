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
	"livehub/internal/auth"
	"livehub/internal/chat"
	"livehub/internal/config"
	"livehub/internal/db"
	"livehub/internal/mail"
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
	if flag.NArg() > 0 {
		if err := runCommand(flag.Args()); err != nil {
			fmt.Fprintln(os.Stderr, "livehub:", err)
			os.Exit(1)
		}
		return
	}
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "livehub:", err)
		os.Exit(1)
	}
}

const usage = `用法：
  livehub                                 启动服务（配置见环境变量 LIVEHUB_*）
  livehub backup <目标文件>               在线备份数据库（服务运行中也可执行）
  livehub reset-password <用户名> <新密码>  重置用户密码并注销其全部会话
  livehub make-admin <用户名>             将用户设为管理员
  livehub -version                        打印版本`

// runCommand 运维子命令（直接操作数据目录中的数据库）。
func runCommand(args []string) error {
	cfg, err := config.Load()
	if err != nil {
		return err
	}
	sqlDB, err := db.Open(cfg.DBPath())
	if err != nil {
		return err
	}
	defer sqlDB.Close()
	st := store.New(sqlDB)
	ctx := context.Background()
	switch args[0] {
	case "backup":
		if len(args) != 2 {
			return fmt.Errorf("%s", usage)
		}
		if _, err := os.Stat(args[1]); err == nil {
			return fmt.Errorf("目标文件已存在：%s", args[1])
		}
		// VACUUM INTO 生成一致的数据库快照，不阻塞正在运行的服务
		if _, err := sqlDB.ExecContext(ctx, `VACUUM INTO ?`, args[1]); err != nil {
			return err
		}
		fmt.Println("已备份到", args[1], "（录像与封面位于", cfg.VODDir(), "和", cfg.ThumbDir(), "，请一并备份）")
	case "reset-password":
		if len(args) != 3 {
			return fmt.Errorf("%s", usage)
		}
		u, err := st.UserByName(ctx, args[1])
		if err != nil {
			return fmt.Errorf("用户不存在：%s", args[1])
		}
		if err := auth.ValidatePassword(args[2]); err != nil {
			return err
		}
		hash, err := auth.HashPassword(args[2], cfg.BcryptCost)
		if err != nil {
			return err
		}
		if err := st.UpdatePassword(ctx, u.ID, hash); err != nil {
			return err
		}
		_ = st.DeleteUserSessions(ctx, u.ID, "")
		st.Audit(ctx, 0, "cli.reset_password", u.Username, "")
		fmt.Println("已重置", u.Username, "的密码")
	case "make-admin":
		if len(args) != 2 {
			return fmt.Errorf("%s", usage)
		}
		u, err := st.UserByName(ctx, args[1])
		if err != nil {
			return fmt.Errorf("用户不存在：%s", args[1])
		}
		role, banned := store.RoleAdmin, false
		if err := st.PatchUser(ctx, u.ID, store.UserPatch{Role: &role, Banned: &banned}); err != nil {
			return err
		}
		st.Audit(ctx, 0, "cli.make_admin", u.Username, "")
		fmt.Println(u.Username, "已成为管理员")
	default:
		return fmt.Errorf("未知命令 %q\n%s", args[0], usage)
	}
	return nil
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

	if err := st.MigrateStreamKeys(ctx); err != nil {
		return err
	}
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
	if cfg.MailEnabled() {
		srv.Mailer = mail.NewSMTP(mail.Config{Host: cfg.SMTPHost, Port: cfg.SMTPPort, User: cfg.SMTPUser,
			Password: cfg.SMTPPassword, From: cfg.SMTPFrom, TLS: cfg.SMTPTLS})
		log.Info("邮件服务已启用", "host", cfg.SMTPHost, "tls", cfg.SMTPTLS)
	}

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
				CandidateIP: cfg.WebRTCCandidateIP, InternalToken: internalToken,
				WebRTCPortMin: cfg.WebRTCPortMin, WebRTCPortMax: cfg.WebRTCPortMax,
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
