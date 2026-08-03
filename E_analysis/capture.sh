#!/bin/bash
# Capture a telemetry run to a timestamped file.
#
# Usage: ./capture.sh [seconds] [device]
# Reset the board (black B2 button) right after this starts.

set -e

DURATION="${1:-120}"
DEVICE="${2:-/dev/cu.usbserial-0001}"
OUT_DIR="$(dirname "$0")/captures"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="$OUT_DIR/$STAMP.log"

mkdir -p "$OUT_DIR"

if [ ! -e "$DEVICE" ]; then
    echo "No such device: $DEVICE" >&2
    echo "Available:" >&2
    ls /dev/cu.* >&2
    exit 1
fi

# The device has no RTC, so record the host wall-clock start alongside the
# capture. Device timestamps are monotonic microseconds since ITS boot; absolute
# time is this value plus the device timestamp.
echo "# capture_start_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$OUT"

stty -f "$DEVICE" 115200 cs8 -cstopb -parenb raw

echo "Capturing $DURATION s from $DEVICE -> $OUT"
echo "Press the black B2 reset button now."

timeout "$DURATION" cat "$DEVICE" >> "$OUT" || true

echo "Done. $(grep -c '^S ' "$OUT" || true) samples captured."
echo "$OUT"
