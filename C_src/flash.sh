#!/bin/bash
set -e

./compile.sh

# Same single source of truth compile.sh uses, so objcopy comes from the
# toolchain being built with rather than whatever is first on PATH.
ENV_FILE="$(dirname "$0")/../.env/setup.env"
: "${RTEMS_LOCAL_PATH:=$(cat "$ENV_FILE" 2>/dev/null | tr -d '\r' \
    | sed -n 's/^[[:space:]]*RTEMS_LOCAL_PATH[[:space:]]*=[[:space:]]*//p' \
    | tail -1 | tr -d '"')}"

# From .exe to .bin
"$RTEMS_LOCAL_PATH/RTEMS_toolchain/rtems/bin/arm-rtems7-objcopy" \
    -O binary ./out/compilation_output.exe ./out/compilation_output.bin

openocd \
-f board/stm32f4discovery.cfg \
-c "program ./out/compilation_output.bin 0x08000000 verify reset exit"