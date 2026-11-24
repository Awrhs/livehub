#!/usr/bin/env bash
# 基准：单机 N 路推流 × M 观众（HTTP-FLV）的 CPU / 内存曲线。
#
# 用法：bench_fanout.sh <streaming_server> [N 路推流，默认 4] [各阶段每路观众数，默认 "0 10 25 50"] [每阶段秒数，默认 20]
# 输出：每个阶段服务器的 CPU%（所有线程合计，100% = 一个核）、峰值 RSS、出口吞吐、观众码率（最低/中位）；
#       写入 bench.csv，并在 GitHub Actions 中写入作业摘要（$GITHUB_STEP_SUMMARY）。
# 通过条件：服务器存活；每个阶段所有观众都持续收到数据，且最低码率 ≥ 源码率的 50%。
set -u

BIN=${1:?usage: bench_fanout.sh <streaming_server> [streams] [viewers-per-stream stages] [seconds]}
N=${2:-4}
STAGES=${3:-"0 10 25 50"}
DUR=${4:-20}
WORK=$(mktemp -d)
LOG="$WORK/server.log"
CSV="$WORK/bench.csv"
HTTP=38080
RTMP=31935
FAILS=0
HERE=$(cd "$(dirname "$0")" && pwd)
pass() { echo "  [PASS] $*"; }
fail() { echo "  [FAIL] $*"; FAILS=$((FAILS + 1)); }

cat >"$WORK/streaming.conf" <<CONF
HTTP_PORT=$HTTP
RTMP_PORT=$RTMP
SRT_PORT=0
RTSP_PORT=0
IO_THREADS=4
HLS_SEGMENT_DURATION=2
HLS_WINDOW_SIZE=5
HLS_OUTPUT_DIR=$WORK/hls
ENABLE_TIMESHIFT=false
JWT_SECRET=bench-secret-bench-secret-bench-secret
LOG_LEVEL=WARN
ENABLE_RATE_LIMIT=false
[vhost:__default__]
CONF

( cd "$WORK" && exec "$BIN" "$WORK/streaming.conf" ) >"$LOG" 2>&1 &
PID=$!
cleanup() {
    pkill -P $$ 2>/dev/null
    kill "$PID" 2>/dev/null
    mkdir -p "${E2E_ARTIFACT_DIR:-/tmp/bench-artifacts}"
    cp "$LOG" "$CSV" "${E2E_ARTIFACT_DIR:-/tmp/bench-artifacts}/" 2>/dev/null || true
}
trap cleanup EXIT
for _ in $(seq 1 50); do curl -sf --max-time 2 "http://127.0.0.1:$HTTP/api/health" >/dev/null && break; sleep 0.2; done

# 样片只编码一次；推流用 -c copy，避免编码器占用 CPU 干扰对服务器的测量
ffmpeg -hide_banner -loglevel error -f lavfi -i "testsrc2=size=1280x720:rate=30" \
    -f lavfi -i "sine=frequency=440:sample_rate=44100" -t 20 \
    -c:v libx264 -preset veryfast -b:v 2000k -maxrate 2000k -bufsize 2000k -g 60 -pix_fmt yuv420p \
    -c:a aac -b:a 128k "$WORK/sample.flv"
SRC_KBPS=$(( $(stat -c %s "$WORK/sample.flv") * 8 / 20 / 1000 ))
echo "== 基准：$N 路推流（720p30，源码率约 ${SRC_KBPS} kbps）× 每路 {$STAGES} 个 HTTP-FLV 观众，每阶段 ${DUR}s"

for i in $(seq 1 "$N"); do
    ffmpeg -hide_banner -loglevel error -re -stream_loop -1 -i "$WORK/sample.flv" -c copy \
        -f flv "rtmp://127.0.0.1:$RTMP/live/bench$i" >"$WORK/push$i.log" 2>&1 &
done
sleep 5

CLK=$(getconf CLK_TCK)
ticks() { awk '{print $14 + $15}' "/proc/$PID/stat" 2>/dev/null; }
rss_kb() { awk '/VmRSS/{print $2}' "/proc/$PID/status" 2>/dev/null; }

