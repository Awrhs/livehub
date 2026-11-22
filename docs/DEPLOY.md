# LiveHub 部署与运维

## 一、快速开始（单机 Docker）

```bash
docker run -d --name livehub --restart unless-stopped \
  -p 8000:8000 -p 1935:1935 -p 10080:10080/udp \
  -v livehub-data:/data \
  ghcr.io/awrhs/livehub:latest
```

打开 `http://<服务器IP>:8000`，**第一个注册的账号自动成为管理员**。镜像支持 amd64 与 arm64（如树莓派 4/5、ARM 云服务器）。

或使用 docker compose（`deploy/` 目录）：

```bash
cd deploy
cp .env.example .env    # 按需修改
docker compose up -d
```

### 端口

| 端口 | 用途 |
|---|---|
| 8000/tcp | 网页、API、聊天、播放（HLS / HTTP-FLV） |
| 1935/tcp | RTMP 推流（OBS 等） |
| 10080/udp | SRT 推流（可选，不需要可不映射） |

媒体服务器的管理端口（8081）只在容器内使用，不要映射到外网。

## 二、开播（OBS）

1. 管理员在「管理后台 → 开播审批」批准主播（或在「站点设置」关闭「开播需要审批」）。
2. 主播打开「主播后台」，复制「服务器」和「推流码」。
3. OBS → 设置 → 直播：服务选「自定义」，填入服务器（如 `rtmp://live.example.com/live`）和推流码（形如 `alice?key=lh_xxx`）。
4. 推荐输出设置：x264、关键帧间隔 2 秒、码率 2500–6000 Kbps、音频 AAC 128 Kbps。

SRT 推流（网络较差时更稳定）：主播后台显示完整的 SRT 地址，OBS 中服务器填该地址、推流码留空。

## 三、HTTPS（推荐）

浏览器的剪贴板、部分播放能力要求 HTTPS。推荐在前面放 Caddy 自动申请证书（`deploy/Caddyfile`）：

```
live.example.com {
	reverse_proxy livehub:8000 {
		flush_interval -1
	}
}
```

并设置环境变量：

```
LIVEHUB_PUBLIC_URL=https://live.example.com
LIVEHUB_TRUST_PROXY=true
```

使用 Nginx 时，`/live/` 路径需关闭缓冲（`proxy_buffering off;`），`/api/v1/channels/` 路径需支持 WebSocket 升级。

RTMP/SRT 推流端口不经过反向代理，直接映射到容器。

## 四、配置项

全部通过环境变量配置；站点名称、注册方式、审批、录像、转码、聊天规则等在网页「管理后台 → 站点设置」中修改，无需重启。

| 变量 | 默认值 | 说明 |
|---|---|---|
| `LIVEHUB_ADDR` | `:8000` | 平台监听地址 |
| `LIVEHUB_DATA_DIR` | `/data` | 数据目录（数据库、录像、封面） |
| `LIVEHUB_PUBLIC_URL` | 空 | 对外地址，用于生成推流地址；以 https 开头时 Cookie 加 Secure |
| `LIVEHUB_RTMP_URL` | `rtmp://<访问域名>/live` | 对外展示的 RTMP 推流地址 |
| `LIVEHUB_SRT_URL` | `srt://<访问域名>:10080` | 对外展示的 SRT 地址 |
| `LIVEHUB_TRUST_PROXY` | `false` | 部署在反向代理后时开启，按 X-Forwarded-For 识别客户端 IP |
| `LIVEHUB_EDGE_URLS` | 空 | 边缘播放节点（逗号分隔），见「多节点」 |
| `LIVEHUB_LOG_LEVEL` | `info` | debug / info / warn / error |
| `LIVEHUB_MEDIA_RTMP_PORT` | `1935` | 媒体服务器 RTMP 端口 |
| `LIVEHUB_MEDIA_SRT_PORT` | `10080` | 媒体服务器 SRT 端口，0 为关闭 |
| `LIVEHUB_WEBRTC_CANDIDATE_IP` | 空 | 设置后开启 WebRTC 播放（服务器对外 IP） |
| `LIVEHUB_WEBRTC_PORT_MIN` / `_MAX` | `50000` / `50099` | WebRTC UDP 端口范围 |

