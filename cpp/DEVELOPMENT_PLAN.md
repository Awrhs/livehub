# C++ 版收尾开发计划（控制面补齐）

> 2026-09-15 制定，**2026-09-16 全部完成**（阶段 1–5 及收尾）。
> 目标：补齐旧 Python 版有而 C++ 版尚未实现的 9 项控制面功能，
> 使 C++ 版成为唯一实现并达到功能超集。数据面（协议栈）无任何缺口，不在本计划内。
> 工作量合计约 **1200–1400 行**（含测试），纯编码约 5–6 个工作日。
>
> **交付清单**：
> - 阶段 1 ✅ `metrics/SystemStats`（/proc 解析纯函数化）+ 丰富 `/api/health`
>   （CPU/内存/负载/连接数/时间戳 + `system_cpu/mem_permille` gauge）、
>   `GET /api/profiles`（按 vhost 档位）、forward URL 严格 rtmp/rtmps 白名单
>   （`parseRtmpUrl` + handler 双层）、`parseProfileSpec` 统一三处档位解析。
> - 阶段 2 ✅ thread-local trace id（`logging::setTraceId`）+ `X-Request-ID`
>   透传/`X-Trace-Id` 回写 + `LOG_JSON` JSON 行日志 + AccessLog 双格式
>   （`formatAccessLogJson` 可单测），请求结束自动清除 trace。
> - 阶段 3 ✅ `TranscodeManager` 优先级队列（priority desc + 提交序 FIFO）、
>   五态生命周期（pending/running/completed/failed/cancelled）、
>   `stats()`/`sweepFinished(TTL)`/终态归档（上限 1000）、
>   `GET /api/transcode`、POST `priority=`、`transcode_tasks_*` 指标。
> - 阶段 4 ✅ `RatePolicy` 最长前缀匹配 + 5 组默认策略（playback 300 /
>   api 100 / auth 10 / transcode 5 / admin 20 每分钟，配置可覆盖，0=关闭）。
> - 阶段 5 ✅ `RedisClient`（手写 RESP2、专用 worker 线程、超时+熔断+fail-open）
>   接入三方：分布式限流（两层判定）、JWT 黑名单跨节点共享（回灌本地）、
>   StreamCache L2（异步写透 + miss 回填）；`AuthFilter` 接入黑名单使
>   logout 真正生效；SIGHUP 热更新 Redis 端点。
> - 顺手修复：`AbrManager` 的 `LOG_INFO <<` 流式写法（printf 宏下必然编译失败）。
> - 新增测试：`test_system_stats` / `test_transcode_tasks` / `test_redis_protocol`，
>   扩展 `test_config` / `test_rate_limiter` / `test_json`。

## 总原则

- **只写代码，不编译不运行**（沿用既有约定）。编译/ASan/实测由 CI
  （`.github/workflows/cpp.yml` 三矩阵）在首次 push 后统一验证。
- 遵循现有约定：namespace `streaming`；头文件 `cpp/include/<module>/`，
  源文件 `cpp/src/<module>/`（CMake GLOB_RECURSE 自动收录，无需改构建脚本，
  仅测试需注册进 `cpp/tests/CMakeLists.txt`）；`LOG_*` 为 printf 风格；
  `JsonValue` 数组用 `.append()`。
- **失败降级统一 fail-open**：外部依赖（Redis）不可用时退回单机内存实现，
  不阻断请求主路径（与旧版 Python 语义一致）。
- 每阶段独立可交付，**一个阶段一个 commit**；新增配置键同步更新
  `cpp/streaming.conf.example` 与 `cpp/README.md`。

## 阶段总览

| 阶段 | 内容 | 对应缺口项 | 预估规模 | 优先级 |
|------|------|-----------|---------|--------|
| 1 | HTTP 层快速补齐：健康检查 / profiles 端点 / 转推白名单 | #4 #7 #9 | ~170 行 | 高 |
| 2 | Trace ID + JSON 结构化日志 | #3 | ~200 行 | 高 |
| 3 | 转码任务优先级队列 + 生命周期统计 | #5 | ~250 行 | 中 |
| 4 | 分路由限流策略 | #2 | ~150 行 | 中 |
| 5 | Redis 客户端 + 分布式共享状态 | #1 | ~450 行 | 中 |
| — | 明确不做：服务端带宽推荐、多播 | #6 #8 | 0 | 见文末 |

依赖关系：阶段 5 依赖阶段 4（先有策略层，再换分布式后端）；其余阶段相互独立，
顺序可调。建议按编号推进，1/4 两项半天内可见效。

