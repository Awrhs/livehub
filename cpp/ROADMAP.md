# 生产可用差距分析与路线图

> 2026-11-21 起草，随迭代持续更新。所有验证均在 GitHub Actions 上完成
> （单元测试三矩阵 + ffmpeg 端到端测试 + ASan）。

## 现状结论

此前的开发约定是"只写代码、不编译不运行"，**整个代码库（约 2.6 万行）从未被编译或运行过**。
首次接入 CI 后发现：代码无法编译（约 60 处编译/链接错误），修通后核心链路几乎每一环都有致命 bug。
经过第一轮修复，**核心链路已端到端打通并由 CI 持续验证**：

| 能力 | 状态 | 验证方式 |
|------|------|----------|
| RTMP 推流（ffmpeg，H.264+AAC，简单/复杂握手） | ✅ 可用 | E2E |
| RTMP 拉流 | ✅ 可用 | E2E：ffprobe + 解码 125 帧无错误 |
| HTTP-FLV 拉流 | ✅ 可用 | E2E：同上 |
| HLS（m3u8 + TS） | ✅ 可用 | E2E：TS 结构 + 解码无错误 |
| /api/health、/metrics | ✅ 可用 | E2E |
| 畸形输入不崩溃、SIGTERM 优雅退出、ASan 干净 | ✅ | E2E |
| WS-FLV / 鉴权 / 限流 / vhost / webhook | ⚠️ 编译通过，未经端到端验证 | — |
| SRT / WebRTC / RTSP / 转码 / DVR / 集群 | ⚠️ 编译通过，未经验证，推测问题很多 | — |

### 第一轮已修复的致命问题（节选）

- **HTTP**：所有响应被丢弃（`send(outputBuffer())` 自我追加后被清空）；epoll 忙循环；
  sendfile 在 Reactor 线程忙等；非法 Content-Length 抛异常打崩进程；
  长连接请求间泄漏参数/鉴权属性；vhost 按 Host 匹配从未生效
- **RTMP**：握手在 C0C1 后即判定完成（所有连接必然失败）；复杂握手算法与规范不符；
  chunk 解析半包时错位；推流结束必然 use-after-free；播放从不下发数据
- **HLS/TS**：所有 PID +0x1000（PAT 不在 PID 0）；PCR 编码错误；payload 末尾补 0xFF 污染 ES；
  SPS/PPS 从未提取成功且未在 IDR 前内联；AAC 偏移错 1 字节；分片 URI 404
- **并发/生命周期**：RTMP 连接表多线程无锁读写；TcpConnection 延迟回调无保活；
  HTTP-FLV 会话立即析构；信号投递到工作线程直接杀进程；SignalGuard 覆盖监听集合
- **fMP4**：trun 标志位全部错位（DVR/时移产物无法播放）

## 路线图

优先级：**P0 = 不解决就不能上线**；P1 = 上线后很快会遇到；P2 = 扩展能力。

### P0 —— 核心链路生产化

- [ ] **慢观众背压**：每个观众的输出缓冲无上限，一个慢客户端即可耗尽内存。
      需要高水位丢帧（丢到下一个关键帧）+ 超限断开
- [ ] **重复推流**：同名流二次推流会静默覆盖（流劫持）。应拒绝（`NetStream.Publish.BadName`）
- [ ] **流名参数**：`live/stream?token=xxx` 的查询串需剥离并用于鉴权，避免进入流名
- [ ] **HLS 分片完整性**：分片在异步写盘完成前就进入 m3u8，播放器可能拿到半截文件；
      断流时补 `#EXT-X-ENDLIST`，延迟清理分片目录
- [ ] **超时**：推流端停止发送（假死）检测；空闲连接超时默认开启
- [ ] **ThreadSanitizer CI**：发布者线程直接回调各观众连接，跨线程交互多，需要 TSan 持续检查
- [ ] **E2E 扩展**：B 帧（`-bf 2`）、48kHz 立体声、多观众并发、推流断开重连、
      WS-FLV、推拉流期间 RSS 不持续增长（soak）

### P1 —— 安全与运维

- [ ] 鉴权端到端验证：RTMP 推/拉、HTTP-FLV/HLS 的 JWT；默认 `JWT_SECRET` 为弱值时拒绝启动或告警
- [ ] 连接数上限（全局 / 单 IP）
- [ ] RTMP Acknowledgement（窗口确认）发送，兼容严格客户端
- [ ] 时间戳跳变/回退处理（分片 discontinuity、FLV 时间戳单调化）
- [ ] HLS 响应头（`Cache-Control`、CORS）、m3u8 与分片的 MIME 类型
- [ ] Dockerfile 在 CI 中构建并做冒烟测试
- [ ] 编译告警清零（`LOG_*` 变参宏告警等），Release 构建开启 `-Werror`
- [ ] 文档：配置项说明与实际代码对齐；部署指南

### P1 —— 性能

- [ ] TS 分片 sendfile 零拷贝（需 TcpConnection 支持带 EPOLLOUT 续传的文件发送）
- [ ] 每包分发的内存拷贝（`MediaPacket::data` 向每个观众各拷贝一次）改为共享缓冲
- [ ] 基准：单机 N 路推流 × M 观众的 CPU/内存曲线（CI 中跑小规模）

### P2 —— 扩展协议与功能（核心稳定后）

按"能否端到端验证"逐个推进；无法验证的模块宁可暂时关闭，也不对外宣称支持。

- [ ] Enhanced RTMP / H.265 推流（ffmpeg 7 原生支持）E2E
- [ ] SRT：评估改用 libsrt 替换手写协议栈
- [ ] WebRTC（WHIP/WHEP）：评估改用 libdatachannel 等成熟库
- [ ] RTSP 拉流 E2E
- [ ] DVR / fMP4 录制产物可播放性验证
- [ ] 转码 / ABR
- [ ] 源站-边缘集群 E2E（两个实例）

## 开发流程

- 本机内存很小，**不在本机编译或测试**；所有验证走 GitHub Actions
- 每次修改：推送到 `main` → CI（三矩阵单测 + E2E/ASan）→ 依据失败输出迭代
- E2E 失败时自动上传 `e2e-artifacts`（服务器日志、HLS 分片、拉流日志），用于离线分析
