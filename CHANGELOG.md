# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.1.0] - 2026-08-04

Instrumentation release. Adds a structured measurement stream and host-side
analysis tooling, producing the reference dataset that later remediation is
measured against.

**The measurement path is unchanged.** `bmp180_do_measurement`,
`bmp180_read_ut`, `bmp180_read_up`, `bmp180_compensate`, all of `i2c1.cpp`, and
every timing constant in `bmp_regs.h` are byte-for-byte as in 1.0.0. Only
reporting was replaced. This was deliberate: the point of the release is to
measure 1.0.0 honestly, not to improve it.

### Added

- **Telemetry wire format, frozen at `v1`** — line-oriented text over the
  existing USART2 console. `S` samples, `E` errors, `D` dropped-record markers,
  plus a session header carrying schema, firmware and temperature interval.
  Stability contract: a tag never changes meaning, new fields append only at
  end-of-line, and parsers ignore unknown tags and unknown trailing fields.
- **Lock-free SPSC ring buffer** (`C_src/inc/telem_ring.h`) decoupling
  acquisition from emission. The acquisition task timestamps and pushes in O(1)
  and never blocks; a lower-priority emitter task formats and writes. On
  overflow the *new* record is dropped and counted — queued records are never
  overwritten, so surviving data stays contiguous in time. Deliberately free of
  RTEMS dependencies so it is unit-testable on the host.
- **Integer formatters** (`C_src/inc/telem_fmt.h`). The emitter uses no
  `printf`: newlib-nano configurations silently mis-format `%llu`, and 64-bit
  microsecond timestamps need to be exact. Each line is assembled into a buffer
  and issued as a single `write()`.
- **Emitter task** (`C_src/src/telemetry.cpp`) with drop reporting. Timestamps
  come from `rtems_clock_get_uptime_nanoseconds()`, which reads the hardware
  timecounter and so resolves finer than the 1 ms tick.
- **Host unit tests** (`C_src/tests/`) for the ring and formatters, compiled
  with the host compiler under `-Wall -Wextra`. Covers FIFO order, wraparound,
  overflow accounting, and the `INT32_MIN` negation trap.
- **Python analysis package** (`E_analysis/`) split three ways so metrics are
  testable without hardware or a display: `parse.py` (text → DataFrames),
  `metrics.py` (DataFrames → numbers), `plot.py` (numbers → charts). Derives
  achieved read frequency, jitter, RMS pressure noise against the datasheet
  Table 3 reference, altitude via datasheet §3.6, temperature drift and loss
  accounting. 24 tests.
- **`metrics.per_block_summary()`** — one row per *contiguous* acquisition block,
  each with its wall-clock span. Required for any mode-to-mode comparison: the
  standard capture profile visits `oss=2` twice, and the two visits differ in
  duration by a factor of eleven, so pooling them silently mixes atmospheric
  drift into sensor noise.
- **Serial capture tooling** (`E_analysis/capture.py`, `capture.sh`) which
  pulses the target reset over the ST-Link, so a run begins at the session
  header without pressing the board's B2 button.

### Changed

- `bmp180_oss_sweep_task` replaced by `bmp180_telemetry_task`, which emits raw
  records instead of computing statistics on the device. Mean, standard
  deviation, peak-to-peak and achieved rate are now derived host-side, where
  they can be recomputed without reflashing and where the derivation is itself
  under test.
- `setupTask` takes an explicit stack size. Both new tasks get 4 KB rather than
  `RTEMS_MINIMUM_STACK_SIZE`; acquisition runs at priority 2, above the emitter
  at 5, so the console can never delay a measurement.
- `CONFIGURE_MAXIMUM_TASKS` raised from 4 to 6.
- `C_src/Makefile` toolchain paths no longer hardcode one developer's home
  directory.

### Removed

- `isqrt32` and the on-device statistics it supported, superseded by
  `E_analysis/metrics.py`.

### Fixed

- `E_analysis/capture.sh` originally used `stty -f` followed by `cat`. On macOS,
  termios settings applied to a `/dev/cu.*` node are reset when the port is
  opened, so captures recorded framing garbage at the driver's default baud. It
  also used `timeout(1)`, which macOS does not ship. Both replaced by
  `capture.py`, which applies termios to an already-open descriptor.
