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
IDLE_TIMEOUT=8
HLS_SEGMENT_DURATION=2
HLS_WINDOW_SIZE=5
HLS_OUTPUT_DIR=$WORK/hls
JWT_SECRET=e2e-secret-e2e-secret-e2e-secret
LOG_LEVEL=INFO
ENABLE_RATE_LIMIT=false
ADMIN_TOKEN=e2e-admin-token-0123456789
[vhost:__default__]
[vhost:localhost]
enable_auth=true
EOF
ADM="e2e-admin-token-0123456789"

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

echo "== 推流（H.264 + AAC，100s）"
ffmpeg -hide_banner -loglevel warning -re \
    -f lavfi -i "testsrc2=size=640x360:rate=25" \
    -f lavfi -i "sine=frequency=1000:sample_rate=44100" \
    -c:v libx264 -preset ultrafast -tune zerolatency -g 50 -pix_fmt yuv420p \
    -c:a aac -b:a 64k -t 100 \
    -f flv "rtmp://127.0.0.1:$RTMP/live/$KEY" >"$WORK/push.log" 2>&1 &
PUSH_PID=$!

# 第二路：B 帧 + 48kHz 立体声，流名带查询串（服务端应剥离，按 e2ebf 播放）
ffmpeg -hide_banner -loglevel warning -re \
    -f lavfi -i "testsrc2=size=320x240:rate=30" \
    -f lavfi -i "sine=frequency=440:sample_rate=48000" -ac 2 \
    -c:v libx264 -preset veryfast -bf 2 -g 60 -pix_fmt yuv420p \
    -c:a aac -b:a 96k -t 100 \
    -f flv "rtmp://127.0.0.1:$RTMP/live/e2ebf?foo=bar" >"$WORK/push_bf.log" 2>&1 &
PUSH2_PID=$!

sleep 8

# 慢观众（流就绪后启动）：极小接收缓冲且从不读取，迫使服务端积压，验证流控
python3 "$(dirname "$0")/slow_viewer.py" 127.0.0.1 $HTTP "/live/$KEY.flv" 40 >"$WORK/slow.out" 2>&1 &
SLOW_PID=$!

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

echo "== WebSocket-FLV"
python3 "$(dirname "$0")/ws_flv_client.py" 127.0.0.1 $HTTP "/live/$KEY.flv" 4 >"$WORK/wsflv.out" 2>&1
WS=$(cat "$WORK/wsflv.out")
vt=$(echo "$WS" | grep -oE 'video_tags=[0-9]+' | cut -d= -f2)
at=$(echo "$WS" | grep -oE 'audio_tags=[0-9]+' | cut -d= -f2)
if echo "$WS" | grep -q 'status=101 flv_header=1' && [ "${vt:-0}" -ge 80 ] && [ "${at:-0}" -ge 80 ]; then
    pass "WS-FLV 正常（$WS）"
else
    fail "WS-FLV 异常（$WS）"
fi

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
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "http://127.0.0.1:$HTTP/api/streams")
[ "$code" = 401 ] && pass "管理接口无令牌返回 401" || fail "管理接口无令牌返回 $code"
curl -sf --max-time 5 -H "Authorization: Bearer $ADM" "http://127.0.0.1:$HTTP/api/streams" >"$WORK/streams.json" 2>&1 \
    && grep -q "$KEY" "$WORK/streams.json" && pass "管理令牌可访问 /api/streams 且列出推流" \
    || fail "/api/streams 未列出推流: $(head -c 200 "$WORK/streams.json")"

echo "== 鉴权（vhost localhost 开启 enable_auth）"
mint() {  # mint <stream> <scope>
    curl -sf --max-time 5 -X POST -H "Authorization: Bearer $ADM" \
        "http://127.0.0.1:$HTTP/api/token?stream=$1&scope=$2" \
        | python3 -c 'import sys,json; print(json.load(sys.stdin)["token"])'
}
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 -X POST "http://127.0.0.1:$HTTP/api/token?stream=x")
[ "$code" = 401 ] && pass "无管理令牌不能签发 token" || fail "无管理令牌签发 token 返回 $code"
PUBTOK=$(mint secure publish); PLAYTOK=$(mint secure play); OTHERTOK=$(mint other publish)
[ -n "$PUBTOK" ] && [ -n "$PLAYTOK" ] && pass "管理员签发流 token" || fail "签发 token 失败"
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 -H "Authorization: Bearer $PLAYTOK" "http://127.0.0.1:$HTTP/api/streams")
[ "$code" = 403 ] && pass "流 token 不能访问管理接口" || fail "流 token 访问管理接口返回 $code"

