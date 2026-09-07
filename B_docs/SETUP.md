# Setup
Steps for building the RTEMS toolchain and BSP for STM32F4 Discovery, on macOS
and Linux. Everything lands in `~/RTEMS`; the only per-platform differences are
the package manager in steps 1, 7 and 8.

Every step below uses `$RTEMS_LOCAL_PATH`, set in step 0. It is a plain shell
variable, so re-export it if you open a new terminal partway through:

```bash
export RTEMS_LOCAL_PATH="$HOME/RTEMS"
```

## 0. Prerequisites
Will install all files in a directory in the home called RTEMS

```bash
export RTEMS_LOCAL_PATH="$HOME/RTEMS"
mkdir -p "$RTEMS_LOCAL_PATH"
cd "$RTEMS_LOCAL_PATH"
git clone https://gitlab.rtems.org/rtems/rtos/rtems.git
git clone https://gitlab.rtems.org/rtems/tools/rtems-source-builder.git rbs
```

## 1. Host packages
The lists come from the [RTEMS user manual](https://docs.rtems.org/docs/main/user/hosts/posix.html).

**macOS** — Xcode command line tools, then texinfo from brew. Installing it
here is specific to avoiding the texinfo manual install that the toolchain
builder would otherwise do itself.
```bash
xcode-select --install
brew install texinfo
```

**Debian/Ubuntu**
```bash
sudo apt install build-essential g++ gdb unzip pax bison flex texinfo \
  python3-dev python-is-python3 libncurses-dev zlib1g-dev \
  ninja-build pkg-config
```

**Fedora**
```bash
sudo dnf install ncurses-devel python3-devel git bison gcc gcc-c++ \
  flex texinfo patch perl-Text-ParseWords zlib-devel
```

**Arch**
```bash
sudo pacman -S base-devel gdb xz unzip ncurses git zlib
```

## 2.  Build toolchain 32 bit version
On macOS the brew texinfo must come first on PATH; on Linux the distribution
one is already fine and the export is a no-op.
```bash
command -v brew >/dev/null && export PATH="$(brew --prefix texinfo)/bin:$PATH"
cd "$RTEMS_LOCAL_PATH/rbs"
./source-builder/sb-set-builder \
  --prefix="$RTEMS_LOCAL_PATH/RTEMS_toolchain/rtems" 7/rtems-arm
```
> Write the prefix as a variable or `"$HOME/..."`, never `--prefix=~/...`: a
> tilde is only expanded at the start of a word, so that form installs the
> toolchain into a directory literally named `~`.

## 3. Add it to PATH
Needed for the **rest of this guide only** — `waf` in steps 4 to 6 locates the
cross compiler through PATH, and the hello-world test in steps 10 to 12 calls
`arm-rtems7-*` by bare name.
```bash
export PATH="$RTEMS_LOCAL_PATH/RTEMS_toolchain/rtems/7/bin:$PATH"
export PATH="$RTEMS_LOCAL_PATH/RTEMS_toolchain/rtems/bin:$PATH"
```

The repository itself does **not** read PATH. `CMakeLists.txt`,
`C_src/Makefile`, `C_src/compile.sh` and `C_src/flash.sh` each build the
absolute tool paths from `RTEMS_LOCAL_PATH` instead, so that one value is all
they need — and it is the same directory exported above, the parent of
`RTEMS_toolchain/`. Record it once, from the repo root:
```bash
cp .env/setup.env.example .env/setup.env
printf 'RTEMS_LOCAL_PATH=%s\n' "$RTEMS_LOCAL_PATH" >> .env/setup.env
```
The example file ships a sample assignment; the appended line wins, because all
four readers take the last one. `.env/setup.env` is gitignored, so this stays
machine-local. Nothing here has to go in your shell profile.

## 4. Verify the STM32F4 BSP
This lists the available BSPs - you should see `arm/stm32f4` in the output.
```bash
cd "$RTEMS_LOCAL_PATH/rtems"
./waf bsplist | grep stm32f4
```

## 5. Create the config file
```bash
cd "$RTEMS_LOCAL_PATH/rtems"
cat > config.ini << 'EOF'
[arm/stm32f4]
BUILD_TESTS = True
# BSP_CONSOLE_BAUD = 115200 -- This throws "Unknown configuration option: BSP_CONSOLE_BAUD" 
EOF
```

## 6. Configure, build, and install
```bash
cd "$RTEMS_LOCAL_PATH/rtems"
./waf configure --prefix="$RTEMS_LOCAL_PATH/RTEMS_toolchain/rtems/7"
./waf
./waf install
```

## 7. Install openOCD
```bash
brew install openocd                 # macOS
sudo apt install openocd             # Debian/Ubuntu
sudo dnf install openocd             # Fedora
sudo pacman -S openocd               # Arch
```

On Linux, OpenOCD also needs udev rules before a non-root user can claim the
ST-Link. Most packages install them; if flashing later fails with
`LIBUSB_ERROR_ACCESS`:
```bash
sudo cp /usr/share/openocd/contrib/60-openocd.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```
Then replug the board.

## 8. Verify the STM32F4 Discovery config files are present:
Resolved from the installed binary, so this works under `/usr` and under a brew
prefix alike.
```bash
OPENOCD_SCRIPTS="$(dirname "$(command -v openocd)")/../share/openocd/scripts"
ls "$OPENOCD_SCRIPTS/board/stm32f4discovery.cfg"
ls "$OPENOCD_SCRIPTS/target/stm32f4x.cfg"
```

## 9. Hello exe dumb test
```bash
mkdir -p "$RTEMS_LOCAL_PATH/rtems_hello"
cd "$RTEMS_LOCAL_PATH/rtems_hello"

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
cd "$RTEMS_LOCAL_PATH/rtems_hello"
arm-rtems7-gcc \
-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 \
-O0 -g \
-B "$RTEMS_LOCAL_PATH/RTEMS_toolchain/rtems/7/arm-rtems7/stm32f4/lib/" \
-qrtems \
hello.c -o hello.exe
```

## 11. Verify the ELF target is correct
```bash
cd "$RTEMS_LOCAL_PATH/rtems_hello"
arm-rtems7-objdump -f hello.exe | head -5
```

## 12. Convert to binary and flash
```bash
cd "$RTEMS_LOCAL_PATH/rtems_hello"
arm-rtems7-objcopy -O binary hello.exe hello.bin
```

## 13. Verify the binary is sane (should be a few hundred KB max)
```bash
cd "$RTEMS_LOCAL_PATH/rtems_hello"
ls -lh hello.bin
xxd hello.bin | head -4   # first bytes should NOT be all-zeros or 0xFF
```

## 14. Now flash with OpenOCD
```bash
openocd \
-f board/stm32f4discovery.cfg \
-c "program hello.bin 0x08000000 verify reset exit"
```
