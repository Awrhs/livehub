-- LiveHub 初始结构（阶段 1）。时间均为 Unix 秒。

CREATE TABLE users (
    id                  INTEGER PRIMARY KEY,
    username            TEXT    NOT NULL UNIQUE COLLATE NOCASE,
    display_name        TEXT    NOT NULL,
    password_hash       TEXT    NOT NULL,
    role                TEXT    NOT NULL DEFAULT 'user',   -- admin | moderator | user
    stream_status       TEXT    NOT NULL DEFAULT 'none',   -- none | pending | approved | rejected
    stream_request_note TEXT    NOT NULL DEFAULT '',
    bio                 TEXT    NOT NULL DEFAULT '',
    banned              INTEGER NOT NULL DEFAULT 0,
    ban_reason          TEXT    NOT NULL DEFAULT '',
    created_at          INTEGER NOT NULL,
    last_login_at       INTEGER
);

CREATE TABLE sessions (
    token_hash  TEXT    PRIMARY KEY,                       -- SHA-256(token)，库中不存明文
    user_id     INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at  INTEGER NOT NULL,
    expires_at  INTEGER NOT NULL,
    ip          TEXT    NOT NULL DEFAULT '',
    user_agent  TEXT    NOT NULL DEFAULT ''
);
CREATE INDEX sessions_user ON sessions(user_id);

-- 每个用户一个频道，slug = 用户名
CREATE TABLE channels (
    id                 INTEGER PRIMARY KEY,
    user_id            INTEGER NOT NULL UNIQUE REFERENCES users(id) ON DELETE CASCADE,
    slug               TEXT    NOT NULL UNIQUE COLLATE NOCASE,
    title              TEXT    NOT NULL DEFAULT '',
    category           TEXT    NOT NULL DEFAULT '',
    description        TEXT    NOT NULL DEFAULT '',
    stream_key         TEXT    NOT NULL,
    is_live            INTEGER NOT NULL DEFAULT 0,
    live_started_at    INTEGER,
    last_live_at       INTEGER,
    current_session_id INTEGER,
    record_enabled     INTEGER NOT NULL DEFAULT 1,
    created_at         INTEGER NOT NULL
);
CREATE INDEX channels_live ON channels(is_live);

-- 每次开播一条记录（录像、统计按场次归档）
CREATE TABLE stream_sessions (
    id           INTEGER PRIMARY KEY,
    channel_id   INTEGER NOT NULL REFERENCES channels(id) ON DELETE CASCADE,
    title        TEXT    NOT NULL,
    category     TEXT    NOT NULL,
    started_at   INTEGER NOT NULL,
    ended_at     INTEGER,
    peak_viewers INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX stream_sessions_channel ON stream_sessions(channel_id, started_at);

CREATE TABLE chat_messages (
    id         INTEGER PRIMARY KEY,
    channel_id INTEGER NOT NULL REFERENCES channels(id) ON DELETE CASCADE,
    user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    body       TEXT    NOT NULL,
    created_at INTEGER NOT NULL,
    deleted    INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX chat_messages_channel ON chat_messages(channel_id, id);

CREATE TABLE settings (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE TABLE invites (
    code       TEXT    PRIMARY KEY,
    created_by INTEGER REFERENCES users(id) ON DELETE SET NULL,
    used_by    INTEGER REFERENCES users(id) ON DELETE SET NULL,
    created_at INTEGER NOT NULL,
    expires_at INTEGER,
    used_at    INTEGER
);

CREATE TABLE audit_log (
    id         INTEGER PRIMARY KEY,
    actor_id   INTEGER,
    action     TEXT    NOT NULL,
    target     TEXT    NOT NULL DEFAULT '',
    detail     TEXT    NOT NULL DEFAULT '',
    created_at INTEGER NOT NULL
);
CREATE INDEX audit_log_time ON audit_log(created_at);
