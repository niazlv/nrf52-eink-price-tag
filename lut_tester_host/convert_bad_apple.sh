#!/usr/bin/env bash
# One-time conversion of Bad Apple to .ebf (eink binary frames).
# Run once; then play with play_bad_apple.sh.
#
# Bad Apple is 480×360 (landscape).  The display is 128×296 (portrait).
# Default: rotate 90° + fill (scale to cover, crop sides) — uses 100% of screen.
#
# Usage:
#   ./convert_bad_apple.sh                     # rotate90 + fill + dither (recommended)
#   ./convert_bad_apple.sh --fit               # rotate90 + fit (letterbox, 57% screen)
#   ./convert_bad_apple.sh --no-rotate         # no rotation (letterbox top band, 32% screen)
#   ./convert_bad_apple.sh --stretch           # rotate90 + stretch (slightly distorted)
#   ./convert_bad_apple.sh --no-dither         # threshold instead of Floyd-Steinberg
#   ./convert_bad_apple.sh --invert            # invert B/W

set -e
cd "$(dirname "$0")"

VIDEO="../videos/bad-apple/$(ls ../videos/bad-apple/ | head -1)"
OUTPUT="bad_apple.ebf"

ROTATE="--rotate 90"
SCALE="--scale fill"
DITHER="--dither"
EXTRA=""

for arg in "$@"; do
    case "$arg" in
        --fit)       SCALE="--scale fit" ;;
        --stretch)   SCALE="--scale stretch" ;;
        --no-rotate) ROTATE="" ;;
        --no-dither) DITHER="" ;;
        --invert)    EXTRA="$EXTRA --invert" ;;
        *)           EXTRA="$EXTRA $arg" ;;
    esac
done

echo "Converting: $VIDEO"
echo "Output:     $OUTPUT"
echo "Options:    $ROTATE $SCALE $DITHER $EXTRA"
echo ""

.venv/bin/python vstream.py convert "$VIDEO" "$OUTPUT" $ROTATE $SCALE $DITHER $EXTRA

echo ""
echo "Done! Now run:  ./play_bad_apple.sh"
