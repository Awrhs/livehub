#!/usr/bin/env bash
# 端到端测试：真实启动 streaming_server，用 ffmpeg 推 RTMP，
# 再用 ffprobe/ffmpeg 从 RTMP / HTTP-FLV / HLS 拉流验证。
#
# 用法：e2e_core.sh <streaming_server 可执行文件路径>
# 依赖：ffmpeg、ffprobe、curl
#
# 每项检查都会执行并打印 PASS/FAIL；任一失败则最终退出码非 0。
set -u

BIN=${1:?usage: e2e_core.sh <streaming_server>}
WORK=$(mktemp -d)
LOG="$WORK/server.log"
HTTP=18080
RTMP=11935
KEY=e2etest
FAILS=0
SERVER_PID=""
PUSH_PID=""

pass() { echo "  [PASS] $*"; }
fail() { echo "  [FAIL] $*"; FAILS=$((FAILS + 1)); }

cleanup() {
    [ -n "$PUSH_PID" ] && kill "$PUSH_PID" 2>/dev/null
    [ -n "$SERVER_PID" ] && kill -9 "$SERVER_PID" 2>/dev/null
    # 把日志留给 CI 上传
    mkdir -p "${E2E_ARTIFACT_DIR:-/tmp/e2e-artifacts}"
    cp -r "$WORK"/* "${E2E_ARTIFACT_DIR:-/tmp/e2e-artifacts}/" 2>/dev/null || true
}
trap cleanup EXIT

cat > "$WORK/streaming.conf" <<EOF
HTTP_PORT=$HTTP
RTMP_PORT=$RTMP
SRT_PORT=19536
IO_THREADS=2
HLS_SEGMENT_DURATION=2
HLS_WINDOW_SIZE=5
HLS_OUTPUT_DIR=$WORK/hls
JWT_SECRET=e2e-secret-e2e-secret-e2e-secret
LOG_LEVEL=INFO
ENABLE_RATE_LIMIT=false
[vhost:__default__]
EOF

echo "== 启动服务器"
( cd "$WORK" && exec "$BIN" "$WORK/streaming.conf" ) >"$LOG" 2>&1 &
SERVER_PID=$!

for _ in $(seq 1 50); do
    curl -sf --max-time 5 "http://127.0.0.1:$HTTP/api/health" >/dev/null 2>&1 && break
    sleep 0.2
done
if curl -sf --max-time 5 "http://127.0.0.1:$HTTP/api/health" >"$WORK/health.json"; then
    pass "health 接口可用: $(head -c 200 "$WORK/health.json")"
else
    fail "服务器未能在 10s 内就绪"
    echo "  -- 进程状态: $(kill -0 $SERVER_PID 2>/dev/null && echo alive || echo dead)"
    echo "  -- 原始 HTTP 交互:"
    printf 'GET /api/health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n' \
        | timeout 5 nc -q3 127.0.0.1 $HTTP | head -20
    echo "  -- 监听端口:"; ss -ltnp 2>/dev/null | grep -E ":($HTTP|$RTMP)\b"
    echo "  -- 线程栈 (gdb):"
    timeout 30 gdb -p "$SERVER_PID" -batch -ex "thread apply all bt 8" 2>/dev/null | grep -E '^#|^Thread' | head -80
    echo "  -- 服务器日志:"; tail -80 "$LOG"
    exit 1
fi

rss_kb() { awk '/VmRSS/{print $2}' "/proc/$SERVER_PID/status" 2>/dev/null; }
RSS0=$(rss_kb)
echo "  [INFO] 启动后 RSS: ${RSS0} kB"

echo "== 推流（H.264 + AAC，60s）"
ffmpeg -hide_banner -loglevel warning -re \
    -f lavfi -i "testsrc2=size=640x360:rate=25" \
    -f lavfi -i "sine=frequency=1000:sample_rate=44100" \
    -c:v libx264 -preset ultrafast -tune zerolatency -g 50 -pix_fmt yuv420p \
    -c:a aac -b:a 64k -t 60 \
    -f flv "rtmp://127.0.0.1:$RTMP/live/$KEY" >"$WORK/push.log" 2>&1 &
PUSH_PID=$!

# 第二路：B 帧 + 48kHz 立体声，流名带查询串（服务端应剥离，按 e2ebf 播放）
ffmpeg -hide_banner -loglevel warning -re \
    -f lavfi -i "testsrc2=size=320x240:rate=30" \
    -f lavfi -i "sine=frequency=440:sample_rate=48000" -ac 2 \
    -c:v libx264 -preset veryfast -bf 2 -g 60 -pix_fmt yuv420p \
    -c:a aac -b:a 96k -t 60 \
    -f flv "rtmp://127.0.0.1:$RTMP/live/e2ebf?foo=bar" >"$WORK/push_bf.log" 2>&1 &
PUSH2_PID=$!

# 慢观众：限速 20KB/s 拉 HTTP-FLV（远低于码率），观察流控
curl -s --limit-rate 20k --max-time 40 "http://127.0.0.1:$HTTP/live/$KEY.flv" -o /dev/null &
SLOW_PID=$!
sleep 8

if kill -0 "$PUSH_PID" 2>/dev/null; then
    pass "推流进程运行中"
else
    fail "推流进程提前退出: $(tail -5 "$WORK/push.log")"
fi

# 拉流并解码 N 秒，校验视频帧数与音视频流都存在
check_pull() {
    local name=$1 url=$2 secs=$3 minframes=$4
    local out="$WORK/pull_$name.log"
    if ! timeout 30 ffprobe -v error -show_entries stream=codec_name,codec_type \
            -of csv=p=0 "$url" >"$out.probe" 2>&1; then
        fail "$name: ffprobe 失败: $(tail -3 "$out.probe")"
        return
    fi
    grep -q '^h264,video' "$out.probe" && pass "$name: 探测到 H.264 视频" \
        || fail "$name: 未探测到 H.264 视频 ($(tr "\n" " " <"$out.probe" | tail -c 300))"
    grep -q '^aac,audio' "$out.probe" && pass "$name: 探测到 AAC 音频" \
        || fail "$name: 未探测到 AAC 音频 ($(tr '\n' ' ' <"$out.probe"))"

    timeout 40 ffmpeg -hide_banner -nostats -i "$url" -t "$secs" \
        -map 0:v:0 -f null - >"$out" 2>&1
    local frames
    frames=$(grep -oE 'frame= *[0-9]+' "$out" | tail -1 | grep -oE '[0-9]+')
    frames=${frames:-0}
    if [ "$frames" -ge "$minframes" ]; then
        pass "$name: 解码 ${secs}s 得到 $frames 帧 (>= $minframes)"
    else
        fail "$name: 解码帧数 $frames < $minframes: $(grep -iE 'error|invalid' "$out" | head -3)"
    fi
    local errs
    errs=$(grep -ciE 'error while decoding|non-existing PPS|corrupt|invalid nal' "$out")
    [ "$errs" -eq 0 ] && pass "$name: 无解码错误" || fail "$name: 出现 $errs 条解码错误"
}

echo "== 重复推流应被拒绝"
timeout 15 ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=160x120:rate=10" \
    -c:v libx264 -preset ultrafast -t 10 -f flv "rtmp://127.0.0.1:$RTMP/live/$KEY" \
    >"$WORK/push_dup.log" 2>&1
rc=$?
if [ "$rc" -ne 0 ] && [ "$rc" -ne 124 ]; then
    pass "同名重复推流被拒绝 (ffmpeg rc=$rc)"
else
    fail "同名重复推流未被拒绝 (rc=$rc): $(tail -3 "$WORK/push_dup.log")"
fi
kill -0 "$PUSH_PID" 2>/dev/null && pass "原推流不受影响" || fail "原推流被中断"

echo "== RTMP 拉流"
check_pull rtmp "rtmp://127.0.0.1:$RTMP/live/$KEY" 5 100

echo "== HTTP-FLV 拉流"
check_pull httpflv "http://127.0.0.1:$HTTP/live/$KEY.flv" 5 100

echo "== HLS"
M3U8="http://127.0.0.1:$HTTP/live/$KEY.m3u8"
if curl -sf --max-time 5 "$M3U8" >"$WORK/playlist.m3u8"; then
    pass "m3u8 可访问"
    grep -q '#EXTM3U' "$WORK/playlist.m3u8" && pass "m3u8 头正确" || fail "m3u8 缺少 #EXTM3U"
    nseg=$(grep -c '^#EXTINF' "$WORK/playlist.m3u8")
    [ "$nseg" -ge 2 ] && pass "m3u8 含 $nseg 个分片" || fail "m3u8 分片数 $nseg < 2"
    seg=$(grep -v '^#' "$WORK/playlist.m3u8" | grep -v '^$' | tail -1)
    if [ -n "$seg" ]; then
        case "$seg" in http*) segurl=$seg ;; /*) segurl="http://127.0.0.1:$HTTP$seg" ;;
            *) segurl="http://127.0.0.1:$HTTP/live/$seg" ;; esac
        if curl -sf --max-time 5 "$segurl" -o "$WORK/seg.ts"; then
            sz=$(stat -c %s "$WORK/seg.ts")
            first=$(head -c1 "$WORK/seg.ts" | od -An -tx1 | tr -d ' ')
            [ "$first" = "47" ] && [ $((sz % 188)) -eq 0 ] \
                && pass "TS 分片格式正确 ($sz 字节)" \
                || fail "TS 分片格式错误 (size=$sz first=0x$first)"
        else
            fail "TS 分片下载失败: $segurl"
        fi
    fi
else
    fail "m3u8 不可访问"
fi
check_pull hls "$M3U8" 4 80

echo "== B 帧 / 48kHz 立体声 / 带查询串的流名"
check_pull bf_flv "http://127.0.0.1:$HTTP/live/e2ebf.flv" 4 100
check_pull bf_hls "http://127.0.0.1:$HTTP/live/e2ebf.m3u8" 4 100

echo "== 多观众并发（6 路 HTTP-FLV + 2 路 RTMP）"
pids=""
for i in 1 2 3 4 5 6; do
    timeout 40 ffmpeg -hide_banner -nostats -i "http://127.0.0.1:$HTTP/live/$KEY.flv" \
        -t 5 -map 0:v:0 -f null - >"$WORK/multi_flv_$i.log" 2>&1 &
    pids="$pids $!"
done
for i in 1 2; do
    timeout 40 ffmpeg -hide_banner -nostats -i "rtmp://127.0.0.1:$RTMP/live/$KEY" \
        -t 5 -map 0:v:0 -f null - >"$WORK/multi_rtmp_$i.log" 2>&1 &
    pids="$pids $!"
done
for p in $pids; do wait "$p"; done
ok=0; total=0
for f in "$WORK"/multi_*.log; do
    total=$((total + 1))
    fr=$(grep -oE 'frame= *[0-9]+' "$f" | tail -1 | grep -oE '[0-9]+')
    [ "${fr:-0}" -ge 100 ] && ok=$((ok + 1))
done
[ "$ok" -eq "$total" ] && pass "并发观众全部正常解码 ($ok/$total)" \
    || fail "并发观众解码不足 ($ok/$total)"

echo "== API"
curl -sf --max-time 5 "http://127.0.0.1:$HTTP/metrics" >"$WORK/metrics.txt" \
    && pass "/metrics 可访问 ($(wc -l <"$WORK/metrics.txt") 行)" || fail "/metrics 不可访问"
curl -sf --max-time 5 "http://127.0.0.1:$HTTP/api/streams" >"$WORK/streams.json" 2>&1 \
    && grep -q "$KEY" "$WORK/streams.json" && pass "/api/streams 列出了推流" \
    || echo "  [INFO] /api/streams 未列出推流（可能需鉴权）: $(head -c 200 "$WORK/streams.json")"

echo "== 健壮性：畸形输入不应使进程崩溃"
printf 'GET / HTTP/1.1\r\nContent-Length: abc\r\n\r\n' | timeout 3 nc -q1 127.0.0.1 $HTTP >/dev/null 2>&1
head -c 4096 /dev/urandom | timeout 3 nc -q1 127.0.0.1 $RTMP >/dev/null 2>&1
head -c 4096 /dev/urandom | timeout 3 nc -q1 127.0.0.1 $HTTP >/dev/null 2>&1
sleep 1
kill -0 "$SERVER_PID" 2>/dev/null && pass "畸形输入后进程存活" || fail "畸形输入导致进程退出"

echo "== 慢观众流控"
kill "$SLOW_PID" 2>/dev/null; wait "$SLOW_PID" 2>/dev/null
if grep -qE 'slow client' "$LOG"; then
    pass "慢观众触发流控: $(grep -m1 -E 'slow client' "$LOG" | cut -c1-160)"
else
    echo "  [INFO] 慢观众未触发流控（积压可能被内核 socket 缓冲吸收）"
fi
RSS1=$(rss_kb)
echo "  [INFO] 推拉流期间 RSS: ${RSS1} kB（启动时 ${RSS0} kB）"

echo "== 推流结束后的清理"
for _ in $(seq 1 60); do kill -0 "$PUSH_PID" 2>/dev/null || break; sleep 1; done
wait "$PUSH2_PID" 2>/dev/null
if kill -0 "$PUSH_PID" 2>/dev/null; then
    fail "推流 ffmpeg 未按时结束（服务端可能停止读取/背压卡死）"
    kill -9 "$PUSH_PID" 2>/dev/null
fi
PUSH_PID=""
sleep 3
kill -0 "$SERVER_PID" 2>/dev/null && pass "推流结束后进程存活" || fail "推流结束后进程退出"

echo "== 优雅退出（SIGTERM）"
kill -TERM "$SERVER_PID"
for _ in $(seq 1 50); do kill -0 "$SERVER_PID" 2>/dev/null || break; sleep 0.2; done
if kill -0 "$SERVER_PID" 2>/dev/null; then
    fail "SIGTERM 后 10s 内未退出"
else
    wait "$SERVER_PID"; rc=$?
    SERVER_PID=""
    [ "$rc" -eq 0 ] && pass "SIGTERM 正常退出 (rc=0)" || fail "SIGTERM 退出码 $rc"
fi

if grep -qE 'ERROR: AddressSanitizer|runtime error:|LeakSanitizer|WARNING: ThreadSanitizer' "$LOG"; then
    fail "Sanitizer 报告了问题:"
    grep -c -E 'WARNING: ThreadSanitizer' "$LOG" | sed 's/^/  TSan 报告数: /'
    grep -A20 -E 'ERROR: AddressSanitizer|runtime error:|LeakSanitizer|WARNING: ThreadSanitizer' "$LOG" | head -80
else
    pass "无 Sanitizer 报告"
fi

echo
echo "== 服务器日志（末尾 40 行）"
tail -60 "$LOG"
echo
if [ "$FAILS" -eq 0 ]; then
    echo "E2E 全部通过"
else
    echo "E2E 失败项: $FAILS"
fi
exit "$FAILS"
