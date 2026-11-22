// Package config 从环境变量读取 LiveHub 的进程级配置。
//
// 站点级设置（站点名、注册模式等）存放在数据库 settings 表，由管理后台修改；
// 这里只放启动时必须确定、修改后需重启的配置。
package config

import (
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

type Config struct {
	Addr    string // 平台 HTTP 监听地址
	DataDir string // 数据目录：数据库、录像、封面、媒体服务器工作目录

	// 对外地址（用于生成推流地址与 Cookie Secure 判断）；为空时按请求 Host 推断
	PublicURL string
	// 对外展示的推流地址覆盖（如 rtmp://live.example.com/live）
	RTMPURL string
	SRTURL  string

	// 媒体服务器
	MediaBin        string // 非空：内嵌模式，由平台拉起并守护该可执行文件
	MediaURL        string // 媒体服务器 HTTP 地址（平台访问管理 API 与反向代理播放）
	MediaAdminToken string // 外部模式必须配置；内嵌模式自动生成
	HookSecret      string // 媒体服务器回调平台时携带的密钥；外部模式必须配置
	HookURL         string // 媒体服务器访问平台的地址（内嵌模式默认 http://127.0.0.1:<端口>）
	MediaRTMPPort   int
	MediaHTTPPort   int
	MediaSRTPort    int // 0 = 关闭 SRT
	MediaDVRDir     string

	FFmpeg  string // 截取封面使用的 ffmpeg
	FFprobe string
	// 外部模式：媒体服务器 INTERNAL_PUBLISH_TOKEN（多清晰度转码回推时携带）
	InternalToken string

	// 边缘播放节点（逗号分隔的对外基础地址，如 https://edge1.example.com）；为空则全部由本机分发
	EdgeURLs []string
	TrustProxy    bool   // 信任 X-Forwarded-For / X-Forwarded-Proto（部署在反向代理后）
	SecureCookies string // auto | true | false
	BcryptCost    int
	LogLevel      string
}

func env(key, def string) string {
	if v, ok := os.LookupEnv(key); ok && v != "" {
		return v
	}
	return def
}

func envInt(key string, def int) (int, error) {
	v := env(key, "")
	if v == "" {
		return def, nil
	}
	n, err := strconv.Atoi(v)
	if err != nil {
		return 0, fmt.Errorf("%s: 需要整数，得到 %q", key, v)
	}
	return n, nil
}

func envBool(key string, def bool) bool {
	switch strings.ToLower(env(key, "")) {
	case "1", "true", "yes", "on":
		return true
	case "0", "false", "no", "off":
		return false
	}
	return def
}

// Load 读取环境变量并校验。
func Load() (*Config, error) {
	c := &Config{
		Addr:            env("LIVEHUB_ADDR", ":8000"),
		DataDir:         env("LIVEHUB_DATA_DIR", "./data"),
		PublicURL:       strings.TrimRight(env("LIVEHUB_PUBLIC_URL", ""), "/"),
		RTMPURL:         env("LIVEHUB_RTMP_URL", ""),
		SRTURL:          env("LIVEHUB_SRT_URL", ""),
		MediaBin:        env("LIVEHUB_MEDIA_BIN", ""),
		MediaURL:        strings.TrimRight(env("LIVEHUB_MEDIA_URL", ""), "/"),
		MediaAdminToken: env("LIVEHUB_MEDIA_ADMIN_TOKEN", ""),
		HookSecret:      env("LIVEHUB_HOOK_SECRET", ""),
		HookURL:         strings.TrimRight(env("LIVEHUB_HOOK_URL", ""), "/"),
		MediaDVRDir:     env("LIVEHUB_MEDIA_DVR_DIR", ""),
		FFmpeg:          env("LIVEHUB_FFMPEG", "ffmpeg"),
		FFprobe:         env("LIVEHUB_FFPROBE", "ffprobe"),
		InternalToken:   env("LIVEHUB_MEDIA_INTERNAL_TOKEN", ""),
		TrustProxy:      envBool("LIVEHUB_TRUST_PROXY", false),
		SecureCookies:   strings.ToLower(env("LIVEHUB_SECURE_COOKIES", "auto")),
		LogLevel:        env("LIVEHUB_LOG_LEVEL", "info"),
	}
	var err error
	if c.MediaRTMPPort, err = envInt("LIVEHUB_MEDIA_RTMP_PORT", 1935); err != nil {
		return nil, err
	}
	if c.MediaHTTPPort, err = envInt("LIVEHUB_MEDIA_HTTP_PORT", 8081); err != nil {
		return nil, err
	}
	if c.MediaSRTPort, err = envInt("LIVEHUB_MEDIA_SRT_PORT", 10080); err != nil {
		return nil, err
	}
	if c.BcryptCost, err = envInt("LIVEHUB_BCRYPT_COST", 10); err != nil {
		return nil, err
	}
	if c.DataDir, err = filepath.Abs(c.DataDir); err != nil {
		return nil, err
	}
	if c.Embedded() {
		if c.MediaURL == "" {
			c.MediaURL = fmt.Sprintf("http://127.0.0.1:%d", c.MediaHTTPPort)
		}
		if c.MediaDVRDir == "" {
			c.MediaDVRDir = filepath.Join(c.DataDir, "media", "dvr")
		}
		if c.HookURL == "" {
			port := "8000"
			if i := strings.LastIndex(c.Addr, ":"); i >= 0 && i+1 < len(c.Addr) {
				port = c.Addr[i+1:]
			}
			c.HookURL = "http://127.0.0.1:" + port
		}
	} else if c.MediaURL != "" {
		// 外部模式：密钥必须显式配置，且两端一致
		if c.MediaAdminToken == "" || c.HookSecret == "" {
			return nil, fmt.Errorf("外部媒体服务器模式需要 LIVEHUB_MEDIA_ADMIN_TOKEN 与 LIVEHUB_HOOK_SECRET")
		}
	}
	for _, e := range strings.Split(env("LIVEHUB_EDGE_URLS", ""), ",") {
		if e = strings.TrimRight(strings.TrimSpace(e), "/"); e != "" {
			c.EdgeURLs = append(c.EdgeURLs, e)
		}
	}
	switch c.SecureCookies {
	case "auto", "true", "false":
	default:
		return nil, fmt.Errorf("LIVEHUB_SECURE_COOKIES 只能是 auto/true/false")
	}
	return c, nil
}

// Embedded 是否由平台拉起媒体服务器子进程。
func (c *Config) Embedded() bool { return c.MediaBin != "" }

// HasMedia 是否配置了媒体服务器（未配置时平台仍可运行，便于单元测试与前端开发）。
func (c *Config) HasMedia() bool { return c.MediaURL != "" }

func (c *Config) DBPath() string       { return filepath.Join(c.DataDir, "livehub.db") }
func (c *Config) VODDir() string       { return filepath.Join(c.DataDir, "vods") }
func (c *Config) ThumbDir() string     { return filepath.Join(c.DataDir, "thumbs") }
func (c *Config) MediaWorkDir() string { return filepath.Join(c.DataDir, "media") }
