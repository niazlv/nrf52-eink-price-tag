#!/usr/bin/env bash
# Play Bad Apple on the e-ink display.
# Run ./convert_bad_apple.sh first to generate bad_apple.ebf.
#
# Usage:
#   ./play_bad_apple.sh                      # guaranteed mode (every frame)
#   ./play_bad_apple.sh --sync               # sync mode (real-time, skips late frames)
#   ./play_bad_apple.sh --loop               # loop forever
#   ./play_bad_apple.sh --enc rle            # less compression, fewer artifacts
#   ./play_bad_apple.sh --address AA:BB:CC   # connect by MAC instead of name

set -e
cd "$(dirname "$0")"

EBF="bad_apple.ebf"
DEVICE="nrf52-E-ink-clock-DEV"

if [ ! -f "$EBF" ]; then
    echo "ERROR: $EBF not found. Run ./convert_bad_apple.sh first."
    exit 1
fi

MODE="guaranteed"
ENC="drle"
EXTRA=""

for arg in "$@"; do
    case "$arg" in
        --sync)       MODE="sync" ;;
        --guaranteed) MODE="guaranteed" ;;
        --loop)       EXTRA="$EXTRA --loop" ;;
        --enc=*)      ENC="${arg#--enc=}" ;;
        --enc)        shift; ENC="$1" ;;
        --address=*)  EXTRA="$EXTRA --address ${arg#--address=}" ;;
        --address)    shift; EXTRA="$EXTRA --address $1" ;;
        *)            EXTRA="$EXTRA $arg" ;;
    esac
done

echo "Playing $EBF  mode=$MODE  enc=$ENC"
echo "Device: $DEVICE"
echo "Ctrl+C to stop."
echo ""

.venv/bin/python vstream.py play "$EBF" \
    --device "$DEVICE" \
    --mode "$MODE" \
    --enc "$ENC" \
    $EXTRA
