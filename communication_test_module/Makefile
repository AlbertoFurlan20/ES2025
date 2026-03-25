RTEMS_PREFIX = /Users/albertofurlan/Developer/PoliMi/RTEMS_toolchain/rtems/7
BSP_LIB      = $(RTEMS_PREFIX)/arm-rtems7/stm32f4/lib
CC           = $(RTEMS_PREFIX)/bin/arm-rtems7-gcc
OBJCOPY      = $(RTEMS_PREFIX)/bin/arm-rtems7-objcopy

CFLAGS = \
    -mcpu=cortex-m4 \
    -mthumb \
    -mfloat-abi=hard \
    -mfpu=fpv4-sp-d16 \
    -O0 -g \
    -B$(BSP_LIB)/ \
    -qrtems

SRCS   = init.c alive.c
TARGET = alive.exe

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $^ -o $@

alive.bin: $(TARGET)
	$(OBJCOPY) -O binary $< $@

flash: alive.bin
	openocd \
	    -f board/stm32f4discovery.cfg \
	    -c "program alive.bin 0x08000000 verify reset exit"

clean:
	rm -f *.exe *.bin *.o