- `metrics.intervals_us` filtered samples by oversampling *value* rather than by
  contiguous *run*. Because `oss=2` appears both in the sweep and in the
  continuous phase, it took an interval straight across the intervening `oss=3`
  block: reported jitter for that mode was 206533 µs against a true 0.43 µs.
- The same pooling flaw affected **noise**, not just intervals — a separate
  function, missed when the first was fixed. `pressure_noise` merged the 10 s
  sweep block at `oss=2` with the 111 s continuous block, letting real
  atmospheric drift inflate that one mode: 4.71 Pa pooled against 5.53 Pa for the
  comparable sweep block, which made the mode look *better* than it was and
  partly masked the defect below. `pressure_noise` now documents that it pools;
  `per_block_summary` is the function to use for comparisons.

### Findings

The baseline confirms B1 is real, reproducible, and **intermittent**.

Each oversampling step should cut RMS pressure noise by about 1.0 Pa. Measured
per transition against a standard error of `s/sqrt(2(n-1))` ≈ 0.17 Pa at
n = 500, **each run has exactly one transition that fails at ~5.4 σ — and it is a
different transition each run**: run 1 at 0→1 (+0.393 Pa observed, z = +5.65),
run 2 at 1→2 (+0.146 Pa, z = +5.39). The transitions that are *not* spoiled match
the datasheet closely (run 2: z = +0.22 and +0.71), so the sensor is capable of
meeting specification and something intermittently prevents it.

That a different mode is spoiled each run is the discriminating evidence. A
systematic cause — miswired pull-up, wrong control byte, arithmetic error — would
degrade the same mode every time; only a stochastic cause moves. Two alternatives
are excluded by the same dataset: inter-sample jitter below 0.5 µs with
bit-identical median intervals across runs rules out the scheduler, and zero
dropped records across 15000+ samples rules out telemetry back-pressure.

One result is recorded as **unexplained**: OSS3 sits about 1 Pa above the
datasheet figure in both runs with little variation between them. That
consistency is unlike the intermittent signature and is more likely a separate
environmental noise floor. It is deliberately not folded into the timing
argument, and should be re-examined once the timing fix lands.

Two new issues were recorded during implementation: `temperature_cdeg` is a
misnomer holding deci-degrees (I17b), and no target build pins `-std=` (I17c).

## [1.0.0] - 2026-08-03

First complete release: a working RTEMS 7 device driver for the Bosch BMP180
digital pressure sensor on the STM32F4-Discovery board, including the hardware
I2C bus driver it sits on top of.

Reference: BST-BMP180-DS000-09 Rev 2.5 (April 2013), ST RM0090.

### Added

#### BMP180 sensor driver (`C_src/src/bmp180.cpp`, `C_src/inc/bmp*.h`)

- Device driver plugged into the RTEMS generic I2C framework (`<dev/i2c/i2c.h>`).
  `bmp180_dev_t` embeds `i2c_dev` as its first member so the framework's upcast
  from the device node to the driver context is well defined.
- `bmp180_register(bus_path, dev_path, oss)` — allocates and initialises the
  device on a given I2C bus, installs the `ioctl`/`destroy` handlers, verifies
  the chip by reading register `0xD0` against the expected `0x55`, and publishes
  the node (e.g. `/dev/bmp180-0`). Returns
  `std::pair<rtems_status_code, bmp180_dev_t*>` so callers get both an RTEMS
  status and the device context.
