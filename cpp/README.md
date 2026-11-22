# Streaming Media Server (C++17)

> **Status (2026-11)** — every module below is now verified end-to-end in CI on every push
> (ASan, TSan, 6-minute soak, Docker smoke, third-party clients: ffmpeg/ffprobe, aiortc).
> See [ROADMAP.md](ROADMAP.md) for what was broken, what was fixed, and what is still open.
>
> | Verified | Notes |
> |---|---|
> | RTMP ingest (H.264/AAC, H.265 via Enhanced RTMP) | simple + complex handshake |
> | RTMP / HTTP-FLV / WS-FLV / HLS / RTSP playback | B-frames, 48 kHz stereo, 8 concurrent viewers |
> | SRT ingest + playback (libsrt) | |
> | WebRTC WHIP ingest / WHEP playback | video only (H.264) |
> | DVR (fMP4), ABR transcode (external ffmpeg) | |
> | Origin–edge cluster (on-demand pull for FLV/RTMP/HLS) | |
> | Auth (admin token, per-stream JWT with stream/scope), webhooks, rate limits, connection limits | |
> | Slow-viewer backpressure, idle timeout, graceful shutdown with live sessions, SIGHUP vhost reload | |

A from-scratch, high-concurrency live streaming media server written in C++17.
RTMP / SRT / WebRTC-WHIP ingest → HLS / HTTP-FLV / WebSocket-FLV / RTSP /
WebRTC-WHEP / SRT egress, DVR/fMP4 recording + timeshift, optional ABR
transcode, multi-tenant vhosts, HTTP webhooks, and an origin/edge cluster.
Built as a systems-programming demonstration: a muduo-style multi-Reactor
networking core, hand-written binary protocol parsers, and a background
asynchronous file-IO writer.

> No framework codecs/servers are used for the data plane: RTMP chunk/AMF0
> (including Enhanced RTMP for H.265/HEVC), FLV, HLS TS/PES/m3u8, fMP4 boxes,
> SRT handshake/ACK/NAK, STUN/DTLS/SRTP/RTP for WebRTC, RTSP/RTP, HTTP, and the
> MD5 used by the consistent-hash ring are all implemented by hand. FFmpeg/libav
> is an *optional* transcode backend (with an external-ffmpeg fork/exec fallback
> and a build stub when absent); OpenSSL is optional (HMAC-SHA256 for RTMP
> complex handshake/JWT, DTLS/SRTP for WebRTC, with a compile stub when absent).

## Architecture

```
                 ┌─────────────────────────────────────────────────┐
 publisher ──RTMP▶│  Acceptor (mainReactor / baseLoop)              │
                 │      │  new conn round-robin                     │
                 │      ▼                                           │
                 │  subReactor pool (one EventLoop per thread) ──┐  │
                 │      │                                        │  │
                 │      ▼                                        ▼  │
 ffplay ─HTTP─▶  │  RtmpConnection       HttpServer (router+middleware)
 HLS/FLV         │   handshake/chunk       /live/*.m3u8  *.ts  *.flv
                 │   AMF0 commands         /api/*   /metrics
                 │      │                        │
                 │      ▼                        ▼
                 │   MediaStream ──sub──▶ RtmpToHls / FlvMuxer
                 │   (GOP cache, seq hdr)     │
                 │                            ▼
                 │                       HlsSource → TsMuxer/Segmenter
                 │                       FlvDemuxer (origin pull)    │
                 │                                                │
                 │   TranscodeManager   Cluster (registry / LB /   │
                 │   (libav | ffmpeg)    health+circuit breaker)   │
                 └─────────────────────────────────────────────────┘
```

## Modules

