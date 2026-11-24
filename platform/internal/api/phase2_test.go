package api

import (
	"context"
	"encoding/json"
	"net/http"
	"strings"
	"testing"
	"time"

	"github.com/coder/websocket"
)

func TestFollowAndLiveNotification(t *testing.T) {
	e := newEnv(t)
	admin := e.client()
	admin.register("alice")
	admin.do("PUT", "/api/v1/admin/settings", map[string]string{"stream_approval": "false"}, nil)
	bob := e.client()
	bob.register("bob")
	key := bob.newKey()
	carol := e.client()
	carol.register("carol")

	var r map[string]any
	if code := carol.do("POST", "/api/v1/channels/bob/follow", nil, &r); code != 200 || r["followers"].(float64) != 1 {
		t.Fatalf("关注失败: %d %v", code, r)
	}
	if code := bob.do("POST", "/api/v1/channels/bob/follow", nil, nil); code != 400 {
		t.Fatalf("关注自己应 400，得到 %d", code)
	}
	var ch map[string]any
	carol.do("GET", "/api/v1/channels/bob", nil, &ch)
	if ch["following"] != true || ch["followers"].(float64) != 1 {
		t.Fatalf("频道详情应显示已关注: %v", ch)
	}
	if code := e.hook("on_publish", "bob", "?key="+key); code != 0 {
		t.Fatalf("推流应放行: %d", code)
	}
	// 开播通知异步发送
	deadline := time.Now().Add(5 * time.Second)
	for {
		var n map[string]any
		carol.do("GET", "/api/v1/me/notifications", nil, &n)
		if n["unread"].(float64) == 1 {
			list := n["notifications"].([]any)
			first := list[0].(map[string]any)
			if first["kind"] != "live" || first["channel_slug"] != "bob" {
				t.Fatalf("通知内容错误: %v", first)
			}
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("未收到开播通知: %v", n)
		}
		time.Sleep(100 * time.Millisecond)
	}
	carol.do("POST", "/api/v1/me/notifications/read", nil, nil)
	var n map[string]any
	carol.do("GET", "/api/v1/me/notifications", nil, &n)
	if n["unread"].(float64) != 0 {
		t.Fatalf("标记已读后未读数应为 0: %v", n)
	}
	// 断线重连（10 分钟内再次开播）不重复通知
	e.hook("on_unpublish", "bob", "")
	e.hook("on_publish", "bob", "?key="+key)
	time.Sleep(300 * time.Millisecond)
	carol.do("GET", "/api/v1/me/notifications", nil, &n)
	if n["unread"].(float64) != 0 {
		t.Fatalf("短时间内重复开播不应重复通知: %v", n)
	}
	var f map[string]any
	carol.do("GET", "/api/v1/me/following", nil, &f)
	if len(f["channels"].([]any)) != 1 {
		t.Fatalf("关注列表应有 1 个频道: %v", f)
	}
	carol.do("DELETE", "/api/v1/channels/bob/follow", nil, nil)
	carol.do("GET", "/api/v1/me/following", nil, &f)
	if len(f["channels"].([]any)) != 0 {
		t.Fatalf("取消关注后列表应为空: %v", f)
	}
}

func TestInternalVariantPublish(t *testing.T) {
	e := newEnv(t)
	e.srv.InternalToken = "itok"
	admin := e.client()
	admin.register("alice")
	key := admin.newKey()
	// 频道未开播时变体推流被拒
	if code := e.hook("on_publish", "alice_720p", "?internal=itok"); code == 0 {
		t.Fatal("频道未开播时变体推流应被拒绝")
	}
	e.hook("on_publish", "alice", "?key="+key)
	if code := e.hook("on_publish", "alice_720p", "?internal=bad"); code == 0 {
		t.Fatal("错误内部令牌应被拒绝")
	}
	if code := e.hook("on_publish", "alice_720p", "?internal=itok"); code != 0 {
		t.Fatalf("正确内部令牌应放行: %d", code)
	}
	// 变体推流不应出现在直播列表
	var lst map[string]any
	e.client().do("GET", "/api/v1/channels?live=1", nil, &lst)
	if n := len(lst["channels"].([]any)); n != 1 {
		t.Fatalf("直播列表应只有 1 个频道，得到 %d", n)
	}
}

