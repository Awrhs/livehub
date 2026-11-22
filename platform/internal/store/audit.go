package store

import (
	"context"
	"database/sql"
)

type AuditEntry struct {
	ID        int64  `json:"id"`
	ActorID   *int64 `json:"actor_id"`
	ActorName string `json:"actor_name"`
	Action    string `json:"action"`
	Target    string `json:"target"`
	Detail    string `json:"detail"`
	CreatedAt int64  `json:"created_at"`
}

// Audit 记录管理操作；actorID 为 0 表示系统。
func (s *Store) Audit(ctx context.Context, actorID int64, action, target, detail string) {
	var actor any
	if actorID != 0 {
		actor = actorID
	}
	// 审计失败不影响主流程
	_, _ = s.DB.ExecContext(ctx, `INSERT INTO audit_log(actor_id, action, target, detail, created_at)
		VALUES (?, ?, ?, ?, ?)`, actor, action, target, detail, s.now())
}

func (s *Store) ListAudit(ctx context.Context, limit, offset int) ([]AuditEntry, error) {
	if limit <= 0 || limit > 200 {
		limit = 100
	}
	rows, err := s.DB.QueryContext(ctx, `SELECT a.id, a.actor_id, COALESCE(u.username, ''), a.action, a.target,
		a.detail, a.created_at FROM audit_log a LEFT JOIN users u ON u.id = a.actor_id
		ORDER BY a.id DESC LIMIT ? OFFSET ?`, limit, offset)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []AuditEntry{}
	for rows.Next() {
		var e AuditEntry
		var actor sql.NullInt64
		if err := rows.Scan(&e.ID, &actor, &e.ActorName, &e.Action, &e.Target, &e.Detail, &e.CreatedAt); err != nil {
			return nil, err
		}
		e.ActorID = nullInt(actor)
		out = append(out, e)
	}
	return out, rows.Err()
}

type Invite struct {
	Code      string `json:"code"`
	CreatedBy *int64 `json:"created_by"`
	UsedBy    *int64 `json:"used_by"`
	UsedByName string `json:"used_by_name"`
	CreatedAt int64  `json:"created_at"`
	ExpiresAt *int64 `json:"expires_at"`
	UsedAt    *int64 `json:"used_at"`
}

func (s *Store) CreateInvite(ctx context.Context, by int64, expiresAt *int64) (*Invite, error) {
	inv := &Invite{Code: RandomToken(8), CreatedBy: &by, CreatedAt: s.now(), ExpiresAt: expiresAt}
	var exp any
	if expiresAt != nil {
		exp = *expiresAt
	}
	_, err := s.DB.ExecContext(ctx, `INSERT INTO invites(code, created_by, created_at, expires_at) VALUES (?, ?, ?, ?)`,
		inv.Code, by, inv.CreatedAt, exp)
	return inv, err
}

func (s *Store) ListInvites(ctx context.Context) ([]Invite, error) {
	rows, err := s.DB.QueryContext(ctx, `SELECT i.code, i.created_by, i.used_by, COALESCE(u.username, ''),
		i.created_at, i.expires_at, i.used_at FROM invites i LEFT JOIN users u ON u.id = i.used_by
		ORDER BY i.created_at DESC LIMIT 200`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []Invite{}
	for rows.Next() {
		var inv Invite
		var by, used, exp, usedAt sql.NullInt64
		if err := rows.Scan(&inv.Code, &by, &used, &inv.UsedByName, &inv.CreatedAt, &exp, &usedAt); err != nil {
			return nil, err
		}
		inv.CreatedBy, inv.UsedBy, inv.ExpiresAt, inv.UsedAt = nullInt(by), nullInt(used), nullInt(exp), nullInt(usedAt)
		out = append(out, inv)
	}
	return out, rows.Err()
}

func (s *Store) DeleteInvite(ctx context.Context, code string) error {
	res, err := s.DB.ExecContext(ctx, `DELETE FROM invites WHERE code = ? AND used_by IS NULL`, code)
	if err != nil {
		return err
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return ErrNotFound
	}
	return nil
}

// InviteValid 邀请码是否可用（注册前预检；最终以事务内核销为准）。
func (s *Store) InviteValid(ctx context.Context, code string) bool {
	var n int
	err := s.DB.QueryRowContext(ctx, `SELECT COUNT(*) FROM invites WHERE code = ? AND used_by IS NULL
		AND (expires_at IS NULL OR expires_at > ?)`, code, s.now()).Scan(&n)
	return err == nil && n == 1
}