| Path        | Responsibility |
|-------------|----------------|
| `core/`     | `Buffer` (readv+extrabuf), `Logger`, `ThreadPool`, `MemoryPool`, `Timestamp`, `SignalGuard`, `AsyncFileWriter` (single background IO worker) |
| `net/`      | epoll `EventLoop`, multi-Reactor `TcpServer`, non-blocking `Connector`/`TcpClient`, `UdpServer`, `TimerQueue` (timerfd), `Channel`, `TcpConnection` (shared_ptr lifecycle, high-water backpressure, TCP_INFO) |
| `api/`      | hand-written HTTP request/response/router, JSON, JWT (HS256), streaming-response path for HTTP-FLV |
| `rtmp/`     | RTMP handshake (simple + HMAC-SHA256 complex), chunk framer/writer, AMF0, `RtmpConnection` session FSM, `MediaStream` GOP cache, AVCC↔Annex-B, **Enhanced RTMP + H.265/HEVC (`HevcParser`)**, AAC ADTS |
| `protocol/` | FLV muxer/demuxer, HTTP-FLV long-conn handler, RFC6455 `WebSocket` + WS-FLV |
| `hls/`      | MPEG-TS/PES/CRC32, `TsMuxer`, `TsDemuxer` (TS→MediaPackets for SRT ingest), `M3u8Playlist` (live/event/VOD + `EXT-X-PROGRAM-DATE-TIME`), `Segmenter`, `HlsSource`/`HlsManager`, ABR master playlist, HTTP handler (sendfile + Range/206) |
| `cache/`    | `LruCache`/`ShardedLruCache` (16 shards), lock-free SPSC `RingBuffer`, mmap `FileCache`, `StreamCache` (optional Redis L2), `RedisClient` (hand-rolled RESP2, dedicated worker thread, circuit breaker, fail-open) |
| `dvr/`      | ISO-BMFF `Mp4BoxWriter`, `Fmp4Recorder` (fMP4 timeshift + VOD), `DvrManager` (scheduled/once recording), `DvrHandler` API, `Mp4Faststart` (moov relocation), `DiskWatermark` |
| `srt/`      | `SrtProtocol`, `SrtSession` (handshake/ACK/ACKACK/NAK/keepalive, reorder + retransmit buffer), `SrtServer` (TS ingest/play) |
| `webrtc/`   | STUN, SDP, RTP packetizer/depacketizer, RTCP, DTLS transport (memory BIO), SRTP, ICE-lite, WHIP ingest / WHEP playback |
| `rtsp/`     | RTSP message/SDP, RTP over TCP interleaving, H.264 FU-A, AAC RFC3640 |
| `webhook/`  | async/sync HTTP webhooks (SRS-compatible `code==0`, per-vhost URLs, timeout/retry) |
| `config/`   | env + KEY=VALUE file config, `[vhost:*]` multi-tenant sections, `VhostManager` |
| `transcode/`| RAII libav wrappers, `Decoder`/`Encoder`/`Transcoder`, `ProcessManager` (fork/exec + signalfd SIGCHLD + non-blocking stderr), `TranscodeManager` (priority queue + task lifecycle/stats + TTL archive), `AbrManager` |
| `cluster/`  | `ConsistentHash` (150 vnodes, self-contained MD5), `NodeRegistry`, `LoadBalancer` (RR/least-conn/hash), `HealthChecker` + circuit breaker, `OriginEdge` HTTP-FLV pull, `StreamForwarder` (RTMP multi-destination push, strict rtmp/rtmps URL whitelist), reverse proxy / 302 |
| `metrics/`  | `Counter`/`Gauge`/`Histogram` + Prometheus text exposition, `SystemStats` (/proc CPU/mem/loadavg/uptime) |
| `middleware/`| sharded token-bucket `RateLimiter` + per-route policies (longest-prefix, Redis fixed-window layer), JWT `AuthFilter` (revocation-aware), `AccessLog` (plain/JSON + trace id) |
| `auth/`     | JWT token blacklist (local + Redis shared, rehydrating) |

## Build

```bash
# Full build (auto-detects OpenSSL + FFmpeg libav; stubs are used if absent)
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build -j

# Debug + ASan/UBSan
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
cmake --build cpp/build -j

# With benchmarks
cmake -S cpp -B cpp/build -DENABLE_BENCHMARKS=ON
```

Run: `./cpp/build/bin/streaming_server [streaming.conf]` (HTTP on 8080,
RTMP on 1935, SRT/UDP on 19535, RTSP on 8554, WebRTC/UDP 50000–50999).
See `streaming.conf.example` for all global keys and `[vhost:*]` sections.
Reload config/vhosts at runtime with `SIGHUP` (no connection drop).

## Tests & benchmarks

```bash
ctest --test-dir cpp/build --output-on-failure
# benchmarks
./cpp/build/bin/bench_buffer && ./cpp/build/bin/bench_lru \
  && ./cpp/build/bin/bench_http && ./cpp/build/bin/bench_concurrent
```

## Configuration

Sources (later overrides earlier): defaults → config file → `STREAM_*` env.

