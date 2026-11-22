#!/usr/bin/env bash
# 扩展模块端到端测试：
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

echo "== WebRTC（aiortc 第三方客户端）"
PY=${WEBRTC_PY:-python3}
if $PY -c "import aiortc" 2>/dev/null; then
    WEBRTC_DEBUG=1 $PY "$(dirname "$0")/webrtc_client.py" whip "http://127.0.0.1:$HTTP/whip/whipin" 25 >"$WORK/whip.out" 2>"$WORK/whip.err" &
    WHIP_PID=$!
    sleep 10
    probe whip_flv "http://127.0.0.1:$HTTP/live/whipin.flv" h264 3 30
    probe whip_hls "http://127.0.0.1:$HTTP/live/whipin.m3u8" h264 3 30
    echo "    whip rtcp: $(grep -ciE 'psfb|pli|keyframe' "$WORK/whip.err") 条 PLI/关键帧相关日志；$(grep -iE 'psfb|pli' "$WORK/whip.err" | head -2 | cut -c1-160 | tr '\n' ' ')"
    $PY "$(dirname "$0")/webrtc_client.py" whep "http://127.0.0.1:$HTTP/whep/avc" 6 >"$WORK/whep.out" 2>"$WORK/whep.err"
    W=$(cat "$WORK/whep.out")
    vf=$(echo "$W" | grep -oE 'video_frames=[0-9]+' | cut -d= -f2)
    [ "${vf:-0}" -ge 60 ] && pass "WHEP 拉流（$W）" || fail "WHEP 拉流异常（$W；$(tail -2 "$WORK/whep.err" | tr '\n' ' ' | cut -c1-200)）"
    wait "$WHIP_PID"
    W=$(cat "$WORK/whip.out")
    echo "$W" | grep -q 'state=connected' && pass "WHIP 会话保持连接（$W）" \
        || { fail "WHIP 会话异常（$W）"; grep -iE 'check|stun|integrity|error|fail|dtls' "$WORK/whip.err" | head -15 | cut -c1-220 | sed 's/^/    whip.err: /'; }
else
    echo "  [INFO] 未安装 aiortc，跳过"
fi

echo "== 转码 / ABR（外部 ffmpeg 进程）"
curl -s --max-time 5 -X POST -H "Authorization: Bearer $ADM" \
    "http://127.0.0.1:$HTTP/api/abr/avc?profiles=480p" >"$WORK/abr.json"
echo "  [INFO] abr: $(head -c 200 "$WORK/abr.json")"
sleep 12
probe abr_480p_flv "http://127.0.0.1:$HTTP/live/avc_480p.flv" h264 3 40
probe abr_480p_hls "http://127.0.0.1:$HTTP/live/avc_480p.m3u8" h264 3 40
curl -s --max-time 5 "http://127.0.0.1:$HTTP/live/avc/master.m3u8" >"$WORK/master.m3u8"
grep -q 'EXT-X-STREAM-INF' "$WORK/master.m3u8" && grep -q 'avc_480p' "$WORK/master.m3u8" \
    && pass "ABR master playlist 列出变体" \
    || fail "ABR master playlist 异常: $(head -c 200 "$WORK/master.m3u8" | tr '\n' ' ')"
res=$(timeout 20 ffprobe -v error -select_streams v:0 -show_entries stream=height -of csv=p=0 \
      "http://127.0.0.1:$HTTP/live/avc_480p.flv" 2>/dev/null | head -1)
[ "$res" = 480 ] && pass "转码输出分辨率 480p" || fail "转码输出分辨率为 ${res:-未知}"

echo "== 源站-边缘集群（边缘按需回源）"
EHTTP=58080; ERTMP=51935
cat >"$WORK/edge.conf" <<EOF2
HTTP_PORT=$EHTTP
RTMP_PORT=$ERTMP
SRT_PORT=59535
RTSP_PORT=0
IO_THREADS=2
HLS_OUTPUT_DIR=$WORK/edge_hls
JWT_SECRET=edge-secret-edge-secret-edge-secret
ENABLE_RATE_LIMIT=false
ORIGIN_URL=http://127.0.0.1:$HTTP
[vhost:__default__]
EOF2
( mkdir -p "$WORK/edge" && cd "$WORK/edge" && exec "$BIN" "$WORK/edge.conf" ) >"$WORK/edge.log" 2>&1 &
EDGE_PID=$!
for _ in $(seq 1 50); do curl -sf --max-time 2 "http://127.0.0.1:$EHTTP/api/health" >/dev/null && break; sleep 0.2; done
probe edge_flv "http://127.0.0.1:$EHTTP/live/avc.flv" h264 3 50
probe edge_rtmp "rtmp://127.0.0.1:$ERTMP/live/avc" h264 3 50
# 边缘 HLS：首个请求触发回源（返回 404），首个分片就绪后可播放（播放器会重试）
curl -s -o /dev/null --max-time 3 "http://127.0.0.1:$EHTTP/live/avc.m3u8"
sleep 7
probe edge_hls "http://127.0.0.1:$EHTTP/live/avc.m3u8" h264 3 50
curl -s -o /dev/null --max-time 3 "http://127.0.0.1:$EHTTP/live/nosuch.flv"
sleep 2
storms=$(grep -c 'edge-nosuch.*reconnect' "$WORK/edge.log")
[ "$storms" -le 3 ] && pass "回源不存在的流不会形成重连风暴（重连 ${storms} 次）" \
    || fail "回源不存在的流形成重连风暴（2 秒内重连 ${storms} 次）"
kill -0 "$EDGE_PID" 2>/dev/null && pass "边缘进程存活" || fail "边缘进程已退出"
grep -E 'WARN|ERROR' "$WORK/edge.log" | sed -E 's/^[0-9-]+ [0-9:.]+ //' | sort | uniq -c | sort -rn | head -5 | sed 's/^/    edge: /'
kill "$EDGE_PID" 2>/dev/null

kill -0 "$SERVER_PID" 2>/dev/null && pass "进程存活" || fail "进程已退出"
echo
echo "== 服务器日志（WARN/ERROR 摘要）"
grep -E 'WARN|ERROR' "$LOG" | sed -E 's/^[0-9-]+ [0-9:.]+ //' | sort | uniq -c | sort -rn | head -25
echo "扩展 E2E 失败项: $FAILS"
exit "$FAILS"
