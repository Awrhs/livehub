package store

import (
	"context"
	"database/sql"
	"errors"
	"strconv"
)

// 站点设置键与默认值
const (
	SettingSiteName         = "site_name"
	SettingRegistrationMode = "registration_mode" // open | invite | closed
	SettingStreamApproval   = "stream_approval"   // true: 开播需管理员审批
	SettingAnnouncement     = "announcement"
	SettingRecordingEnabled = "recording_enabled" // 全站是否允许录像
	SettingTranscodeEnabled = "transcode_enabled" // 全站是否开启多清晰度转码
	SettingChatSlowModeSec  = "chat_slow_mode_sec"
	SettingChatBlockedWords = "chat_blocked_words" // 换行分隔

	// 内部密钥（内嵌媒体服务器模式自动生成，不对外暴露）
	SettingMediaAdminToken = "internal_media_admin_token"
	SettingHookSecret      = "internal_hook_secret"
	SettingMediaJWTSecret  = "internal_media_jwt_secret"
)

var settingDefaults = map[string]string{
	SettingSiteName:         "LiveHub",
	SettingRegistrationMode: "open",
	SettingStreamApproval:   "true",
	SettingAnnouncement:     "",
	SettingRecordingEnabled: "true",
	SettingTranscodeEnabled: "false",
	SettingChatSlowModeSec:  "0",
	SettingChatBlockedWords: "",
}

// PublicSettingKeys 管理后台可读写的设置。
var PublicSettingKeys = []string{
	SettingSiteName, SettingRegistrationMode, SettingStreamApproval, SettingAnnouncement,
	SettingRecordingEnabled, SettingTranscodeEnabled, SettingChatSlowModeSec, SettingChatBlockedWords,
}

func (s *Store) Setting(ctx context.Context, key string) (string, error) {
	var v string
	err := s.DB.QueryRowContext(ctx, `SELECT value FROM settings WHERE key = ?`, key).Scan(&v)
	if errors.Is(err, sql.ErrNoRows) {
		return settingDefaults[key], nil
	}
	return v, err
}

func (s *Store) SettingBool(ctx context.Context, key string) bool {
	v, _ := s.Setting(ctx, key)
	b, _ := strconv.ParseBool(v)
	return b
}

func (s *Store) SettingInt(ctx context.Context, key string) int {
	v, _ := s.Setting(ctx, key)
	n, _ := strconv.Atoi(v)
	return n
}

func (s *Store) SetSetting(ctx context.Context, key, value string) error {
	_, err := s.DB.ExecContext(ctx, `INSERT INTO settings(key, value) VALUES (?, ?)
		ON CONFLICT(key) DO UPDATE SET value = excluded.value`, key, value)
	return err
}

// AllPublicSettings 返回全部可管理设置（含默认值）。
func (s *Store) AllPublicSettings(ctx context.Context) (map[string]string, error) {
	out := make(map[string]string, len(PublicSettingKeys))
	for _, k := range PublicSettingKeys {
		v, err := s.Setting(ctx, k)
		if err != nil {
			return nil, err
		}
		out[k] = v
	}
	return out, nil
}

// EnsureSecret 读取内部密钥，不存在则生成并持久化（重启后保持不变）。
func (s *Store) EnsureSecret(ctx context.Context, key string) (string, error) {
	var v string
	err := s.DB.QueryRowContext(ctx, `SELECT value FROM settings WHERE key = ?`, key).Scan(&v)
	if err == nil && v != "" {
		return v, nil
	}
	if err != nil && !errors.Is(err, sql.ErrNoRows) {
		return "", err
	}
	v = RandomToken(24)
	if err := s.SetSetting(ctx, key, v); err != nil {
		return "", err
	}
	return v, nil
}
