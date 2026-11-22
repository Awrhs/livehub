-- 阶段 2：关注、通知、录像回放、频道聊天管理、举报。

CREATE TABLE follows (
    user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    channel_id INTEGER NOT NULL REFERENCES channels(id) ON DELETE CASCADE,
    created_at INTEGER NOT NULL,
    PRIMARY KEY (user_id, channel_id)
);
CREATE INDEX follows_channel ON follows(channel_id);

CREATE TABLE notifications (
    id         INTEGER PRIMARY KEY,
    user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    kind       TEXT    NOT NULL,                -- live | stream_approved | stream_rejected | report_resolved
    channel_id INTEGER REFERENCES channels(id) ON DELETE CASCADE,
    body       TEXT    NOT NULL DEFAULT '',
    created_at INTEGER NOT NULL,
    read_at    INTEGER
);
CREATE INDEX notifications_user ON notifications(user_id, id);

CREATE TABLE vods (
    id           INTEGER PRIMARY KEY,
    channel_id   INTEGER NOT NULL REFERENCES channels(id) ON DELETE CASCADE,
    session_id   INTEGER REFERENCES stream_sessions(id) ON DELETE SET NULL,
    title        TEXT    NOT NULL,
    category     TEXT    NOT NULL DEFAULT '',
    file         TEXT    NOT NULL,              -- 相对 VOD 目录的文件名
    duration_sec REAL    NOT NULL DEFAULT 0,
    size_bytes   INTEGER NOT NULL DEFAULT 0,
    views        INTEGER NOT NULL DEFAULT 0,
    created_at   INTEGER NOT NULL
);
CREATE INDEX vods_channel ON vods(channel_id, id);

-- 频道内禁言：until 为 NULL 表示永久
CREATE TABLE channel_bans (
    channel_id INTEGER NOT NULL REFERENCES channels(id) ON DELETE CASCADE,
    user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    until      INTEGER,
    by_user    INTEGER REFERENCES users(id) ON DELETE SET NULL,
    created_at INTEGER NOT NULL,
    PRIMARY KEY (channel_id, user_id)
);

-- 频道房管（由频道主任命，可管理该频道聊天）
CREATE TABLE channel_moderators (
    channel_id INTEGER NOT NULL REFERENCES channels(id) ON DELETE CASCADE,
    user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at INTEGER NOT NULL,
    PRIMARY KEY (channel_id, user_id)
);

CREATE TABLE reports (
    id          INTEGER PRIMARY KEY,
    reporter_id INTEGER REFERENCES users(id) ON DELETE SET NULL,
    channel_id  INTEGER NOT NULL REFERENCES channels(id) ON DELETE CASCADE,
    reason      TEXT    NOT NULL,
    status      TEXT    NOT NULL DEFAULT 'open',  -- open | resolved | dismissed
    resolved_by INTEGER REFERENCES users(id) ON DELETE SET NULL,
    created_at  INTEGER NOT NULL,
    resolved_at INTEGER
);
CREATE INDEX reports_status ON reports(status, id);