try_push() {  # try_push <url> -> 0 表示推流成功持续 4s
    timeout 8 ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=160x120:rate=10" \
        -c:v libx264 -preset ultrafast -t 4 -f flv "$1" >/dev/null 2>&1
}
try_push "rtmp://localhost:$RTMP/live/secure" && fail "无 token 推流未被拒绝" || pass "无 token 推流被拒绝"
try_push "rtmp://localhost:$RTMP/live/secure?token=$OTHERTOK" && fail "其他流的 token 推流未被拒绝" || pass "其他流的 token 推流被拒绝"
try_push "rtmp://localhost:$RTMP/live/secure?token=$PLAYTOK" && fail "play token 推流未被拒绝" || pass "play token 推流被拒绝"

ffmpeg -hide_banner -loglevel warning -re \
    -f lavfi -i "testsrc2=size=320x240:rate=25" -f lavfi -i "sine=frequency=500:sample_rate=44100" \
    -c:v libx264 -preset ultrafast -g 50 -pix_fmt yuv420p -c:a aac -t 25 \
    -f flv "rtmp://localhost:$RTMP/live/secure?token=$PUBTOK" >"$WORK/push_secure.log" 2>&1 &
SECURE_PID=$!
sleep 7
kill -0 "$SECURE_PID" 2>/dev/null && pass "正确 token 推流成功" || fail "正确 token 推流失败: $(tail -2 "$WORK/push_secure.log")"
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 3 "http://localhost:$HTTP/live/secure.flv")
[ "$code" = 403 ] && pass "HTTP-FLV 无 token 返回 403" || fail "HTTP-FLV 无 token 返回 $code"
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 3 "http://localhost:$HTTP/live/secure.m3u8")
[ "$code" = 403 ] && pass "HLS 无 token 返回 403" || fail "HLS 无 token 返回 $code"
check_pull secure_flv "http://localhost:$HTTP/live/secure.flv?token=$PLAYTOK" 3 60
curl -sf --max-time 5 "http://localhost:$HTTP/live/secure.m3u8?token=$PLAYTOK" >"$WORK/secure.m3u8"
grep -v '^#' "$WORK/secure.m3u8" | grep -q "token=" && pass "m3u8 分片 URI 携带 token" || fail "m3u8 分片 URI 未携带 token"
check_pull secure_hls "http://localhost:$HTTP/live/secure.m3u8?token=$PLAYTOK" 3 60
check_pull secure_rtmp "rtmp://localhost:$RTMP/live/secure?token=$PLAYTOK" 3 60
wait "$SECURE_PID" 2>/dev/null

echo "== 健壮性：畸形输入不应使进程崩溃"
printf 'GET / HTTP/1.1\r\nContent-Length: abc\r\n\r\n' | timeout 3 nc -q1 127.0.0.1 $HTTP >/dev/null 2>&1
head -c 4096 /dev/urandom | timeout 3 nc -q1 127.0.0.1 $RTMP >/dev/null 2>&1
head -c 4096 /dev/urandom | timeout 3 nc -q1 127.0.0.1 $HTTP >/dev/null 2>&1
sleep 1
kill -0 "$SERVER_PID" 2>/dev/null && pass "畸形输入后进程存活" || fail "畸形输入导致进程退出"

echo "== 空闲超时：只连不发的连接应被断开"
python3 -c '
import socket, time, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
s.settimeout(20)
t = time.time()
try:
    d = s.recv(1)
except Exception:
    d = None
print("closed_after=%.1f" % (time.time() - t) if d == b"" else "not_closed")
' $RTMP >"$WORK/idle.out" 2>&1
grep -q closed_after "$WORK/idle.out" && pass "空闲 RTMP 连接被断开 ($(cat "$WORK/idle.out"))" \
    || fail "空闲 RTMP 连接未被断开 ($(cat "$WORK/idle.out"))"

