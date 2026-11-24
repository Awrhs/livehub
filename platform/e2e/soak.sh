#!/usr/bin/env bash
# LiveHub 长稳测试：两路直播 + 反复进出的聊天用户与 FLV 观众 + HLS 轮询 + 中途断线重推，
# 持续 SOAK_MINUTES 分钟，每分钟记录平台与媒体服务器的内存（RSS）、文件描述符、线程数，
# 结束时检查是否持续增长（泄漏）以及日志中是否有 panic / 崩溃。
#
# 用法：platform/e2e/soak.sh <livehub> <streaming_server>
set -uo pipefail
LIVEHUB_BIN=${1:?}
MEDIA_BIN=${2:?}
MIN=${SOAK_MINUTES:-30}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=$(mktemp -d)
ART=${E2E_ARTIFACT_DIR:-$WORK/artifacts}
mkdir -p "$ART"
BASE=http://127.0.0.1:8000
PIDS=()
cleanup() {
  for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done
  [ -n "${LH:-}" ] && kill "$LH" 2>/dev/null && wait "$LH" 2>/dev/null
  cp "$WORK/livehub.log" "$ART/soak-livehub.log" 2>/dev/null
}
trap cleanup EXIT

LIVEHUB_ADDR=:8000 LIVEHUB_DATA_DIR="$WORK/data" LIVEHUB_MEDIA_BIN="$MEDIA_BIN" LIVEHUB_MEDIA_SRT_PORT=0 \
  LIVEHUB_BCRYPT_COST=4 LIVEHUB_REGISTER_PER_MIN=1000 "$LIVEHUB_BIN" >"$WORK/livehub.log" 2>&1 &
LH=$!
for _ in $(seq 1 60); do
  [ "$(curl -s $BASE/healthz | jq -r .media 2>/dev/null)" = "true" ] && break
  sleep 0.5
done

H=(-H 'X-Requested-With: livehub' -H 'Content-Type: application/json')
reg() { curl -s "${H[@]}" -c "$WORK/$1.jar" -b "$WORK/$1.jar" -X POST "$BASE/api/v1/auth/register" -d "{\"username\":\"$1\",\"password\":\"password123\"}" >/dev/null; }
reg admin1
curl -s "${H[@]}" -b "$WORK/admin1.jar" -X PUT "$BASE/api/v1/admin/settings" -d '{"stream_approval":"false"}' >/dev/null
KEYS=()
for u in soaka soakb; do
  reg $u
  KEYS+=("$(curl -s "${H[@]}" -b "$WORK/$u.jar" -X POST "$BASE/api/v1/me/channel/key" | jq -r .new_key.publish_key)")
done

push() { # push <key> <seconds> <log>
  ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=1280x720:rate=30" \
    -f lavfi -i "sine=frequency=440:sample_rate=48000" -t "$2" \
    -c:v libx264 -preset superfast -bf 2 -g 60 -b:v 2500k -pix_fmt yuv420p -c:a aac -b:a 128k \
    -f flv "rtmp://127.0.0.1:1935/live/$1" >>"$ART/$3" 2>&1
}
TOTAL=$((MIN * 60))
push "${KEYS[0]}" "$TOTAL" soak-push-a.log &
PIDS+=($!)
# 第二路每 10 分钟断开重推一次（模拟主播网络抖动重连）
(
  left=$TOTAL
  while [ $left -gt 0 ]; do
    seg=$((left < 600 ? left : 600))
    push "${KEYS[1]}" "$seg" soak-push-b.log
    left=$((left - seg))
  done
) &
PIDS+=($!)
sleep 8

# HLS 观众：持续轮询播放列表与分片
(
  while true; do
    for ch in soaka soakb; do
      pl=$(curl -s "$BASE/live/$ch.m3u8")
      seg=$(echo "$pl" | grep -v '^#' | tail -1)
      [ -n "$seg" ] && curl -s -o /dev/null "$BASE/live/$seg" 2>/dev/null
    done
    sleep 2
  done
) &
PIDS+=($!)

