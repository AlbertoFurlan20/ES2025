#!/bin/bash
set -e

BAUD=115200

if ! command -v screen >/dev/null; then
    echo "screen not found. Install it:" >&2
    echo "  macOS          brew install screen   (usually preinstalled)" >&2
    echo "  Debian/Ubuntu  sudo apt install screen" >&2
    echo "  Fedora         sudo dnf install screen" >&2
    echo "  RHEL/Rocky     sudo dnf install epel-release && sudo dnf install screen" >&2
    echo "  Arch           sudo pacman -S screen" >&2
    exit 1
fi

# First glob that matches wins: macOS names the CH340 /dev/cu.usbserial-*,
# Linux gives it /dev/ttyUSB* and a CDC-ACM adapter /dev/ttyACM*. Pass the
# path as $1 to skip the search when more than one adapter is plugged in.
if [ -n "$1" ]; then
    DEV="$1"
else
    shopt -s nullglob
    CANDIDATES=(/dev/cu.usbserial-* /dev/ttyUSB* /dev/ttyACM*)
    shopt -u nullglob
    DEV="${CANDIDATES[0]}"
fi

if [ -z "$DEV" ]; then
    echo "No serial device found. Plug the board in, or pass the path: $0 /dev/ttyUSB0" >&2
    exit 1
fi

if [ ! -e "$DEV" ]; then
    echo "$DEV does not exist." >&2
    exit 1
fi

# The device node exists for everyone on Linux but belongs to a group, so a
# fresh account gets "Permission denied" from screen with no explanation.
# macOS grants the console user access directly and never hits this.
if [ ! -r "$DEV" ] || [ ! -w "$DEV" ]; then
    echo "$DEV is not readable and writable by $(id -un)." >&2
    if [ "$(uname -s)" = Linux ]; then
        echo "Serial devices belong to a group (dialout on Debian/Ubuntu and" >&2
        echo "Fedora, uucp on Arch). Add yourself once:" >&2
        echo "  sudo usermod -aG \$(stat -c '%G' \"$DEV\") \$USER" >&2
        echo "then log out and back in for the new group to apply." >&2
    fi
    exit 1
fi

# A detached screen holds the port open and the next one fails on a busy
# device, which reads as a dead board rather than a stale session.
if command -v lsof >/dev/null && lsof "$DEV" >/dev/null 2>&1; then
    echo "$DEV is already open. Reattach with 'screen -r', or close it:" >&2
    lsof "$DEV" >&2
    exit 1
fi

echo "Attaching to $DEV at $BAUD. Detach with Ctrl-A d, quit with Ctrl-A k."
exec screen "$DEV" "$BAUD"
