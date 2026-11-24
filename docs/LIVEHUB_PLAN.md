# LiveHub 开发计划

LiveHub：社区自部署的多主播直播平台（类 Twitch）。本文件记录设计、决策与进度，是自主迭代的依据。

## 已定决策（2026-11-22，用户确认）

| 项 | 决定 |
|---|---|
| 定位 | 社区/社团自部署的多主播直播平台 |
| 平台层 | Go（`platform/`），前端 React + TypeScript + Vite（`web/`），构建产物嵌入 Go 二进制 |
| 媒体层 | 现有 C++ 服务器（`cpp/`），通过 webhook + 管理 API 与平台交互 |
| 界面语言 | 中英双语，默认中文 |
| 许可证 | AGPL-3.0 |
| 默认策略 | 开放注册、开播需管理员审批（可配置 open / approval / invite）；首个注册用户为管理员 |
| 发布 | 三阶段完成且 CI 全绿 → v1.0.0 tag + GitHub Release + ghcr.io/awrhs/livehub |

## 自行决定的技术选型

- 数据库：SQLite（modernc.org/sqlite，纯 Go，无 cgo），WAL 模式；单实例平台
- HTTP：标准库 net/http（Go 1.22+ 路由），WebSocket：github.com/coder/websocket
- 密码：bcrypt；会话：随机 token（库中存 SHA-256），HttpOnly + SameSite=Lax Cookie；
  写操作要求 JSON Content-Type 并校验 Origin（防 CSRF）
- 部署：单容器——Go 进程生成媒体服务器配置并作为子进程拉起、守护（崩溃自动重启）；
  也支持 docker compose 拆分部署（`LIVEHUB_MEDIA_EXTERNAL=1`）
- 播放：平台反向代理 `/live/*` 到媒体服务器（单一对外 HTTP 端口）；hls.js（默认）+ mpegts.js（低延迟 FLV）
- 依赖锁文件：本机无 Go/Node（内存不足），由 CI 生成 go.sum / package-lock.json

## 媒体层对接

- 推流地址：`rtmp://<host>/live`，推流码：`<频道名>?key=<密钥>`；SRT：`#!::r=live/<频道名>?key=<密钥>,m=publish`
- 媒体服务器 vhost 配置 `webhook_on_publish` / `webhook_on_unpublish` 指向平台
  `/internal/media/hook?secret=<内部密钥>`，`webhook_fail_closed=true`
- on_publish：平台校验频道存在、用户有开播权限且未被封禁、密钥匹配 → `{"code":0}`；否则 `{"code":403}`
- 开播后平台异步：调用 `/api/record/<频道>/start`（录像，可配）、`/api/abr/<频道>`（多清晰度，可配）
- on_unpublish：标记下播，停止录像，录像文件移入平台 VOD 目录，生成封面
- 对账：每 10s 拉 `/api/streams` 修正直播状态（防止回调丢失）
- 封禁/断流：`DELETE /api/streams/<频道>`
- 观众数：直播间页面的 WebSocket（聊天）在线数

C++ 改动（已完成）：`webhook_fail_closed`、on_publish/on_play 回调带 `param`（查询串）、`DVR_DIR`、
`SRT_PORT=0` 关闭 SRT、踢流真正断开推流连接、`INTERNAL_PUBLISH_TOKEN`（转码回推鉴权）、`DELETE /api/abr/:stream`、
转码任务并发执行、录像停止等待写盘、录像 moov 缺音频轨修复、ABR 主播放列表相对路径、
WHEP 音频（AAC→Opus）、WHEP 空闲会话回收、`WEBRTC_PORT_MIN/MAX`。

## 阶段与进度

