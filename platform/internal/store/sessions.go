package store

import (
	"context"
	"time"
)

const SessionTTL = 30 * 24 * time.Hour

// CreateSession 创建会话，返回明文 token（只在此处出现一次）。
func (s *Store) CreateSession(ctx context.Context, userID int64, ip, ua string) (string, error) {
	tok := RandomToken(32)
	now := s.now()
	if len(ua) > 256 {
		ua = ua[:256]
	}
	_, err := s.DB.ExecContext(ctx, `INSERT INTO sessions(token_hash, user_id, created_at, expires_at, ip, user_agent)
		VALUES (?, ?, ?, ?, ?, ?)`, HashToken(tok), userID, now, now+int64(SessionTTL/time.Second), ip, ua)
	if err != nil {
		return "", err
	}
	return tok, nil
}

// UserBySession 校验会话 token，返回用户；过期或不存在返回 ErrNotFound。
func (s *Store) UserBySession(ctx context.Context, token string) (*User, error) {
	if token == "" {
		return nil, ErrNotFound
	}
	return scanUser(s.DB.QueryRowContext(ctx, `SELECT u.id, u.username, u.display_name, u.password_hash, u.role,
		u.stream_status, u.stream_request_note, u.bio, u.banned, u.ban_reason, u.created_at, u.last_login_at
		FROM sessions s JOIN users u ON u.id = s.user_id
		WHERE s.token_hash = ? AND s.expires_at > ?`, HashToken(token), s.now()))
}

func (s *Store) DeleteSession(ctx context.Context, token string) error {
	_, err := s.DB.ExecContext(ctx, `DELETE FROM sessions WHERE token_hash = ?`, HashToken(token))
	return err
}

// DeleteUserSessions 注销用户全部会话（改密码、封禁时调用）；keep 非空时保留该会话。
func (s *Store) DeleteUserSessions(ctx context.Context, userID int64, keep string) error {
	_, err := s.DB.ExecContext(ctx, `DELETE FROM sessions WHERE user_id = ? AND token_hash != ?`,
		userID, HashToken(keep))
	return err
}

// PurgeExpiredSessions 清理过期会话。
func (s *Store) PurgeExpiredSessions(ctx context.Context) error {
	_, err := s.DB.ExecContext(ctx, `DELETE FROM sessions WHERE expires_at <= ?`, s.now())
	return err
}
