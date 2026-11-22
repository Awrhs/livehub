package store

import (
	"context"
	"database/sql"
)

// ---------- 关注 ----------

func (s *Store) Follow(ctx context.Context, userID, channelID int64) error {
	_, err := s.DB.ExecContext(ctx, `INSERT OR IGNORE INTO follows(user_id, channel_id, created_at) VALUES (?, ?, ?)`,
		userID, channelID, s.now())
	return err
}

func (s *Store) Unfollow(ctx context.Context, userID, channelID int64) error {
	_, err := s.DB.ExecContext(ctx, `DELETE FROM follows WHERE user_id = ? AND channel_id = ?`, userID, channelID)
	return err
}

func (s *Store) IsFollowing(ctx context.Context, userID, channelID int64) bool {
	var n int
	_ = s.DB.QueryRowContext(ctx, `SELECT COUNT(*) FROM follows WHERE user_id = ? AND channel_id = ?`, userID, channelID).Scan(&n)
	return n > 0
}

func (s *Store) FollowerCount(ctx context.Context, channelID int64) int {
	var n int
	_ = s.DB.QueryRowContext(ctx, `SELECT COUNT(*) FROM follows WHERE channel_id = ?`, channelID).Scan(&n)
	return n
}

// FollowedChannels 用户关注的频道（直播中的在前）。
func (s *Store) FollowedChannels(ctx context.Context, userID int64) ([]*Channel, error) {
	rows, err := s.DB.QueryContext(ctx, `SELECT `+channelCols+channelFrom+`
		JOIN follows f ON f.channel_id = c.id WHERE f.user_id = ? AND u.banned = 0
		ORDER BY c.is_live DESC, COALESCE(c.live_started_at, 0) DESC, COALESCE(c.last_live_at, 0) DESC`, userID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []*Channel
	for rows.Next() {
		c, err := scanChannel(rows)
		if err != nil {
			return nil, err
		}
		out = append(out, c)
	}
	return out, rows.Err()
}

// ---------- 通知 ----------

type Notification struct {
	ID          int64   `json:"id"`
	Kind        string  `json:"kind"`
	ChannelSlug *string `json:"channel_slug"`
	ChannelName *string `json:"channel_name"`
	Body        string  `json:"body"`
	CreatedAt   int64   `json:"created_at"`
	ReadAt      *int64  `json:"read_at"`
}

// NotifyFollowers 给频道全部关注者发送开播通知（同一频道 10 分钟内只发一次，避免断线重连刷屏）。
func (s *Store) NotifyFollowers(ctx context.Context, channelID int64, body string) (int64, error) {
	now := s.now()
	res, err := s.DB.ExecContext(ctx, `INSERT INTO notifications(user_id, kind, channel_id, body, created_at)
		SELECT f.user_id, 'live', ?, ?, ? FROM follows f
		WHERE f.channel_id = ? AND NOT EXISTS (
			SELECT 1 FROM notifications n WHERE n.user_id = f.user_id AND n.channel_id = ? AND n.kind = 'live'
			AND n.created_at > ?)`, channelID, body, now, channelID, channelID, now-600)
	if err != nil {
		return 0, err
	}
	return res.RowsAffected()
}

func (s *Store) Notify(ctx context.Context, userID int64, kind string, channelID *int64, body string) error {
	var ch any
	if channelID != nil {
		ch = *channelID
	}
	_, err := s.DB.ExecContext(ctx, `INSERT INTO notifications(user_id, kind, channel_id, body, created_at)
		VALUES (?, ?, ?, ?, ?)`, userID, kind, ch, body, s.now())
	return err
}

func (s *Store) ListNotifications(ctx context.Context, userID int64, limit int) ([]Notification, int, error) {
	if limit <= 0 || limit > 100 {
		limit = 30
	}
	rows, err := s.DB.QueryContext(ctx, `SELECT n.id, n.kind, c.slug, u.display_name, n.body, n.created_at, n.read_at
		FROM notifications n LEFT JOIN channels c ON c.id = n.channel_id LEFT JOIN users u ON u.id = c.user_id
		WHERE n.user_id = ? ORDER BY n.id DESC LIMIT ?`, userID, limit)
	if err != nil {
		return nil, 0, err
	}
	defer rows.Close()
	out := []Notification{}
	for rows.Next() {
		var n Notification
		var slug, name sql.NullString
		var read sql.NullInt64
		if err := rows.Scan(&n.ID, &n.Kind, &slug, &name, &n.Body, &n.CreatedAt, &read); err != nil {
			return nil, 0, err
		}
		if slug.Valid {
			n.ChannelSlug, n.ChannelName = &slug.String, &name.String
		}
		n.ReadAt = nullInt(read)
		out = append(out, n)
	}
	if err := rows.Err(); err != nil {
		return nil, 0, err
	}
	unread := s.UnreadCount(ctx, userID)
	return out, unread, nil
}

func (s *Store) UnreadCount(ctx context.Context, userID int64) int {
	var n int
	_ = s.DB.QueryRowContext(ctx, `SELECT COUNT(*) FROM notifications WHERE user_id = ? AND read_at IS NULL`, userID).Scan(&n)
	return n
}

func (s *Store) MarkNotificationsRead(ctx context.Context, userID int64) error {
	_, err := s.DB.ExecContext(ctx, `UPDATE notifications SET read_at = ? WHERE user_id = ? AND read_at IS NULL`,
		s.now(), userID)
	return err
}

// PruneNotifications 删除 30 天前的通知。
func (s *Store) PruneNotifications(ctx context.Context) error {
	_, err := s.DB.ExecContext(ctx, `DELETE FROM notifications WHERE created_at < ?`, s.now()-30*86400)
	return err
}

// ---------- 频道禁言与房管 ----------

// ChannelBan 禁言（until 为 nil 表示永久）。
func (s *Store) ChannelBan(ctx context.Context, channelID, userID, by int64, until *int64) error {
	var u any
	if until != nil {
		u = *until
	}
	_, err := s.DB.ExecContext(ctx, `INSERT INTO channel_bans(channel_id, user_id, until, by_user, created_at)
		VALUES (?, ?, ?, ?, ?) ON CONFLICT(channel_id, user_id) DO UPDATE SET until = excluded.until,
		by_user = excluded.by_user, created_at = excluded.created_at`, channelID, userID, u, by, s.now())
	return err
}

func (s *Store) ChannelUnban(ctx context.Context, channelID, userID int64) error {
	_, err := s.DB.ExecContext(ctx, `DELETE FROM channel_bans WHERE channel_id = ? AND user_id = ?`, channelID, userID)
	return err
}

// ChannelBanUntil 返回禁言状态：banned=false 表示未禁言；until=nil 表示永久。
func (s *Store) ChannelBanUntil(ctx context.Context, channelID, userID int64) (bool, *int64) {
	var until sql.NullInt64
	err := s.DB.QueryRowContext(ctx, `SELECT until FROM channel_bans WHERE channel_id = ? AND user_id = ?`,
		channelID, userID).Scan(&until)
	if err != nil {
		return false, nil
	}
	if until.Valid && until.Int64 <= s.now() {
		return false, nil
	}
	return true, nullInt(until)
}

func (s *Store) AddModerator(ctx context.Context, channelID, userID int64) error {
	_, err := s.DB.ExecContext(ctx, `INSERT OR IGNORE INTO channel_moderators(channel_id, user_id, created_at)
		VALUES (?, ?, ?)`, channelID, userID, s.now())
	return err
}

func (s *Store) RemoveModerator(ctx context.Context, channelID, userID int64) error {
	_, err := s.DB.ExecContext(ctx, `DELETE FROM channel_moderators WHERE channel_id = ? AND user_id = ?`, channelID, userID)
	return err
}

func (s *Store) IsModerator(ctx context.Context, channelID, userID int64) bool {
	var n int
	_ = s.DB.QueryRowContext(ctx, `SELECT COUNT(*) FROM channel_moderators WHERE channel_id = ? AND user_id = ?`,
		channelID, userID).Scan(&n)
	return n > 0
}

func (s *Store) ListModerators(ctx context.Context, channelID int64) ([]ChatUser, error) {
	rows, err := s.DB.QueryContext(ctx, `SELECT u.id, u.username, u.display_name, u.role FROM channel_moderators m
		JOIN users u ON u.id = m.user_id WHERE m.channel_id = ? ORDER BY m.created_at`, channelID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []ChatUser{}
	for rows.Next() {
		var u ChatUser
		if err := rows.Scan(&u.ID, &u.Username, &u.DisplayName, &u.Role); err != nil {
			return nil, err
		}
		out = append(out, u)
	}
	return out, rows.Err()
}

// ---------- 举报 ----------

type Report struct {
	ID           int64   `json:"id"`
	ReporterName *string `json:"reporter_name"`
	ChannelSlug  string  `json:"channel_slug"`
	ChannelName  string  `json:"channel_name"`
	ChannelOwner int64   `json:"channel_owner_id"`
	Reason       string  `json:"reason"`
	Status       string  `json:"status"`
	CreatedAt    int64   `json:"created_at"`
	ResolvedAt   *int64  `json:"resolved_at"`
}

func (s *Store) CreateReport(ctx context.Context, reporter, channelID int64, reason string) error {
	// 同一用户对同一频道的未处理举报只保留一条
	var n int
	_ = s.DB.QueryRowContext(ctx, `SELECT COUNT(*) FROM reports WHERE reporter_id = ? AND channel_id = ? AND status = 'open'`,
		reporter, channelID).Scan(&n)
	if n > 0 {
		return ErrConflict
	}
	_, err := s.DB.ExecContext(ctx, `INSERT INTO reports(reporter_id, channel_id, reason, created_at) VALUES (?, ?, ?, ?)`,
		reporter, channelID, reason, s.now())
	return err
}

func (s *Store) ListReports(ctx context.Context, status string) ([]Report, error) {
	if status == "" {
		status = "open"
	}
	rows, err := s.DB.QueryContext(ctx, `SELECT r.id, ru.username, c.slug, cu.display_name, c.user_id, r.reason,
		r.status, r.created_at, r.resolved_at FROM reports r
		JOIN channels c ON c.id = r.channel_id JOIN users cu ON cu.id = c.user_id
		LEFT JOIN users ru ON ru.id = r.reporter_id WHERE r.status = ? ORDER BY r.id DESC LIMIT 200`, status)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []Report{}
	for rows.Next() {
		var r Report
		var name sql.NullString
		var resolved sql.NullInt64
		if err := rows.Scan(&r.ID, &name, &r.ChannelSlug, &r.ChannelName, &r.ChannelOwner, &r.Reason, &r.Status,
			&r.CreatedAt, &resolved); err != nil {
			return nil, err
		}
		if name.Valid {
			r.ReporterName = &name.String
		}
		r.ResolvedAt = nullInt(resolved)
		out = append(out, r)
	}
	return out, rows.Err()
}

func (s *Store) ResolveReport(ctx context.Context, id, by int64, status string) error {
	res, err := s.DB.ExecContext(ctx, `UPDATE reports SET status = ?, resolved_by = ?, resolved_at = ?
		WHERE id = ? AND status = 'open'`, status, by, s.now(), id)
	if err != nil {
		return err
	}
	if n, _ := res.RowsAffected(); n == 0 {
		return ErrNotFound
	}
	return nil
}

func (s *Store) OpenReportCount(ctx context.Context) int {
	var n int
	_ = s.DB.QueryRowContext(ctx, `SELECT COUNT(*) FROM reports WHERE status = 'open'`).Scan(&n)
	return n
}