内部密钥（媒体服务器管理令牌、回调密钥）首次启动时自动生成并保存在数据库中。

## 五、WebRTC 超低延迟播放（可选）

默认提供标准（HLS，约 6–10 秒延迟）与低延迟（HTTP-FLV，约 1–3 秒）两种播放模式。需要亚秒级延迟时可开启 WebRTC：

```bash
docker run ... \
  -e LIVEHUB_WEBRTC_CANDIDATE_IP=<服务器公网 IP> \
  -p 50000-50099:50000-50099/udp \
  ghcr.io/awrhs/livehub:latest
```

- 每个 WebRTC 观众占用一个 UDP 端口，端口范围由 `LIVEHUB_WEBRTC_PORT_MIN` / `LIVEHUB_WEBRTC_PORT_MAX` 配置（默认 50000–50099，即最多 100 名 WebRTC 观众），映射的端口范围必须与之一致。
- 音频由服务器实时从 AAC 转码为 Opus。
- 浏览器关闭后 30 秒内自动回收会话与端口。
- 开启后播放器出现「超低延迟（WebRTC）」选项；其余观众仍可使用 HLS / FLV。

## 六、多节点（边缘分发）

单台服务器的瓶颈通常是出口带宽：一路 3 Mbps 的直播有 1000 人观看需要约 3 Gbps。观众较多时，可以增加**边缘节点**分担播放流量：

1. 在另一台服务器运行媒体服务器的边缘模式（使用 `cpp/Dockerfile` 构建的镜像），配置回源到 LiveHub：

   ```
   STREAM_ORIGIN_URL=http://live.example.com:8000
   ```

   或配置文件中写 `ORIGIN_URL=http://live.example.com:8000`。边缘节点在有观众请求某个频道时，按需从 LiveHub 拉流。

2. 在 LiveHub 中配置边缘节点的对外地址：

   ```
   LIVEHUB_EDGE_URLS=https://edge1.example.com,https://edge2.example.com
   ```

LiveHub 每 10 秒检查一次边缘节点健康状态，按频道名把观众分配到健康的边缘节点（同一频道的观众集中在同一边缘，提高回源复用）；边缘全部不可用时自动回落到本机分发。推流、聊天、录像仍由主节点负责。

## 七、备份与恢复

数据目录 `/data` 包含：

| 路径 | 内容 |
|---|---|
| `livehub.db*` | 数据库（用户、频道、聊天、设置） |
| `vods/` | 录像回放文件 |
| `thumbs/` | 封面 |
| `media/` | 媒体服务器工作目录（HLS 分片等临时文件，无需备份） |

在线备份数据库（服务运行中可执行，生成一致快照）：

```bash
docker exec livehub /opt/livehub/livehub backup /data/backup-$(date +%F).db
```

恢复：停止容器，把备份文件复制为 `/data/livehub.db`（同时删除 `livehub.db-wal`、`livehub.db-shm`），再启动。

## 八、常用运维命令

```bash
# 忘记管理员密码：重置密码（同时注销该用户所有登录）
docker exec livehub /opt/livehub/livehub reset-password admin 新密码至少8位

# 把用户设为管理员（例如原管理员账号被误封）
docker exec livehub /opt/livehub/livehub make-admin alice

# 查看版本
docker exec livehub /opt/livehub/livehub -version

# 健康检查
curl http://127.0.0.1:8000/healthz
```

## 九、升级

```bash
docker pull ghcr.io/awrhs/livehub:latest
docker compose up -d     # 或删除旧容器后用相同参数重新 docker run
```

数据库结构在启动时自动迁移。升级前建议先执行一次备份。

## 十、资源与容量参考

- 转码（多清晰度）默认关闭：每路直播开启后约占用 1–2 个 CPU 核心。
- 录像按原始码率保存：3 Mbps 的直播每小时约 1.3 GB，可在站点设置中配置保留天数。
- 观众数统计基于直播间页面的在线连接数。
