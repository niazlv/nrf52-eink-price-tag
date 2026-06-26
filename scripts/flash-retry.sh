#!/usr/bin/env bash
# ============================================================
# flash-retry.sh — keep re-flashing a .hex until it VERIFIES OK.
#
# For devices with flaky SWD connections (cheap price tags): a single
# bad attempt is normal, so we just keep trying. Two modes, auto-picked:
#
#   1. SERVER mode  — if an OpenOCD telnet server is already listening
#      (default localhost:4444, same as `make flash-gdb`), each attempt
#      sends `reset halt / program … verify / reset run` over telnet.
#      No adapter re-init between tries → fastest for a stable J-Link
#      with a flaky *target* connection.
#
#   2. STANDALONE   — if no server is up, spawn a fresh `openocd … program
#      … verify reset exit` per attempt (same invocation as
#      `make flash-openocd`). Fully re-inits adapter+target each try →
#      survives the J-Link link itself dropping.
#
# Usage:
#   scripts/flash-retry.sh [HEX]
#
# Env overrides:
#   OPENOCD_HOST=localhost  OPENOCD_PORT=4444
#   FLASH_MAX_RETRIES=0        # 0 = infinite
#   FLASH_RETRY_DELAY=2        # seconds between attempts
#   FLASH_ATTEMPT_TIMEOUT=40   # per-attempt watchdog (seconds)
#   OPENOCD_IFACE=interface/jlink.cfg
#   OPENOCD_TARGET=target/nrf52.cfg
#   OPENOCD_BIN=openocd
#   CONTINUOUS=0               # 1 = after success, wait & flash next device forever
# ============================================================
set -u

HEX="${1:-build/merged.hex}"
HOST="${OPENOCD_HOST:-localhost}"
PORT="${OPENOCD_PORT:-4444}"
MAX="${FLASH_MAX_RETRIES:-0}"
DELAY="${FLASH_RETRY_DELAY:-2}"
ATTEMPT_TIMEOUT="${FLASH_ATTEMPT_TIMEOUT:-40}"
IFACE="${OPENOCD_IFACE:-interface/jlink.cfg}"
TARGET="${OPENOCD_TARGET:-target/nrf52.cfg}"
OPENOCD_BIN="${OPENOCD_BIN:-openocd}"
CONTINUOUS="${CONTINUOUS:-0}"

# --- resolve HEX to an absolute path: openocd's cwd may differ from ours ---
case "$HEX" in
  /*) ;;
  *)  HEX="$PWD/$HEX" ;;
esac

if [ ! -f "$HEX" ]; then
  echo "!!! HEX not found: $HEX"
  echo "    Build it first ('make'), or pass an existing one, e.g.:"
  echo "      scripts/flash-retry.sh build_display_square_demo/merged.hex"
  exit 2
fi

# --- portable per-attempt watchdog (macOS has no GNU timeout) ---
run_with_timeout() {
  local secs="$1"; shift
  "$@" &
  local pid=$!
  ( sleep "$secs"; kill -TERM "$pid" 2>/dev/null; sleep 2; kill -KILL "$pid" 2>/dev/null ) &
  local wd=$!
  wait "$pid" 2>/dev/null; local rc=$?
  kill "$wd" 2>/dev/null; wait "$wd" 2>/dev/null
  return "$rc"
}

# --- detect a running OpenOCD telnet server ---
server_up() {
  (exec 3<>"/dev/tcp/$HOST/$PORT") 2>/dev/null && { exec 3>&- ; return 0; }
  return 1
}

attempt_via_telnet() {
  local out
  out=$(printf 'reset halt\nprogram %s verify\nreset run\nexit\n' "$HEX" \
          | nc -w "$ATTEMPT_TIMEOUT" "$HOST" "$PORT" 2>&1)
  echo "$out" | grep -Ei 'verified|programming|error|fail|could' || true
  # couldn't-open = wrong path, not a flaky link — fatal, stop spinning.
  if printf '%s' "$out" | grep -qi "couldn't open"; then
    echo "!!! OpenOCD can't open the hex (path wrong from openocd's cwd?). Stopping."
    return 99
  fi
  printf '%s' "$out" | grep -q 'Verified OK'
}

attempt_via_standalone() {
  run_with_timeout "$ATTEMPT_TIMEOUT" \
    "$OPENOCD_BIN" -f "$IFACE" -f "$TARGET" \
    -c "program $HEX verify reset exit"
}

flash_until_ok() {
  local n=0 rc
  while :; do
    n=$((n+1))
    printf '\n=== attempt %d ===\n' "$n"
    if [ "$MODE" = server ]; then
      attempt_via_telnet; rc=$?
    else
      attempt_via_standalone; rc=$?
    fi
    if [ "$rc" -eq 0 ]; then
      echo ">>> SUCCESS on attempt $n"
      return 0
    fi
    if [ "$rc" -eq 99 ]; then       # fatal (missing/unopenable hex)
      return 4
    fi
    if [ "$MAX" -ne 0 ] && [ "$n" -ge "$MAX" ]; then
      echo "!!! Gave up after $n attempts."
      return 1
    fi
    sleep "$DELAY"
  done
}

if server_up; then
  MODE=server
  echo ">>> Retry-flash (SERVER mode) $HEX via $HOST:$PORT"
else
  MODE=standalone
  echo ">>> Retry-flash (STANDALONE mode) $HEX via $IFACE / $TARGET"
fi
echo ">>> max=$MAX (0=infinite) delay=${DELAY}s timeout=${ATTEMPT_TIMEOUT}s  — Ctrl-C to stop"

if [ "$CONTINUOUS" = 1 ]; then
  dev=0
  while :; do
    dev=$((dev+1))
    echo ""
    echo "########## device #$dev — connect a tag, flashing… ##########"
    flash_until_ok || { echo "!!! device #$dev failed/aborted"; exit 1; }
    echo ">>> device #$dev done. Swap to the next tag; retrying in 3s (Ctrl-C to stop)…"
    sleep 3
  done
else
  flash_until_ok
  exit $?
fi
