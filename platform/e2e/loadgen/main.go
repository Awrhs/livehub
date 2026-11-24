// loadgen：长稳测试的负载生成器。模拟登录观众反复进出直播间聊天、发言，
// 以及 HTTP-FLV 观众反复连接/断开，持续指定时长。
//
//	go run ./e2e/loadgen -base http://127.0.0.1:8000 -channels a,b -chatters 20 -flv 10 -duration 30m
package main

import (
	"bytes"
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"log"
	"math/rand"
	"net/http"
	"net/http/cookiejar"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/coder/websocket"
)

var (
	base     = flag.String("base", "http://127.0.0.1:8000", "LiveHub 地址")
	channels = flag.String("channels", "", "频道列表（逗号分隔）")
	chatters = flag.Int("chatters", 20, "聊天用户数")
	flvN     = flag.Int("flv", 10, "HTTP-FLV 观众数")
	duration = flag.Duration("duration", 30*time.Minute, "持续时间")
	prefix   = flag.String("prefix", "load", "测试账号前缀")
)

var sent, recv, flvBytes, errs atomic.Int64

func newClient() *http.Client {
	jar, _ := cookiejar.New(nil)
	return &http.Client{Jar: jar, Timeout: 30 * time.Second}
}

func post(c *http.Client, path string, body any) error {
	b, _ := json.Marshal(body)
	req, _ := http.NewRequest("POST", *base+path, bytes.NewReader(b))
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("X-Requested-With", "livehub")
	resp, err := c.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode/100 != 2 {
		data, _ := io.ReadAll(resp.Body)
		return fmt.Errorf("%s: %d %s", path, resp.StatusCode, data)
	}
	return nil
}

func chatter(ctx context.Context, id int, chs []string) {
	c := newClient()
	name := fmt.Sprintf("%s%d", *prefix, id)
	if err := post(c, "/api/v1/auth/register", map[string]string{"username": name, "password": "password123"}); err != nil {
		if err := post(c, "/api/v1/auth/login", map[string]string{"username": name, "password": "password123"}); err != nil {
			log.Printf("登录失败 %s: %v", name, err)
			errs.Add(1)
			return
		}
	}
	ws := "ws" + strings.TrimPrefix(*base, "http")
	for ctx.Err() == nil {
		ch := chs[rand.Intn(len(chs))]
		// 每次会话 30–120 秒后断开重连（模拟观众进出）
		sess, cancel := context.WithTimeout(ctx, time.Duration(30+rand.Intn(90))*time.Second)
		conn, _, err := websocket.Dial(sess, ws+"/api/v1/channels/"+ch+"/chat/ws", &websocket.DialOptions{
			HTTPClient: c, HTTPHeader: http.Header{"Origin": {*base}},
		})
		if err != nil {
			errs.Add(1)
			cancel()
			time.Sleep(2 * time.Second)
			continue
		}
		go func() {
			for {
				if _, _, err := conn.Read(sess); err != nil {
					return
				}
				recv.Add(1)
			}
		}()
		tick := time.NewTicker(time.Duration(3+rand.Intn(5)) * time.Second)
	loop:
		for {
			select {
			case <-sess.Done():
				break loop
			case <-tick.C:
				msg := fmt.Sprintf(`{"type":"send","body":"%s 的消息 %d"}`, name, time.Now().Unix())
				if conn.Write(sess, websocket.MessageText, []byte(msg)) == nil {
					sent.Add(1)
				}
			}
		}
		tick.Stop()
		conn.Close(websocket.StatusNormalClosure, "")
		cancel()
	}
}

func flvViewer(ctx context.Context, chs []string) {
	for ctx.Err() == nil {
		ch := chs[rand.Intn(len(chs))]
		sess, cancel := context.WithTimeout(ctx, time.Duration(20+rand.Intn(60))*time.Second)
		req, _ := http.NewRequestWithContext(sess, "GET", *base+"/live/"+ch+".flv", nil)
		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			if sess.Err() == nil {
				errs.Add(1)
			}
			cancel()
			time.Sleep(time.Second)
			continue
		}
		n, _ := io.Copy(io.Discard, resp.Body)
		flvBytes.Add(n)
		resp.Body.Close()
		cancel()
	}
}

func main() {
	flag.Parse()
	chs := strings.Split(*channels, ",")
	ctx, cancel := context.WithTimeout(context.Background(), *duration)
	defer cancel()
	var wg sync.WaitGroup
	for i := 0; i < *chatters; i++ {
		wg.Add(1)
		go func(i int) { defer wg.Done(); chatter(ctx, i, chs) }(i)
		time.Sleep(200 * time.Millisecond)
	}
	for i := 0; i < *flvN; i++ {
		wg.Add(1)
		go func() { defer wg.Done(); flvViewer(ctx, chs) }()
	}
	go func() {
		t := time.NewTicker(time.Minute)
		defer t.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-t.C:
				log.Printf("聊天发送 %d 接收 %d，FLV %.1f MB，错误 %d", sent.Load(), recv.Load(), float64(flvBytes.Load())/1e6, errs.Load())
			}
		}
	}()
	wg.Wait()
	fmt.Printf("RESULT sent=%d recv=%d flv_mb=%.1f errors=%d\n", sent.Load(), recv.Load(), float64(flvBytes.Load())/1e6, errs.Load())
}
