#!/usr/bin/env bash
# LiveHub 全链路 E2E：平台（内嵌模式拉起 C++ 媒体服务器）+ 真实 ffmpeg 推拉流。
#
# 用法：platform/e2e/e2e.sh <livehub 可执行文件> <streaming_server 可执行文件>
# 依赖：curl jq ffmpeg ffprobe
set -uo pipefail

LIVEHUB_BIN=${1:?livehub 路径}
MEDIA_BIN=${2:?streaming_server 路径}
WORK=$(mktemp -d)
ART=${E2E_ARTIFACT_DIR:-$WORK/artifacts}
mkdir -p "$ART"
BASE=http://127.0.0.1:8000
RTMP=rtmp://127.0.0.1:1935/live
PASS=0
FAIL=0
PIDS=()

pass() { echo "  [PASS] $*"; PASS=$((PASS + 1)); }
fail() { echo "  [FAIL] $*"; FAIL=$((FAIL + 1)); }

cleanup() {
  for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done
  [ -n "${LH_PID:-}" ] && kill "$LH_PID" 2>/dev/null && wait "$LH_PID" 2>/dev/null
  cp -r "$WORK/data/media/streaming.conf" "$ART/" 2>/dev/null
  cp "$WORK/livehub.log" "$ART/" 2>/dev/null
}
trap cleanup EXIT

# api <cookie 文件> <方法> <路径> [JSON 体] —— 输出响应体，HTTP 状态码写入 $WORK/status
api() {
  local jar=$1 method=$2 path=$3 body=${4:-}
  local args=(-s -o "$WORK/body" -w '%{http_code}' -b "$jar" -c "$jar" -X "$method"
    -H 'X-Requested-With: livehub' -H 'Content-Type: application/json')
  [ -n "$body" ] && args+=(--data "$body")
  curl "${args[@]}" "$BASE/api/v1$path" >"$WORK/status"
  cat "$WORK/body"
}
status() { cat "$WORK/status"; }

# wait_for <秒> <命令…>：命令成功即返回 0
wait_for() {
  local n=$1
  shift
  for _ in $(seq 1 "$((n * 2))"); do
    if "$@" >/dev/null 2>&1; then return 0; fi
    sleep 0.5
  done
  return 1
}

is_live() { [ "$(curl -s "$BASE/api/v1/channels/$1" | jq -r .channel.is_live)" = "true" ]; }
is_offline() { [ "$(curl -s "$BASE/api/v1/channels/$1" | jq -r .channel.is_live)" = "false" ]; }
media_ok() { [ "$(curl -s "$BASE/healthz" | jq -r .media)" = "true" ]; }

push() { # push <推流码> <秒数> <日志名>
  ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=640x360:rate=25" \
    -f lavfi -i "sine=frequency=440:sample_rate=44100" -t "$2" \
    -c:v libx264 -preset ultrafast -tune zerolatency -g 50 -pix_fmt yuv420p -c:a aac -b:a 64k \
    -f flv "$RTMP/$1" >"$ART/$3.log" 2>&1
}

echo "== 启动边缘媒体节点（回源到平台）"
mkdir -p "$WORK/edge"
cat >"$WORK/edge/edge.conf" <<EOF
HTTP_PORT=8082
RTMP_PORT=1936
SRT_PORT=0
RTSP_PORT=0
ORIGIN_URL=http://127.0.0.1:8000
NODE_ID=edge-1
HLS_OUTPUT_DIR=$WORK/edge/hls
DVR_DIR=$WORK/edge/dvr
EOF
(cd "$WORK/edge" && exec "$MEDIA_BIN" edge.conf >"$ART/edge.log" 2>&1) &
PIDS+=($!)

echo "== 启动 LiveHub（内嵌媒体服务器）"
# 有 mailpit（CI 中以容器运行）时开启邮件功能
if curl -sf http://127.0.0.1:8025/api/v1/messages >/dev/null 2>&1; then
  export LIVEHUB_SMTP_HOST=127.0.0.1 LIVEHUB_SMTP_PORT=1025 LIVEHUB_SMTP_TLS=none \
    LIVEHUB_SMTP_FROM="LiveHub <noreply@livehub.test>"
  MAILPIT=1
