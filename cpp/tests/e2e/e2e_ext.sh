#!/usr/bin/env bash
# 扩展模块端到端测试（实验性，CI 中 continue-on-error）：
#   H.265/Enhanced RTMP、RTSP 拉流、SRT 推流、DVR 录制
# 用法：e2e_ext.sh <streaming_server>
set -u

BIN=${1:?usage: e2e_ext.sh <streaming_server>}
WORK=$(mktemp -d)
LOG="$WORK/server.log"
HTTP=38080
RTMP=31935
SRT=39535
ADM="ext-admin-token-0123456789"
FAILS=0
pass() { echo "  [PASS] $*"; }
fail() { echo "  [FAIL] $*"; FAILS=$((FAILS + 1)); }

cat >"$WORK/streaming.conf" <<EOF
HTTP_PORT=$HTTP
RTMP_PORT=$RTMP
SRT_PORT=$SRT
IO_THREADS=2
HLS_SEGMENT_DURATION=2
HLS_WINDOW_SIZE=5
HLS_OUTPUT_DIR=$WORK/hls
JWT_SECRET=ext-secret-ext-secret-ext-secret
ADMIN_TOKEN=$ADM
LOG_LEVEL=INFO
ENABLE_RATE_LIMIT=false
[vhost:__default__]
EOF

