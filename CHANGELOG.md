# Changelog

Firmware changes only. Documentation, host tooling and build scripts are not
tracked here — see the git history for those.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.3.0] - 2026-08-07

Remediation round R2 complete. Three live-path bugs fixed; with B1 from 1.2.1,
every bug that was reachable on the boot path is now closed except B12 and B13.

### Fixed

- **B8 — `oss` was not validated in `bmp180_register`.** The `SET_OSS` ioctl
  range-checked its argument but the registration entry point stored `oss`
  straight into the device. `bmp180_read_up` then indexes two 4-element tables
  with it, so an out-of-range value — trivial to pass, since `bmp180_oss_t` is a
  plain unscoped enum — read past both and wrote a garbage control byte to the
  sensor. Registration now rejects it with `RTEMS_INVALID_NUMBER` before
  allocating anything. Both entry points share one `bmp180_oss_is_valid()`
  predicate rather than duplicating the comparison.
- **B9 — division by zero in the compensation math.** The datasheet formula
  divides by `X1 + MD` with no guard. The calibration sanity check rejects
  all-zero and all-ones coefficients but cannot rule out a runtime `X1` that
  cancels `MD`. On Cortex-M4 the outcome depends on `DIV_0_TRP` in `SCB->CCR`:
  either a silent zero or a UsageFault. `bmp180_compensate` now returns `EIO`
  instead, propagated through `bmp180_do_measurement` to the ioctl, which already
  maps a non-zero result to `-EIO`. The datasheet selftest still prints `PASS`,
  so the guard does not perturb the reference vector.
- **B11 — `setupTask` took `rtems_id` by value.** `rtems_task_create` filled a
  local copy the caller never saw, so no task could be deleted, suspended or
  signalled from `Entrypoint`. Now an `rtems_id*` out-parameter, and the two dead
  `constexpr` ids at the call site are real variables that receive the ids.

### Notes

- Verified across three consecutive 150 s captures. **Pressure noise falls
  monotonically with oversampling in all three** — the first time that has held
  across consecutive runs; it managed 1 of 4 pre-fix and 1 of 2 in the first
  post-fix pair. Intervals exactly 13/16/22/34 ms, zero drops, errors or
  malformed lines throughout.
- Noise still does not reach the datasheet figure, by up to 0.87 Pa — **7 cm** of
  altitude. That is the environmental floor, fitted at 0.5–2.4 Pa over the same
  runs, not a firmware limit.

## [1.2.1] - 2026-08-05

First release to change the measurement path since 1.0.0.

### Fixed

- **B1 — conversion waits could expire before the conversion finished.**
  `rtems_task_wake_after(n)` blocks for between `n-1` and `n` ticks: the call
  lands somewhere inside the current tick, so the first one is partial. Every
  conversion constant sat at exactly the datasheet maximum, so at a 1 ms tick any
  wait could come up 1 ms short. Reading `0xF6` mid-conversion returns the
  *previous* result — no error, no NAK, just a silently stale sample.

  Each constant is now `datasheet_max + 1` tick, so even the worst-case `n-1`
  wait clears the specification with ~0.5 ms of margin:

  | | was | now | datasheet max | worst case |
  |---|-----|-----|---------------|------------|
  | temperature | 5 ms | 6 ms | 4.5 ms | 5.0 ms |
  | pressure OSS0 | 5 ms | 6 ms | 4.5 ms | 5.0 ms |
  | pressure OSS1 | 8 ms | 9 ms | 7.5 ms | 8.0 ms |
  | pressure OSS2 | 14 ms | 15 ms | 13.5 ms | 14.0 ms |
  | pressure OSS3 | 26 ms | 27 ms | 25.5 ms | 26.0 ms |

  Costs 2 ms per measurement — a cycle contains one temperature and one pressure
  conversion — taking the measured interval from 11/14/20/32 ms to
  13/16/22/34 ms. Confirmed on hardware to the microsecond.

### Notes

- **The fix does not improve pressure noise, and the reasoning that predicted it
  would was wrong.** The argument was: B1 causes stale reads, stale reads stop
  noise falling with oversampling, so fixing B1 fixes the noise. The first half
  holds; the second does not. With reads provably landing after the conversion
  completes, the noise is statistically indistinguishable from before.

  The noise is environmental. Fitting a floor in quadrature across six captures
  gives 0–2.6 Pa, tracking neither the firmware version nor anything else under
  software control — the two quietest runs predate this fix. At 8.33 cm of
  altitude per Pa, the whole anomaly is 0–22 cm.

  B1 was worth fixing on its own terms: the off-by-one is real and provable from
  the RTEMS semantics and the datasheet, independent of what the noise does.

## [1.2.0] - 2026-08-05

Cleanup release. Deletes every unreachable function in the tree. Eight known
bugs close, seven of them by deletion rather than by being debugged.

The measurement path is byte-for-byte as in 1.0.0.

### Removed

- **`bmp180_task_manual`** — bypassed the `/dev` node entirely, driving
  `bmp180_do_measurement` on the raw device struct. It could never have been
  wired up as written: it passed the device path as the bus path, and
  `Entrypoint` already registers that node. Closes B2, B3, B4, B5, B10, B14.
- **`bmp180_task`** — periodic reader that went through the device node
  correctly and then `printf`-ed at a fixed 1 Hz. Superseded by
  `bmp180_telemetry_task`, the same consumer emitting parseable records at the
  sensor's own rate. Closes B6, its last remaining site.
- **`alive_task` and `alive.cpp`** — heartbeat whose call site was commented out
  in 1.0.0 and deleted in 1.1.0.
