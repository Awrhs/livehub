#!/usr/bin/env bash
# Soak 测试：长时间推拉流 + 连接反复建立/销毁，检测内存与 fd 泄漏。
#
# 用法：soak.sh <streaming_server> [时长秒，默认 360]
# 通过条件：预热后 RSS 增长 < 30% + 32MB；fd 数不持续增长；进程存活且优雅退出。
set -u

BIN=${1:?usage: soak.sh <streaming_server> [seconds]}
DURATION=${2:-360}
WORK=$(mktemp -d)
LOG="$WORK/server.log"
HTTP=28080
RTMP=21935
FAILS=0
pass() { echo "  [PASS] $*"; }
fail() { echo "  [FAIL] $*"; FAILS=$((FAILS + 1)); }

cat >"$WORK/streaming.conf" <<EOF
HTTP_PORT=$HTTP
RTMP_PORT=$RTMP
SRT_PORT=29536
IO_THREADS=2
IDLE_TIMEOUT=15
HLS_SEGMENT_DURATION=2
HLS_WINDOW_SIZE=5
HLS_OUTPUT_DIR=$WORK/hls
JWT_SECRET=soak-secret-soak-secret-soak-secret
LOG_LEVEL=WARN
ENABLE_RATE_LIMIT=false
[vhost:__default__]
EOF

( cd "$WORK" && exec "$BIN" "$WORK/streaming.conf" ) >"$LOG" 2>&1 &
SERVER_PID=$!
cleanup() {
    pkill -P $$ 2>/dev/null
    kill -9 "$SERVER_PID" 2>/dev/null
    mkdir -p "${E2E_ARTIFACT_DIR:-/tmp/soak-artifacts}"
    cp "$LOG" "$WORK"/*.csv "${E2E_ARTIFACT_DIR:-/tmp/soak-artifacts}/" 2>/dev/null || true
}
trap cleanup EXIT
for _ in $(seq 1 50); do curl -sf --max-time 2 "http://127.0.0.1:$HTTP/api/health" >/dev/null && break; sleep 0.2; done

push() {  # push <key> <seconds>
    ffmpeg -hide_banner -loglevel error -re \
        -f lavfi -i "testsrc2=size=640x360:rate=25" -f lavfi -i "sine=frequency=800:sample_rate=44100" \
        -c:v libx264 -preset ultrafast -tune zerolatency -g 50 -pix_fmt yuv420p -c:a aac -b:a 64k \
        -t "$2" -f flv "rtmp://127.0.0.1:$RTMP/live/$1" >/dev/null 2>&1
}
rss() { awk '/VmRSS/{print $2}' "/proc/$SERVER_PID/status" 2>/dev/null; }
fds() { ls "/proc/$SERVER_PID/fd" 2>/dev/null | wc -l; }

# 长期推流 a；推流 b 每 60s 重启一次（覆盖推流建立/销毁路径）
push soak_a $((DURATION + 30)) &
( end=$((SECONDS + DURATION)); while [ $SECONDS -lt $end ]; do push soak_b 55; sleep 5; done ) &

# 观众反复连接/断开：每 10s 一批 HTTP-FLV(curl) + WS 握手 + HLS 拉取 + 立即断开的连接
(
    end=$((SECONDS + DURATION))
    while [ $SECONDS -lt $end ]; do
        for i in 1 2 3 4 5 6; do
            curl -s --max-time 8 "http://127.0.0.1:$HTTP/live/soak_a.flv" -o /dev/null &
        done
        curl -s --max-time 8 "http://127.0.0.1:$HTTP/live/soak_b.flv" -o /dev/null &
        timeout 8 ffmpeg -hide_banner -loglevel error -i "rtmp://127.0.0.1:$RTMP/live/soak_a" \
            -t 6 -f null - >/dev/null 2>&1 &
        timeout 8 ffmpeg -hide_banner -loglevel error -i "http://127.0.0.1:$HTTP/live/soak_a.m3u8" \
            -t 6 -f null - >/dev/null 2>&1 &
        for i in $(seq 1 20); do  # 短连接：API + 半开连接
            curl -s --max-time 2 "http://127.0.0.1:$HTTP/api/health" -o /dev/null
            (exec 3<>/dev/tcp/127.0.0.1/$RTMP; exec 3>&-) 2>/dev/null
        done
        sleep 10
    done
) &

echo "t_sec,rss_kb,fds" >"$WORK/samples.csv"
START=$SECONDS
while [ $((SECONDS - START)) -lt "$DURATION" ]; do
    sleep 15
    kill -0 "$SERVER_PID" 2>/dev/null || { fail "服务器进程在 $((SECONDS - START))s 时退出"; break; }
    echo "$((SECONDS - START)),$(rss),$(fds)" | tee -a "$WORK/samples.csv"
done

# 评估：预热期（前 25%）之后的样本
python3 - "$WORK/samples.csv" <<'PY' >"$WORK/verdict.txt"
import csv, sys
rows = [r for r in csv.DictReader(open(sys.argv[1])) if r["rss_kb"]]
n = len(rows)
warm = rows[n // 4:]
if len(warm) < 4:
    print("insufficient"); sys.exit()
r0 = sum(int(r["rss_kb"]) for r in warm[:2]) / 2
r1 = sum(int(r["rss_kb"]) for r in warm[-2:]) / 2
f0 = sum(int(r["fds"]) for r in warm[:2]) / 2
f1 = sum(int(r["fds"]) for r in warm[-2:]) / 2
print(f"rss_start={r0:.0f} rss_end={r1:.0f} fds_start={f0:.0f} fds_end={f1:.0f}")
PY
V=$(cat "$WORK/verdict.txt")
echo "  [INFO] $V"
r0=$(echo "$V" | grep -oE 'rss_start=[0-9]+' | cut -d= -f2)
r1=$(echo "$V" | grep -oE 'rss_end=[0-9]+' | cut -d= -f2)
f0=$(echo "$V" | grep -oE 'fds_start=[0-9]+' | cut -d= -f2)
f1=$(echo "$V" | grep -oE 'fds_end=[0-9]+' | cut -d= -f2)
if [ -n "$r0" ] && [ "$r1" -le $((r0 * 13 / 10 + 32768)) ]; then
    pass "RSS 稳定（${r0} → ${r1} kB）"
else
    fail "RSS 持续增长（${r0:-?} → ${r1:-?} kB）"
fi
if [ -n "$f0" ] && [ "$f1" -le $((f0 + 30)) ]; then
    pass "fd 数稳定（${f0} → ${f1}）"
else
    fail "fd 数持续增长（${f0:-?} → ${f1:-?}）"
fi

pkill -P $$ 2>/dev/null
sleep 2
kill -TERM "$SERVER_PID"
for _ in $(seq 1 50); do kill -0 "$SERVER_PID" 2>/dev/null || break; sleep 0.2; done
if kill -0 "$SERVER_PID" 2>/dev/null; then
    fail "SIGTERM 后未退出"
else
    wait "$SERVER_PID"; rc=$?
    [ "$rc" -eq 0 ] && pass "优雅退出" || fail "退出码 $rc"
fi
grep -E 'ERROR' "$LOG" | sort | uniq -c | sort -rn | head -10
echo "Soak 失败项: $FAILS"
exit "$FAILS"
