RTEMS_PREFIX = /Users/albertofurlan/Developer/PoliMi/RTEMS_toolchain/rtems/7
RTEMS_TOOL_PREFIX = /Users/albertofurlan/Developer/PoliMi/RTEMS_toolchain/rtems
BSP_LIB      = $(RTEMS_PREFIX)/arm-rtems7/stm32f4/lib
# Use g++ instead of gcc for C++ code
CXX          = $(RTEMS_TOOL_PREFIX)/bin/arm-rtems7-g++
OBJCOPY      = $(RTEMS_TOOL_PREFIX)/bin/arm-rtems7-objcopy

CXXFLAGS = \
    -mcpu=cortex-m4 \
    -mthumb \
    -mfloat-abi=hard \
    -mfpu=fpv4-sp-d16 \
    -O0 -g \
    -B$(BSP_LIB)/ \
    -qrtems \
    -Iinc

SRCS   = src/*.cpp
INCLUDES = inc/*.h
$(warning "Single compile, so do not `include` lib files, use fwd declarations instead")

TARGET = sensor.exe

all: $(TARGET)

$(TARGET): $(SRCS) $(INCLUDES)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $@

sensor.bin: $(TARGET)
	$(OBJCOPY) -O binary $< $@

flash: sensor.bin
	openocd \
	    -f board/stm32f4discovery.cfg \
	    -c "program sensor.bin 0x08000000 verify reset exit"

clean:
	rm -f *.exe *.bin *.o
