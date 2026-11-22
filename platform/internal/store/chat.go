package store

import "context"

type ChatUser struct {
	ID          int64  `json:"id"`
	Username    string `json:"username"`
	DisplayName string `json:"display_name"`
	Role        string `json:"role"`
}

type ChatMessage struct {
	ID        int64    `json:"id"`
	ChannelID int64    `json:"-"`
	User      ChatUser `json:"user"`
	Body      string   `json:"body"`
	CreatedAt int64    `json:"ts"`
}

func (s *Store) InsertChat(ctx context.Context, channelID int64, u ChatUser, body string) (*ChatMessage, error) {
	now := s.now()
	res, err := s.DB.ExecContext(ctx, `INSERT INTO chat_messages(channel_id, user_id, body, created_at) VALUES (?, ?, ?, ?)`,
		channelID, u.ID, body, now)
	if err != nil {
		return nil, err
	}
	id, _ := res.LastInsertId()
	return &ChatMessage{ID: id, ChannelID: channelID, User: u, Body: body, CreatedAt: now}, nil
}

// RecentChat 最近 limit 条未删除消息，按时间正序。
func (s *Store) RecentChat(ctx context.Context, channelID int64, limit int) ([]*ChatMessage, error) {
	if limit <= 0 || limit > 200 {
		limit = 50
	}
	rows, err := s.DB.QueryContext(ctx, `SELECT m.id, m.body, m.created_at, u.id, u.username, u.display_name, u.role
		FROM chat_messages m JOIN users u ON u.id = m.user_id
		WHERE m.channel_id = ? AND m.deleted = 0 ORDER BY m.id DESC LIMIT ?`, channelID, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []*ChatMessage
	for rows.Next() {
		m := &ChatMessage{ChannelID: channelID}
		if err := rows.Scan(&m.ID, &m.Body, &m.CreatedAt, &m.User.ID, &m.User.Username, &m.User.DisplayName,
			&m.User.Role); err != nil {
			return nil, err
		}
		out = append(out, m)
	}
	// 反转为正序
	for i, j := 0, len(out)-1; i < j; i, j = i+1, j-1 {
		out[i], out[j] = out[j], out[i]
	}
	return out, rows.Err()
}

// DeleteChat 软删除消息，返回消息所属频道；不存在返回 ErrNotFound。
func (s *Store) DeleteChat(ctx context.Context, id int64) (int64, error) {
	var ch int64
	if err := s.DB.QueryRowContext(ctx, `SELECT channel_id FROM chat_messages WHERE id = ?`, id).Scan(&ch); err != nil {
		return 0, ErrNotFound
	}
	_, err := s.DB.ExecContext(ctx, `UPDATE chat_messages SET deleted = 1 WHERE id = ?`, id)
	return ch, err
}

// PruneChat 每个频道只保留最近 keep 条消息。
func (s *Store) PruneChat(ctx context.Context, keep int) error {
	_, err := s.DB.ExecContext(ctx, `DELETE FROM chat_messages WHERE id IN (
		SELECT id FROM (SELECT id, ROW_NUMBER() OVER (PARTITION BY channel_id ORDER BY id DESC) AS rn
		FROM chat_messages) WHERE rn > ?)`, keep)
	return err
}
