package api

import (
	"context"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func TestAccountLockout(t *testing.T) {
	e := newEnv(t)
	e.client().register("alice")
	c := e.client()
	for i := 0; i < loginMaxFailures; i++ {
		if code := c.do("POST", "/api/v1/auth/login", map[string]string{"username": "alice", "password": "wrong-password"}, nil); code != 401 {
			t.Fatalf("第 %d 次错误登录应 401，得到 %d", i+1, code)
		}
	}
	var r map[string]map[string]string
	if code := c.do("POST", "/api/v1/auth/login", map[string]string{"username": "Alice", "password": "password123"}, &r); code != 429 || r["error"]["code"] != "account_locked" {
		t.Fatalf("多次失败后应锁定账号，得到 %d %v", code, r)
	}
}

func TestEdgeSelection(t *testing.T) {
	healthy := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
	}))
	defer healthy.Close()
	e := newEnv(t)
	e.srv.Cfg.EdgeURLs = []string{healthy.URL, "http://127.0.0.1:1"} // 第二个不可达
	e.srv.edges = nil
	e.srv.initEdges()
	e.srv.checkEdges(context.Background())
	for _, slug := range []string{"alice", "bob", "carol", "dave"} {
		if got := e.srv.pickEdge(slug); got != healthy.URL {
			t.Fatalf("应只选择健康的边缘节点，%s → %q", slug, got)
		}
	}
	admin := e.client()
	admin.register("alice")
	key := admin.newKey()
	e.hook("on_publish", "alice", "?key="+key)
	var ch map[string]any
	e.client().do("GET", "/api/v1/channels/alice", nil, &ch)
	pb := ch["channel"].(map[string]any)["playback"].(map[string]any)
	if !strings.HasPrefix(pb["flv"].(string), healthy.URL+"/live/") {
		t.Fatalf("播放地址应指向边缘节点: %v", pb)
	}
	// 边缘全部不可用时回落到本机
	healthy.Close()
	time.Sleep(50 * time.Millisecond)
	e.srv.checkEdges(context.Background())
	if got := e.srv.pickEdge("alice"); got != "" {
		t.Fatalf("无健康边缘时应回落本机，得到 %q", got)
	}
}