fi
LIVEHUB_EDGE_URLS=http://127.0.0.1:8082 LIVEHUB_ADDR=:8000 LIVEHUB_DATA_DIR="$WORK/data" LIVEHUB_MEDIA_BIN="$MEDIA_BIN" \
  LIVEHUB_MEDIA_RTMP_PORT=1935 LIVEHUB_MEDIA_HTTP_PORT=8081 LIVEHUB_MEDIA_SRT_PORT=10080 \
  LIVEHUB_BCRYPT_COST=4 LIVEHUB_LOG_LEVEL=debug \
  "$LIVEHUB_BIN" >"$WORK/livehub.log" 2>&1 &
LH_PID=$!
if wait_for 30 media_ok; then pass "平台与媒体服务器就绪"; else
  fail "平台未就绪"
  tail -50 "$WORK/livehub.log"
  exit 1
fi

echo "== 前端页面"
if curl -s "$BASE/" | grep -q '<div id="root">'; then pass "首页返回前端"; else fail "首页未返回前端"; fi
if curl -s "$BASE/bob" | grep -q '<div id="root">'; then pass "频道页路由回落到前端"; else fail "频道页路由"; fi

echo "== 注册与开播审批"
A=$WORK/alice.jar B=$WORK/bob.jar C=$WORK/carol.jar
role=$(api "$A" POST /auth/register '{"username":"alice","password":"password123"}' | jq -r .role)
[ "$role" = "admin" ] && pass "首个用户为管理员" || fail "首个用户角色=$role"
api "$B" POST /auth/register '{"username":"bob","password":"password123","display_name":"Bob 的频道"}' >/dev/null
[ "$(status)" = "201" ] && pass "注册 bob" || fail "注册 bob: $(status)"
api "$C" POST /auth/register '{"username":"carol","password":"password123"}' >/dev/null
[ "$(api "$B" GET /me | jq -r .channel.has_stream_key)" = "false" ] && pass "新用户尚未生成推流码" || fail "新用户已有推流码"
KEY=$(api "$B" POST /me/channel/key | jq -r .new_key.publish_key)
api "$B" GET /me | grep -q "${KEY#*key=}" && fail "/me 泄露了推流码明文" || pass "推流码只在生成时返回"
grep -aq "${KEY#*key=}" "$WORK/data/livehub.db"* && fail "数据库中存有推流码明文" || pass "数据库只存推流码摘要"
INGEST=$(api "$B" GET /me | jq -r .ingest.rtmp)
[ "$INGEST" = "rtmp://127.0.0.1/live" ] && pass "推流地址 $INGEST" || fail "推流地址=$INGEST"
[[ "$KEY" == bob\?key=lh_* ]] && pass "推流码格式" || fail "推流码=$KEY"

timeout 15 bash -c "$(declare -f push); RTMP=$RTMP ART=$ART push '$KEY' 5 push_unapproved"
if is_offline bob; then pass "未审批推流被拒绝"; else fail "未审批推流未被拒绝"; fi

api "$B" POST /me/stream-request '{"note":"E2E"}' >/dev/null
BOB_ID=$(api "$A" GET '/admin/users?stream_status=pending' | jq -r '.users[0].id')
api "$A" PATCH "/admin/users/$BOB_ID" '{"stream_status":"approved"}' >/dev/null
[ "$(status)" = "200" ] && pass "管理员批准开播" || fail "批准失败 $(status)"
[ "$(api "$B" GET /me | jq -r .can_stream)" = "true" ] && pass "bob 可开播" || fail "bob 不可开播"

