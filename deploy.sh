#!/usr/bin/env bash
set -euo pipefail

REMOTE_HOST="niazl@192.168.99.114"
REMOTE_WEB_DIR="/var/www/lut_tester"
PORT=7341
WEB_DIR="lut_tester_host/web"

# Read firmware version from VERSION file
FW_VERSION_MAJOR=$(grep 'VERSION_MAJOR' VERSION | awk '{print $3}')
FW_VERSION_MINOR=$(grep 'VERSION_MINOR' VERSION | awk '{print $3}')
FW_PATCHLEVEL=$(grep 'PATCHLEVEL'    VERSION | awk '{print $3}')
FW_VERSION="${FW_VERSION_MAJOR}.${FW_VERSION_MINOR}.${FW_PATCHLEVEL}"

# Bump web patch version (+1)
WV=$(tr -d '[:space:]' < "${WEB_DIR}/WEB_VERSION")
MAJOR=$(echo "$WV" | cut -d. -f1)
MINOR=$(echo "$WV" | cut -d. -f2)
PATCH=$(echo "$WV" | cut -d. -f3)
NEW_VER="${MAJOR}.${MINOR}.$((PATCH + 1))"

echo "$NEW_VER" > "${WEB_DIR}/WEB_VERSION"
sed -i '' "s/const CACHE = 'eink-v[^']*'/const CACHE = 'eink-v${NEW_VER}'/" "${WEB_DIR}/sw.js"
printf '{"fw":"%s","web":"%s","build":"%s"}\n' \
    "$FW_VERSION" "$NEW_VER" "$(date '+%Y-%m-%d %H:%M:%S')" > "${WEB_DIR}/version.json"
echo ">>> Web version bumped: v${WV} → v${NEW_VER} (sw.js, version.json)"

echo "==> Syncing web files to ${REMOTE_HOST}:${REMOTE_WEB_DIR}"
rsync -avz --delete lut_tester_host/web/ "${REMOTE_HOST}:${REMOTE_WEB_DIR}/"

echo "==> Reloading nginx"
ssh "$REMOTE_HOST" "sudo systemctl reload nginx"

echo "==> Deployed. Available at http://192.168.99.114:${PORT}"