---

## 阶段 1：HTTP 层快速补齐（#4 #7 #9）

### 1a. 系统指标 + /api/health 丰富化（#4）

现状：`/api/health` 仅返回 `status+service`；`TranscodeManager::cpuUsage()`
已有读 `/proc/stat` 的实现但未暴露。

- 新建 `cpp/include/metrics/SystemStats.hpp` + `cpp/src/metrics/SystemStats.cpp`：
  ```cpp
  namespace streaming {
  // /proc/stat 两次采样差值求 CPU 占比（把 TranscodeManager::cpuUsage 的
  // 实现抽到这里，TranscodeManager 改为调用它，避免两份代码）
  double systemCpuUsage();
  double systemMemUsage();   // /proc/meminfo: 1 - MemAvailable/MemTotal
  double systemLoadAvg1();   // /proc/loadavg 第一列
  int64_t systemUptimeSec(); // /proc/uptime
  }
  ```
- 修改 `cpp/src/main.cpp` `/api/health`：返回
  `status / cpu_usage / memory_usage / loadavg1 / active_connections
  （rtmp_streams_active + srt_sessions 两个现有 gauge 之和）/ uptime / timestamp`。
- 同时把 CPU/内存注册成 Prometheus gauge（`system_cpu_ratio`、
  `system_mem_ratio`），在 60s 定时器（现有 DiskWatermark 定时器旁）里刷新。

### 1b. GET /api/profiles 转码档位列表（#7）

- 修改 `cpp/src/main.cpp`：新增 `GET /api/profiles`，从当前 vhost
  （复用 vhost 中间件注入的 `req.attribute("vhost")`，缺省 `__default__`）
  读取转码档位，返回
  `[{name, width, height, video_bitrate, audio_bitrate}]`。
- 档位数据源：`VhostManager` 中已有的 transcode_profiles 配置
  （`applyVhostKv` 已支持该键）。若 Vhost 结构未保存完整档位字段，
  在 `config/Vhost` 中补齐解析（profiles 串格式沿用现有
  `480p,720p` + 全局档位表）。

### 1c. /api/forward URL 协议白名单（#9）

- 修改 `cpp/src/cluster/StreamForwarder.cpp` `parseRtmpUrl()`：
  前缀既非 `rtmp://` 也非 `rtmps://` 时直接 `return false`
  （当前会静默 fallthrough 继续解析）。
- 修改 `cpp/src/main.cpp` `/api/forward/:stream` handler：解析失败返回
  `400 {"error":"unsupported url scheme, expect rtmp:// or rtmps://"}`，
  不再走到 addTarget。
- 说明：C++ 侧 fork/exec 走 argv 不经 shell，命令注入在结构上已免疫，
  本项只补 scheme 严格校验（防误配 + SSRF 面）。

**测试**：`test_config.cpp` 补 vhost profiles 解析用例；新增
`test_system_stats.cpp`（用注入的字符串解析 /proc 格式，不依赖真机读数）。

---

## 阶段 2：Trace ID + JSON 结构化日志（#3）

### 设计要点

- **线程本地 trace id**（每个请求在其 Reactor 线程内同步处理完毕，
  thread_local 足以贯穿，无需改 LOG 宏签名）：
  ```cpp
  // core/Logger.hpp 增补
  namespace streaming::logging {
  void setTraceId(std::string id);   // 请求开始时设置，结束清空
  const std::string& traceId();      // LOG_* 输出时追加 [trace=xxxx]（非空时）
  }
  ```
- **JSON 日志模式**：`core/Logger` 增加 `setJsonMode(bool)`
  （配置 `STREAM_LOG_JSON=1`），输出
  `{"ts":"...","level":"INFO","module":"...","trace_id":"...","msg":"..."}`
  （msg 走 JSON 转义，复用 `api/Json` 的字符串转义逻辑）。
- **请求侧接线**（`cpp/src/main.cpp`）：
  - 新增 trace-id 中间件并置于链路**最前**（在限流之前）：
    优先透传客户端 `X-Request-ID`，否则生成 16 位 hex；写入
    `req.setAttribute("trace_id", ...)` 并在响应头回写 `X-Trace-Id`
    （便于客户端排障关联）。
  - `middleware/AccessLog` 改为 JSON 行：
    `{"ts","trace_id","method","path","status","elapsed_ms","ip","bytes"}`
    （耗时与 bytes 字段数据均已具备；ip 取 `TcpConnection::peerAddr()`）。