echo "== 邮件：验证邮箱、找回密码（mailpit）"
# mail_token <收件人> <主题关键字>：从 mailpit 取最新一封匹配邮件中的令牌
mail_token() {
  local id
  id=$(curl -s "http://127.0.0.1:8025/api/v1/search?query=to:$1" |
    jq -r --arg s "$2" '[.messages[] | select(.Subject | contains($s))][0].ID // empty')
  [ -n "$id" ] || return 1
  curl -s "http://127.0.0.1:8025/api/v1/message/$id" | jq -r .Text | grep -o 'token=[A-Za-z0-9%]*' | head -1 | cut -d= -f2
}
wait_token() { # wait_token <收件人> <主题关键字>
  local t
  for _ in $(seq 1 30); do
    t=$(mail_token "$1" "$2") && [ -n "$t" ] && { echo "$t"; return 0; }
    sleep 0.5
  done
  return 1
}
if [ -n "${MAILPIT:-}" ]; then
  [ "$(curl -s "$BASE/api/v1/site" | jq -r .mail_enabled)" = "true" ] && pass "邮件服务已启用" || fail "邮件服务未启用"
  api "$C" POST /me/email '{"email":"carol@example.com"}' >/dev/null
  if VT=$(wait_token carol@example.com 验证); then
    pass "收到验证邮件（SMTP 实际投递）"
    api "$C" POST /auth/verify-email "{\"token\":\"$VT\"}" >/dev/null
    [ "$(api "$C" GET /me | jq -r .email_verified)" = "true" ] && pass "邮箱验证成功" || fail "邮箱未验证"
  else
    fail "没有收到验证邮件"
    grep -i "邮件\|mail" "$WORK/livehub.log" | tail -5
  fi
  api "$WORK/anon.jar" POST /auth/forgot '{"login":"carol"}' >/dev/null
  if RT=$(wait_token carol@example.com 重置); then
    pass "收到重置密码邮件"
    api "$WORK/anon.jar" POST /auth/reset "{\"token\":\"$RT\",\"password\":\"brandnew123\"}" >/dev/null
    [ "$(status)" = "200" ] && pass "通过邮件链接重置密码" || fail "重置密码失败 $(status)"
    api "$C" POST /auth/login '{"username":"carol","password":"brandnew123"}' >/dev/null
    [ "$(status)" = "200" ] && pass "新密码可登录" || fail "新密码登录失败 $(status)"
    api "$WORK/carol3.jar" POST /auth/login '{"username":"carol","password":"password123"}' >/dev/null
    [ "$(status)" = "401" ] && pass "旧密码失效" || fail "旧密码仍可登录 $(status)"
    api "$WORK/anon.jar" POST /auth/reset "{\"token\":\"$RT\",\"password\":\"another12345\"}" >/dev/null
    [ "$(status)" = "400" ] && pass "重置链接只能用一次" || fail "重置链接可重复使用"
  else
    fail "没有收到重置密码邮件"
  fi
else
  echo "  [SKIP] 未检测到 mailpit，跳过邮件测试"
fi

echo "== 错误推流码被拒绝"
timeout 15 bash -c "$(declare -f push); RTMP=$RTMP ART=$ART push 'bob?key=wrong' 5 push_wrongkey"
if is_offline bob; then pass "错误密钥推流被拒绝"; else fail "错误密钥推流未被拒绝"; fi
timeout 15 bash -c "$(declare -f push); RTMP=$RTMP ART=$ART push 'carol?key=x' 5 push_carol"
if is_offline carol; then pass "无权限用户推流被拒绝"; else fail "carol 推流未被拒绝"; fi

echo "== RTMP 推流 → 直播中"
push "$KEY" 45 push_ok &
PUSH_PID=$!
PIDS+=("$PUSH_PID")
if wait_for 20 is_live bob; then pass "开播后频道显示直播中"; else fail "未进入直播状态"; fi
n=$(curl -s "$BASE/api/v1/channels?live=1" | jq '.channels | length')
[ "$n" = "1" ] && pass "直播列表有 1 个频道" || fail "直播列表数量=$n"

echo "== 经平台代理播放"
if ffprobe -v error -rw_timeout 10000000 -show_entries stream=codec_name -of csv=p=0 "$BASE/live/bob.flv" >"$WORK/flv.txt" 2>&1 &&
  grep -q h264 "$WORK/flv.txt" && grep -q aac "$WORK/flv.txt"; then
  pass "HTTP-FLV 播放（h264+aac）"
else
  fail "HTTP-FLV 播放: $(cat "$WORK/flv.txt")"
