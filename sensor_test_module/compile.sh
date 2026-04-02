rm compilation_output.exe | true

arm-rtems7-g++ \
   -mcpu=cortex-m4 -mthumb \
   -mfloat-abi=hard -mfpu=fpv4-sp-d16 \
   -O0 -g -B/Users/albertofurlan/Developer/PoliMi/RTEMS_toolchain/rtems/7/arm-rtems7/stm32f4/lib/ \
   -qrtems \
   -Iinc \
   src/*.cpp -o compilation_output.exe
