#!/bin/bash
set -e

./compile.sh

# From .exe to .bin
arm-rtems7-objcopy -O binary ./out/compilation_output.exe ./out/compilation_output.bin

openocd \
-f board/stm32f4discovery.cfg \
-c "program ./out/compilation_output.bin 0x08000000 verify reset exit"