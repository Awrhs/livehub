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
| 重复推流拒绝、流名查询串、断流 ENDLIST / 重推 DISCONTINUITY | ✅ | E2E |
| 慢观众背压（丢帧至关键帧 / 超限断开）、空闲超时 | ✅ | E2E |
| TSan 下全部 E2E 场景无数据竞争报告 | ✅ | E2E (TSan) |
| 鉴权：管理令牌、流 token（stream/scope 绑定）、RTMP/HTTP-FLV/HLS 播放鉴权 | ✅ | E2E |
| WS-FLV | ✅ | E2E |
| 带活跃推流/观众时 SIGTERM 优雅退出 | ✅ | E2E (ASan/TSan)、Soak |
| 6 分钟连接反复建立/销毁：RSS/fd 不增长 | ✅ | Soak（Release） |
| Docker 镜像构建、推拉流、docker stop | ✅ | Docker 冒烟 |
| H.265 / Enhanced RTMP（RTMP/HTTP-FLV/HLS 拉流） | ✅ | 扩展 E2E |
| RTSP 拉流（TCP） | ✅ | 扩展 E2E |
| DVR 录制为 fMP4（可播放、从关键帧开始、音视频同基准） | ✅ | 扩展 E2E |
| SRT（libsrt）：推流 → FLV/HLS、RTMP 推流 → SRT 拉流、鉴权、拒绝抢占 | ✅ | 扩展 E2E |
| webhook（on_publish/on_play/on_unpublish 通知、code≠0 拒绝、慢回调不阻塞 IO 线程） | ✅ | 运维 E2E |
| 限流（API 429、不误伤播放）、连接数上限（全局/单 IP） | ✅ | 运维 E2E |
| WebRTC：WHIP 推流 → FLV/HLS、WHEP 拉流（与 aiortc 第三方实现互通，仅视频 H.264） | ✅ | 扩展 E2E |
| 源站-边缘集群：边缘按需回源（HTTP-FLV / RTMP / HLS），不存在的流不形成重连风暴 | ✅ | 扩展 E2E |
| 转码 / ABR（外部 ffmpeg，变体流 + master playlist） | ✅ | 扩展 E2E |
| 集群负载均衡 / 健康检查 / 反向代理 / 多平台转推 | ⚠️ 编译通过，未经验证 | — |

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

- [x] **慢观众背压**：每个观众的输出缓冲无上限，一个慢客户端即可耗尽内存。
      需要高水位丢帧（丢到下一个关键帧）+ 超限断开
- [x] **重复推流**：同名流二次推流会静默覆盖（流劫持）。应拒绝（`NetStream.Publish.BadName`）
- [x] **流名参数**：`live/stream?token=xxx` 的查询串需剥离并用于鉴权，避免进入流名
- [x] **HLS 分片完整性**：分片在异步写盘完成前就进入 m3u8，播放器可能拿到半截文件；
      断流时补 `#EXT-X-ENDLIST`，延迟清理分片目录
- [x] **超时**：推流端停止发送（假死）检测；空闲连接超时默认开启（读写均无进展 30s 断开）
- [x] **ThreadSanitizer CI**：发布者线程直接回调各观众连接，跨线程交互多，需要 TSan 持续检查
- [x] **E2E 扩展**：B 帧、48kHz 立体声、8 路并发观众、重复推流、断流重推、慢观众、空闲超时
- [x] **E2E 补齐**：WS-FLV；6 分钟 soak（RSS/fd 稳定）；带活跃连接的优雅退出

### P1 —— 安全与运维

- [x] 鉴权重做并端到端验证：此前 `/api/token` 无需鉴权（任何人可领 token）、HTTP 播放不鉴权、
      源码内置 JWT 密钥。现管理员签发绑定 stream/scope/ttl 的 token，RTMP/HTTP-FLV/HLS/WS-FLV 统一校验
- [x] 连接数上限（全局 / 单 IP）：MAX_CONNECTIONS / MAX_CONNECTIONS_PER_IP
- [x] RTMP Acknowledgement（窗口确认）发送，兼容严格客户端
- [x] 时间戳跳变/回退处理：推流时间戳回退 >2s 或前跳 >10s 时平移为连续时间线，所有输出（FLV/HLS/WebRTC/录像）单调；单元测试覆盖
- [x] HLS 响应头：m3u8 `application/vnd.apple.mpegurl`、分片 `video/MP2T`、CORS、分片 `Cache-Control: public, max-age=60`
- [x] Dockerfile 在 CI 中构建并做冒烟测试（修复默认 CMD 引用不存在的配置、工作目录不可写）
- [x] GCC 编译告警清零并开启 `-Werror`；clang Release 同样开启 `-Werror`，告警为 0
- [x] 文档：`streaming.conf.example` 覆盖全部 68 个配置项，CI（`tools/check_config_docs.sh`）检查代码与文档一致；部署指南见 `docs/DEPLOY.md`

### 第二轮修复的致命问题（节选）

- **所有连接 fd 都是阻塞的**（`::accept` 不继承 O_NONBLOCK）：一个不读数据的客户端可卡死整个
  IO 线程上的全部连接；EMFILE 时 epoll 忙循环
- **带活跃连接退出必崩**：IO 线程仍在运行时析构局部对象；IO 线程的 EventLoop 随线程退出析构，
  与 quit() 唤醒竞争。现为两阶段有序关闭