| Key / env | Default | Description |
|-----------|---------|-------------|
| `STREAM_HTTP_PORT` | 8080 | HTTP/HLS/HTTP-FLV/API port |
| `STREAM_RTMP_PORT` | 1935 | RTMP ingest port |
| `STREAM_SRT_PORT` | 19535 | SRT ingest/play UDP port |
| `STREAM_IO_THREADS` | 4 | subReactor threads |
| `STREAM_HLS_SEGMENT_DURATION` | 6 | TS segment target duration (s) |
| `STREAM_HLS_WINDOW_SIZE` | 5 | live sliding window (segments) |
| `STREAM_HLS_OUTPUT_DIR` | `./hls` | segment output directory |
| `STREAM_CACHE_SIZE_MB` | 256 | hot-segment cache |
| `STREAM_JWT_SECRET` | random per start | **must be set in production** (random → tokens die on restart) |
| `STREAM_ADMIN_TOKEN` | empty | bearer token for admin APIs; empty = admin API disabled |
| `STREAM_LOG_LEVEL` / `STREAM_LOG_JSON` | `INFO` / off | log level; JSON lines with trace ids for Loki/ELK |
| `STREAM_RATE_LIMIT_PER_SEC` / `_BURST` | 100 / 200 | per-IP global token bucket (routes without a policy) |
| `STREAM_RATE_{PLAYBACK,API,AUTH,TRANSCODE,ADMIN}_PER_MIN` | 300/100/10/5/20 | per-route rate policies (longest-prefix match; 0 = off) |
| `STREAM_REDIS_HOST` | (empty) | enable distributed state: rate limiting, JWT blacklist sharing, segment cache L2 (RESP2 client with fail-open) |
| `STREAM_REDIS_PORT` / `_TIMEOUT_MS` / `_PASSWORD` | 6379 / 50 / – | Redis endpoint tuning |
| `STREAM_ORIGIN_URL` | (empty) | set on edge nodes to HTTP-FLV pull from origin |
| `STREAM_CLUSTER_NODES` | (empty) | comma-separated `ip:httpPort:rtmpPort` peers |
| `STREAM_TRANSCODE_PROFILES` | `480p,720p` | ABR ladder |
| `STREAM_TRANSCODE_CONCURRENCY` | #cpu | max concurrent transcode jobs |
| `STREAM_TRANSCODE_TASK_TTL_SEC` | 3600 | finished-task retention for `GET /api/transcode` |

Per-tenant configuration lives in `[vhost:<name>]` sections (exact domain,
`*.suffix` wildcard, or `__default__`): `enable_auth`, `jwt_secret`,
`allow_apps`, `webhook_on_publish/play/unpublish/stop/dvr/hls`, `enable_hls`,
`enable_dvr`/`dvr_dir`, `enable_transcode`/`transcode_profiles`,
`max_publishers`/`max_viewers`/`max_bandwidth_bps`. See
`streaming.conf.example` for the full list.

## Integrating with ffmpeg / ffplay

```bash
# Publish (RTMP)
ffmpeg -re -i in.mp4 -c copy -f flv rtmp://127.0.0.1:1935/live/test

# Publish H.265/HEVC over Enhanced RTMP
ffmpeg -re -i in.mp4 -c:v libx265 -c:a aac -f flv \
  -flvflags no_duration_filesize rtmp://127.0.0.1:1935/live/test

# Publish over SRT (MPEG-TS)
ffmpeg -re -i in.mp4 -c copy -f mpegts \
  "srt://127.0.0.1:19535?streamid=#!::r=live/test,m=publish"

# Play HLS / HTTP-FLV / WebSocket-FLV / RTSP / SRT
ffplay http://127.0.0.1:8080/live/test.m3u8
ffplay http://127.0.0.1:8080/live/test.flv
ffplay rtsp://127.0.0.1:8554/live/test
ffplay "srt://127.0.0.1:19535?streamid=#!::r=live/test,m=request"

# Auth（vhost 开启 enable_auth 时，推/拉流 URL 需带 ?token=<jwt>）
# 由管理员签发流 token：stream 绑定流名，scope=publish|play|all，ttl 秒
TOKEN=$(curl -s -XPOST -H "Authorization: Bearer $ADMIN_TOKEN" \
  "localhost:8080/api/token?stream=test&scope=publish&ttl=3600" | jq -r .token)
ffmpeg -re -i input.mp4 -c copy -f flv "rtmp://127.0.0.1/live/test?token=$TOKEN"
# HLS/HTTP-FLV/WS-FLV 同样校验 ?token=（m3u8 中分片 URI 会自动携带 token）

# DVR control
curl -XPOST localhost:8080/api/record/test/start
curl -XPOST localhost:8080/api/record/test/stop

# Forward a live stream to multiple upstreams (rtmp/rtmps only)
curl -XPOST "localhost:8080/api/forward/test?url=rtmp://a.example.com/live/test"

# Transcode profiles (per-vhost) and task management
curl localhost:8080/api/profiles
curl -XPOST localhost:8080/api/transcode \
  -d 'input=rtmp://127.0.0.1:1935/live/test&output=rtmp://127.0.0.1:1935/live/testv&profiles=480p&priority=5'
curl localhost:8080/api/transcode    # task list + stats
```

