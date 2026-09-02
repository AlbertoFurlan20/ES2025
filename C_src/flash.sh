#!/bin/bash
set -e

# ./out and the openocd board file are relative, and compile.sh is a sibling, so
# anchor to this script's directory rather than the caller's.
cd "$(dirname "$0")"

./compile.sh

# Same single source of truth compile.sh uses, so objcopy comes from the
# toolchain being built with rather than whatever is first on PATH.
ENV_FILE="../.env/setup.env"
: "${RTEMS_LOCAL_PATH:=$(cat "$ENV_FILE" 2>/dev/null | tr -d '\r' \
    | sed -n 's/^[[:space:]]*RTEMS_LOCAL_PATH[[:space:]]*=[[:space:]]*//p' \
    | tail -1 | tr -d '"')}"

OBJCOPY="$RTEMS_LOCAL_PATH/RTEMS_toolchain/rtems/bin/arm-rtems7-objcopy"

if [ ! -x "$OBJCOPY" ]; then
    echo "No RTEMS objcopy at $OBJCOPY" >&2
    exit 1
fi

# Fail before the build artefact is touched, and name the package: OpenOCD is
# spelled differently by every package manager.
if ! command -v openocd >/dev/null; then
    echo "openocd not found. Install it:" >&2
    echo "  macOS          brew install open-ocd" >&2
    echo "  Debian/Ubuntu  sudo apt install openocd" >&2
    echo "  Fedora         sudo dnf install openocd" >&2
    echo "  Arch           sudo pacman -S openocd" >&2
    exit 1
fi

# From .exe to .bin
"$OBJCOPY" -O binary ./out/compilation_output.exe ./out/compilation_output.bin

STATUS=0
openocd \
-f board/stm32f4discovery.cfg \
-c "program ./out/compilation_output.bin 0x08000000 verify reset exit" || STATUS=$?

# macOS lets any user claim the ST-Link; Linux does not until udev grants it,
# and the failure surfaces as a bare LIBUSB_ERROR_ACCESS that reads like broken
# hardware. Point at the actual fix instead of leaving that on screen.
if [ "$STATUS" -ne 0 ] && [ "$(uname -s)" = Linux ]; then
    echo >&2
    echo "If that was a permission or LIBUSB_ERROR_ACCESS failure, the ST-Link" >&2
    echo "needs udev rules (run once, then replug the board):" >&2
    echo "  sudo cp \"\$(dirname \"\$(command -v openocd)\")/../share/openocd/contrib/60-openocd.rules\" /etc/udev/rules.d/" >&2
    echo "  sudo udevadm control --reload-rules && sudo udevadm trigger" >&2
    echo "Distribution packages usually ship that file already; check" >&2
    echo "/usr/share/openocd/contrib/60-openocd.rules first." >&2
fi

exit "$STATUS"