- 中间件最终顺序：**trace-id → 限流 → vhost → 鉴权 → handler**。

**测试**：`test_json.cpp` 补日志转义用例；AccessLog 格式化函数抽成可单测的
纯函数并补用例。

---

## 阶段 3：转码任务优先级队列 + 生命周期统计（#5）

### 设计要点

对齐旧版 `task_manager.py` 的能力（优先级队列 / 状态机 / 取消 / TTL 清理 /
统计），在 `TranscodeManager` 上扩展而非另建模块：

```cpp
// include/transcode/TranscodeManager.hpp
enum class TaskState { kPending, kRunning, kCompleted, kFailed, kCancelled };

struct TranscodeTaskInfo {
    std::string id, stream, input, output;
    int priority = 0;              // 大者先调度；同优先级按提交序号 FIFO
    TaskState state = TaskState::kPending;
    int64_t submittedAt = 0, startedAt = 0, endedAt = 0;
    std::string error;
};

// TranscodeManager 增加：
//   submit(..., int priority)            —— queue_ 由 vector 改为
//     (priority desc, seq asc) 稳定排序插入（multimap 或手写插入排序）
//   std::vector<TranscodeTaskInfo> listTasks() const;
//   JsonValue taskStats();               // 各状态计数 + 平均排队时长
//   void sweepFinished(int olderThanSec);// 终态任务 TTL 清理
//   cancel() 扩展：kPending 移出队列；kRunning 走 ProcessManager 停进程
```

- 调度点：`maxConcurrent_` 空位释放时从队头取最高优先级任务（现有逻辑
  只改取队头的来源）。
- TTL 清理与统计输出挂到现有 30s `tickSchedules` 定时器
  （DvrManager 同款模式），清理周期 `STREAM_TRANSCODE_TASK_TTL_SEC`
  （默认 3600）。
- 指标：`transcode_tasks_pending / running / failed` 三个 gauge 随状态迁移刷新。
- API：`/api/transcode` 增加 `GET`（返回任务列表 + stats），
  `POST` body 可选 `priority=N`。

**测试**：新增 `test_transcode_tasks.cpp`（纯逻辑：优先级出队顺序、
同优先级 FIFO、取消状态迁移、TTL 清理边界）。

---

## 阶段 4：分路由限流策略（#2）

### 设计要点

对齐旧版 5 条策略，落在现有 `middleware/RateLimiter` 上（本地令牌桶，
阶段 5 再换分布式后端，接口不变）：

```cpp
// include/middleware/RateLimiter.hpp 增加：
//   路由前缀 → (每秒速率, 突发) 的有序表（最长前缀匹配）
void addPolicy(const std::string& pathPrefix, double rate, double burst);
// RateLimitMiddleware::operator() 内：
//   policy = 最长匹配 req.path()；未命中 → 全局默认
//   key = policyName + ":" + clientIp（ip 取 peerAddr）
```

默认策略表（键名沿用旧版语义，全部可用配置覆盖）：

| 策略 | 前缀 | 默认限速 |
|------|------|---------|
| playback | `/live/` | 300/min（=5/s, burst 10） |
| api | `/api/` | 100/min |
| auth | `/api/token`、`/api/logout` | 10/min |
| transcode | `/api/transcode`、`/api/record`、`/api/forward`、`/api/abr` | 5/min |
| admin | `/api/nodes`、`/api/cluster`、`/api/vhosts`、`/api/dvr`、`/api/streams` | 20/min |

- 匹配优先级：`/api/token` 比 `/api/` 更长，命中 auth 策略（最长前缀）。
- 配置键：`STREAM_RATE_PLAYBACK_PER_MIN` 等 5 个，0 表示关闭该策略。
- 中间件位置不变（trace-id 之后、vhost 之前）；被限流响应为
  `429 + Retry-After`（现有行为保持）。

**测试**：`test_rate_limiter.cpp` 补最长前缀匹配、策略 key 隔离
（不同前缀互不挤占配额）用例。

---

## 阶段 5：Redis 客户端 + 分布式共享状态（#1）

> 本计划最大单项。对齐旧版 `redis_client.py`：分布式限流 / JWT 黑名单
> 跨节点共享 / KV 缓存，Redis 不可用自动降级内存。

### 5a. RESP2 协议客户端（同步、独立线程）

设计决策：限流与黑名单的判定在请求主路径上需要**同步**答案，而 Reactor
线程不可阻塞——因此客户端跑在**专用后台线程 + 互斥等待**上，
带 50ms 超时与熔断，超时/熔断期间全部 fail-open 走本地实现。

