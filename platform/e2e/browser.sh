#!/usr/bin/env bash
# 浏览器 E2E：启动全新的 LiveHub（内嵌媒体服务器），运行 Playwright 用户旅程测试。
# 用法：platform/e2e/browser.sh <livehub> <streaming_server> [playwright 参数，如测试文件]
set -uo pipefail
LIVEHUB_BIN=${1:?}
MEDIA_BIN=${2:?}
shift 2
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=$(mktemp -d)
ART=${E2E_ARTIFACT_DIR:-$WORK/artifacts}
mkdir -p "$ART"

# WebRTC 候选地址使用本机网卡 IP（Firefox 默认忽略回环地址候选，且更接近真实部署）
CAND=$( (hostname -I 2>/dev/null || true) | awk '{print $1}')
[ -n "$CAND" ] || CAND=127.0.0.1
LIVEHUB_ADDR=:8000 LIVEHUB_DATA_DIR="$WORK/data" LIVEHUB_MEDIA_BIN="$MEDIA_BIN" \
  LIVEHUB_MEDIA_RTMP_PORT=1935 LIVEHUB_MEDIA_HTTP_PORT=8081 LIVEHUB_MEDIA_SRT_PORT=0 \
  LIVEHUB_WEBRTC_CANDIDATE_IP="$CAND" LIVEHUB_BCRYPT_COST=4 LIVEHUB_REGISTER_PER_MIN="${LIVEHUB_REGISTER_PER_MIN:-100}" "$LIVEHUB_BIN" >"$ART/livehub-browser.log" 2>&1 &
LH=$!
trap 'kill $LH 2>/dev/null; wait $LH 2>/dev/null' EXIT

for _ in $(seq 1 60); do
  [ "$(curl -s http://127.0.0.1:8000/healthz | jq -r .media 2>/dev/null)" = "true" ] && break
  sleep 0.5
done

cd "$ROOT/web"
npx playwright test "$@"
rc=$?
# 播放器离开 WebRTC 模式时应 DELETE 结束会话（仅在跑主用户旅程时检查）
if [ $# -eq 0 ] && [ $rc -eq 0 ]; then
  ended=0
  for _ in $(seq 1 20); do
    grep -q 'WHEP 会话 .* 已由客户端结束' "$ART/livehub-browser.log" && { ended=1; break; }
    sleep 0.5
  done
  if [ $ended = 1 ]; then
    echo "  [PASS] 播放器离开 WebRTC 模式时结束了 WHEP 会话（DELETE）"
  else
    echo "  [FAIL] 播放器离开 WebRTC 模式时未结束 WHEP 会话"
    rc=1
  fi
fi
cp -r playwright-report test-results "$ART/" 2>/dev/null
exit $rc
