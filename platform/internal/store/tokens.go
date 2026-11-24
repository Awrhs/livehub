package store

import (
	"context"
	"database/sql"
	"errors"
	"strings"
	"time"
)

const (
	TokenVerifyEmail   = "verify_email"
	TokenResetPassword = "reset_password"
)

// SetEmail 设置（更换）邮箱，状态置为未验证；邮箱已被他人使用返回 ErrConflict。
func (s *Store) SetEmail(ctx context.Context, userID int64, email string) error {
	_, err := s.DB.ExecContext(ctx, `UPDATE users SET email = ?, email_verified = 0 WHERE id = ?`, email, userID)
	if isUniqueErr(err) {
		return ErrConflict
	}
	return err
}

// UserByEmail 按已设置的邮箱查找（不区分大小写）。
func (s *Store) UserByEmail(ctx context.Context, email string) (*User, error) {
	if strings.TrimSpace(email) == "" {
		return nil, ErrNotFound
	}
	return scanUser(s.DB.QueryRowContext(ctx, `SELECT `+userCols+` FROM users WHERE email = ? COLLATE NOCASE`, email))
}

// CreateToken 生成一次性令牌（同一用户同一用途的旧令牌作废），返回明文。
func (s *Store) CreateToken(ctx context.Context, userID int64, purpose, email string, ttl time.Duration) (string, error) {
	tok := RandomToken(24)
	now := s.now()
	err := s.Tx(ctx, func(tx *sql.Tx) error {
		if _, err := tx.ExecContext(ctx, `DELETE FROM user_tokens WHERE user_id = ? AND purpose = ?`, userID, purpose); err != nil {
			return err
		}
		_, err := tx.ExecContext(ctx, `INSERT INTO user_tokens(token_hash, user_id, purpose, email, created_at, expires_at)
			VALUES (?, ?, ?, ?, ?, ?)`, HashToken(tok), userID, purpose, email, now, now+int64(ttl/time.Second))
		return err
	})
	return tok, err
}

// LastTokenAt 该用户该用途最近一次生成令牌的时间（限制发信频率）；没有返回 0。
func (s *Store) LastTokenAt(ctx context.Context, userID int64, purpose string) int64 {
	var t sql.NullInt64
	_ = s.DB.QueryRowContext(ctx, `SELECT MAX(created_at) FROM user_tokens WHERE user_id = ? AND purpose = ?`,
		userID, purpose).Scan(&t)
	return t.Int64
}

// ConsumeToken 校验并作废令牌，返回用户 ID 与令牌关联的邮箱；无效或过期返回 ErrNotFound。
func (s *Store) ConsumeToken(ctx context.Context, token, purpose string) (int64, string, error) {
	var uid int64
	var email string
	err := s.Tx(ctx, func(tx *sql.Tx) error {
		err := tx.QueryRowContext(ctx, `SELECT user_id, email FROM user_tokens
			WHERE token_hash = ? AND purpose = ? AND expires_at > ?`, HashToken(token), purpose, s.now()).Scan(&uid, &email)
		if errors.Is(err, sql.ErrNoRows) {
			return ErrNotFound
		}
		if err != nil {
			return err
		}
		_, err = tx.ExecContext(ctx, `DELETE FROM user_tokens WHERE token_hash = ?`, HashToken(token))
		return err
	})
	return uid, email, err
}

// MarkEmailVerified 仅当用户当前邮箱仍是令牌对应的地址时标记为已验证（防止换邮箱后旧链接生效）。
func (s *Store) MarkEmailVerified(ctx context.Context, userID int64, email string) (bool, error) {
	res, err := s.DB.ExecContext(ctx, `UPDATE users SET email_verified = 1 WHERE id = ? AND email = ? COLLATE NOCASE`,
		userID, email)
	if err != nil {
		return false, err
	}
	n, _ := res.RowsAffected()
	return n == 1, nil
}
