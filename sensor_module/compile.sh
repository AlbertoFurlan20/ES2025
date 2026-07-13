#!/bin/bash
set -e

# Sources environment variables for ALL child processes
set -a
. ../.env/setup.env
set +a

# Clean and recreate /out
rm -rf ./out
mkdir ./out

arm-rtems7-g++ \
   -mcpu=cortex-m4 -mthumb \
   -mfloat-abi=hard -mfpu=fpv4-sp-d16 \
   -O0 -g -B "$TARGET_DIR/RTEMS_toolchain/rtems/7/arm-rtems7/stm32f4/lib/" \
   -qrtems \
   -Iinc \
   src/*.cpp -o ./out/compilation_output.exe