echo "streams,viewers,cpu_pct,peak_rss_mb,egress_mbps,min_kbps,median_kbps,ok" >"$CSV"
ROWS=""
for m in $STAGES; do
    total=$((N * m))
    t0=$(ticks); s0=$(date +%s.%N)
    rm -f "$WORK"/v_*.json
    if [ "$m" -gt 0 ]; then
        for i in $(seq 1 "$N"); do
            python3 "$HERE/flv_viewers.py" 127.0.0.1 $HTTP "/live/bench$i.flv" "$m" "$DUR" >"$WORK/v_$i.json" &
        done
    fi
    peak=0
    end=$((SECONDS + DUR))
    while [ $SECONDS -lt "$end" ]; do
        r=$(rss_kb); [ "${r:-0}" -gt "$peak" ] && peak=$r
        sleep 1
    done
    # 等待本阶段观众进程结束（各自按时长退出）
    for _ in $(seq 1 30); do pgrep -f "flv_viewers.py 127.0.0.1 $HTTP" >/dev/null || break; sleep 1; done
    t1=$(ticks); s1=$(date +%s.%N)
    if ! kill -0 "$PID" 2>/dev/null; then fail "服务器在 $total 个观众阶段退出"; break; fi
    cpu=$(awk -v a="$t0" -v b="$t1" -v c="$CLK" -v x="$s0" -v y="$s1" 'BEGIN{printf "%.1f", (b-a)/c/(y-x)*100}')
    rss=$(awk -v k="$peak" 'BEGIN{printf "%.1f", k/1024}')
    read -r ok bytes mn med errs < <(python3 - "$WORK" "$DUR" <<'PY'
import glob, json, statistics, sys
work, dur = sys.argv[1], float(sys.argv[2])
ok = total = 0
kbps, errs = [], set()
for f in glob.glob(work + "/v_*.json"):
    try:
        d = json.load(open(f))
    except Exception as e:  # noqa: BLE001
        errs.add("bad output " + f)
        continue
    ok += d["ok"]; total += d["bytes"]; kbps += d["kbps"]; errs.update(d["errors"])
mn = min(kbps) if kbps else 0
med = statistics.median(kbps) if kbps else 0
print(ok, total, mn, med, ("|".join(sorted(errs)) or "-").replace(" ", "_"))
PY
)
    egress=$(awk -v b="$bytes" -v d="$DUR" 'BEGIN{printf "%.1f", b*8/d/1e6}')
    echo "$N,$total,$cpu,$rss,$egress,$mn,$med,$ok" >>"$CSV"
    ROWS+="| $N | $total | $cpu | $rss | $egress | $mn | $med |"$'\n'
    echo "  观众 $total：CPU ${cpu}%  峰值RSS ${rss}MB  出口 ${egress}Mbps  观众码率 最低 ${mn} / 中位 ${med} kbps  正常 $ok/$total"
    if [ "$m" -gt 0 ]; then
        if [ "$ok" = "$total" ] && awk -v a="$mn" -v b="$SRC_KBPS" 'BEGIN{exit !(a >= b*0.5)}'; then
            pass "$total 个观众均持续收到数据（最低 ${mn} kbps）"
        else
            fail "$total 个观众中 $ok 个正常，最低码率 ${mn} kbps（源 ${SRC_KBPS} kbps），错误：$errs"
        fi
    fi
done

if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    {
        echo "### 基准：单机 $N 路推流 × M 个 HTTP-FLV 观众"
        echo
        echo "源：720p30 H.264 + AAC，约 ${SRC_KBPS} kbps；每阶段 ${DUR} 秒；IO_THREADS=4；CPU% 为服务器全部线程合计（100% = 1 核）。"
        echo
        echo "| 推流 | 观众 | 服务器 CPU % | 峰值 RSS (MB) | 出口 (Mbps) | 观众最低 kbps | 观众中位 kbps |"
        echo "|---|---|---|---|---|---|---|"
        printf "%s" "$ROWS"
    } >>"$GITHUB_STEP_SUMMARY"
fi

echo "== 结果：FAIL=$FAILS"
[ "$FAILS" -eq 0 ]
