#!/bin/bash
set -e

source ./.env/build_env_setup.sh

rm -f compilation_output.exe

arm-rtems7-g++ \
   -mcpu=cortex-m4 -mthumb \
   -mfloat-abi=hard -mfpu=fpv4-sp-d16 \
   -O0 -g -B "$TARGET_DIR/RTEMS_toolchain/rtems/7/arm-rtems7/stm32f4/lib/" \
   -qrtems \
   -Iinc \
   src/*.cpp -o compilation_output.exe
