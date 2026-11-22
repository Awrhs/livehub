package store

import (
	"context"
	"database/sql"
	"errors"
	"strings"
)

type Channel struct {
	ID               int64  `json:"id"`
	UserID           int64  `json:"user_id"`
	Slug             string `json:"slug"`
	Title            string `json:"title"`
	Category         string `json:"category"`
	Description      string `json:"description"`
	StreamKey        string `json:"-"`
	IsLive           bool   `json:"is_live"`
	LiveStartedAt    *int64 `json:"live_started_at"`
	LastLiveAt       *int64 `json:"last_live_at"`
	CurrentSessionID *int64 `json:"-"`
	RecordEnabled    bool   `json:"record_enabled"`
	CreatedAt        int64  `json:"created_at"`

	// 关联的主播信息
	DisplayName string `json:"display_name"`
	OwnerBanned bool   `json:"-"`
}

// NewStreamKey 生成推流密钥。
func NewStreamKey() string { return "lh_" + RandomToken(16) }

const channelCols = `c.id, c.user_id, c.slug, c.title, c.category, c.description, c.stream_key, c.is_live,
	c.live_started_at, c.last_live_at, c.current_session_id, c.record_enabled, c.created_at,
	u.display_name, u.banned`

const channelFrom = ` FROM channels c JOIN users u ON u.id = c.user_id `

func scanChannel(r scanner) (*Channel, error) {
	var c Channel
	var started, last, sess sql.NullInt64
	err := r.Scan(&c.ID, &c.UserID, &c.Slug, &c.Title, &c.Category, &c.Description, &c.StreamKey, &c.IsLive,
		&started, &last, &sess, &c.RecordEnabled, &c.CreatedAt, &c.DisplayName, &c.OwnerBanned)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, ErrNotFound
	}
	if err != nil {
		return nil, err
	}
	c.LiveStartedAt = nullInt(started)
	c.LastLiveAt = nullInt(last)
	c.CurrentSessionID = nullInt(sess)
	return &c, nil
}

func (s *Store) ChannelBySlug(ctx context.Context, slug string) (*Channel, error) {
	return scanChannel(s.DB.QueryRowContext(ctx, `SELECT `+channelCols+channelFrom+`WHERE c.slug = ?`, slug))
}

func (s *Store) ChannelByUser(ctx context.Context, userID int64) (*Channel, error) {
	return scanChannel(s.DB.QueryRowContext(ctx, `SELECT `+channelCols+channelFrom+`WHERE c.user_id = ?`, userID))
}

func (s *Store) ChannelByID(ctx context.Context, id int64) (*Channel, error) {
	return scanChannel(s.DB.QueryRowContext(ctx, `SELECT `+channelCols+channelFrom+`WHERE c.id = ?`, id))
}

type ChannelFilter struct {
	LiveOnly bool
	Query    string
	Category string
	Limit    int
	Offset   int
}

