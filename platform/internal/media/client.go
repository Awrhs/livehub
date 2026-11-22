// Package media 对接 C++ 媒体服务器：管理 API 客户端、子进程托管、配置生成。
package media

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"time"
)

// Client 调用媒体服务器管理 API（Authorization: Bearer <ADMIN_TOKEN>）。
type Client struct {
	BaseURL    string
	AdminToken string
	HTTP       *http.Client
}

func NewClient(baseURL, token string) *Client {
	return &Client{BaseURL: baseURL, AdminToken: token, HTTP: &http.Client{Timeout: 5 * time.Second}}
}

// Stream 对应 GET /api/streams 中的一项。
type Stream struct {
	Key          string  `json:"key"`
	Publisher    bool    `json:"publisher"`
	Viewers      int     `json:"viewers"`
	Width        int     `json:"width"`
	Height       int     `json:"height"`
	FPS          int     `json:"fps"`
	VideoBitrate int     `json:"video_bitrate"`
	AudioBitrate int     `json:"audio_bitrate"`
	UptimeSec    float64 `json:"uptime_sec"`
}

func (c *Client) do(ctx context.Context, method, path string, out any) (int, error) {
	req, err := http.NewRequestWithContext(ctx, method, c.BaseURL+path, nil)
	if err != nil {
		return 0, err
	}
	req.Header.Set("Authorization", "Bearer "+c.AdminToken)
	resp, err := c.HTTP.Do(req)
	if err != nil {
		return 0, err
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(io.LimitReader(resp.Body, 4<<20))
	if err != nil {
		return resp.StatusCode, err
	}
	if out != nil && resp.StatusCode/100 == 2 {
		if err := json.Unmarshal(body, out); err != nil {
			return resp.StatusCode, fmt.Errorf("解析 %s 响应失败: %w", path, err)
		}
	}
	return resp.StatusCode, nil
}

// ListStreams 当前所有活跃流。
func (c *Client) ListStreams(ctx context.Context) ([]Stream, error) {
	var r struct {
		Streams []Stream `json:"streams"`
	}
	code, err := c.do(ctx, http.MethodGet, "/api/streams", &r)
	if err != nil {
		return nil, err
	}
	if code != http.StatusOK {
		return nil, fmt.Errorf("GET /api/streams: HTTP %d", code)
	}
	return r.Streams, nil
}

// Kick 断开一路推流。流不存在时返回 false。
func (c *Client) Kick(ctx context.Context, key string) (bool, error) {
	code, err := c.do(ctx, http.MethodDelete, "/api/streams/"+url.PathEscape(key), nil)
	if err != nil {
		return false, err
	}
	return code == http.StatusOK, nil
}

// StartRecord 开始录像；流尚不存在返回 (false, nil)。
func (c *Client) StartRecord(ctx context.Context, key string) (bool, error) {
	code, err := c.do(ctx, http.MethodPost, "/api/record/"+url.PathEscape(key)+"/start", nil)
	if err != nil {
		return false, err
	}
	return code == http.StatusOK, nil
}

// StopRecord 停止录像（文件写完并做 faststart 后返回）。
func (c *Client) StopRecord(ctx context.Context, key string) (bool, error) {
	code, err := c.do(ctx, http.MethodPost, "/api/record/"+url.PathEscape(key)+"/stop", nil)
	if err != nil {
		return false, err
	}
	return code == http.StatusOK, nil
}

// StartABR 为流开启多清晰度转码（profiles 如 "480p,720p"）。
func (c *Client) StartABR(ctx context.Context, key, profiles string) error {
	code, err := c.do(ctx, http.MethodPost, "/api/abr/"+url.PathEscape(key)+"?profiles="+url.QueryEscape(profiles), nil)
	if err != nil {
		return err
	}
	if code != http.StatusOK {
		return fmt.Errorf("POST /api/abr: HTTP %d", code)
	}
	return nil
}

// Healthy 媒体服务器是否可访问。
func (c *Client) Healthy(ctx context.Context) bool {
	code, err := c.do(ctx, http.MethodGet, "/api/health", nil)
	return err == nil && code == http.StatusOK
}
