#!/bin/bash
set -e

./compile.sh

# Same single source of truth compile.sh uses, so objcopy comes from the
# toolchain being built with rather than whatever is first on PATH.
LOCAL_CMAKE="$(dirname "$0")/../local.cmake"
: "${RTEMS_LOCAL_PATH:=$(sed -n 's/^[[:space:]]*set(RTEMS_LOCAL_PATH[[:space:]]\{1,\}\([^)]*\)).*/\1/p' "$LOCAL_CMAKE" 2>/dev/null)}"

# From .exe to .bin
"$RTEMS_LOCAL_PATH/RTEMS_toolchain/rtems/bin/arm-rtems7-objcopy" \
    -O binary ./out/compilation_output.exe ./out/compilation_output.bin

openocd \
-f board/stm32f4discovery.cfg \
-c "program ./out/compilation_output.bin 0x08000000 verify reset exit"