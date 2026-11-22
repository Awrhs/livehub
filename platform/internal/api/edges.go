package api

import (
	"context"
	"hash/fnv"
	"net/http"
	"sync/atomic"
	"time"
)

// edge 边缘播放节点：C++ 媒体服务器以边缘模式运行（ORIGIN_URL 指向本平台），
// 观众从边缘拉流，边缘按需经平台播放代理回源，分摊源站出口带宽。
type edge struct {
	url     string
	healthy atomic.Bool
}

func (s *Server) initEdges() {
	for _, u := range s.Cfg.EdgeURLs {
		s.edges = append(s.edges, &edge{url: u})
	}
}

// checkEdges 探测边缘节点健康状态（/api/health）。
func (s *Server) checkEdges(ctx context.Context) {
	client := &http.Client{Timeout: 3 * time.Second}
	for _, e := range s.edges {
		req, err := http.NewRequestWithContext(ctx, http.MethodGet, e.url+"/api/health", nil)
		if err != nil {
			continue
		}
		resp, err := client.Do(req)
		ok := err == nil && resp.StatusCode == http.StatusOK
		if resp != nil {
			resp.Body.Close()
		}
		if was := e.healthy.Swap(ok); was != ok {
			s.Log.Info("边缘节点状态变化", "edge", e.url, "healthy", ok)
		}
	}
}

// pickEdge 按频道名一致地选择健康的边缘节点（同一频道的观众集中在同一边缘，提高回源复用）；
// 无可用边缘时返回空串（由本机分发）。
func (s *Server) pickEdge(slug string) string {
	var healthy []*edge
	for _, e := range s.edges {
		if e.healthy.Load() {
			healthy = append(healthy, e)
		}
	}
	if len(healthy) == 0 {
		return ""
	}
	h := fnv.New32a()
	_, _ = h.Write([]byte(slug))
	return healthy[int(h.Sum32()%uint32(len(healthy)))].url
}

func (s *Server) runEdgeChecks(ctx context.Context) {
	if len(s.edges) == 0 {
		return
	}
	s.checkEdges(ctx)
	t := time.NewTicker(10 * time.Second)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			s.checkEdges(ctx)
		}
	}
}