### 阶段 1：MVP
- [x] 仓库骨架：LICENSE、platform/、web/、CI 工作流（Go 测试、前端构建、全链路 E2E）
- [x] 账号：注册/登录/登出/会话、首个用户为管理员、注册模式
- [x] 频道：每用户一个频道、标题/分类/简介、推流密钥查看与重置、开播申请与审批
- [x] 媒体对接：webhook 鉴权、直播状态、对账、子进程托管媒体服务器
- [x] 直播间列表与直播间页（播放器）
- [x] 聊天：WebSocket、历史、限速、登录后发言
- [x] 前端：首页、直播间、登录注册、主播后台、i18n
- [x] E2E：注册→审批→ffmpeg 推流→直播中→拉流→聊天→下播；错误密钥被拒

### 阶段 2：完善
- [x] 关注、开播通知（站内信，30 秒轮询）
- [x] 录像回放（VOD 列表、播放、删除）
- [x] 直播封面（ffmpeg 截帧）、VOD 封面
- [x] 多清晰度（ABR）与播放器清晰度切换
- [x] 管理后台：用户管理、开播审批、封禁、站点设置、举报处理、审计日志
- [x] 聊天管理：删除消息、禁言、房管
- [x] Playwright 浏览器 E2E

### 阶段 3：增强
- [x] 低延迟播放（HTTP-FLV / WebRTC，后者需音频 AAC→Opus）
- [x] 多节点：边缘媒体节点分发播放流量
- [x] 防滥用：登录/注册限速、聊天慢速模式与敏感词、邀请码
- [x] 部署文档、备份恢复、升级说明
- [x] 发布 v1.0.0（2026-11-22：GitHub Release + ghcr.io/awrhs/livehub:1.0.0，amd64 + arm64）

## 待用户确认的事项

（自主迭代中遇到的、需要用户事后确认的取舍记录在此）

- 推流密钥以明文存储在数据库（便于主播随时查看复制，同 Owncast 做法）；数据库文件需妥善保护。

## 第二轮：修复已知缺陷（2026-11-24）

用户决定：推流码只存摘要（仅生成时显示一次）；增加邮件功能（验证邮箱、找回密码）。

- [x] 推流码只存 SHA-256 摘要，生成时显示一次；v1.0.0 的明文推流码启动时自动迁移为摘要且继续有效
- [x] 邮件：注册/设置邮箱、邮箱验证、邮件找回密码、管理员测试发信；E2E 用 mailpit 验证真实 SMTP 投递
- [x] 管理员网页重置密码（临时密码）
- [x] 首页只列开播过的频道；头像上传；聊天表情与 @提及；观看人数按用户/IP 去重
- [x] 开播通知实时推送（SSE）+ 桌面通知
- [x] 手机端布局（播放器 → 信息 → 聊天 → 简介）
- [x] 安全响应头（CSP、禁止被嵌入等）
- [x] 磁盘保护（剩余空间不足时停止录像、通知管理员）、系统资源面板；媒体服务器可关闭时移录制
- [x] WebRTC 单端口复用（所有观众共用一个 UDP 端口）、RTCP 发送报告实现音画同步
- [x] 测试新增：OBS 风格推流（1080p60/High/B 帧/6Mbps）、断线重连、Chrome/Firefox/WebKit 兼容性、30 分钟长稳

CI 测出并已修复的新问题：
- 主播断线重连被当作新的一场（开播时长重置、回放被拆成多段）→ 宽限期内续用场次并自动合并回放
- 回放出现在列表时封面尚未生成 → 先截封面再登记
- 频道页加载瞬间开播时页面停留在「未开播」→ 聊天连接建立时同步直播状态
- 浏览器不支持 H.264（或其 WebRTC 不含 H.264）时播放器无限重试 → 给出明确提示
- 边缘节点启动晚于平台时最长 10 秒才被启用 → 不健康节点每 2 秒重试

已知限制（环境原因，非缺陷）：Playwright 自带的 Firefox 构建的 WebRTC 不含 H.264（正式版 Firefox 会自动下载 OpenH264 插件），
该情况下 WebRTC 模式提示切换到 FLV/HLS；Firefox 的 HLS 与 FLV 播放均通过测试。
