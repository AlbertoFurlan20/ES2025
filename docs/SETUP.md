# Setup
Steps for building the RTEMS toolchain and BSP for STM32F4 Discovery on macOS.

## 0. Prerequisites
Will install all files in a directory in the home called RTEMS



```bash
cd
mkdir RTEMS
cd RTEMS
git clone https://gitlab.rtems.org/rtems/rtos/rtems.git
git clone https://gitlab.rtems.org/rtems/tools/rtems-source-builder.git rbs
```

## 1. Brew install texinfo 
- this specific to avoid the texinfo manual install that does the toolchain builder
```bash
brew install texinfo
```

## 2.  Build toolchain 32 bit version
```bash
export PATH="$(brew --prefix texinfo)/bin:$PATH"
cd ~/RTEMS/rbs
./source-builder/sb-set-builder --prefix=~/RTEMS/RTEMS_toolchain/rtems 7/rtems-arm
```

## 3. Add it to PATH
```bash
export PATH=~/RTEMS/RTEMS_toolchain/rtems/7/bin:$PATH
export PATH=~/RTEMS/RTEMS_toolchain/rtems/bin:$PATH
```

## 4. Verify the STM32F4 BSP
This lists the available BSPs - you should see `arm/stm32f4` in the output.
```bash
cd  ~/RTEMS/rtems
./waf bsplist | grep stm32f4
```

## 5. Create the config file
```bash
cd  ~/RTEMS/rtems
cat > config.ini << 'EOF'
[arm/stm32f4]
BUILD_TESTS = True
# BSP_CONSOLE_BAUD = 115200 -- This throws "Unknown configuration option: BSP_CONSOLE_BAUD" 
EOF
```

## 6. Configure, build, and install
```bash
cd  ~/RTEMS/rtems
./waf configure --prefix=~/RTEMS/RTEMS_toolchain/rtems/7
./waf
./waf install
```

## 7. Install openOCD
```bash
brew install openocd
```

## 8. Verify the STM32F4 Discovery config files are present:
```bash
ls $(brew --prefix openocd)/share/openocd/scripts/board/stm32f4discovery.cfg
ls $(brew --prefix openocd)/share/openocd/scripts/target/stm32f4x.cfg
```

## 9. Hello exe dumb test
```bash
mkdir -p ~/RTEMS/rtems_hello
cd ~/RTEMS/rtems_hello

cat > hello.c << 'EOF'
#include <rtems.h>
#include <rtems/bspIo.h>
#include <stdio.h>

rtems_task Init(rtems_task_argument ignored)
{
printf("*** Hello from RTEMS on STM32F407 ***\n");
rtems_task_suspend(RTEMS_SELF);
}

#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_CONSOLE_DRIVER
#define CONFIGURE_MAXIMUM_TASKS             1
#define CONFIGURE_RTEMS_INIT_TASKS_TABLE
#define CONFIGURE_INIT_TASK_ENTRY_POINT     Init
#define CONFIGURE_INIT
#include <rtems/confdefs.h>
EOF
```

## 10. Compile the hello world
```bash
cd ~/RTEMS/rtems_hello/
arm-rtems7-gcc \
-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 \
-O0 -g \
-B ~/RTEMS/RTEMS_toolchain/rtems/7/arm-rtems7/stm32f4/lib/ \
-qrtems \
hello.c -o hello.exe
```

## 11. Verify the ELF target is correct
```bash
cd ~/RTEMS/rtems_hello/
arm-rtems7-objdump -f hello.exe | head -5
```

## 12. Convert to binary and flash
```bash
cd ~/RTEMS/rtems_hello/
arm-rtems7-objcopy -O binary hello.exe hello.bin
```

## 13. Verify the binary is sane (should be a few hundred KB max)
```bash
cd ~/RTEMS/rtems_hello/
ls -lh hello.bin
xxd hello.bin | head -4   # first bytes should NOT be all-zeros or 0xFF
```

## 14. Now flash with OpenOCD
```bash
openocd \
-f board/stm32f4discovery.cfg \
-c "program hello.bin 0x08000000 verify reset exit"
```