fi
hls_ready() { curl -sf "$BASE/live/bob.m3u8" | grep -q '#EXTINF'; }
if wait_for 20 hls_ready; then
  if ffprobe -v error -show_entries stream=codec_name -of csv=p=0 "$BASE/live/bob.m3u8" >"$WORK/hls.txt" 2>&1 &&
    grep -q h264 "$WORK/hls.txt"; then
    pass "HLS 播放"
  else
    fail "HLS ffprobe: $(cat "$WORK/hls.txt")"
  fi
else
  fail "HLS 播放列表未就绪"
fi
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/live/bob.flv")
[ "$code" = "405" ] && pass "播放代理拒绝非 GET" || fail "播放代理 POST 返回 $code"
code=$(curl -s -o /dev/null -w '%{http_code}' "$BASE/live/../api/streams")
[ "$code" != "200" ] && pass "播放代理不暴露媒体管理 API" || fail "播放代理泄露管理 API"

echo "== 边缘节点分发"
on_edge() { [ "$(curl -s "$BASE/api/v1/channels/bob" | jq -r .channel.playback.flv)" = "http://127.0.0.1:8082/live/bob.flv" ]; }
if wait_for 10 on_edge; then pass "播放地址指向边缘节点"; else fail "播放地址=$(curl -s "$BASE/api/v1/channels/bob" | jq -r .channel.playback.flv)"; fi
if ffprobe -v error -rw_timeout 15000000 -show_entries stream=codec_name -of csv=p=0 "http://127.0.0.1:8082/live/bob.flv" >"$WORK/edge.txt" 2>&1 &&
  grep -q h264 "$WORK/edge.txt"; then
  pass "边缘节点回源播放 HTTP-FLV"
else
  fail "边缘播放失败: $(cat "$WORK/edge.txt" | head -5)"
fi

echo "== 同名重复推流被拒绝"
timeout 15 bash -c "$(declare -f push); RTMP=$RTMP ART=$ART push '$KEY' 5 push_dup"
if kill -0 "$PUSH_PID" 2>/dev/null && is_live bob; then pass "重复推流不影响原推流"; else fail "重复推流影响了原推流"; fi

echo "== 管理员断流"
api "$A" POST /admin/channels/bob/kick >/dev/null
if wait_for 10 is_offline bob; then pass "断流后频道下播"; else fail "断流后仍在直播"; fi
if wait_for 10 bash -c "! kill -0 $PUSH_PID 2>/dev/null"; then pass "推流端被断开"; else fail "推流端仍在推流"; fi

echo "== 推流结束自动下播"
push "$KEY" 8 push_short &
PIDS+=($!)
wait_for 20 is_live bob && pass "再次开播" || fail "再次开播失败"
if wait_for 30 is_offline bob; then pass "推流结束后自动下播"; else fail "推流结束后未下播"; fi

echo "== 阶段 2：关注通知、封面、多清晰度、录像回放"
api "$C" POST /channels/bob/follow >/dev/null
[ "$(status)" = "200" ] && pass "carol 关注 bob" || fail "关注失败 $(status)"
api "$A" PUT /admin/settings '{"transcode_enabled":"true"}' >/dev/null
sleep 3  # 等待上一场录像归档完成
VODS_BEFORE=$(curl -s "$BASE/api/v1/channels/bob/vods" | jq '.vods | length')
push "$KEY" 40 push_phase2 &
PIDS+=($!)
wait_for 20 is_live bob && pass "开播（转码开启）" || fail "开播失败"
has_notice() { [ "$(api "$C" GET /me/notifications | jq -r .unread)" -ge 1 ]; }
if wait_for 10 has_notice; then pass "关注者收到开播通知"; else fail "关注者未收到开播通知"; fi
has_thumb() { [ -n "$(curl -s "$BASE/api/v1/channels/bob" | jq -r '.channel.thumbnail // empty')" ]; }
if wait_for 25 has_thumb; then
  T=$(curl -s "$BASE/api/v1/channels/bob" | jq -r .channel.thumbnail)
  ct=$(curl -s -o "$ART/live_thumb.jpg" -w '%{content_type}' "$BASE$T")
  [[ "$ct" == image/jpeg* ]] && pass "直播封面 $T" || fail "封面类型 $ct"
