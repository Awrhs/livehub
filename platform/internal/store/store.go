// Package store 封装 LiveHub 的数据访问。
package store

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"errors"
	"strings"
	"time"
)

var (
	ErrNotFound = errors.New("not found")
	ErrConflict = errors.New("conflict")
)

type Store struct {
	DB *sql.DB
	// Now 可在测试中替换
	Now func() time.Time
}

func New(db *sql.DB) *Store {
	return &Store{DB: db, Now: time.Now}
}

func (s *Store) now() int64 { return s.Now().Unix() }

// RandomToken 生成 n 字节随机数的十六进制串。
func RandomToken(n int) string {
	b := make([]byte, n)
	if _, err := rand.Read(b); err != nil {
		panic(err) // 系统随机源不可用时无法安全运行
	}
	return hex.EncodeToString(b)
}

// HashToken 返回 token 的 SHA-256 十六进制摘要（会话 token 库中只存摘要）。
func HashToken(t string) string {
	h := sha256.Sum256([]byte(t))
	return hex.EncodeToString(h[:])
}

func isUniqueErr(err error) bool {
	return err != nil && strings.Contains(err.Error(), "UNIQUE constraint failed")
}

func nullInt(v sql.NullInt64) *int64 {
	if !v.Valid {
		return nil
	}
	x := v.Int64
	return &x
}

// Tx 在事务中执行 fn。
func (s *Store) Tx(ctx context.Context, fn func(tx *sql.Tx) error) error {
	tx, err := s.DB.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	if err := fn(tx); err != nil {
		tx.Rollback()
		return err
	}
	return tx.Commit()
}
