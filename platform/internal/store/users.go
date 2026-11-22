package store

import (
	"context"
	"database/sql"
	"errors"
	"strings"
)

const (
	RoleAdmin     = "admin"
	RoleModerator = "moderator"
	RoleUser      = "user"

	StreamNone     = "none"
	StreamPending  = "pending"
	StreamApproved = "approved"
	StreamRejected = "rejected"
)

type User struct {
	ID                int64  `json:"id"`
	Username          string `json:"username"`
	DisplayName       string `json:"display_name"`
	PasswordHash      string `json:"-"`
	Role              string `json:"role"`
	StreamStatus      string `json:"stream_status"`
	StreamRequestNote string `json:"stream_request_note"`
	Bio               string `json:"bio"`
	Banned            bool   `json:"banned"`
	BanReason         string `json:"ban_reason"`
	CreatedAt         int64  `json:"created_at"`
	LastLoginAt       *int64 `json:"last_login_at"`
}

// IsStaff 管理员或站务（可处理聊天、审核）。
func (u *User) IsStaff() bool { return u.Role == RoleAdmin || u.Role == RoleModerator }

const userCols = `id, username, display_name, password_hash, role, stream_status,
	stream_request_note, bio, banned, ban_reason, created_at, last_login_at`

type scanner interface{ Scan(dest ...any) error }

func scanUser(r scanner) (*User, error) {
	var u User
	var last sql.NullInt64
	err := r.Scan(&u.ID, &u.Username, &u.DisplayName, &u.PasswordHash, &u.Role, &u.StreamStatus,
		&u.StreamRequestNote, &u.Bio, &u.Banned, &u.BanReason, &u.CreatedAt, &last)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, ErrNotFound
	}
	if err != nil {
		return nil, err
	}
	u.LastLoginAt = nullInt(last)
	return &u, nil
}

// CountUsers 用户总数（为 0 时首个注册者成为管理员）。
func (s *Store) CountUsers(ctx context.Context) (int, error) {
	var n int
	err := s.DB.QueryRowContext(ctx, `SELECT COUNT(*) FROM users`).Scan(&n)
	return n, err
}

// CreateUserWithChannel 在同一事务中创建用户及其频道。
// 若库中尚无用户，则该用户成为管理员（避免并发注册时出现两个"首个用户"）。
// invite 非空时同时核销邀请码（失效或已用返回 ErrNotFound）。
func (s *Store) CreateUserWithChannel(ctx context.Context, username, displayName, passwordHash, invite string) (*User, error) {
	now := s.now()
	var created *User
	err := s.Tx(ctx, func(tx *sql.Tx) error {
		var n int
		if err := tx.QueryRowContext(ctx, `SELECT COUNT(*) FROM users`).Scan(&n); err != nil {
			return err
		}
		role := RoleUser
		if n == 0 {
			role = RoleAdmin
		}
		res, err := tx.ExecContext(ctx, `INSERT INTO users(username, display_name, password_hash, role, created_at)
			VALUES (?, ?, ?, ?, ?)`, username, displayName, passwordHash, role, now)
		if isUniqueErr(err) {
			return ErrConflict
		}
		if err != nil {
			return err
		}
		id, _ := res.LastInsertId()
		if _, err := tx.ExecContext(ctx, `INSERT INTO channels(user_id, slug, title, stream_key, created_at)
			VALUES (?, ?, ?, ?, ?)`, id, username, displayName+" 的直播间", NewStreamKey(), now); err != nil {
			if isUniqueErr(err) {
				return ErrConflict
			}
			return err
		}
		if invite != "" {
			r, err := tx.ExecContext(ctx, `UPDATE invites SET used_by = ?, used_at = ?
				WHERE code = ? AND used_by IS NULL AND (expires_at IS NULL OR expires_at > ?)`,
				id, now, invite, now)
			if err != nil {
				return err
			}
			if k, _ := r.RowsAffected(); k != 1 {
				return ErrNotFound
			}
		}
		created, err = scanUser(tx.QueryRowContext(ctx, `SELECT `+userCols+` FROM users WHERE id = ?`, id))
		return err
	})
	if err != nil {
		return nil, err
	}
	return created, nil
}

func (s *Store) UserByID(ctx context.Context, id int64) (*User, error) {
	return scanUser(s.DB.QueryRowContext(ctx, `SELECT `+userCols+` FROM users WHERE id = ?`, id))
}