else
  fail "未生成直播封面"
fi
has_abr() { [ -n "$(curl -s "$BASE/api/v1/channels/bob" | jq -r '.channel.playback.abr // empty')" ]; }
if wait_for 30 has_abr; then
  ABR=$(curl -s "$BASE/api/v1/channels/bob" | jq -r .channel.playback.abr)
  curl -s "$BASE$ABR" >"$ART/master.m3u8"
  if grep -q "bob_720p" "$ART/master.m3u8" && grep -q "bob_480p" "$ART/master.m3u8"; then pass "多清晰度主播放列表"; else fail "主播放列表内容: $(head -c 300 "$ART/master.m3u8")"; fi
  v720() { curl -sf "$BASE/live/bob_720p.m3u8" | grep -q '#EXTINF'; }
  if wait_for 20 v720; then pass "720p 档位可播放"; else fail "720p 档位不可播放"; fi
else
  fail "多清晰度未就绪"
  grep -i "abr\|transcode\|变体\|internal" "$WORK/livehub.log" | tail -20
fi
api "$A" PUT /admin/settings '{"transcode_enabled":"false"}' >/dev/null
if wait_for 40 is_offline bob; then pass "推流结束下播"; else fail "推流结束未下播"; fi
vod_ready() { [ "$(curl -s "$BASE/api/v1/channels/bob/vods" | jq '.vods | length')" -gt "$VODS_BEFORE" ]; }
if wait_for 30 vod_ready; then
  VOD=$(curl -s "$BASE/api/v1/channels/bob/vods" | jq '.vods[0]')
  echo "  [INFO] VOD: $(echo "$VOD" | jq -c '{id,title,duration_sec,size_bytes,url,thumbnail}')"
  VURL=$(echo "$VOD" | jq -r .url)
  dur=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$BASE$VURL" 2>&1)
  if awk "BEGIN{exit !($dur > 25)}" 2>/dev/null; then pass "录像回放时长 ${dur}s"; else fail "录像时长异常: $dur"; fi
  code=$(curl -s -o /dev/null -w '%{http_code}' -H 'Range: bytes=0-1023' "$BASE$VURL")
  [ "$code" = "206" ] && pass "录像支持 Range 拖动" || fail "Range 返回 $code"
  VT=$(echo "$VOD" | jq -r '.thumbnail // empty')
  [ -n "$VT" ] && [ "$(curl -s -o /dev/null -w '%{http_code}' "$BASE$VT")" = "200" ] && pass "录像封面" || fail "录像封面缺失"
  VID=$(echo "$VOD" | jq -r .id)
  api "$C" DELETE "/vods/$VID" >/dev/null
  [ "$(status)" = "403" ] && pass "非主播不能删除录像" || fail "非主播删除录像返回 $(status)"
else
  fail "未生成录像回放"
  grep -i "录像\|record" "$WORK/livehub.log" | tail -20
fi

echo "== OBS 风格推流：1080p60、High profile、B 帧、6 Mbps"
ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=1920x1080:rate=60" \
  -f lavfi -i "sine=frequency=440:sample_rate=48000" -t 25 \
  -c:v libx264 -preset superfast -profile:v high -bf 2 -g 120 -b:v 6000k -maxrate 6000k -bufsize 12000k -pix_fmt yuv420p \
  -c:a aac -b:a 160k -f flv "$RTMP/$KEY" >"$ART/push_obs.log" 2>&1 &
OBS_PID=$!
PIDS+=("$OBS_PID")
wait_for 20 is_live bob && pass "1080p60 推流开播" || fail "1080p60 推流未开播"
ffprobe -v error -rw_timeout 15000000 -select_streams v:0 -show_entries stream=width,height,profile,has_b_frames,avg_frame_rate \
  -of default=nw=1 "$BASE/live/bob.flv" >"$WORK/obs.txt" 2>&1