- **鉴权形同虚设**：`/api/token` 任何人可调用；HTTP 播放不鉴权；源码内置 JWT 密钥
- 空闲超时定时器随连接数无限累积且从未启用；写盘队列无上限
- Enhanced RTMP 格式与规范不符（H.265 不可用）；TsDemuxer 丢弃几乎所有 TS 包
- HLS：新源可能被立即回收；分片写盘未完成即发布；断流无 ENDLIST
- TcpConnection::state_ 跨线程数据竞争（TSan）

### P1 —— 其它已知问题

- [x] SIGHUP 热加载：只原子替换 vhost 表（顶层设置需重启），消除数据竞争；E2E 验证
- [x] 日志刷盘：WARN 以上立即刷新，其余至多 200ms；服务空闲时由后台线程刷新（原实现最后几行可能一直滞留缓冲区）
- [x] webhook：RTMP 推/拉流鉴权异步化；修复同步等待超时后访问已销毁栈对象（UAF）；worker 扩至 4 个
- [x] webhook：HTTP-FLV / WS-FLV / SRT 拉流的 on_play 异步化（HttpServer 支持延迟响应）；E2E 验证慢回调不阻塞 IO 线程
- [x] AsyncFileWriter 队列满时丢弃整个分片（不发布、打 discontinuity），录像保留已写部分；单元测试覆盖
- [x] clang `-Wunused-private-field` 告警清理（clang Release `-Werror` 通过）

### P1 —— 性能

- [x] TS 分片 sendfile 零拷贝：TcpConnection::sendFile 写满时等 EPOLLOUT 续传、与前后数据保序、断开时释放文件；内存缓存命中的分片共享发送；单元测试覆盖
- [x] 分发共享缓冲：每个包分配全局序号，HTTP-FLV / WS-FLV 每包只序列化一次、所有观众共享（跨线程投递只传引用）；跨线程发送去掉 lambda 捕获的多余拷贝。RTMP 播放因每连接分块参数不同仍逐连接打包
- [x] 基准：`tests/e2e/bench_fanout.sh`（4 路推流 × 0/40/100/200 个 HTTP-FLV 观众），每次 CI 输出 CPU/RSS/吞吐表格到作业摘要并断言观众码率

### P2 —— 扩展协议与功能（核心稳定后）

按"能否端到端验证"逐个推进；无法验证的模块宁可暂时关闭，也不对外宣称支持。

- [x] Enhanced RTMP / H.265：按规范重写解析（原实现格式与规范不符），RTMP/FLV/HLS 拉流 E2E 通过
- [x] SRT：改用 libsrt 重写（手写协议栈无法握手；TsDemuxer 有 6 处致命错误，从未运行过）
- [x] WebRTC（WHIP/WHEP）：逐层修复后与 aiortc 互通。修复链条：SDP 未按 offer 对应 m 行 →
      /whip /whep 被当作管理接口 → STUN 完整性校验多算 MI 属性头 → Binding Response 在签名后
      才写头部 → FINGERPRINT CRC 覆盖自身属性头 → DTLS 未设 MTU、BIO pair 截断 record、RSA 证书
      no shared cipher、多指纹取错算法 → SRTP 代码从未编译过（API 全错）、密钥方向/布局错误 →
      RTP/RTCP 复用判断把动态 PT 当 RTCP → NALU 未组帧、序列头未保存 → WHEP 提前订阅、
      缺 SPS/PPS → PLI 缓冲区不足从未发出
- [x] WebRTC 音频：AAC → Opus 转码（每观众独立），E2E 与 Chrome/Firefox 验证有音频轨
- [x] WebRTC：NACK 重传（5% 丢包 E2E 验证）、DELETE 结束会话（WHIP/WHEP 规范）
- [x] WebRTC 拥塞控制：解析观众 RR 丢包率与 REMB 带宽估计，拥塞时只发关键帧、解除后从关键帧恢复。
      服务器只转发主播码流、无法调节编码码率，接收端估计（REMB/RR）已足够驱动降帧，故不实现发送端
      带宽估计（TWCC）；弱网观众应使用 HLS 多清晰度
- [x] RTSP 拉流 E2E
- [x] DVR / fMP4 录制产物可播放（修复 trun 标志位、非关键帧起录、音视频基准不一致）
- [x] 转码 / ABR：修复变体流名重复追加档位后缀、进程内 libav 输出从未接通却报“完成”
      （统一走外部 ffmpeg）、路由首个匹配导致 master.m3u8 被遮蔽（改为特异性匹配）
- [x] 转码后端定为外部 ffmpeg 进程（设计决定：故障隔离、可单独 kill/限时/统计，进程开销相对编码可忽略），删除从未接通的进程内 libav 分支
- [x] 源站-边缘集群 E2E（两个实例）：修复边缘首次回源即 abort（bad_weak_ptr）、FlvDemuxer
      在二进制流中二次“跳过 HTTP 头”导致无法解析、404 时无间隔重连风暴、RTMP/HLS 不回源、
      SO_REUSEPORT 默认开启导致端口冲突静默分流、bind 失败被忽略
- [x] 边缘回源释放：无订阅者且无 HLS 请求超过 `EDGE_IDLE_SEC`（默认 30s）停止回源；E2E 验证

## 开发流程

- 本机内存很小，**不在本机编译或测试**；所有验证走 GitHub Actions
- 每次修改：推送到 `main` → CI（三矩阵单测 + E2E/ASan）→ 依据失败输出迭代
- E2E 失败时自动上传 `e2e-artifacts`（服务器日志、HLS 分片、拉流日志），用于离线分析
