-- 头像、邮箱（验证与找回密码）

ALTER TABLE users ADD COLUMN avatar_version INTEGER NOT NULL DEFAULT 0; -- 0 = 无头像；每次上传递增（用于缓存失效）
ALTER TABLE users ADD COLUMN email TEXT NOT NULL DEFAULT '';
ALTER TABLE users ADD COLUMN email_verified INTEGER NOT NULL DEFAULT 0;
CREATE UNIQUE INDEX users_email ON users(email COLLATE NOCASE) WHERE email != '';

-- 一次性令牌：邮箱验证、找回密码（库中只存摘要）
CREATE TABLE user_tokens (
    token_hash TEXT    PRIMARY KEY,
    user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    purpose    TEXT    NOT NULL,               -- verify_email | reset_password
    email      TEXT    NOT NULL DEFAULT '',    -- verify_email：待验证的地址
    created_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL
);
CREATE INDEX user_tokens_user ON user_tokens(user_id, purpose);