echo "  [INFO] $(tr '\n' ' ' <"$WORK/obs.txt")"
grep -q "width=1920" "$WORK/obs.txt" && grep -q "profile=High" "$WORK/obs.txt" && pass "FLV 拉流保留 1080p High profile" || fail "FLV 流参数异常"
grep -q "has_b_frames=[1-9]" "$WORK/obs.txt" && pass "B 帧透传" || fail "B 帧丢失"
hls_new() { curl -sf "$BASE/live/bob.m3u8" | grep -q '#EXTINF'; }
if wait_for 20 hls_new && ffprobe -v error -show_entries format=duration -of csv=p=0 "$BASE/live/bob.m3u8" >"$WORK/obs_hls.txt" 2>&1; then
  pass "1080p60 HLS 可播放"
else
  fail "1080p60 HLS 播放失败: $(head -c 300 "$WORK/obs_hls.txt")"
fi
# 主播网络中断后立即重连（OBS 自动重连）：旧连接被强杀，新连接马上推流
kill -9 "$OBS_PID" 2>/dev/null
push "$KEY" 20 push_reconnect &
PIDS+=($!)
sleep 2
if wait_for 20 is_live bob; then pass "断线立即重连后恢复直播"; else fail "断线重连后未恢复直播"; fi
if ffprobe -v error -rw_timeout 15000000 -show_entries stream=codec_name -of csv=p=0 "$BASE/live/bob.flv" 2>&1 | grep -q h264; then
  pass "重连后可正常播放"
else
  fail "重连后无法播放"
fi
wait_for 40 is_offline bob

echo "== SRT 推流"
SID="#!::r=live/$KEY,m=publish"
SRT_URL="srt://127.0.0.1:10080?streamid=$(jq -rn --arg s "$SID" '$s|@uri')&latency=200000"
ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=640x360:rate=25" -t 20 \
  -c:v libx264 -preset ultrafast -g 50 -pix_fmt yuv420p -f mpegts "$SRT_URL" >"$ART/push_srt.log" 2>&1 &
PIDS+=($!)
if wait_for 20 is_live bob; then pass "SRT 推流开播"; else fail "SRT 推流未开播: $(head -c 300 "$ART/push_srt.log")"; fi
SID_BAD="#!::r=live/bob?key=wrong,m=publish"
timeout 10 ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=320x180:rate=25" -t 5 \
  -c:v libx264 -preset ultrafast -f mpegts "srt://127.0.0.1:10080?streamid=$(jq -rn --arg s "$SID_BAD" '$s|@uri')" \
  >"$ART/push_srt_bad.log" 2>&1
[ $? -ne 0 ] && pass "SRT 错误密钥被拒绝" || fail "SRT 错误密钥未被拒绝"
wait_for 30 is_offline bob

echo "== 封禁主播"
push "$KEY" 40 push_ban &
PUSH_PID=$!
PIDS+=("$PUSH_PID")
wait_for 20 is_live bob
api "$A" PATCH "/admin/users/$BOB_ID" '{"banned":true,"ban_reason":"E2E"}' >/dev/null
if wait_for 10 is_offline bob || [ "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/api/v1/channels/bob")" = "404" ]; then
  pass "封禁后直播结束"
else
  fail "封禁后仍在直播"
fi
if wait_for 10 bash -c "! kill -0 $PUSH_PID 2>/dev/null"; then pass "封禁后推流端被断开"; else fail "封禁后推流端未断开"; fi
api "$B" GET /me >/dev/null
[ "$(status)" = "401" ] && pass "封禁后会话失效" || fail "封禁后会话仍有效 $(status)"
api "$A" PATCH "/admin/users/$BOB_ID" '{"banned":false}' >/dev/null

echo "== 媒体服务器崩溃自动恢复"
MPID=$(pgrep -f "streaming_server .*streaming.conf" | head -1)  # 匹配子进程命令行，避免误杀本脚本
if [ -n "$MPID" ]; then
  kill -9 "$MPID"
  sleep 1
  if wait_for 30 media_ok; then pass "媒体服务器崩溃后被重新拉起"; else fail "媒体服务器未恢复"; fi
  push "$KEY" 10 push_after_restart &
  PIDS+=($!)
  if wait_for 20 is_live bob; then pass "恢复后可正常开播"; else fail "恢复后无法开播"; fi
else
  fail "找不到媒体服务器进程"
fi

echo
echo "== 结果：PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