```cpp
// include/cache/RedisClient.hpp
struct RedisReply {                       // RESP2 五类回复
    enum Type { kSimple, kError, kInt, kBulk, kArray, kNil } type;
    std::string str; int64_t integer = 0;
    std::vector<RedisReply> arr;
};
class RedisClient : NonCopyable {
public:
    static RedisClient& instance();       // 配置驱动单例
    bool enabled() const;                 // 未配置或熔断中 → false
    // —— 三个高级接口（覆盖本计划全部用途，不做通用命令层）——
    bool incrWithTtl(const std::string& key, int windowSec, int64_t* count); // 固定窗口计数（限流）
    bool setWithTtl(const std::string& key, const std::string& val, int ttlSec); // 黑名单写入
    bool get(const std::string& key, std::string* val);                    // 黑名单/缓存读取
};
```

- 实现要点：POSIX socket（不占用 Reactor 的 `net/TcpClient`，后者是异步的）；
  RESP 编码（`*N\r\n$len\r\n...` 数组命令）与解码手写；单连接复用 +
  断线重连；连续 5 次失败熔断 10s（复用/抽出 `cluster/HealthChecker` 中
  的 CircuitBreaker）；PING 保活。
- 单元可测性：编码/解码抽成 `encodeCommand(args) / parseReply(bytes)`
  两个纯函数，不依赖网络。

### 5b. 三个使用方接入（全部 fail-open）

1. **分布式限流**：`RateLimitMiddleware` 判定顺序改为
   `RedisClient 可用 → incrWithTtl("rl:{policy}:{ip}", 60) 超额即拒 →
   不可用 → 现有本地 sharded token bucket`。注意保留本地桶作为
   Redis 抖动时的兜底（两层都过才算过）。
2. **JWT 黑名单共享**：`auth/TokenBlacklist` 写入时同步写 Redis
   （key `bl:{jti}`，TTL = token 剩余有效期）；校验时 Redis 命中即拒，
   未命中再查本地（本地仍保留，作为降级路径）。`/api/logout` 无需改动。
3. **StreamCache L2（可选，最后做）**：TS 分片异步回填 Redis
   （best-effort，不阻塞响应），命中本地未命中 Redis 时回填本地。
   若时间紧可裁掉此项，不影响前两个使用方。

### 5c. 配置

`STREAM_REDIS_HOST`（空 = 不启用，纯单机模式，默认空）、
`STREAM_REDIS_PORT`（6379）、`STREAM_REDIS_TIMEOUT_MS`（50），
全部进 `streaming.conf.example` + README。

**测试**：新增 `test_redis_protocol.cpp`（encodeCommand/parseReply 往返、
RESP 五类回复、畸形输入容错）。

---

## 明确不做（记录决策，避免反复讨论）

| 项 | 理由 |
|----|------|
| #6 `/api/recommend-profile` 服务端带宽推荐 | 已被 HLS master playlist（`#EXT-X-STREAM-INF`）客户端侧 ABR **方案性取代**：由播放器根据实测带宽自动切换档位，比旧版"客户端上报带宽→服务端推荐"的同步接口更标准、少一次往返。 |
| #8 多播（multicast 注册表 / 阈值切换） | 旧版 Python 实现中仅有注册表与阈值判断，**从未有真实的多播发送路径**，属死代码。如未来确需 UDP 多播分发，应作为新特性单独立项（TS over UDP 组播出方向）。 |

## 工作量汇总

| 阶段 | 纯编码 | 测试 | 合计 |
|------|--------|------|------|
| 1 HTTP 快速补齐 | ~120 行 | ~80 行 | ~200 行 |
| 2 Trace ID + JSON 日志 | ~150 行 | ~50 行 | ~200 行 |
| 3 转码任务管理 | ~200 行 | ~60 行 | ~260 行 |
| 4 分路由限流 | ~100 行 | ~50 行 | ~150 行 |
| 5 Redis 分布式 | ~350 行 | ~100 行 | ~450 行 |
| **合计** | **~920 行** | **~340 行** | **~1260 行** |

## 完成后状态

9 项缺口清零：C++ 版对旧版 Python 形成**严格功能超集**
（Python 全部能力均有对应实现，另有协议栈/DVR/集群/vhost 等大量超出项）。
届时剩余工作只有非编码项：首次提交并触发 CI 三矩阵编译验证、
ffmpeg/ffplay/OBS 协议级实测、压测数据。