Every HTTP response carries an `X-Trace-Id` header (echoed from the client's
`X-Request-ID` when supplied) that is also attached to every log line produced
while serving that request — grep it to follow one request across access logs
and server logs. With `STREAM_LOG_JSON=1` both access and server logs are
single-line JSON ready for Loki/ELK ingestion.

### Docker

```bash
docker build -t streaming-server cpp/
docker run --rm -p 8080:8080 -p 1935:1935 -p 19999:19999/udp \
  -e STREAM_JWT_SECRET=change-me streaming-server
```

## Resume highlights

- epoll **multi-Reactor** (main/sub loops, one-loop-per-thread, `eventfd` wakeup,
  `timerfd` timers, non-blocking connector with exponential backoff) plus a UDP
  reactor for SRT/WebRTC.
- Hand-written protocol stacks: **RTMP** (simple + HMAC complex handshake, chunk
  fmt 0–3 + extended timestamp, AMF0, **Enhanced RTMP / H.265-HEVC**), **FLV**,
  **HLS** (MPEG-TS/PES/CRC32, TS demuxer, live/event/VOD playlists), **SRT**
  (handshake/ACK/ACKACK/NAK, reorder + retransmit, TS over SRT), **RTSP/RTP**,
  and **WebRTC** (STUN/DTLS/SRTP/ICE-lite/RTP packetization, WHIP/WHEP).
- **DVR / fMP4**: hand-rolled ISO-BMFF boxes (ftyp/moov/moof/mdat, avcC/hvcC,
  trun/tfhd/tfdt), timeshift segments, VOD recording with moov faststart
  relocation, disk-watermark eviction.
- **Async disk IO**: a single background writer takes all HLS/DVR writes/`fsync`s
  off Reactor threads with a bounded byte queue.
- **Origin/edge cluster**: MD5 consistent hash, least-conn load balancing,
  active health probes + 3-state circuit breaker, HTTP-FLV origin pull, and
  multi-destination RTMP push forwarding.
- **Multi-tenancy**: vhost matching (exact / `*.suffix` / default) with
  per-vhost auth, quotas, webhooks, HLS/DVR policies; config + vhost hot-reload
  via `SIGHUP` without dropping connections.
- Production: Prometheus metrics (+ `/proc` system gauges), per-route rate
  limiting (5 policy families, optional Redis fixed-window layer with fail-open),
  JWT auth with revocation blacklist (HTTP + RTMP/SRT via webhook),
  per-request trace ids + JSON logging, transcode task priority queue with
  lifecycle/stats, SRS-compatible HTTP webhooks, structured access log,
  signalfd graceful shutdown.
- Optional FFmpeg/libav transcode (decode→scale/resample→encode) with an
  external-ffmpeg fork/exec fallback; builds without OpenSSL/libav using stubs.

## Status

Implemented end-to-end: RTMP (H.264 + H.265/Enhanced-RTMP) and SRT ingest;
HLS, HTTP-FLV, WebSocket-FLV, RTSP, SRT, and WebRTC-WHEP egress; DVR/fMP4
timeshift + VOD; ABR transcode; multi-platform RTMP forwarding; origin/edge
cluster; vhost multi-tenancy; HTTP webhooks; JWT auth (with logout/revocation);
per-route rate limiting; trace-id logging; transcode task management; and
metrics — the full control-plane feature set of the retired Python prototype
plus everything beyond it. Optional Redis gives distributed rate limiting,
shared JWT blacklist, and a segment-cache L2 with automatic fail-open
degradation. The tree compiles with or without OpenSSL/libav (stubs are used
when absent). Protocol-by-protocol live validation against
`ffmpeg`/`ffplay`/OBS and load/performance data are the remaining non-code
work items.