- Calibration handling: reads the 22-byte EEPROM block at `0xAA`, parses it into
  the 11 signed/unsigned 16-bit Bosch coefficients (AC1–AC6, B1, B2, MB, MC, MD),
  and rejects the block with `EIO` if any word reads back as `0x0000` or `0xFFFF`
  (the datasheet's "invalid coefficient" markers). Coefficients are cached in the
  device context behind a `calib_loaded` flag and reloaded lazily on first use.
- Uncompensated readings:
  - `bmp180_read_ut` — writes `0x2E` to `ctrl_meas`, waits the conversion time,
    reads the 16-bit result from `0xF6`/`0xF7`.
  - `bmp180_read_up` — writes the OSS-dependent control byte
    (`0x34`/`0x74`/`0xB4`/`0xF4`), waits the matching conversion time, reads the
    24-bit result from `0xF6`/`0xF7`/`0xF8` and shifts it right by `8 - oss`.
- `bmp180_compensate` — the Bosch compensation algorithm from datasheet §3.5
  implemented with integer arithmetic only. No floating point, no FPU or
  soft-float dependency, so it is usable from any task context. Produces
  temperature in units of 0.1 °C and pressure in Pa.
- `bmp180_do_measurement` — full acquisition cycle: lazy calibration load, raw
  temperature read, raw pressure read, compensation, results written into a
  caller-supplied `bmp180_measurement_t`.
- Four oversampling modes exposed as `bmp180_oss_t`
  (`ULTRA_LOW_POWER`, `STANDARD`, `HIGH_RESOLUTION`, `ULTRA_HIGH_RES`) with the
  per-mode conversion delays from the datasheet (5 / 8 / 14 / 26 ms) applied
  through `rtems_task_wake_after`, so the driver yields the CPU while the sensor
  converts instead of busy-waiting.
- `ioctl` interface on the device node (`C_src/inc/bmp180_ioctls.h`):
  - `BMP180_IOCTL_READ_MEASUREMENT` — trigger a full cycle, return compensated
    temperature and pressure.
  - `BMP180_IOCTL_SET_OSS` — change the oversampling mode, with range validation.
  - `BMP180_IOCTL_GET_OSS` — read back the configured mode.
  - `BMP180_IOCTL_SOFT_RESET` — write `0xB6` to `0xE0`, invalidate the cached
    calibration and wait out the 10 ms power-on-reset settling window.
  All handlers return negative `errno` values (`-EINVAL`, `-EIO`, `-ENOTTY`), the
  convention the RTEMS I2C framework converts into `errno` + `-1` for userland.
- `bmp180_selftest()` — hardware-independent validation of the compensation math
  against the datasheet worked example (`UT=27898`, `UP=23843`, `oss=0`, expecting
  `T=150`, `P=69964`). Runs at boot and prints PASS/FAIL, separating math bugs
  from wiring and bus problems.
- Register map, conversion times, chip-ID and soft-reset constants factored into
  `C_src/inc/bmp_regs.h`; data types into `C_src/inc/bmp_types.h`.

#### STM32F4 I2C1 bus driver (`C_src/src/i2c1.cpp`, `C_src/inc/i2c1.h`)

- Custom polled I2C master for the STM32F4 BSP, written because the BSP ships no
  driver for the modern `<dev/i2c/i2c.h>` framework — only the legacy
  `stm32f4_i2c` API, which is disabled and whose F4 GPIO mux is
  `#error Not implemented`.
- `stm32f4_register_i2c1(bus_path)` — enables the GPIOB and I2C1 clocks,
  configures PB6 (SCL) and PB7 (SDA) as AF4, open-drain, 50 MHz, internal
  pull-up, software-resets the peripheral, programs the timing registers and
  registers the bus (e.g. `/dev/i2c-1`).
- `transfer` implements the three distinct reception sequences required by
  RM0090 §27.3.3 — single byte, two bytes via the POS method, and N > 2 with the
  BTF-based handling of the final three bytes — plus repeated-START between
  messages, which is what makes register reads work.
- Standard-mode 100 kHz clock setup derived from `STM32F4_PCLK1`, with `CCR` and
  `TRISE` computed from the actual bus clock rather than hardcoded.
- Every wait is bounded by a poll budget (`I2C_POLL_BUDGET`), and NAKs are
  detected and reported as `-EIO`. A missing, unpowered or disconnected sensor
  therefore returns an error to the caller instead of hanging the whole RTEMS
  system.
- Failure paths always emit a STOP condition and clear the acknowledge-failure
  flag, leaving the bus usable for the next transfer.

#### Application tasks (`C_src/src/init.cpp`, `C_src/src/sensor.cpp`, `C_src/src/alive.cpp`)

- `Entrypoint` — boot sequence: run the compensation self-test, bring up the I2C1
  bus, register the BMP180 node once, then create and start the reader task.
  Registration failure is logged but non-fatal, so the system still boots and
  reports liveness.
- `bmp180_task` — periodic reader over the device node: opens `/dev/bmp180-0`,
  sets the oversampling mode, and prints compensated temperature and pressure on
  a fixed interval derived from `rtems_clock_get_ticks_per_second()`. Self-deletes
  after more than three *consecutive* read failures (the counter resets on any
  success, so isolated glitches do not kill the task).
- `bmp180_oss_sweep_task` — characterisation task, the default reader in this
  release. For each of the four oversampling modes it discards warm-up samples,
  takes 32 back-to-back measurements, and reports mean pressure, standard
  deviation, peak-to-peak spread, mean temperature and wall-clock milliseconds per
  sample; then drops into a steady read loop at `BMP180_READ_FREQUENCY` (5 Hz).
  Sampling is deliberately back-to-back so the timing column reflects the sensor's
  own conversion time and the noise figures are not aliased by the scheduler tick.
- `isqrt32` — integer square root, so the noise statistics need no floating-point
  `printf` support.
- `bmp180_task_manual` — alternative reader that drives `bmp180_do_measurement`
  directly instead of going through the device node, for debugging the driver
  below the `ioctl` layer.
- `alive_task` — heartbeat task printing `[f] alive` once per second, used to
  prove the system keeps scheduling during I2C fault-injection tests.
- RTEMS configuration: 1 ms tick, 4 tasks, and `CONFIGURE_MAXIMUM_FILE_DESCRIPTORS`
  raised to 8 — the default of 3 is consumed entirely by stdin/stdout/stderr, so
  without this every `open()` of the bus or device node fails with `ENFILE`.

#### Build, flash and tooling

- CMake build (`CMakeLists.txt`, `C_src/CMakeLists.txt`) targeting the
  `arm-rtems7` toolchain: Cortex-M4, Thumb, hard-float `fpv4-sp-d16`, `-qrtems`,
  C++17, with a post-build `objcopy` step producing a flashable `.bin`.
- Per-developer toolchain paths kept out of version control: `local.cmake`
  (with `local.cmake.example` as template) for CMake and `.env/setup.env` for the
  shell scripts. The build fails with an explicit message if `RTEMS_LOCAL_PATH`
  is unset, rather than failing obscurely later.
- Standalone `C_src/Makefile` with `all` / `sensor.bin` / `flash` / `clean`
  targets for building without CMake.
- Helper scripts: `compile.sh` (environment-driven build), `flash.sh` (build and
  program via OpenOCD at `0x08000000`), `attach_to_device.sh` (serial console).
- `D_examples/communication_test_module` — minimal heartbeat example kept
  separate from driver sources, used to validate toolchain and console bring-up
  independently of the sensor.

#### Documentation

- `C_src/README.md` — driver architecture, `ioctl` surface and task control flow.
- `C_src/TESTING.md` — six-step validation ladder: datasheet self-test, altitude
  response (~12 Pa/m), absolute cross-check against METAR QNH, the automated OSS
  noise sweep and how to read it, I2C fault injection, and temperature
  cross-check. Includes a pin/console/flash quick-reference table.
- `B_docs/SETUP.md` — toolchain and environment setup.
- `B_docs/PIN_CONFIG.md` — BMP180 and CP2102 wiring, plus how to identify and
  change the BSP console USART.
- `B_docs/ASSIGMENT.md` — original assignment statement.
- `A_report/` — LaTeX project report sources.

### Known limitations

Scope limits of the 1.0.0 design are listed below.

- Only the sequences the BMP180 needs are implemented in the I2C bus driver:
  7-bit addressing, polled transfers, standard-mode 100 kHz. 10-bit addressing,
  fast mode, interrupt/DMA transfers and the `I2C_M_NOSTART` / `I2C_M_TEN` flags
  are not supported.
- A single BMP180 instance on a single bus is assumed; the driver is not
  serialised for concurrent access from multiple tasks.
- Altitude is not computed — the driver reports raw compensated pressure in Pa
  and leaves the barometric conversion to the caller.
- `bmp180_task_manual` is a debug path and is not wired into the boot sequence.

[1.1.0]: https://github.com/AlbertoFurlan20/ES2025/releases/tag/v1.1.0
[1.0.0]: https://github.com/AlbertoFurlan20/ES2025/releases/tag/v1.0.0
