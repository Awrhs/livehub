// Package ratelimit 提供按键（IP/用户）的内存令牌桶限流。
package ratelimit

import (
	"sync"
	"time"
)

type bucket struct {
	tokens float64
	last   time.Time
}

// Limiter 令牌桶：每秒补充 rate 个令牌，容量 burst。
type Limiter struct {
	mu      sync.Mutex
	rate    float64
	burst   float64
	buckets map[string]*bucket
	now     func() time.Time
	lastGC  time.Time
}

func New(ratePerSec float64, burst int) *Limiter {
	return &Limiter{rate: ratePerSec, burst: float64(burst), buckets: map[string]*bucket{}, now: time.Now}
}

// PerMinute 每分钟 n 次、允许突发 burst 次。
func PerMinute(n, burst int) *Limiter { return New(float64(n)/60.0, burst) }

// Allow 消耗一个令牌；不足返回 false。
func (l *Limiter) Allow(key string) bool {
	l.mu.Lock()
	defer l.mu.Unlock()
	now := l.now()
	b, ok := l.buckets[key]
	if !ok {
		b = &bucket{tokens: l.burst, last: now}
		l.buckets[key] = b
	}
	b.tokens += now.Sub(b.last).Seconds() * l.rate
	if b.tokens > l.burst {
		b.tokens = l.burst
	}
	b.last = now
	l.gc(now)
	if b.tokens < 1 {
		return false
	}
	b.tokens--
	return true
}

// gc 定期清理已回满的桶，防止内存随键数增长
func (l *Limiter) gc(now time.Time) {
	if now.Sub(l.lastGC) < time.Minute {
		return
	}
	l.lastGC = now
	for k, b := range l.buckets {
		if b.tokens+now.Sub(b.last).Seconds()*l.rate >= l.burst {
			delete(l.buckets, k)
		}
	}
}
