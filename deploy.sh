#!/usr/bin/env bash
set -euo pipefail

REMOTE_HOST="niazl@192.168.99.114"
REMOTE_WEB_DIR="/var/www/lut_tester"
PORT=7341

echo "==> Syncing web files to ${REMOTE_HOST}:${REMOTE_WEB_DIR}"
rsync -avz --delete lut_tester_host/web/ "${REMOTE_HOST}:${REMOTE_WEB_DIR}/"

echo "==> Reloading nginx"
ssh "$REMOTE_HOST" "sudo systemctl reload nginx"

echo "==> Deployed. Available at http://192.168.99.114:${PORT}"
