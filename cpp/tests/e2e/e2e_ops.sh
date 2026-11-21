#!/usr/bin/env bash
# 运维相关功能端到端测试：webhook（通知 + 同步鉴权）、限流、单 IP 连接上限
# 用法：e2e_ops.sh <streaming_server>
set -u

BIN=${1:?usage: e2e_ops.sh <streaming_server>}
WORK=$(mktemp -d)
LOG="$WORK/server.log"
HTTP=48080
RTMP=41935
HOOK=39090
FAILS=0
pass() { echo "  [PASS] $*"; }
fail() { echo "  [FAIL] $*"; FAILS=$((FAILS + 1)); }

# ---- webhook 接收端：记录请求体；流名含 denyme 时返回 code=1 拒绝 ----
cat >"$WORK/hook.py" <<'PY'
import http.server, sys, json
log = open(sys.argv[2], "a", buffering=1)
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(n).decode(errors="replace")
        log.write(self.path + " " + body.replace("\n", " ") + "\n")
        if "slowme" in body:
            import time; time.sleep(4)   # 模拟慢 webhook
        code = 1 if "denyme" in body else 0
        out = json.dumps({"code": code, "msg": "denied" if code else "ok"}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)
    def log_message(self, *a):
        pass
http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
PY
python3 "$WORK/hook.py" $HOOK "$WORK/hooks.log" &
HOOK_PID=$!

cat >"$WORK/streaming.conf" <<EOF
HTTP_PORT=$HTTP
RTMP_PORT=$RTMP
SRT_PORT=49536
IO_THREADS=1
HLS_OUTPUT_DIR=$WORK/hls
JWT_SECRET=ops-secret-ops-secret-ops-secret
ENABLE_RATE_LIMIT=true
RATE_LIMIT_PER_SEC=10000
RATE_LIMIT_BURST=20000
RATE_API_PER_MIN=20
RATE_PLAYBACK_PER_MIN=100000
MAX_CONNECTIONS_PER_IP=30
[vhost:__default__]
webhook_on_publish=http://127.0.0.1:$HOOK/hook/publish
webhook_on_unpublish=http://127.0.0.1:$HOOK/hook/unpublish
webhook_on_play=http://127.0.0.1:$HOOK/hook/play
EOF

( cd "$WORK" && exec "$BIN" "$WORK/streaming.conf" ) >"$LOG" 2>&1 &
SERVER_PID=$!
cleanup() {
    pkill -P $$ 2>/dev/null
    kill -9 "$SERVER_PID" "$HOOK_PID" 2>/dev/null
    mkdir -p "${E2E_ARTIFACT_DIR:-/tmp/ops-artifacts}"
    cp "$LOG" "$WORK/hooks.log" "${E2E_ARTIFACT_DIR:-/tmp/ops-artifacts}/" 2>/dev/null || true
}
trap cleanup EXIT
for _ in $(seq 1 50); do curl -sf --max-time 2 "http://127.0.0.1:$HTTP/api/health" >/dev/null && break; sleep 0.2; done
sleep 1

push() {  # push <key> <seconds>
    timeout $(($2 + 10)) ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=160x120:rate=15" \
        -c:v libx264 -preset ultrafast -g 30 -t "$2" -f flv "rtmp://127.0.0.1:$RTMP/live/$1" >/dev/null 2>&1
}

echo "== webhook：通知与同步鉴权"
push ok1 12 &
PUSH=$!
sleep 5
grep -q '/hook/publish.*ok1' "$WORK/hooks.log" && pass "on_publish 回调已触发" \
    || fail "on_publish 未触发: $(head -3 "$WORK/hooks.log" 2>/dev/null)"
timeout 10 ffmpeg -hide_banner -loglevel error -i "rtmp://127.0.0.1:$RTMP/live/ok1" -t 2 -f null - >/dev/null 2>&1
grep -q '/hook/play.*ok1' "$WORK/hooks.log" && pass "on_play 回调已触发" || fail "on_play 未触发"
wait "$PUSH"
sleep 3
grep -q '/hook/unpublish.*ok1' "$WORK/hooks.log" && pass "on_unpublish 回调已触发" || fail "on_unpublish 未触发"
push denyme 4 && fail "webhook 返回 code=1 时推流未被拒绝" || pass "webhook 返回 code=1 时推流被拒绝"
grep -E 'denied|denyme' "$LOG" | head -2 | sed 's/^/    /'

