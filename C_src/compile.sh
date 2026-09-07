#!/bin/bash
set -e

# -Iinc, src/*.cpp and ./out are all relative, so the script only ever worked
# when invoked from C_src. Anchor to its own directory instead of trusting the
# caller's, which makes ../flash.sh and editor build buttons work too.
cd "$(dirname "$0")"

# Toolchain location comes from one place only: ../.env/setup.env, a plain
# KEY=VALUE file that is gitignored and machine-local (copy
# .env/setup.env.example to create it). The root CMakeLists.txt, flash.sh and
# C_src/Makefile read the same file, so the four build paths cannot drift apart.
# Override for one command with the environment instead of editing it:
#     RTEMS_LOCAL_PATH=/somewhere/else ./compile.sh
ENV_FILE="../.env/setup.env"
: "${RTEMS_LOCAL_PATH:=$(cat "$ENV_FILE" 2>/dev/null | tr -d '\r' \
    | sed -n 's/^[[:space:]]*RTEMS_LOCAL_PATH[[:space:]]*=[[:space:]]*//p' \
    | tail -1 | tr -d '"')}"

if [ -z "$RTEMS_LOCAL_PATH" ]; then
    echo "Missing RTEMS_LOCAL_PATH. Copy .env/setup.env.example to .env/setup.env and set your toolchain path." >&2
    exit 1
fi

RTEMS_ROOT="$RTEMS_LOCAL_PATH/RTEMS_toolchain/rtems"
CXX="$RTEMS_ROOT/bin/arm-rtems7-g++"

# The toolchain is built per host, so a path that is valid on one machine points
# at nothing (or at a binary for the wrong OS) on another. Say which path failed
# rather than letting the shell report a bare "No such file or directory".
if [ ! -x "$CXX" ]; then
    echo "No RTEMS compiler at $CXX" >&2
    echo "RTEMS_LOCAL_PATH is '$RTEMS_LOCAL_PATH'. Point it at a toolchain built for this host (a macOS build does not run on Linux, and vice versa)." >&2
    exit 1
fi

# Unmatched globs are passed through literally by default and reach the compiler
# as a missing filename, so a subdirectory holding only headers would break the
# build. nullglob drops them instead.
shopt -s nullglob
SOURCES=(src/*.cpp src/*/*.cpp)
shopt -u nullglob

if [ ${#SOURCES[@]} -eq 0 ]; then
    echo "No .cpp files found under src/." >&2
    exit 1
fi

# Clean and recreate /out
rm -rf ./out
mkdir ./out

"$CXX" \
   -mcpu=cortex-m4 -mthumb \
   -mfloat-abi=hard -mfpu=fpv4-sp-d16 \
   -O0 -g \
   -std=c++17 \
   -Wall -Wextra -Werror \
   -fno-exceptions -fno-rtti \
   -B "$RTEMS_ROOT/7/arm-rtems7/stm32f4/lib/" \
   -qrtems \
   -Iinc \
   "${SOURCES[@]}" -o ./out/compilation_output.exe
