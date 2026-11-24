# LiveHub

**社区自部署的多主播直播平台。**一条命令部署，主播用 OBS 推流，观众在网页上看直播、聊天、关注、看回放。

[![LiveHub](https://github.com/Awrhs/livehub/actions/workflows/livehub.yml/badge.svg)](https://github.com/Awrhs/livehub/actions/workflows/livehub.yml)
[![C++](https://github.com/Awrhs/livehub/actions/workflows/cpp.yml/badge.svg)](https://github.com/Awrhs/livehub/actions/workflows/cpp.yml)
[![License: AGPL v3](https://img.shields.io/badge/License-AGPL_v3-blue.svg)](LICENSE)

适合学校、社团、游戏社区、企业内部等需要**自己掌控数据**的场景：多个主播共用一个站点，管理员审批开播、管理用户与内容。

## 功能

| 观众 | 主播 | 管理员 |
|---|---|---|
| 直播列表、分类、搜索 | OBS（RTMP）/ SRT 推流 | 首个注册账号即管理员 |
| 网页播放：标准 HLS、低延迟 FLV（1–3 秒）、超低延迟 WebRTC（可选） | 推流码管理（随时重置） | 注册方式：开放 / 邀请码 / 关闭 |
| 多清晰度自动切换（开启转码时） | 直播标题、分类、简介 | 开播审批、封禁、断流 |
| 实时聊天、观看人数 | 自动录像，下播后生成回放 | 举报处理、审计日志 |
| 关注主播、开播通知 | 任命房管，删除消息、禁言 | 聊天慢速模式、屏蔽词 |
| 录像回放（可拖动） | 申请开播 | 录像保留天数、站点公告 |
| 中文 / English 界面 | | 边缘节点分担播放流量 |

## 快速开始

```bash
docker run -d --name livehub --restart unless-stopped \
  -p 8000:8000 -p 1935:1935 -p 10080:10080/udp \
  -v livehub-data:/data \
  ghcr.io/awrhs/livehub:latest
```

打开 `http://<服务器IP>:8000` 注册第一个账号（自动成为管理员）。主播在「主播后台」复制推流地址与推流码，填入 OBS 即可开播。

HTTPS、配置项、多节点、备份与升级见 **[部署文档](docs/DEPLOY.md)**。

## 架构

```
                 ┌──────────────────────── LiveHub 容器 ────────────────────────┐
  OBS ──RTMP/SRT──►  C++ 媒体服务器  ──webhook（推流鉴权/开播/下播）──►  Go 平台  │◄── 浏览器
                 │  RTMP/SRT 接入、HLS/FLV/WebRTC 分发、    管理 API ◄──  账号、频道、聊天、  │   （React 前端、
                 │  录像、多清晰度转码                                 通知、回放、管理后台  │    播放经平台代理）
                 └──────────────────────────────────────────────────────────────┘
                         ▲ 回源（HTTP-FLV）
                   边缘媒体节点（可选，分担播放带宽）
```

- **平台层** `platform/`：Go，SQLite（纯 Go 驱动，无需单独数据库），内嵌前端，单个二进制。
- **前端** `web/`：React + TypeScript + Vite，hls.js / mpegts.js / WebRTC 播放。
- **媒体层** `cpp/`：C++17 自研流媒体服务器（RTMP、SRT、WebRTC、HLS、HTTP-FLV、DVR、ABR），由平台作为子进程托管。详见 [cpp/README.md](cpp/README.md)。

## 质量保障

每次推送由 GitHub Actions 验证：

- Go 单元测试（含竞态检测）：账号、审批、推流鉴权、聊天、关注通知、房管禁言、举报、边缘选择……
- 全链路 E2E：真实 ffmpeg 推流 → 平台鉴权 → HLS / HTTP-FLV / 边缘节点播放 → 多清晰度 → 录像回放 → 断流 / 封禁 → 媒体服务器崩溃自动恢复
- 浏览器 E2E（Playwright + Chrome）：管理员初始化 → 主播申请 → 审批 → 推流 → 观众网页观看（HLS / FLV / WebRTC）→ 聊天 → 关注
- 一体化 Docker 镜像冒烟测试
- 媒体服务器：ASan / TSan / 6 分钟 soak / 第三方客户端（ffmpeg、aiortc）互通

## 开发

本地开发（需要 Go 1.23、Node 22、CMake 与 C++17 编译器）：

```bash
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Release && cmake --build cpp/build -j --target streaming_server
cd web && npm install && npm run build && cp -r dist/* ../platform/internal/webui/dist/ && cd ..
cd platform && LIVEHUB_MEDIA_BIN=../cpp/build/bin/streaming_server go run ./cmd/livehub
```

前端热更新：另开终端 `cd web && npm run dev`（代理到 :8000）。

设计与进度记录见 [docs/LIVEHUB_PLAN.md](docs/LIVEHUB_PLAN.md)。

## 许可证

[GNU AGPL-3.0](LICENSE)：可自由使用、修改、部署；修改后以网络服务形式向他人提供时，须同样开源修改内容。
