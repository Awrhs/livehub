#!/usr/bin/env bash
# 浏览器 E2E：启动全新的 LiveHub（内嵌媒体服务器），运行 Playwright 用户旅程测试。
# 用法：platform/e2e/browser.sh <livehub> <streaming_server>
set -uo pipefail
LIVEHUB_BIN=${1:?}
MEDIA_BIN=${2:?}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=$(mktemp -d)
ART=${E2E_ARTIFACT_DIR:-$WORK/artifacts}
mkdir -p "$ART"

LIVEHUB_ADDR=:8000 LIVEHUB_DATA_DIR="$WORK/data" LIVEHUB_MEDIA_BIN="$MEDIA_BIN" \
  LIVEHUB_MEDIA_RTMP_PORT=1935 LIVEHUB_MEDIA_HTTP_PORT=8081 LIVEHUB_MEDIA_SRT_PORT=0 \
  LIVEHUB_BCRYPT_COST=4 "$LIVEHUB_BIN" >"$ART/livehub-browser.log" 2>&1 &
LH=$!
trap 'kill $LH 2>/dev/null; wait $LH 2>/dev/null' EXIT

for _ in $(seq 1 60); do
  [ "$(curl -s http://127.0.0.1:8000/healthz | jq -r .media 2>/dev/null)" = "true" ] && break
  sleep 0.5
done

cd "$ROOT/web"
npx playwright test
rc=$?
cp -r playwright-report test-results "$ART/" 2>/dev/null
exit $rc