// ListChannels 直播中的在前（按开播时间倒序），其余按最近直播时间倒序。不含被封禁主播。
func (s *Store) ListChannels(ctx context.Context, f ChannelFilter) ([]*Channel, error) {
	where := []string{"u.banned = 0"}
	var args []any
	if f.LiveOnly {
		where = append(where, "c.is_live = 1")
	}
	if q := strings.TrimSpace(f.Query); q != "" {
		where = append(where, "(c.slug LIKE ? OR c.title LIKE ? OR u.display_name LIKE ?)")
		like := "%" + q + "%"
		args = append(args, like, like, like)
	}
	if f.Category != "" {
		where = append(where, "c.category = ?")
		args = append(args, f.Category)
	}
	if f.Limit <= 0 || f.Limit > 100 {
		f.Limit = 60
	}
	rows, err := s.DB.QueryContext(ctx, `SELECT `+channelCols+channelFrom+`WHERE `+strings.Join(where, " AND ")+
		` ORDER BY c.is_live DESC, COALESCE(c.live_started_at, 0) DESC, COALESCE(c.last_live_at, 0) DESC, c.id DESC
		LIMIT ? OFFSET ?`, append(args, f.Limit, f.Offset)...)
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

// LiveChannels 当前标记为直播中的频道（对账用，含被封禁者）。
func (s *Store) LiveChannels(ctx context.Context) ([]*Channel, error) {
	rows, err := s.DB.QueryContext(ctx, `SELECT `+channelCols+channelFrom+`WHERE c.is_live = 1`)
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

// Categories 现有分类及直播中频道数。
type CategoryCount struct {
	Name string `json:"name"`
	Live int    `json:"live"`
}

func (s *Store) Categories(ctx context.Context) ([]CategoryCount, error) {
	rows, err := s.DB.QueryContext(ctx, `SELECT c.category, SUM(c.is_live)`+channelFrom+
		`WHERE c.category != '' AND u.banned = 0 GROUP BY c.category ORDER BY SUM(c.is_live) DESC, COUNT(*) DESC LIMIT 50`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []CategoryCount{}
	for rows.Next() {
		var c CategoryCount
		if err := rows.Scan(&c.Name, &c.Live); err != nil {
			return nil, err
		}
		out = append(out, c)
	}
	return out, rows.Err()
}

func (s *Store) UpdateChannel(ctx context.Context, id int64, title, category, description string, record bool) error {
	_, err := s.DB.ExecContext(ctx, `UPDATE channels SET title = ?, category = ?, description = ?, record_enabled = ?
		WHERE id = ?`, title, category, description, record, id)
	return err
}

func (s *Store) ResetStreamKey(ctx context.Context, id int64) (string, error) {
	k := NewStreamKey()
	_, err := s.DB.ExecContext(ctx, `UPDATE channels SET stream_key = ? WHERE id = ?`, k, id)
	return k, err
}

// SetLive 标记开播：创建场次记录。已在直播中则返回 false（幂等）。
func (s *Store) SetLive(ctx context.Context, channelID int64) (bool, error) {
	changed := false
	now := s.now()
	err := s.Tx(ctx, func(tx *sql.Tx) error {
		var live bool
		var title, category string
		if err := tx.QueryRowContext(ctx, `SELECT is_live, title, category FROM channels WHERE id = ?`, channelID).
			Scan(&live, &title, &category); err != nil {
			if errors.Is(err, sql.ErrNoRows) {
				return ErrNotFound
			}
			return err
		}
		if live {
			return nil
		}
		res, err := tx.ExecContext(ctx, `INSERT INTO stream_sessions(channel_id, title, category, started_at)
			VALUES (?, ?, ?, ?)`, channelID, title, category, now)
		if err != nil {
			return err
		}
		sid, _ := res.LastInsertId()
		if _, err := tx.ExecContext(ctx, `UPDATE channels SET is_live = 1, live_started_at = ?, last_live_at = ?,
			current_session_id = ? WHERE id = ?`, now, now, sid, channelID); err != nil {
			return err
		}
		changed = true
		return nil
	})
	return changed, err
}

// SetOffline 标记下播并结束场次。未在直播返回 (0, false)；否则返回结束的场次 ID。
func (s *Store) SetOffline(ctx context.Context, channelID int64) (int64, bool, error) {
	var sid int64
	changed := false
	now := s.now()
	err := s.Tx(ctx, func(tx *sql.Tx) error {
		var live bool
		var cur sql.NullInt64
		if err := tx.QueryRowContext(ctx, `SELECT is_live, current_session_id FROM channels WHERE id = ?`, channelID).
			Scan(&live, &cur); err != nil {
			if errors.Is(err, sql.ErrNoRows) {
				return ErrNotFound
			}
			return err
		}
		if !live {
			return nil
		}
		if _, err := tx.ExecContext(ctx, `UPDATE channels SET is_live = 0, live_started_at = NULL, last_live_at = ?,
			current_session_id = NULL WHERE id = ?`, now, channelID); err != nil {
			return err
		}
		if cur.Valid {
			sid = cur.Int64
			if _, err := tx.ExecContext(ctx, `UPDATE stream_sessions SET ended_at = ? WHERE id = ?`, now, sid); err != nil {
				return err
			}
		}
		changed = true
		return nil
	})
	return sid, changed, err
}

// UpdatePeakViewers 更新当前场次峰值观众数。
func (s *Store) UpdatePeakViewers(ctx context.Context, sessionID int64, viewers int) error {
	_, err := s.DB.ExecContext(ctx, `UPDATE stream_sessions SET peak_viewers = MAX(peak_viewers, ?) WHERE id = ?`,
		viewers, sessionID)
	return err
}

// ResetAllLive 启动时调用：媒体服务器重启后所有推流都已断开。
func (s *Store) ResetAllLive(ctx context.Context) error {
	live, err := s.LiveChannels(ctx)
	if err != nil {
		return err
	}
	for _, c := range live {
		if _, _, err := s.SetOffline(ctx, c.ID); err != nil {
			return err
		}
	}
	return nil
}
