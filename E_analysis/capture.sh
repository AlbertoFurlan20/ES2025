#!/bin/bash
# Capture a telemetry run to a timestamped file.
#
# Usage: ./capture.sh [seconds] [device]
#
# Thin wrapper over bmp180_analysis/capture.py rather than a `stty` + `cat`
# pipeline, for two reasons found on real hardware:
#
#   1. On macOS, termios settings applied to a /dev/cu.* node with `stty -f` are
#      reset when the port is opened, so `cat` reads at the driver default baud
#      and records framing garbage. capture.py applies them to an already-open
#      descriptor and holds it for the whole run.
#   2. macOS ships no `timeout(1)`.
#
# By default the target is reset over the ST-Link once the port is already
# draining, so the capture begins at the session header and nobody has to press
# the board's B2 button. Set NO_RESET=1 to capture a board that is already running.

set -e

DURATION="${1:-120}"
DEVICE="${2:-/dev/cu.usbserial-0001}"
HERE="$(cd "$(dirname "$0")" && pwd)"

if [ ! -e "$DEVICE" ]; then
    echo "No such device: $DEVICE" >&2
    echo "Available:" >&2
    ls /dev/cu.* >&2
    exit 1
fi

PY="$HERE/.venv/bin/python"
if [ ! -x "$PY" ]; then
    PY="$(command -v python3)"
fi

RESET_ARGS=()
if [ -z "$NO_RESET" ]; then
    RESET_ARGS=(--reset-from "$HERE/../C_src")
fi

cd "$HERE"
exec "$PY" -m bmp180_analysis.capture "$DURATION" \
    --device "$DEVICE" \
    "${RESET_ARGS[@]}"