(cd "$ROOT/platform" && go run ./e2e/loadgen -base "$BASE" -channels soaka,soakb -chatters 20 -flv 10 \
  -duration "$((MIN - 1))m" >"$ART/soak-loadgen.log" 2>&1) &
PIDS+=($!)

echo "minute,lh_rss_kb,lh_fd,lh_threads,media_rss_kb,media_fd,media_threads" >"$ART/soak.csv"
sample() {
  local m=$1 mp
  mp=$(pgrep -f "streaming_server .*streaming.conf" | head -1)
  local lr lf lt mr mf mt
  lr=$(awk '/VmRSS/{print $2}' /proc/$LH/status); lf=$(ls /proc/$LH/fd | wc -l); lt=$(awk '/Threads/{print $2}' /proc/$LH/status)
  mr=$(awk '/VmRSS/{print $2}' /proc/$mp/status 2>/dev/null); mf=$(ls /proc/$mp/fd 2>/dev/null | wc -l); mt=$(awk '/Threads/{print $2}' /proc/$mp/status 2>/dev/null)
  echo "$m,$lr,$lf,$lt,$mr,$mf,$mt" >>"$ART/soak.csv"
  echo "  [$m 分钟] 平台 RSS=${lr}KB fd=$lf 线程=$lt | 媒体 RSS=${mr}KB fd=$mf 线程=$mt | $(tail -1 "$ART/soak-loadgen.log" 2>/dev/null)"
}
for m in $(seq 1 "$MIN"); do
  sleep 60
  sample "$m"
done
wait "${PIDS[3]}" 2>/dev/null

echo "== 结果"
FAIL=0
cat "$ART/soak-loadgen.log" | tail -2
# 以第 5 分钟（预热完成）为基准，与最后 3 分钟的平均值比较
check() { # check <列号> <名称> <允许增长比例> <允许绝对增长>
  local col=$1 name=$2 ratio=$3 abs=$4
  awk -F, -v c="$col" -v n="$name" -v r="$ratio" -v a="$abs" '
    NR==1 {next}
    { v[NR-1]=$c; cnt=NR-1 }
    END {
      base=v[5]; last=(v[cnt]+v[cnt-1]+v[cnt-2])/3
      grow=last-base
      printf "  %s：第5分钟 %d → 最后3分钟均值 %.0f（增长 %.0f）\n", n, base, last, grow
      if (grow > base*r && grow > a) { print "  [FAIL] " n " 持续增长"; exit 1 }
    }' "$ART/soak.csv" || FAIL=1
}
check 2 "平台内存(KB)" 0.5 30000
check 3 "平台 fd" 0.5 50
check 4 "平台线程" 0.5 20
check 5 "媒体内存(KB)" 0.5 60000
check 6 "媒体 fd" 0.5 50
check 7 "媒体线程" 0.5 20
if grep -qiE "panic|fatal error|data race" "$WORK/livehub.log"; then
  echo "  [FAIL] 日志中出现 panic"
  grep -iE -m5 "panic|fatal error" "$WORK/livehub.log"
  FAIL=1
fi
if grep -q "媒体服务器退出" "$WORK/livehub.log"; then
  echo "  [FAIL] 媒体服务器在长稳测试中退出"
  FAIL=1
fi
res=$(grep RESULT "$ART/soak-loadgen.log")
sent=$(echo "$res" | sed -n 's/.*sent=\([0-9]*\).*/\1/p')
errs=$(echo "$res" | sed -n 's/.*errors=\([0-9]*\).*/\1/p')
[ "${sent:-0}" -gt 1000 ] || { echo "  [FAIL] 聊天消息过少（$res）"; FAIL=1; }
[ "${errs:-999}" -lt 20 ] || { echo "  [FAIL] 负载错误过多（$res）"; FAIL=1; }
[ $FAIL -eq 0 ] && echo "长稳测试通过" || echo "长稳测试失败"
exit $FAIL
