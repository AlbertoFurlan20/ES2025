#!/bin/bash
set -e

# Toolchain location comes from one place only: ../local.cmake, which is
# gitignored and machine-local (copy local.cmake.example to create it). The root
# CMakeLists.txt and C_src/Makefile read the same file, so the three build paths
# can no longer drift apart. Override with the environment if you must:
#     RTEMS_LOCAL_PATH=/somewhere/else ./compile.sh
LOCAL_CMAKE="$(dirname "$0")/../local.cmake"
: "${RTEMS_LOCAL_PATH:=$(sed -n 's/^[[:space:]]*set(RTEMS_LOCAL_PATH[[:space:]]\{1,\}\([^)]*\)).*/\1/p' "$LOCAL_CMAKE" 2>/dev/null)}"

if [ -z "$RTEMS_LOCAL_PATH" ]; then
    echo "Missing RTEMS_LOCAL_PATH. Copy local.cmake.example to local.cmake and set your toolchain path." >&2
    exit 1
fi

RTEMS_ROOT="$RTEMS_LOCAL_PATH/RTEMS_toolchain/rtems"

# Clean and recreate /out
rm -rf ./out
mkdir ./out

"$RTEMS_ROOT/bin/arm-rtems7-g++" \
   -mcpu=cortex-m4 -mthumb \
   -mfloat-abi=hard -mfpu=fpv4-sp-d16 \
   -O0 -g \
   -std=c++17 \
   -Wall -Wextra -Werror \
   -fno-exceptions -fno-rtti \
   -B "$RTEMS_ROOT/7/arm-rtems7/stm32f4/lib/" \
   -qrtems \
   -Iinc \
   src/*.cpp src/*/*.cpp -o ./out/compilation_output.exe
