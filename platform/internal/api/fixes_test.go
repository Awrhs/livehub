package api

import (
	"bufio"
	"bytes"
	"context"
	"image"
	"image/color"
	"image/png"
	"io"
	"net/http"
	"strings"
	"testing"
	"time"

	"github.com/coder/websocket"
)

func TestHomeListsOnlyEverLiveChannels(t *testing.T) {
	e := newEnv(t)
	admin := e.client()
	admin.register("alice")
	key := admin.newKey()
	e.client().register("bob") // 从未开播
	var r map[string]any
	e.client().do("GET", "/api/v1/channels", nil, &r)
	if n := len(r["channels"].([]any)); n != 0 {
		t.Fatalf("首页不应列出从未开播的频道，得到 %d", n)
	}
	e.hook("on_publish", "alice", "?key="+key)
	e.hook("on_unpublish", "alice", "")
	e.client().do("GET", "/api/v1/channels", nil, &r)
	if n := len(r["channels"].([]any)); n != 1 {
		t.Fatalf("开播过的频道应列出，得到 %d", n)
	}
	e.client().do("GET", "/api/v1/channels?q=bob", nil, &r)
	if n := len(r["channels"].([]any)); n != 1 {
		t.Fatalf("搜索应能找到从未开播的频道，得到 %d", n)
	}
}

func TestAvatarUpload(t *testing.T) {
	e := newEnv(t)
	c := e.client()
	c.register("alice")
	img := image.NewRGBA(image.Rect(0, 0, 400, 300))
	for x := 0; x < 400; x++ {
		for y := 0; y < 300; y++ {
			img.Set(x, y, color.RGBA{uint8(x), uint8(y), 128, 255})
		}
	}
	var buf bytes.Buffer
	_ = png.Encode(&buf, img)
	req, _ := http.NewRequest("POST", e.ts.URL+"/api/v1/me/avatar", &buf)
	req.Header.Set("X-Requested-With", "livehub")
	req.Header.Set("Content-Type", "image/png")
	resp, err := c.hc.Do(req)
	if err != nil || resp.StatusCode != 200 {
		t.Fatalf("上传头像失败: %v %v", err, resp.StatusCode)
	}
	resp.Body.Close()
	var me map[string]any
	c.do("GET", "/api/v1/me", nil, &me)
	url, _ := me["avatar_url"].(string)
	if !strings.HasPrefix(url, "/avatars/") {
		t.Fatalf("头像地址错误: %v", me["avatar_url"])
	}
	resp, err = http.Get(e.ts.URL + url)
	if err != nil {
		t.Fatal(err)
	}
	data, _ := io.ReadAll(resp.Body)
	resp.Body.Close()
	cfg, format, err := image.DecodeConfig(bytes.NewReader(data))
	if err != nil || format != "jpeg" || cfg.Width != 256 || cfg.Height != 256 {
		t.Fatalf("头像应为 256x256 JPEG，得到 %v %s %dx%d", err, format, cfg.Width, cfg.Height)
	}
	// 非图片被拒绝
	req, _ = http.NewRequest("POST", e.ts.URL+"/api/v1/me/avatar", strings.NewReader("not an image"))
	req.Header.Set("X-Requested-With", "livehub")
	resp, _ = c.hc.Do(req)
	resp.Body.Close()
	if resp.StatusCode != 400 {
		t.Fatalf("非图片应 400，得到 %d", resp.StatusCode)
	}
}

func TestAdminResetPassword(t *testing.T) {
	e := newEnv(t)
	admin := e.client()
	admin.register("alice")
	bob := e.client()
	me := bob.register("bob")
	var r map[string]string
	if code := admin.do("POST", "/api/v1/admin/users/"+itoa(int(me["id"].(float64)))+"/password", nil, &r); code != 200 || len(r["password"]) < 8 {
		t.Fatalf("重置密码失败: %d %v", code, r)
	}
	if code := bob.do("GET", "/api/v1/me", nil, nil); code != 401 {
		t.Fatalf("重置后旧会话应失效，得到 %d", code)
	}
	if code := e.client().do("POST", "/api/v1/auth/login", map[string]string{"username": "bob", "password": r["password"]}, nil); code != 200 {
		t.Fatalf("临时密码应能登录，得到 %d", code)
	}
	if code := bob.do("POST", "/api/v1/admin/users/1/password", nil, nil); code != 401 {
		t.Fatalf("非管理员应被拒绝，得到 %d", code)
	}
}

func TestSecurityHeaders(t *testing.T) {
	e := newEnv(t)
	resp, err := http.Get(e.ts.URL + "/api/v1/site")
	if err != nil {
		t.Fatal(err)
	}
	resp.Body.Close()
	for _, h := range []string{"Content-Security-Policy", "X-Frame-Options", "X-Content-Type-Options"} {
		if resp.Header.Get(h) == "" {
			t.Fatalf("缺少安全响应头 %s", h)
		}
	}
	if !strings.Contains(resp.Header.Get("Content-Security-Policy"), "frame-ancestors 'none'") {
		t.Fatalf("CSP 不完整: %s", resp.Header.Get("Content-Security-Policy"))
	}
}

