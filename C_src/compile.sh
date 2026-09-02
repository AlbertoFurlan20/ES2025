#!/bin/bash
set -e

# Toolchain location comes from one place only: ../.env/setup.env, a plain
# KEY=VALUE file that is gitignored and machine-local (copy
# .env/setup.env.example to create it). The root CMakeLists.txt, flash.sh and
# C_src/Makefile read the same file, so the four build paths cannot drift apart.
# Override for one command with the environment instead of editing it:
#     RTEMS_LOCAL_PATH=/somewhere/else ./compile.sh
ENV_FILE="$(dirname "$0")/../.env/setup.env"
: "${RTEMS_LOCAL_PATH:=$(cat "$ENV_FILE" 2>/dev/null | tr -d '\r' \
    | sed -n 's/^[[:space:]]*RTEMS_LOCAL_PATH[[:space:]]*=[[:space:]]*//p' \
    | tail -1 | tr -d '"')}"

if [ -z "$RTEMS_LOCAL_PATH" ]; then
    echo "Missing RTEMS_LOCAL_PATH. Copy .env/setup.env.example to .env/setup.env and set your toolchain path." >&2
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
