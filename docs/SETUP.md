# Setup
Steps for building the RTEMS toolchain and BSP for STM32F4 Discovery on macOS.

## 0. Set Target Env
This is where the toolchain install and all the required tools will live
```
- Albi: /Users/albertofurlan/Developer/PoliMi
- Tom: /Volumes/POLI/tools
```
```bash
export TARGET_DIR=<replace_with_yours>
```

## 0.5 Prerequisites
```bash
git clone https://gitlab.rtems.org/rtems/rtos/rtems.git $TARGET_DIR/rtems
git clone https://gitlab.rtems.org/rtems/tools/rtems-source-builder.git $TARGET_DIR/rtems-source-builder
```

## 1. Brew install texinfo 
- this specific to avoid the texinfo manual install that does the toolchain builder
```bash
brew install texinfo
```

## 2.  Build toolchain 32 bit version
At the root of the cloned repo `rtems-source-builder` run the commands
```bash
export PATH="$(brew --prefix texinfo)/bin:$PATH"
$TARGET_DIR/rtems-source-builder/source-builder/sb-set-builder --prefix=$TARGET_DIR/RTEMS_toolchain/rtems 7/rtems-arm
```

## 3. Add it to PATH
```bash
cat >> ~/.bash_profile << EOF

# RTEMS Toolchain                                                                          
export PATH="$TARGET_DIR/RTEMS_toolchain/rtems/7/bin:\$PATH"
export PATH="$TARGET_DIR/RTEMS_toolchain/rtems/bin:\$PATH"
EOF
source ~/.bash_profile
```

## 4. Verify the STM32F4 BSP
This lists the available BSPs - you should see `arm/stm32f4` in the output.
```bash
cd $TARGET_DIR/rtems
./waf bsplist | grep stm32f4
```

## 5. Create the config file
```bash
cat > config.ini << 'EOF'
[arm/stm32f4]
BUILD_TESTS = True
# BSP_CONSOLE_BAUD = 115200 -- This throws "Unknown configuration option: BSP_CONSOLE_BAUD"
STM32F4_ENABLE_USART_2 = True
STM32F4_ENABLE_USART_3 = False
EOF
```
Console UART wired to PA2 (TX) / PA3 (RX) = USART2 (see `PIN_CONFIG.md`). Without this override the BSP defaults to USART3, and console output silently goes nowhere.

## 6. Configure, build, and install
```bash
./waf configure --prefix=$TARGET_DIR/RTEMS_toolchain/rtems/7
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
mkdir -p ~/Desktop/rtems_hello && cd ~/Desktop/rtems_hello

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
arm-rtems7-gcc \
-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 \
-O0 -g \
-B$TARGET_DIR/RTEMS_toolchain/rtems/7/arm-rtems7/stm32f4/lib/ \
-qrtems \
hello.c -o hello.exe
```

## 11. Verify the ELF target is correct
```bash
arm-rtems7-objdump -f hello.exe | head -5
```

## 12. Convert to binary and flash
```bash
arm-rtems7-objcopy -O binary hello.exe hello.bin
```

## 13. Verify the binary is sane (should be a few hundred KB max)
```bash
ls -lh hello.bin
xxd hello.bin | head -4   # first bytes should NOT be all-zeros or 0xFF
```

## 14. Now flash with OpenOCD
```bash
openocd \
-f board/stm32f4discovery.cfg \
-c "program hello.bin 0x08000000 verify reset exit"
```