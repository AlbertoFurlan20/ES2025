# Toolchain location comes from one place only: ../.env/setup.env, a plain
# KEY=VALUE file that is gitignored and machine-local (copy
# .env/setup.env.example to create it). The root CMakeLists.txt, compile.sh and
# flash.sh read the same file, so the four build paths cannot drift apart.
# Override on the command line if you must:
#     make RTEMS_LOCAL_PATH=/somewhere/else
ENV_FILE         := $(abspath ../.env/setup.env)
RTEMS_LOCAL_PATH ?= $(shell sed -n 's/^[[:space:]]*RTEMS_LOCAL_PATH[[:space:]]*=[[:space:]]*//p' $(ENV_FILE) 2>/dev/null | tr -d '"\r' | tail -1)

ifeq ($(strip $(RTEMS_LOCAL_PATH)),)
$(error Missing RTEMS_LOCAL_PATH. Copy .env/setup.env.example to .env/setup.env and set your toolchain path)
endif

RTEMS_ROOT   = $(RTEMS_LOCAL_PATH)/RTEMS_toolchain/rtems
BSP_LIB      = $(RTEMS_ROOT)/7/arm-rtems7/stm32f4/lib
# Use g++ instead of gcc for C++ code
CXX          = $(RTEMS_ROOT)/bin/arm-rtems7-g++
OBJCOPY      = $(RTEMS_ROOT)/bin/arm-rtems7-objcopy

CXXFLAGS = \
    -mcpu=cortex-m4 \
    -mthumb \
    -mfloat-abi=hard \
    -mfpu=fpv4-sp-d16 \
    -O0 -g \
    -std=c++17 \
    -Wall -Wextra -Werror \
    -fno-exceptions \
    -fno-rtti \
    -B$(BSP_LIB)/ \
    -qrtems \
    -Iinc

# One folder per module under src/ and inc/, so both patterns are needed:
# init.cpp sits at the top, everything else one level down.
SRCS     = $(wildcard src/*.cpp src/*/*.cpp)
INCLUDES = $(wildcard inc/*.h inc/*/*.h)
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
