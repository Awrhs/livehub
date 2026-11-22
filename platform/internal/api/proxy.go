package api

import (
	"log/slog"
	"net/http"
	"net/http/httputil"
	"net/url"
	"strings"
)

// newPlaybackProxy 把 /live/* 播放请求反向代理到媒体服务器。
// FlushInterval=-1：HTTP-FLV 是长连接流，必须每次写入立即刷新。
func newPlaybackProxy(mediaURL string, log *slog.Logger) http.Handler {
	target, err := url.Parse(mediaURL)
	if err != nil {
		panic(err)
	}
	p := &httputil.ReverseProxy{
		Rewrite: func(pr *httputil.ProxyRequest) {
			pr.SetURL(target)
			pr.Out.Host = target.Host
			// 平台会话与凭据不转发给媒体服务器
			pr.Out.Header.Del("Cookie")
			pr.Out.Header.Del("Authorization")
		},
		FlushInterval: -1,
		ErrorHandler: func(w http.ResponseWriter, r *http.Request, err error) {
			log.Debug("播放代理失败", "path", r.URL.Path, "err", err)
			w.WriteHeader(http.StatusBadGateway)
		},
		ModifyResponse: func(resp *http.Response) error {
			// 媒体服务器的 CORS 头对同源播放无意义；统一由平台决定
			resp.Header.Del("Access-Control-Allow-Origin")
			return nil
		},
	}
	return p
}

// handlePlayback 只放行播放相关的 GET/HEAD 请求（HLS 播放列表/分片、HTTP-FLV、WS-FLV）。
func (s *Server) handlePlayback(w http.ResponseWriter, r *http.Request) {
	if s.proxy == nil {
		http.NotFound(w, r)
		return
	}
	if r.Method != http.MethodGet && r.Method != http.MethodHead {
		w.WriteHeader(http.StatusMethodNotAllowed)
		return
	}
	p := r.URL.Path
	ok := false
	for _, ext := range []string{".m3u8", ".ts", ".flv", ".m4s", ".mp4"} {
		if strings.HasSuffix(p, ext) {
			ok = true
			break
		}
	}
	if !ok || strings.Contains(p, "..") {
		http.NotFound(w, r)
		return
	}
	s.proxy.ServeHTTP(w, r)
}
