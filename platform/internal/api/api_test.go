package api

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/cookiejar"
	"net/http/httptest"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/coder/websocket"

	"livehub/internal/chat"
	"livehub/internal/config"
	"livehub/internal/db"
	"livehub/internal/ratelimit"
	"livehub/internal/store"
)

type testEnv struct {
	t   *testing.T
	srv *Server
	ts  *httptest.Server
}

func newEnv(t *testing.T) *testEnv {
	t.Helper()
	dir := t.TempDir()
	d, err := db.Open(filepath.Join(dir, "test.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { d.Close() })
	cfg := &config.Config{DataDir: dir, BcryptCost: 4, SecureCookies: "false", MediaRTMPPort: 1935}
	log := slog.New(slog.NewTextHandler(io.Discard, nil))
	s := New(cfg, store.New(d), nil, chat.NewHub(), log, "hooksecret", nil, "test")
	// 测试来自同一 IP，放宽限流
	s.registerLimiter = ratelimit.New(1000, 1000)
	s.authLimiter = ratelimit.New(1000, 1000)
	s.apiLimiter = ratelimit.New(1000, 1000)
	ts := httptest.NewServer(s.Handler())
	t.Cleanup(ts.Close)
	return &testEnv{t: t, srv: s, ts: ts}
}

type client struct {
	env *testEnv
	hc  *http.Client
}

func (e *testEnv) client() *client {
	jar, _ := cookiejar.New(nil)
	return &client{env: e, hc: &http.Client{Jar: jar, Timeout: 10 * time.Second}}
}

// do 发送请求并把 JSON 响应解析到 out（可为 nil），返回状态码。
func (c *client) do(method, path string, body any, out any) int {
	c.env.t.Helper()
	var rd io.Reader
	if body != nil {
		b, _ := json.Marshal(body)
		rd = bytes.NewReader(b)
	}
	req, _ := http.NewRequest(method, c.env.ts.URL+path, rd)
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("X-Requested-With", "livehub")
	resp, err := c.hc.Do(req)
	if err != nil {
		c.env.t.Fatal(err)
	}
	defer resp.Body.Close()
	data, _ := io.ReadAll(resp.Body)
	if out != nil {
		if err := json.Unmarshal(data, out); err != nil {
			c.env.t.Fatalf("%s %s: 响应不是 JSON: %s", method, path, data)
		}
	}
	return resp.StatusCode
}

func (c *client) register(name string) map[string]any {
	c.env.t.Helper()
	var me map[string]any
	if code := c.do("POST", "/api/v1/auth/register", map[string]string{"username": name, "password": "password123"}, &me); code != 201 {
		c.env.t.Fatalf("注册 %s 失败: %d %v", name, code, me)
	}
	return me
}

func (e *testEnv) hook(action, stream, param string) int {
	e.t.Helper()
	b, _ := json.Marshal(map[string]string{"action": action, "app": "live", "stream": stream, "param": param})
	resp, err := http.Post(e.ts.URL+"/internal/media/hook?secret=hooksecret", "application/json", bytes.NewReader(b))
	if err != nil {
		e.t.Fatal(err)
	}
	defer resp.Body.Close()
	var r hookReply
	_ = json.NewDecoder(resp.Body).Decode(&r)
	return r.Code
}

func TestFirstUserIsAdminAndRegistrationModes(t *testing.T) {
	e := newEnv(t)
	var site map[string]any
	e.client().do("GET", "/api/v1/site", nil, &site)
	if site["setup_required"] != true {
		t.Fatalf("空站点应提示初始化: %v", site)
	}
	admin := e.client()
	me := admin.register("alice")
	if me["role"] != "admin" {
		t.Fatalf("首个用户应为管理员: %v", me["role"])
	}
	bob := e.client().register("bob")
	if bob["role"] != "user" {
		t.Fatalf("第二个用户应为普通用户: %v", bob["role"])
	}
	// 重名（不区分大小写）
	var errResp map[string]any
	if code := e.client().do("POST", "/api/v1/auth/register", map[string]string{"username": "Bob", "password": "password123"}, &errResp); code != 409 {
		t.Fatalf("重名应 409，得到 %d", code)
	}
	// 保留名与非法名
	for _, n := range []string{"admin", "ab", "1abc", "carol_720p"} {
		if code := e.client().do("POST", "/api/v1/auth/register", map[string]string{"username": n, "password": "password123"}, nil); code != 400 {
			t.Fatalf("用户名 %q 应被拒绝，得到 %d", n, code)
		}
	}
	// 关闭注册
	if code := admin.do("PUT", "/api/v1/admin/settings", map[string]string{"registration_mode": "closed"}, nil); code != 200 {
		t.Fatalf("修改设置失败: %d", code)
	}
	if code := e.client().do("POST", "/api/v1/auth/register", map[string]string{"username": "carol", "password": "password123"}, nil); code != 403 {
		t.Fatalf("关闭注册后应 403，得到 %d", code)
	}
	// 邀请制
	admin.do("PUT", "/api/v1/admin/settings", map[string]string{"registration_mode": "invite"}, nil)
	var inv map[string]any
	if code := admin.do("POST", "/api/v1/admin/invites", map[string]int{}, &inv); code != 201 {
		t.Fatalf("创建邀请码失败: %d", code)
	}
	if code := e.client().do("POST", "/api/v1/auth/register", map[string]string{"username": "carol", "password": "password123"}, nil); code != 403 {
		t.Fatalf("无邀请码应 403，得到 %d", code)
	}
	body := map[string]string{"username": "carol", "password": "password123", "invite": inv["code"].(string)}
	if code := e.client().do("POST", "/api/v1/auth/register", body, nil); code != 201 {
		t.Fatalf("凭邀请码注册失败: %d", code)
	}
	body["username"] = "dave"
	if code := e.client().do("POST", "/api/v1/auth/register", body, nil); code != 403 {
		t.Fatalf("邀请码重复使用应 403，得到 %d", code)
	}
}

func TestLoginLogoutAndCSRF(t *testing.T) {
	e := newEnv(t)
	e.client().register("alice")
	c := e.client()
	if code := c.do("POST", "/api/v1/auth/login", map[string]string{"username": "alice", "password": "wrong-password"}, nil); code != 401 {
		t.Fatalf("错误密码应 401，得到 %d", code)
	}
	if code := c.do("POST", "/api/v1/auth/login", map[string]string{"username": "ALICE", "password": "password123"}, nil); code != 200 {
		t.Fatalf("登录失败（用户名应不区分大小写）: %d", code)
	}
	if code := c.do("GET", "/api/v1/me", nil, nil); code != 200 {
		t.Fatalf("登录后 /me 应 200，得到 %d", code)
	}
	// 缺少 X-Requested-With 的写请求被拒绝
	req, _ := http.NewRequest("POST", e.ts.URL+"/api/v1/auth/logout", nil)
	resp, err := c.hc.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	resp.Body.Close()
	if resp.StatusCode != 403 {
		t.Fatalf("缺少 CSRF 头应 403，得到 %d", resp.StatusCode)
	}
	// 跨站 Origin 被拒绝
	req, _ = http.NewRequest("POST", e.ts.URL+"/api/v1/auth/logout", nil)
	req.Header.Set("X-Requested-With", "livehub")
	req.Header.Set("Origin", "https://evil.example.com")
	resp, err = c.hc.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	resp.Body.Close()
	if resp.StatusCode != 403 {
		t.Fatalf("跨站 Origin 应 403，得到 %d", resp.StatusCode)
	}
	if code := c.do("POST", "/api/v1/auth/logout", nil, nil); code != 200 {
		t.Fatalf("登出失败: %d", code)
	}
	if code := c.do("GET", "/api/v1/me", nil, nil); code != 401 {
		t.Fatalf("登出后 /me 应 401，得到 %d", code)
	}
}

func TestStreamApprovalAndPublishHook(t *testing.T) {
	e := newEnv(t)
	admin := e.client()
	admin.register("alice")
	bob := e.client()
	me := bob.register("bob")
	if me["can_stream"] != false {
		t.Fatalf("默认需要审批，新用户不应能开播: %v", me["can_stream"])
	}
	ch := me["channel"].(map[string]any)
	key := ch["stream_key"].(string)
	if ch["publish_key"] != "bob?key="+key {
		t.Fatalf("推流码格式错误: %v", ch["publish_key"])
	}
	// 未审批：拒绝
	if code := e.hook("on_publish", "bob", "?key="+key); code == 0 {
		t.Fatal("未审批的用户推流应被拒绝")
	}
	// 申请并审批
	if code := bob.do("POST", "/api/v1/me/stream-request", map[string]string{"note": "想直播编程"}, nil); code != 200 {
		t.Fatalf("申请开播失败: %d", code)
	}
	var users map[string]any
	admin.do("GET", "/api/v1/admin/users?stream_status=pending", nil, &users)
	list := users["users"].([]any)
	if len(list) != 1 {
		t.Fatalf("应有 1 个待审批用户: %v", users)
	}
	bobID := int(list[0].(map[string]any)["id"].(float64))
	if code := admin.do("PATCH", "/api/v1/admin/users/"+itoa(bobID), map[string]string{"stream_status": "approved"}, nil); code != 200 {
		t.Fatalf("审批失败: %d", code)
	}
	// 错误密钥、大小写不一致、未知频道：拒绝
	if code := e.hook("on_publish", "bob", "?key=wrong"); code == 0 {
		t.Fatal("错误密钥应被拒绝")
	}
	if code := e.hook("on_publish", "Bob", "?key="+key); code == 0 {
		t.Fatal("流名大小写不一致应被拒绝")
	}
	if code := e.hook("on_publish", "nobody", "?key="+key); code == 0 {
		t.Fatal("未知频道应被拒绝")
	}
	// 正确：放行并标记直播中
	if code := e.hook("on_publish", "bob", "?key="+key); code != 0 {
		t.Fatalf("正确密钥应放行，得到 code=%d", code)
	}
	var lst map[string]any
	e.client().do("GET", "/api/v1/channels?live=1", nil, &lst)
	chs := lst["channels"].([]any)
	if len(chs) != 1 || chs[0].(map[string]any)["slug"] != "bob" {
		t.Fatalf("直播列表应只有 bob: %v", lst)
	}
	pb := chs[0].(map[string]any)["playback"].(map[string]any)
	if pb["hls"] != "/live/bob.m3u8" || pb["flv"] != "/live/bob.flv" {
		t.Fatalf("播放地址错误: %v", pb)
	}
	// 重置密钥后旧密钥失效
	var me2 map[string]any
	bob.do("POST", "/api/v1/me/channel/key", nil, &me2)
	newKey := me2["channel"].(map[string]any)["stream_key"].(string)
	if newKey == key {
		t.Fatal("重置后密钥应变化")
	}
	if code := e.hook("on_publish", "bob", "?key="+key); code == 0 {
		t.Fatal("旧密钥应失效")
	}
	// 下播
	e.hook("on_unpublish", "bob", "")
	e.client().do("GET", "/api/v1/channels?live=1", nil, &lst)
	if n := len(lst["channels"].([]any)); n != 0 {
		t.Fatalf("下播后直播列表应为空，得到 %d", n)
	}
	// 回调密钥错误
	resp, _ := http.Post(e.ts.URL+"/internal/media/hook?secret=bad", "application/json", strings.NewReader(`{}`))
	resp.Body.Close()
	if resp.StatusCode != 403 {
		t.Fatalf("回调密钥错误应 403，得到 %d", resp.StatusCode)
	}
}

func TestBanEndsStreamAndSessions(t *testing.T) {
	e := newEnv(t)
	admin := e.client()
	admin.register("alice")
	admin.do("PUT", "/api/v1/admin/settings", map[string]string{"stream_approval": "false"}, nil)
	bob := e.client()
	me := bob.register("bob")
	if me["can_stream"] != true {
		t.Fatal("关闭审批后新用户应能开播")
	}
	key := me["channel"].(map[string]any)["stream_key"].(string)
	if code := e.hook("on_publish", "bob", "?key="+key); code != 0 {
		t.Fatalf("推流应放行: %d", code)
	}
	bobID := int(me["id"].(float64))
	if code := admin.do("PATCH", "/api/v1/admin/users/"+itoa(bobID), map[string]any{"banned": true, "ban_reason": "违规"}, nil); code != 200 {
		t.Fatalf("封禁失败: %d", code)
	}
	if code := bob.do("GET", "/api/v1/me", nil, nil); code != 401 {
		t.Fatalf("封禁后会话应失效，得到 %d", code)
	}
	if code := e.hook("on_publish", "bob", "?key="+key); code == 0 {
		t.Fatal("封禁用户推流应被拒绝")
	}
	var ch map[string]any
	if code := e.client().do("GET", "/api/v1/channels/bob", nil, &ch); code != 404 {
		t.Fatalf("被封禁主播的频道应 404，得到 %d", code)
	}
	var errResp map[string]map[string]string
	if code := e.client().do("POST", "/api/v1/auth/login", map[string]string{"username": "bob", "password": "password123"}, &errResp); code != 403 || errResp["error"]["code"] != "banned" {
		t.Fatalf("封禁用户登录应 403 banned，得到 %d %v", code, errResp)
	}
	// 管理员不能封禁自己
	var adminMe map[string]any
	admin.do("GET", "/api/v1/me", nil, &adminMe)
	if code := admin.do("PATCH", "/api/v1/admin/users/"+itoa(int(adminMe["id"].(float64))), map[string]any{"banned": true}, nil); code != 400 {
		t.Fatalf("封禁自己应 400，得到 %d", code)
	}
}

func TestChatWebSocket(t *testing.T) {
	e := newEnv(t)
	alice := e.client()
	alice.register("alice")
	bob := e.client()
	bob.register("bob")

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	wsURL := "ws" + strings.TrimPrefix(e.ts.URL, "http") + "/api/v1/channels/alice/chat/ws"
	dial := func(c *client) *websocket.Conn {
		opts := &websocket.DialOptions{HTTPClient: c.hc, HTTPHeader: http.Header{"Origin": {e.ts.URL}}}
		conn, _, err := websocket.Dial(ctx, wsURL, opts)
		if err != nil {
			t.Fatal(err)
		}
		return conn
	}
	read := func(conn *websocket.Conn, typ string) map[string]any {
		for {
			_, data, err := conn.Read(ctx)
			if err != nil {
				t.Fatalf("读取 %s 失败: %v", typ, err)
			}
			var m map[string]any
			_ = json.Unmarshal(data, &m)
			if m["type"] == typ {
				return m
			}
		}
	}
	guest := dial(e.client())
	defer guest.CloseNow()
	hello := read(guest, "hello")
	if hello["me"] != nil {
		t.Fatal("游客 hello 不应包含 me")
	}
	bc := dial(bob)
	defer bc.CloseNow()
	read(bc, "hello")
	// 游客发言被拒
	_ = guest.Write(ctx, websocket.MessageText, []byte(`{"type":"send","body":"hi"}`))
	if m := read(guest, "error"); m["code"] != "unauthorized" {
		t.Fatalf("游客发言应 unauthorized: %v", m)
	}
	// bob 发言，游客收到
	_ = bc.Write(ctx, websocket.MessageText, []byte(`{"type":"send","body":"你好 LiveHub"}`))
	m := read(guest, "msg")
	msg := m["message"].(map[string]any)
	if msg["body"] != "你好 LiveHub" || msg["user"].(map[string]any)["username"] != "bob" {
		t.Fatalf("消息内容错误: %v", m)
	}
	// 频道主删除消息，游客收到 delete
	id := int(msg["id"].(float64))
	if code := alice.do("DELETE", "/api/v1/chat/messages/"+itoa(id), nil, nil); code != 200 {
		t.Fatalf("频道主删除消息失败: %d", code)
	}
	if d := read(guest, "delete"); int(d["id"].(float64)) != id {
		t.Fatalf("删除事件错误: %v", d)
	}
	// 非频道主不能删除
	_ = bc.Write(ctx, websocket.MessageText, []byte(`{"type":"send","body":"second"}`))
	m = read(guest, "msg")
	id2 := int(m["message"].(map[string]any)["id"].(float64))
	if code := bob.do("DELETE", "/api/v1/chat/messages/"+itoa(id2), nil, nil); code != 403 {
		t.Fatalf("非频道主删除应 403，得到 %d", code)
	}
	// 历史只含未删除消息
	var hist map[string]any
	e.client().do("GET", "/api/v1/channels/alice/chat", nil, &hist)
	if n := len(hist["messages"].([]any)); n != 1 {
		t.Fatalf("历史应有 1 条，得到 %d", n)
	}
	// 观众数
	if n := e.srv.Hub.Count(1); n != 2 {
		t.Fatalf("房间连接数应为 2，得到 %d", n)
	}
}

func TestSPAFallbackAndAPINotFound(t *testing.T) {
	e := newEnv(t)
	var r map[string]any
	if code := e.client().do("GET", "/api/v1/nope", nil, &r); code != 404 {
		t.Fatalf("未知 API 应 404，得到 %d", code)
	}
}

func itoa(n int) string { return strconv.Itoa(n) }
