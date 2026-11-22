package store

import (
	"context"
	"database/sql"
	"errors"
)

type VOD struct {
	ID          int64   `json:"id"`
	ChannelID   int64   `json:"channel_id"`
	ChannelSlug string  `json:"channel_slug"`
	DisplayName string  `json:"display_name"`
	OwnerID     int64   `json:"-"`
	Title       string  `json:"title"`
	Category    string  `json:"category"`
	File        string  `json:"-"`
	DurationSec float64 `json:"duration_sec"`
	SizeBytes   int64   `json:"size_bytes"`
	Views       int64   `json:"views"`
	CreatedAt   int64   `json:"created_at"`
}

const vodCols = `v.id, v.channel_id, c.slug, u.display_name, c.user_id, v.title, v.category, v.file,
	v.duration_sec, v.size_bytes, v.views, v.created_at`
const vodFrom = ` FROM vods v JOIN channels c ON c.id = v.channel_id JOIN users u ON u.id = c.user_id `

func scanVOD(r scanner) (*VOD, error) {
	var v VOD
	err := r.Scan(&v.ID, &v.ChannelID, &v.ChannelSlug, &v.DisplayName, &v.OwnerID, &v.Title, &v.Category, &v.File,
		&v.DurationSec, &v.SizeBytes, &v.Views, &v.CreatedAt)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, ErrNotFound
	}
	return &v, err
}

// CreateVOD 登记录像；返回 ID。
func (s *Store) CreateVOD(ctx context.Context, channelID int64, sessionID int64, title, category, file string,
	duration float64, size int64) (int64, error) {
	var sess any
	if sessionID != 0 {
		sess = sessionID
	}
	res, err := s.DB.ExecContext(ctx, `INSERT INTO vods(channel_id, session_id, title, category, file, duration_sec,
		size_bytes, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)`, channelID, sess, title, category, file, duration, size, s.now())
	if err != nil {
		return 0, err
	}
	return res.LastInsertId()
}

func (s *Store) VODByID(ctx context.Context, id int64) (*VOD, error) {
	return scanVOD(s.DB.QueryRowContext(ctx, `SELECT `+vodCols+vodFrom+`WHERE v.id = ?`, id))
}

func (s *Store) ListVODs(ctx context.Context, channelID int64, limit, offset int) ([]*VOD, error) {
	if limit <= 0 || limit > 100 {
		limit = 30
	}
	where, args := "u.banned = 0", []any{}
	if channelID != 0 {
		where += " AND v.channel_id = ?"
		args = append(args, channelID)
	}
	rows, err := s.DB.QueryContext(ctx, `SELECT `+vodCols+vodFrom+`WHERE `+where+` ORDER BY v.id DESC LIMIT ? OFFSET ?`,
		append(args, limit, offset)...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []*VOD{}
	for rows.Next() {
		v, err := scanVOD(rows)
		if err != nil {
			return nil, err
		}
		out = append(out, v)
	}
	return out, rows.Err()
}

func (s *Store) DeleteVOD(ctx context.Context, id int64) error {
	_, err := s.DB.ExecContext(ctx, `DELETE FROM vods WHERE id = ?`, id)
	return err
}

func (s *Store) RenameVOD(ctx context.Context, id int64, title string) error {
	_, err := s.DB.ExecContext(ctx, `UPDATE vods SET title = ? WHERE id = ?`, title, id)
	return err
}

func (s *Store) IncVODViews(ctx context.Context, id int64) {
	_, _ = s.DB.ExecContext(ctx, `UPDATE vods SET views = views + 1 WHERE id = ?`, id)
}

// SessionInfo 场次的标题与分类（录像沿用开播时的标题）。
func (s *Store) SessionInfo(ctx context.Context, id int64) (title, category string, err error) {
	err = s.DB.QueryRowContext(ctx, `SELECT title, category FROM stream_sessions WHERE id = ?`, id).Scan(&title, &category)
	return
}