func (s *Store) UserByName(ctx context.Context, username string) (*User, error) {
	return scanUser(s.DB.QueryRowContext(ctx, `SELECT `+userCols+` FROM users WHERE username = ?`, username))
}

func (s *Store) TouchLogin(ctx context.Context, id int64) error {
	_, err := s.DB.ExecContext(ctx, `UPDATE users SET last_login_at = ? WHERE id = ?`, s.now(), id)
	return err
}

func (s *Store) UpdateProfile(ctx context.Context, id int64, displayName, bio string) error {
	_, err := s.DB.ExecContext(ctx, `UPDATE users SET display_name = ?, bio = ? WHERE id = ?`, displayName, bio, id)
	return err
}

func (s *Store) UpdatePassword(ctx context.Context, id int64, hash string) error {
	_, err := s.DB.ExecContext(ctx, `UPDATE users SET password_hash = ? WHERE id = ?`, hash, id)
	return err
}

// RequestStream 提交开播申请（已批准的不改变状态）。
func (s *Store) RequestStream(ctx context.Context, id int64, note string) error {
	_, err := s.DB.ExecContext(ctx, `UPDATE users SET stream_status = 'pending', stream_request_note = ?
		WHERE id = ? AND stream_status != 'approved'`, note, id)
	return err
}

// UserFilter 管理后台用户列表筛选。
type UserFilter struct {
	Query        string
	StreamStatus string
	Role         string
	Banned       *bool
	Limit        int
	Offset       int
}

func (s *Store) ListUsers(ctx context.Context, f UserFilter) ([]*User, int, error) {
	where := []string{"1=1"}
	var args []any
	if q := strings.TrimSpace(f.Query); q != "" {
		where = append(where, "(username LIKE ? OR display_name LIKE ?)")
		like := "%" + q + "%"
		args = append(args, like, like)
	}
	if f.StreamStatus != "" {
		where = append(where, "stream_status = ?")
		args = append(args, f.StreamStatus)
	}
	if f.Role != "" {
		where = append(where, "role = ?")
		args = append(args, f.Role)
	}
	if f.Banned != nil {
		where = append(where, "banned = ?")
		args = append(args, *f.Banned)
	}
	cond := strings.Join(where, " AND ")
	var total int
	if err := s.DB.QueryRowContext(ctx, `SELECT COUNT(*) FROM users WHERE `+cond, args...).Scan(&total); err != nil {
		return nil, 0, err
	}
	if f.Limit <= 0 || f.Limit > 200 {
		f.Limit = 50
	}
	rows, err := s.DB.QueryContext(ctx, `SELECT `+userCols+` FROM users WHERE `+cond+
		` ORDER BY id DESC LIMIT ? OFFSET ?`, append(args, f.Limit, f.Offset)...)
	if err != nil {
		return nil, 0, err
	}
	defer rows.Close()
	var out []*User
	for rows.Next() {
		u, err := scanUser(rows)
		if err != nil {
			return nil, 0, err
		}
		out = append(out, u)
	}
	return out, total, rows.Err()
}

// UserPatch 管理员修改用户；nil 字段不修改。
type UserPatch struct {
	Role         *string
	StreamStatus *string
	Banned       *bool
	BanReason    *string
}

func (s *Store) PatchUser(ctx context.Context, id int64, p UserPatch) error {
	sets := []string{}
	var args []any
	if p.Role != nil {
		sets = append(sets, "role = ?")
		args = append(args, *p.Role)
	}
	if p.StreamStatus != nil {
		sets = append(sets, "stream_status = ?")
		args = append(args, *p.StreamStatus)
	}
	if p.Banned != nil {
		sets = append(sets, "banned = ?")
		args = append(args, *p.Banned)
	}
	if p.BanReason != nil {
		sets = append(sets, "ban_reason = ?")
		args = append(args, *p.BanReason)
	}
	if len(sets) == 0 {
		return nil
	}
	args = append(args, id)
	res, err := s.DB.ExecContext(ctx, `UPDATE users SET `+strings.Join(sets, ", ")+` WHERE id = ?`, args...)
	if err != nil {
		return err
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return ErrNotFound
	}
	return nil
}

// CountAdmins 用于防止撤销最后一个管理员。
func (s *Store) CountAdmins(ctx context.Context) (int, error) {
	var n int
	err := s.DB.QueryRowContext(ctx, `SELECT COUNT(*) FROM users WHERE role = 'admin' AND banned = 0`).Scan(&n)
	return n, err
}