func TestReportsAndChannelModeration(t *testing.T) {
	e := newEnv(t)
	admin := e.client()
	admin.register("alice")
	bob := e.client()
	bobMe := bob.register("bob")
	carol := e.client()
	carolMe := carol.register("carol")
	dave := e.client()
	dave.register("dave")

	// 举报：重复举报 409，管理员处理
	if code := carol.do("POST", "/api/v1/channels/bob/report", map[string]string{"reason": "违规内容"}, nil); code != 201 {
		t.Fatalf("举报失败: %d", code)
	}
	if code := carol.do("POST", "/api/v1/channels/bob/report", map[string]string{"reason": "again"}, nil); code != 409 {
		t.Fatalf("重复举报应 409，得到 %d", code)
	}
	var reps map[string]any
	admin.do("GET", "/api/v1/admin/reports", nil, &reps)
	list := reps["reports"].([]any)
	if len(list) != 1 {
		t.Fatalf("应有 1 条待处理举报: %v", reps)
	}
	rid := int(list[0].(map[string]any)["id"].(float64))
	if code := admin.do("POST", "/api/v1/admin/reports/"+itoa(rid), map[string]string{"status": "dismissed"}, nil); code != 200 {
		t.Fatalf("处理举报失败: %d", code)
	}
	if code := carol.do("GET", "/api/v1/admin/reports", nil, nil); code != 403 {
		t.Fatalf("普通用户访问举报列表应 403，得到 %d", code)
	}

	// 房管：bob 任命 dave 为房管，dave 禁言 carol
	if code := dave.do("POST", "/api/v1/channels/bob/bans", map[string]any{"user_id": carolMe["id"]}, nil); code != 403 {
		t.Fatalf("非房管禁言应 403，得到 %d", code)
	}
	if code := bob.do("POST", "/api/v1/channels/bob/moderators", map[string]string{"username": "dave"}, nil); code != 200 {
		t.Fatalf("任命房管失败: %d", code)
	}
	if code := dave.do("POST", "/api/v1/channels/bob/bans", map[string]any{"user_id": bobMe["id"]}, nil); code != 403 {
		t.Fatalf("房管不能禁言频道主，得到 %d", code)
	}
	if code := dave.do("POST", "/api/v1/channels/bob/bans", map[string]any{"user_id": carolMe["id"], "duration_sec": 600}, nil); code != 200 {
		t.Fatalf("房管禁言失败: %d", code)
	}

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	wsURL := "ws" + strings.TrimPrefix(e.ts.URL, "http") + "/api/v1/channels/bob/chat/ws"
	conn, _, err := websocket.Dial(ctx, wsURL, &websocket.DialOptions{HTTPClient: carol.hc, HTTPHeader: http.Header{"Origin": {e.ts.URL}}})
	if err != nil {
		t.Fatal(err)
	}
	defer conn.CloseNow()
	_ = conn.Write(ctx, websocket.MessageText, []byte(`{"type":"send","body":"hello"}`))
	for {
		_, data, err := conn.Read(ctx)
		if err != nil {
			t.Fatal(err)
		}
		var m map[string]any
		_ = json.Unmarshal(data, &m)
		if m["type"] == "error" {
			if m["code"] != "timed_out" {
				t.Fatalf("被禁言用户应收到 timed_out: %v", m)
			}
			break
		}
		if m["type"] == "msg" {
			t.Fatal("被禁言用户的消息不应被广播")
		}
	}
	// 解除禁言
	if code := dave.do("DELETE", "/api/v1/channels/bob/bans/"+itoa(int(carolMe["id"].(float64))), nil, nil); code != 200 {
		t.Fatalf("解除禁言失败: %d", code)
	}
}

func TestSettingsValidation(t *testing.T) {
	e := newEnv(t)
	admin := e.client()
	admin.register("alice")
	for _, body := range []map[string]string{
		{"registration_mode": "weird"},
		{"stream_approval": "maybe"},
		{"chat_slow_mode_sec": "-1"},
		{"vod_retention_days": "abc"},
		{"site_name": ""},
		{"unknown_key": "x"},
	} {
		if code := admin.do("PUT", "/api/v1/admin/settings", body, nil); code != 400 {
			t.Fatalf("非法设置 %v 应 400，得到 %d", body, code)
		}
	}
}