func TestViewerCountDedupAndLiveEvents(t *testing.T) {
	e := newEnv(t)
	alice := e.client()
	alice.register("alice")
	key := alice.newKey()
	bob := e.client()
	bob.register("bob")
	bob.do("POST", "/api/v1/channels/alice/follow", nil, nil)

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	// 同一用户开两个标签页：只计一名观众
	wsURL := "ws" + strings.TrimPrefix(e.ts.URL, "http") + "/api/v1/channels/alice/chat/ws"
	for i := 0; i < 2; i++ {
		conn, _, err := websocket.Dial(ctx, wsURL, &websocket.DialOptions{HTTPClient: bob.hc, HTTPHeader: http.Header{"Origin": {e.ts.URL}}})
		if err != nil {
			t.Fatal(err)
		}
		defer conn.CloseNow()
	}
	time.Sleep(100 * time.Millisecond)
	if n := e.srv.Hub.Count(1); n != 1 {
		t.Fatalf("同一用户多个连接应计为 1 名观众，得到 %d", n)
	}

	// SSE：开播通知实时推送
	req, _ := http.NewRequestWithContext(ctx, "GET", e.ts.URL+"/api/v1/me/events", nil)
	resp, err := bob.hc.Do(req)
	if err != nil || resp.StatusCode != 200 {
		t.Fatalf("订阅事件失败: %v", err)
	}
	defer resp.Body.Close()
	got := make(chan string, 1)
	go func() {
		sc := bufio.NewScanner(resp.Body)
		for sc.Scan() {
			if strings.HasPrefix(sc.Text(), "data:") {
				got <- sc.Text()
				return
			}
		}
	}()
	time.Sleep(100 * time.Millisecond)
	e.hook("on_publish", "alice", "?key="+key)
	select {
	case line := <-got:
		if !strings.Contains(line, `"kind":"live"`) {
			t.Fatalf("事件内容错误: %s", line)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("未收到实时开播通知")
	}
}

func TestDiskGuardAndSystemStats(t *testing.T) {
	e := newEnv(t)
	admin := e.client()
	admin.register("alice")
	e.srv.checkDisk(context.Background()) // 首次采样
	var st map[string]any
	admin.do("GET", "/api/v1/admin/stats", nil, &st)
	sys := st["system"].(map[string]any)
	if sys["disk_total"].(float64) <= 0 || sys["mem_total"].(float64) <= 0 {
		t.Fatalf("系统资源统计缺失: %v", sys)
	}
	if st["disk_low"] != false {
		t.Fatalf("阈值为 0 时不应告警: %v", st["disk_low"])
	}
	e.srv.Cfg.MinFreeMB = 1 << 40 // 远大于任何磁盘
	e.srv.checkDisk(context.Background())
	admin.do("GET", "/api/v1/admin/stats", nil, &st)
	if st["disk_low"] != true {
		t.Fatal("剩余空间低于阈值时应告警")
	}
	var n map[string]any
	admin.do("GET", "/api/v1/me/notifications", nil, &n)
	list := n["notifications"].([]any)
	if len(list) == 0 || list[0].(map[string]any)["kind"] != "disk_low" {
		t.Fatalf("管理员应收到磁盘告警通知: %v", n)
	}
}

func TestReconnectResumesSession(t *testing.T) {
	e := newEnv(t)
	e.srv.Cfg.ReconnectGraceSec = 60
	alice := e.client()
	alice.register("alice")
	key := alice.newKey()
	var ch map[string]any
	e.hook("on_publish", "alice", "?key="+key)
	e.client().do("GET", "/api/v1/channels/alice", nil, &ch)
	started := ch["channel"].(map[string]any)["live_started_at"]
	e.hook("on_unpublish", "alice", "")
	e.hook("on_publish", "alice", "?key="+key) // 宽限期内重连
	e.client().do("GET", "/api/v1/channels/alice", nil, &ch)
	if ch["channel"].(map[string]any)["live_started_at"] != started {
		t.Fatalf("重连应续用场次，开播时间不变: %v → %v", started, ch["channel"].(map[string]any)["live_started_at"])
	}
	var n int
	_ = e.srv.Store.DB.QueryRow(`SELECT COUNT(*) FROM stream_sessions`).Scan(&n)
	if n != 1 {
		t.Fatalf("重连不应创建新场次，得到 %d 个场次", n)
	}
	// 关闭宽限期后重新开播是新的一场
	e.srv.Cfg.ReconnectGraceSec = 0
	e.hook("on_unpublish", "alice", "")
	e.hook("on_publish", "alice", "?key="+key)
	_ = e.srv.Store.DB.QueryRow(`SELECT COUNT(*) FROM stream_sessions`).Scan(&n)
	if n != 2 {
		t.Fatalf("超过宽限期应创建新场次，得到 %d", n)
	}
}