- The three corresponding forward declarations in `init.cpp`.
- `sensor.cpp` drops from 304 to 94 lines.

### Fixed

- **B7 — registration failure is no longer silent.** A failed chip-id read at
  boot now pushes `E <t_us> 19` (`ENODEV`) into the telemetry stream before the
  tasks start, and the session header moved above registration so the stream is
  never headerless. Previously the failure printed to a console nobody captures,
  justified by a heartbeat that no longer ran. Still non-fatal.
- **I2 — seven functions declared `static` in a shared header.** `static` gives
  internal linkage, so declaring them in `bmp.h` handed every other translation
  unit a symbol it could never link against. Declarations moved into
  `bmp180.cpp`; `bmp.h` falls from 158 to 70 lines and now exposes only what
  callers use.
- `-Wunused-parameter` in `Entrypoint`.

### Changed

- **The I2C driver is no longer hardcoded to I2C1.** Base address, RCC clock
  index, alternate-function number and the SCL/SDA pins move into a
  `stm32f4_i2c_hw` config struct; `stm32f4_register_i2c1(path)` becomes
  `stm32f4_register_i2c(path, hw)`. The transfer engine was already
  instance-agnostic — most of it reads the register block through a pointer — so
  only the bring-up path changed. Adding I2C2 or I2C3 is one constant and one
  call. `i2c1.{h,cpp}` renamed to `i2c.{h,cpp}`.
- **`-std=c++17` pinned.** The target build previously compiled C++17 only
  because this GCC defaults to `gnu++17`.
- **`-fno-exceptions -fno-rtti`.** `bmp180_task_manual` was the only
  `throw`/`catch`/`<stdexcept>` user in the tree. Measured saving: 520 bytes of
  `.text` at `-O0`.
- Telemetry session header reports the firmware version from a single
  `DRIVER_VERSION` constant rather than a literal at the call site.

### Notes

- The `[latent]` bug category is now empty. Every remaining bug — B1, B8, B9,
  B11, B12, B13 — is on the live boot path. B12 is an unreachable *branch* in
  live code, not dead code, and needs a guard added rather than code removed.

## [1.1.0] - 2026-08-04

Instrumentation release. Replaces on-device statistics with a structured
measurement stream.

**The measurement path is unchanged.** `bmp180_do_measurement`,
`bmp180_read_ut`, `bmp180_read_up`, `bmp180_compensate`, all of the I2C driver
and every timing constant in `bmp_regs.h` are byte-for-byte as in 1.0.0. Only
reporting was replaced — the point of the release is to measure 1.0.0 honestly,
not to improve it.

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
  RTEMS dependencies so it is unit-testable on a host.
- **Integer formatters** (`C_src/inc/telem_fmt.h`). The emitter uses no
  `printf`: newlib-nano configurations silently mis-format `%llu`, and 64-bit
  microsecond timestamps must be exact. Each line is assembled into a buffer and
  issued as a single `write()`.
- **Emitter task** (`C_src/src/telemetry.cpp`) with drop reporting. Timestamps
  come from `rtems_clock_get_uptime_nanoseconds()`, which reads the hardware
  timecounter and so resolves finer than the 1 ms tick.

### Changed

- `bmp180_oss_sweep_task` replaced by `bmp180_telemetry_task`, which emits raw
  records instead of computing statistics on the device.
- `setupTask` takes an explicit stack size. Both tasks get 4 KB rather than
  `RTEMS_MINIMUM_STACK_SIZE`; acquisition runs at priority 2, above the emitter
  at 5, so the console can never delay a measurement.
- `CONFIGURE_MAXIMUM_TASKS` raised from 4 to 6.

### Removed

- `isqrt32` and the on-device statistics it supported.

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

#### STM32F4 I2C1 bus driver

- Custom polled I2C master for the STM32F4 BSP, written because the BSP ships no
  driver for the modern `<dev/i2c/i2c.h>` framework.
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

#### Application tasks

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
  directly instead of going through the device node.
- `alive_task` — heartbeat task printing `[f] alive` once per second.
- RTEMS configuration: 1 ms tick, 4 tasks, and `CONFIGURE_MAXIMUM_FILE_DESCRIPTORS`
  raised to 8 — the default of 3 is consumed entirely by stdin/stdout/stderr, so
  without this every `open()` of the bus or device node fails with `ENFILE`.

### Known limitations

- Only the sequences the BMP180 needs are implemented in the I2C bus driver:
  7-bit addressing, polled transfers, standard-mode 100 kHz. 10-bit addressing,
  fast mode, interrupt/DMA transfers and the `I2C_M_NOSTART` / `I2C_M_TEN` flags
  are not supported.
- A single BMP180 instance on a single bus is assumed; the driver is not
  serialised for concurrent access from multiple tasks.
- Altitude is not computed — the driver reports raw compensated pressure in Pa
  and leaves the barometric conversion to the caller.
- `bmp180_task_manual` is a debug path and is not wired into the boot sequence.

[1.3.0]: https://github.com/AlbertoFurlan20/ES2025/releases/tag/v1.3.0
[1.2.1]: https://github.com/AlbertoFurlan20/ES2025/releases/tag/v1.2.1
[1.2.0]: https://github.com/AlbertoFurlan20/ES2025/releases/tag/v1.2.0
[1.1.0]: https://github.com/AlbertoFurlan20/ES2025/releases/tag/v1.1.0
[1.0.0]: https://github.com/AlbertoFurlan20/ES2025/releases/tag/v1.0.0