echo "== 慢 webhook 不阻塞 IO 线程（单 IO 线程，回调延迟 4s）"
push ok2 30 &
OK2=$!
sleep 4
first_frame_ms() {  # 拉流到首个解码帧的耗时（关闭探测等待）
    local t0; t0=$(date +%s%N)
    timeout 15 ffmpeg -hide_banner -loglevel error -analyzeduration 0 -probesize 32768 \
        -i "rtmp://127.0.0.1:$RTMP/live/ok2" -frames:v 1 -f null - >/dev/null 2>&1 || { echo 99999; return; }
    echo $(( ($(date +%s%N) - t0) / 1000000 ))
}
base=$(first_frame_ms)
push slowme 6 &
SLOW=$!
sleep 0.5
during=$(first_frame_ms)
[ "$during" -lt $((base + 1500)) ] \
    && pass "慢 webhook 等待期间同线程其它连接正常（首帧 ${during}ms，基线 ${base}ms）" \
    || fail "慢 webhook 阻塞了 IO 线程（首帧 ${during}ms，基线 ${base}ms）"
wait "$SLOW" && pass "慢 webhook 放行后推流成功" || fail "慢 webhook 放行后推流失败"
kill "$OK2" 2>/dev/null; wait "$OK2" 2>/dev/null

echo "== 限流（/api/ 每分钟 20 次）"
codes=""
for i in $(seq 1 40); do
    codes="$codes $(curl -s -o /dev/null -w '%{http_code}' --max-time 3 "http://127.0.0.1:$HTTP/api/health")"
done
n429=$(echo "$codes" | tr ' ' '\n' | grep -c '^429$')
n200=$(echo "$codes" | tr ' ' '\n' | grep -c '^200$')
[ "$n429" -ge 10 ] && [ "$n200" -ge 5 ] && pass "限流生效（200×$n200，429×$n429）" \
    || fail "限流未按预期生效（200×$n200，429×$n429）"
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 3 "http://127.0.0.1:$HTTP/live/none.m3u8")
[ "$code" != 429 ] && pass "播放路径不受 API 限流影响（$code）" || fail "播放路径被 API 限流误伤"

echo "== 单 IP 连接上限（30）"
python3 - $RTMP >"$WORK/connlimit.out" <<'PY'
import socket, sys, time
port = int(sys.argv[1])
socks = []
for _ in range(50):
    s = socket.socket(); s.settimeout(2); s.connect(("127.0.0.1", port)); socks.append(s)
time.sleep(1)
closed = 0
for s in socks:
    s.setblocking(False)
    try:
        if s.recv(1) == b"":
            closed += 1
    except BlockingIOError:
        pass
    except OSError:
        closed += 1
print(closed)
PY
closed=$(cat "$WORK/connlimit.out")
[ "${closed:-0}" -ge 15 ] && pass "超出上限的连接被拒绝（50 个中 $closed 个被关闭）" \
    || fail "单 IP 连接上限未生效（50 个中 ${closed:-0} 个被关闭）"
sleep 2
ok=$(python3 -c '
import socket, sys, time
s = socket.create_connection(("127.0.0.1", int(sys.argv[1]))); s.setblocking(False); time.sleep(1)
try:
    print("closed" if s.recv(1) == b"" else "open")
except BlockingIOError:
    print("open")
' $RTMP)
[ "$ok" = open ] && pass "连接释放后计数回收，可正常建立新连接" || fail "连接释放后计数未回收（$ok）"

echo "== SIGHUP 热加载 vhost"
sleep 61  # 等待 API 限流窗口重置（每分钟 20 次）
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 3 -H "Host: reloaded.local" "http://127.0.0.1:$HTTP/live/x.m3u8")
[ "$code" = 404 ] && pass "热加载前 reloaded.local 走默认 vhost（404）" || fail "热加载前返回 $code"
cat >>"$WORK/streaming.conf" <<EOF2
[vhost:reloaded.local]
enable_auth=true
EOF2
kill -HUP "$SERVER_PID"
sleep 1
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 3 -H "Host: reloaded.local" "http://127.0.0.1:$HTTP/live/x.m3u8")
[ "$code" = 403 ] && pass "SIGHUP 后新 vhost 生效（需鉴权，403）" || fail "SIGHUP 后返回 $code（期望 403）"
grep -q 'config reloaded' "$LOG" && pass "日志记录热加载完成" || fail "日志未记录热加载"

kill -0 "$SERVER_PID" 2>/dev/null && pass "进程存活" || fail "进程已退出"
echo "运维 E2E 失败项: $FAILS"
exit "$FAILS"
