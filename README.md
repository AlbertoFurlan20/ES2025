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


On Linux, OpenOCD needs udev rules before it can claim the ST-Link, and your
user must be in the serial group (`dialout` on Debian/Ubuntu) to open the
console. Both scripts print the fix when they hit it.

---
## Running it end to end

Everything below assumes the hardware is on the desk. Steps 1-4 are once per
machine; 5 onwards is the loop.

### 0. What you need

STM32F407 Discovery, a BMP180 breakout, a CP2102 (or any 3.3 V USB-to-TTL
adapter), two USB cables. On the host: OpenOCD, `screen`, `python3`, and the
RTEMS toolchain built in step 2.

### 1. Wire it

| Signal | Discovery pin | Other end |
|--------|---------------|-----------|
| SCL | PB6 | BMP180 SCL |
| SDA | PB7 | BMP180 SDA |
| 3.3 V | 3V | BMP180 VIN |
| GND | GND | BMP180 GND |
| Console TX | PA2 | CP2102 RXD |
| Console RX | PA3 | CP2102 TXD |
| GND | GND | CP2102 GND |

I2C1 at 100 kHz, sensor address `0x77`; console USART2 at 115200 8N1. The
ST-Link is the board's own mini-USB and needs no wiring. Full notes:
[`B_docs/PIN_CONFIG.md`](B_docs/PIN_CONFIG.md).

### 2. Toolchain and BSP

Full instructions: [`B_docs/SETUP.md`](B_docs/SETUP.md). **One BSP option is not
optional**: the stock `arm/stm32f4` BSP puts the console on USART3, this project
uses USART2, so the BSP must be configured with

```ini
[arm/stm32f4]
BUILD_TESTS = True
STM32F4_ENABLE_USART_2 = True
STM32F4_ENABLE_USART_3 = False
```

before `./waf configure`. Check the installed
`.../arm-rtems7/stm32f4/lib/include/bspopts.h` afterwards: it must define
`STM32F4_ENABLE_USART_2`. A BSP built without it produces a board that flashes
and runs and prints nothing.

### 3. Point the build at the toolchain

```bash
cp .env/setup.env.example .env/setup.env
# edit: RTEMS_LOCAL_PATH=/path/to/where/RTEMS_toolchain/lives
```

### 4. Host-side analysis environment (optional, for step 7)

```bash
cd E_analysis
uv venv && uv pip install -r requirements.txt
```

The scripts look for `E_analysis/.venv` and fall back to the system `python3`.

### 5. Build and flash

```bash
cd C_src
./compile.sh          # or: make
make flash            # build + program at 0x08000000 over OpenOCD
```

### 6. Watch it run

```bash
./attach_to_device.sh          # 115200, finds the adapter itself
                               # detach: Ctrl-A then d
```

Press the black **B2** reset button. A healthy boot:

```
[SELFTEST] compensate: T=150 (exp 150)  P=69964 (exp 69964)  -> PASS
#BMP180 v1 fw=2.0.0 temp_ms=1000
[[DEBUG]] BMP180 registered on /dev/bmp180-0
S 1043221 244 96822 0
S 1054220 244 96825 0
```

- `[SELFTEST] ... PASS` - the Bosch compensation math checked against the
  datasheet worked example, before any hardware is touched. A `FAIL` here means
  the arithmetic is broken independently of wiring.
- `#BMP180 v1 ...` - session header: wire format version, firmware version,
  temperature cache interval.
- `S <t_us> <t_cdeg> <p_pa> <oss>` - one sample: timestamp, temperature in
  0.1 degC, pressure in Pa, oversampling mode.
- `E <t_us> <errno>` would be an acquisition error, `D <t_us> <lost>` a gap.
  A healthy run has neither.

On boot the firmware sweeps the four oversampling modes - 500 samples each after
3 discarded - then settles at `oss=2` and streams continuously.

### 7. Drive it from the console

Type into the same `screen` session. One command per line; nothing is echoed,
and anything unrecognised is ignored.

| Line | Effect |
|------|--------|
| `O0` | oversampling off, ~5 ms per sample, noisiest |
| `O1` | 2x, ~7 ms |
| `O2` | 4x, ~11 ms |
| `O3` | 8x, ~19 ms, quietest |
| `R` | soft reset; sensor returns to power-on defaults |

Confirmation is in the data, not in a reply: the last field of the following `S`
records changes to the mode they were actually taken at, and the interval
between them changes with it.

The first accepted command **cancels the boot sweep**, which steers `oss`
itself. A capture meant for analysis is therefore one where no command was
typed.

### 8. Capture and analyse a run

```bash
cd E_analysis
./capture.sh 150                    # seconds; resets the board over the ST-Link
                                    # writes captures/YYYYMMDD-HHMMSS.log
./capture.sh 150 /dev/ttyUSB0       # explicit device
NO_RESET=1 ./capture.sh 60          # capture a board already running
```

Then, on the log it wrote:

```bash
.venv/bin/python -c "
from bmp180_analysis.parse import parse_file
from bmp180_analysis import metrics
s = parse_file('captures/<file>.log')[-1]
print(metrics.per_block_summary(s))
"
```

`per_block_summary` gives, per sweep block, the sample count, the median
acquisition interval and the pressure noise. A good run reads about
5000/7000/11000/19000 us for `oss` 0 to 3, noise falling as `oss` rises, and no
`E` or `D` records at all.

To regenerate the report's figure set from a capture:

```bash
.venv/bin/python make_report_figures.py figures/out captures/<file>.log
```

### 9. Tests that need no board

```bash
make -C C_src/tests run     # telemetry formatter + seqlock snapshot, on the host
```

### 10. Going further

[`C_src/TESTING.md`](C_src/TESTING.md) is a ten-step validation ladder:
datasheet self-test, altitude response, absolute accuracy, the OSS sweep,
fault injection (pull SDA mid-run and watch one `E 5` appear while the console
stays alive), teardown and I2C bus recovery. Three build flags select the tests
a normal build must not carry: `-DBMP180_TEARDOWN_TEST`, `-DI2C_RECOVERY_TEST`,
`-DBMP180_NO_BUS_LOCK`.

### Troubleshooting

| Symptom | Cause |
|---------|-------|
| Board flashes, console silent | BSP built without `STM32F4_ENABLE_USART_2` (step 2), or TX/RX swapped |
| Garbage characters | wrong baud, or on macOS a `/dev/tty.*` node instead of `/dev/cu.*` |
| `E 5` immediately, no samples | sensor not answering on I2C1 - check PB6/PB7 and 3.3 V |
| Dead after a reset, stays dead | slave holding SDA. 2.0.0 recovers on the next boot; older builds need a power cycle |
| OpenOCD cannot claim the ST-Link (Linux) | udev rules missing; `./flash.sh` prints the fix |