( cd "$WORK" && exec "$BIN" "$WORK/streaming.conf" ) >"$LOG" 2>&1 &
SERVER_PID=$!
cleanup() {
    pkill -P $$ 2>/dev/null
    kill -9 "$SERVER_PID" 2>/dev/null
    mkdir -p "${E2E_ARTIFACT_DIR:-/tmp/ext-artifacts}"
    cp -r "$WORK"/* "${E2E_ARTIFACT_DIR:-/tmp/ext-artifacts}/" 2>/dev/null || true
}
trap cleanup EXIT
for _ in $(seq 1 50); do curl -sf --max-time 2 "http://127.0.0.1:$HTTP/api/health" >/dev/null && break; sleep 0.2; done

# probe <name> <url> <expect_codec> <secs> <minframes> [extra ffmpeg input opts]
probe() {
    local name=$1 url=$2 codec=$3 secs=$4 minf=$5; shift 5
    local out="$WORK/$name.log"
    timeout 30 ffprobe -v error "$@" -show_entries stream=codec_name -of csv=p=0 "$url" >"$out.probe" 2>&1
    grep -q "^$codec" "$out.probe" && pass "$name: 探测到 $codec" \
        || { fail "$name: 未探测到 $codec ($(tr '\n' ' ' <"$out.probe" | tail -c 200))"; return; }
    timeout 40 ffmpeg -hide_banner -nostats "$@" -i "$url" -t "$secs" -map 0:v:0 -f null - >"$out" 2>&1
    local fr
    fr=$(grep -oE 'frame= *[0-9]+' "$out" | tail -1 | grep -oE '[0-9]+')
    [ "${fr:-0}" -ge "$minf" ] && pass "$name: 解码 $fr 帧" \
        || fail "$name: 解码帧数 ${fr:-0} < $minf ($(grep -iE 'error' "$out" | head -2 | tr '\n' ' '))"
}

echo "== ffmpeg 版本: $(ffmpeg -version | head -1)"

echo "== H.265 / Enhanced RTMP"
ffmpeg -hide_banner -loglevel warning -re -f lavfi -i "testsrc2=size=640x360:rate=25" \
    -f lavfi -i "sine=frequency=700:sample_rate=44100" \
    -c:v libx265 -preset ultrafast -x265-params "keyint=50:log-level=error" -pix_fmt yuv420p \
    -c:a aac -t 40 -f flv "rtmp://127.0.0.1:$RTMP/live/hevc" >"$WORK/push_hevc.log" 2>&1 &
sleep 9
if grep -qiE 'error|not supported|invalid' "$WORK/push_hevc.log"; then
    fail "HEVC 推流报错: $(head -3 "$WORK/push_hevc.log" | tr '\n' ' ')"
fi
probe hevc_flv "http://127.0.0.1:$HTTP/live/hevc.flv" hevc 3 50
probe hevc_hls "http://127.0.0.1:$HTTP/live/hevc.m3u8" hevc 3 50
probe hevc_rtmp "rtmp://127.0.0.1:$RTMP/live/hevc" hevc 3 50

echo "== H.264 推流（供 RTSP / DVR 使用）"
ffmpeg -hide_banner -loglevel warning -re -f lavfi -i "testsrc2=size=640x360:rate=25" \
    -f lavfi -i "sine=frequency=900:sample_rate=44100" \
    -c:v libx264 -preset ultrafast -tune zerolatency -g 50 -pix_fmt yuv420p \
    -c:a aac -t 120 -f flv "rtmp://127.0.0.1:$RTMP/live/avc" >"$WORK/push_avc.log" 2>&1 &
sleep 6

echo "== RTSP 拉流"
probe rtsp_tcp "rtsp://127.0.0.1:8554/live/avc" h264 3 50 -rtsp_transport tcp

echo "== DVR 录制"
curl -s --max-time 5 -X POST -H "Authorization: Bearer $ADM" \
    "http://127.0.0.1:$HTTP/api/record/avc/start" >"$WORK/dvr_start.json"
echo "  [INFO] start: $(head -c 200 "$WORK/dvr_start.json")"
sleep 8
curl -s --max-time 5 -X POST -H "Authorization: Bearer $ADM" \
    "http://127.0.0.1:$HTTP/api/record/avc/stop" >"$WORK/dvr_stop.json"
echo "  [INFO] stop: $(head -c 200 "$WORK/dvr_stop.json")"
sleep 2
mp4=$(find "$WORK" -name '*.mp4' -size +10k 2>/dev/null | head -1)
if [ -n "$mp4" ]; then
    pass "生成录像文件 $(basename "$mp4") ($(stat -c %s "$mp4") 字节)"
    probe dvr_mp4 "$mp4" h264 3 50
else
    fail "未生成录像文件（$(find "$WORK" -name '*.mp4' | head -3 | tr '\n' ' ')）"
fi

echo "== SRT"
if ffmpeg -hide_banner -protocols 2>/dev/null | grep -qw srt; then
    timeout 40 ffmpeg -hide_banner -loglevel warning -re -f lavfi -i "testsrc2=size=320x240:rate=25" \
        -f lavfi -i "sine=frequency=600:sample_rate=48000" \
        -c:v libx264 -preset ultrafast -g 50 -pix_fmt yuv420p -c:a aac -t 30 \
        -f mpegts "srt://127.0.0.1:$SRT?streamid=#!::r=live/srtin,m=publish" >"$WORK/push_srt.log" 2>&1 &
    sleep 9
    probe srt_in_flv "http://127.0.0.1:$HTTP/live/srtin.flv" h264 3 40
    probe srt_in_hls "http://127.0.0.1:$HTTP/live/srtin.m3u8" h264 3 40
    probe srt_out "srt://127.0.0.1:$SRT?streamid=#!::r=live/avc,m=request" h264 3 40
    # 重复推流到已有 RTMP 流应被拒绝
    timeout 8 ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=160x120:rate=10" \
        -c:v libx264 -preset ultrafast -t 4 -f mpegts \
        "srt://127.0.0.1:$SRT?streamid=#!::r=live/avc,m=publish" >/dev/null 2>&1 \
        && fail "SRT 抢占已有 RTMP 流未被拒绝" || pass "SRT 抢占已有 RTMP 流被拒绝"
    grep -iE 'error|fail' "$WORK/push_srt.log" | head -3
else
    echo "  [INFO] ffmpeg 未编译 SRT 支持，跳过"
fi

kill -0 "$SERVER_PID" 2>/dev/null && pass "进程存活" || fail "进程已退出"
echo
echo "== 服务器日志（WARN/ERROR 摘要）"
grep -E 'WARN|ERROR' "$LOG" | sed -E 's/^[0-9-]+ [0-9:.]+ //' | sort | uniq -c | sort -rn | head -25
echo "扩展 E2E 失败项: $FAILS"
exit "$FAILS"