echo "== 慢观众流控"
wait "$SLOW_PID" 2>/dev/null
if grep -qE 'slow client' "$LOG"; then
    pass "慢观众触发流控: $(grep -m1 -E 'slow client' "$LOG" | cut -c1-160)"
else
    fail "慢观众未触发流控（$(cat "$WORK/slow.out")）"
fi
RSS1=$(rss_kb)
echo "  [INFO] 推拉流期间 RSS: ${RSS1} kB（启动时 ${RSS0} kB）"

echo "== 推流结束后的清理"
for _ in $(seq 1 100); do kill -0 "$PUSH_PID" 2>/dev/null || break; sleep 1; done
wait "$PUSH2_PID" 2>/dev/null
if kill -0 "$PUSH_PID" 2>/dev/null; then
    fail "推流 ffmpeg 未按时结束（服务端可能停止读取/背压卡死）"
    kill -9 "$PUSH_PID" 2>/dev/null
fi
PUSH_PID=""
sleep 3
kill -0 "$SERVER_PID" 2>/dev/null && pass "推流结束后进程存活" || fail "推流结束后进程退出"

echo "== HLS 断流与重推"
sleep 2
curl -sf --max-time 5 "$M3U8" >"$WORK/playlist_end.m3u8"
grep -q '#EXT-X-ENDLIST' "$WORK/playlist_end.m3u8" && pass "断流后 m3u8 含 ENDLIST" \
    || fail "断流后 m3u8 缺少 ENDLIST: $(tail -3 "$WORK/playlist_end.m3u8" | tr '\n' ' ')"
ffmpeg -hide_banner -loglevel warning -re \
    -f lavfi -i "testsrc2=size=640x360:rate=25" -f lavfi -i "sine=frequency=1000:sample_rate=44100" \
    -c:v libx264 -preset ultrafast -tune zerolatency -g 50 -pix_fmt yuv420p -c:a aac -t 15 \
    -f flv "rtmp://127.0.0.1:$RTMP/live/$KEY" >"$WORK/push_re.log" 2>&1 &
PUSH_PID=$!
sleep 9
curl -sf --max-time 5 "$M3U8" >"$WORK/playlist_re.m3u8"
grep -q '#EXT-X-DISCONTINUITY' "$WORK/playlist_re.m3u8" && pass "重推后 m3u8 含 DISCONTINUITY" \
    || fail "重推后 m3u8 缺少 DISCONTINUITY"
grep -q '#EXT-X-ENDLIST' "$WORK/playlist_re.m3u8" && fail "重推后 m3u8 仍含 ENDLIST" \
    || pass "重推后 m3u8 已移除 ENDLIST"
check_pull hls_re "$M3U8" 2 40
wait "$PUSH_PID" 2>/dev/null
PUSH_PID=""

echo "== 优雅退出（SIGTERM，存在活跃推流与观众）"
ffmpeg -hide_banner -loglevel quiet -re -f lavfi -i "testsrc2=size=320x240:rate=25" \
    -f lavfi -i "sine=frequency=1000:sample_rate=44100" -c:v libx264 -preset ultrafast -g 50 \
    -pix_fmt yuv420p -c:a aac -t 30 -f flv "rtmp://127.0.0.1:$RTMP/live/$KEY" >/dev/null 2>&1 &
LIVE_PUSH=$!
sleep 4
curl -s --max-time 20 "http://127.0.0.1:$HTTP/live/$KEY.flv" -o /dev/null &
timeout 20 ffmpeg -hide_banner -loglevel quiet -i "rtmp://127.0.0.1:$RTMP/live/$KEY" -f null - >/dev/null 2>&1 &
python3 "$(dirname "$0")/ws_flv_client.py" 127.0.0.1 $HTTP "/live/$KEY.flv" 15 >/dev/null 2>&1 &
sleep 3
kill -TERM "$SERVER_PID"
for _ in $(seq 1 50); do kill -0 "$SERVER_PID" 2>/dev/null || break; sleep 0.2; done
if kill -0 "$SERVER_PID" 2>/dev/null; then
    fail "SIGTERM 后 10s 内未退出"
else
    wait "$SERVER_PID"; rc=$?
    SERVER_PID=""
    [ "$rc" -eq 0 ] && pass "SIGTERM 正常退出 (rc=0)" || fail "SIGTERM 退出码 $rc"
    kill "$LIVE_PUSH" 2>/dev/null
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
