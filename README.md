# ES2025

Official repo for the 2025/2026 edition of the Embedded Systems and Advanced
Operating Systems courses.

Project: an RTEMS I2C driver for the Bosch BMP180 pressure sensor, running on
the STM32F407 Discovery.

## Layout

| Folder | What it is |
|--------|------------|
| [`A_report/`](A_report/README.md) | LaTeX report for the submission. `make report.pdf`. |
| `B_docs/` | [Assignment](B_docs/ASSIGMENT.md), [pin configuration](B_docs/PIN_CONFIG.md), [toolchain setup](B_docs/SETUP.md). |
| [`C_src/`](C_src/README.md) | The firmware. Driver, I2C master, tasks. Testing guide in [`TESTING.md`](C_src/TESTING.md). |
| `E_analysis/` | Host-side Python tooling. `capture.sh` records a telemetry run off the serial port; `bmp180_analysis/` parses it into metrics and figures. |

---
## Setup

Once per machine:

```bash
cp .env/setup.env.example .env/setup.env
# then edit RTEMS_LOCAL_PATH to point at your toolchain
```

`.env/setup.env` is gitignored and is the only place the toolchain path is
written — the root `CMakeLists.txt`, `C_src/Makefile`, `compile.sh` and
`flash.sh` all read it, so they cannot drift apart. The toolchain is expected at
`$RTEMS_LOCAL_PATH/RTEMS_toolchain/rtems`; building it is
[`B_docs/SETUP.md`](B_docs/SETUP.md).

Override for a single command with the environment instead of editing the file:
```bash
RTEMS_LOCAL_PATH=/somewhere/else ./compile.sh
```

---
## Source Code @ C_src/

```
inc/    headers, one folder per module (only inc/ is on the include path)
src/    sources, mirroring inc/
tests/  host-side unit tests
out/    build output (gitignored)
```

Module breakdown and architecture: [`C_src/README.md`](C_src/README.md).

### Scripts

Run from `C_src/`, or from anywhere by path. macOS and Linux.

| Script | What it does |
|--------|--------------|
| `./compile.sh` | Cross-compiles `src/` into `out/compilation_output.exe`. |
| `./flash.sh` | Runs `compile.sh`, converts to `.bin`, programs the board at `0x08000000` over OpenOCD. |
| `./attach_to_device.sh [dev]` | Serial console at 115200 via `screen`. Finds the adapter itself; pass a path when more than one is plugged in. Detach with `Ctrl-A d`. |

`make` and `make flash` in `C_src/` are equivalent to the first two; the root
`CMakeLists.txt` exists for IDE builds.

### Console commands

The board reads commands on the same serial link it streams telemetry over, one
per line. Anything else is ignored, and nothing is echoed back.

| Line | Effect |
|------|--------|
| `O0`..`O3` | switch oversampling mode (ultra low power .. ultra high resolution) |
| `R` | soft reset; the sensor returns to power-on defaults |

Confirmation arrives as the `oss` field of the following samples. The first
command cancels the boot oversampling sweep, so a capture meant for analysis is
one where no command was typed. Details in
[`C_src/README.md`](C_src/README.md).

On Linux, OpenOCD needs udev rules before it can claim the ST-Link, and your
user must be in the serial group (`dialout` on Debian/Ubuntu) to open the
console. Both scripts print the fix when they hit it.
