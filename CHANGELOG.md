# Changelog

Firmware changes only. Documentation, host tooling and build scripts are not
tracked here — see the git history for those.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [2.0.0] - 2026-09-02

Two tasks and one way to the sensor. The driver, the bus and the acquisition
timing are untouched; what changed is who is allowed to talk to them.

### Changed

- **The application layer is now the only way to the sensor, and the default
  build.** `bmp_app_sampler_task` owns `/dev/bmp180-0`; nothing else opens it.
  What used to be `bmp180_telemetry_task` in `sensor.cpp` — a task that held its
  own fd, issued its own ioctls and pushed its own records — is now
  `src/telemetry/task.cpp`, a consumer with no descriptor at all. It drives the
  OSS sweep through `bmp_app_set_oss`, reads through `bmp_app_read`, and is the
  only producer of `S` records.

  The sweep profile is unchanged — 3 warm-up samples discarded, 500 recorded per
  mode, then a continuous run at `OSS=HIGH_RESOLUTION` — so captures stay
  comparable against the v1.5.0 gate. It keys each block on the sample's own
  `oss` field rather than assuming a mode change landed immediately, which the
  old task could assume because it issued the ioctl itself.

- **The sampler publishes, then sends an event; it no longer knows what
  telemetry is.** `bmp_app_subscribe` registers one task to be woken with
  `BMP_APP_EVENT_SAMPLE` after each publish. Polling would have been simpler and
  wrong: the snapshot is one deep, so a consumer that misses a publish loses
  that sample for good, and `metrics.py` derives the sample rate and the jitter
  from differences between consecutive `S` records — every miss reads as a
  doubled interval, in the numbers the report is built on.

  The event does not make loss impossible, it makes it rare and visible. An
  event is a bit, not a count, so two publishes landing before the consumer runs
  coalesce. The consumer watches `seq` and emits a `D` record for the hole, so
  the analysis can tell a real interval from one spanning a gap. That is now the
  only meaning of `D`.

- **The emitter task and the telemetry ring are gone; three tasks become two.**
  `telem_emitter_task` and `TelemRing` existed so that a stalled UART could never
  delay acquisition. Acquisition no longer goes anywhere near the console — the
  sampler publishes into a snapshot and nothing else — so the queue sat between
  a producer and a consumer that had become the same task.
  `bmp180_telemetry_task` formats each record and writes it to USART2 itself,
  `telem_push_*` become `telem_emit_*` and write synchronously, and `init.cpp`
  writes its registration error the same way the session header was already
  written.

  | Task | Prio | Role |
  |------|------|------|
  | `SAMP` | 2 | owns the fd, acquires, publishes, sends the sample event |
  | `TELE` | 3 | reads the surface, drives the sweep, writes USART2 |

  What this costs: the blocking `write()` now happens in the task that also has
  to be running to observe publishes, so console backpressure can lose samples
  where before it only delayed them. At OSS0 that is ~200 lines/s of about 40
  bytes against 11 520 B/s at 115200 baud — roughly 30% headroom — and any loss
  arrives as a `D` record rather than as a longer cycle time.

- **Error reporting moved from the sampler to the telemetry task.** The sampler
  records an errno in `bmp_app_last_error()` and stops there, which drops
  `telemetry/wire.h` from the app layer entirely. The consumer polls that value
  on the edge, both when an event arrives and when its wait times out.

  The timeout is what makes the no-publish cases reportable: a sampler that
  failed its `open()` deletes itself and sends nothing, and a sampler mid-outage
  publishes nothing, yet both leave an errno that reaches the stream within
  `ERROR_POLL_MS`. This reverses the reasoning recorded under Fixed below, which
  was sound while the sampler was the only task certain to be running during a
  fault, and stopped being true once the consumer sat directly below it in
  priority and ran in the yield that fix introduced.

- **One folder per module, mirrored across `src/`, `inc/` and `tests/`.**
  `bmp180/` is the driver, `i2c/` the polled STM32F4 master below it, `bmp_app/`
  the sampler task and its surface, `telemetry/` the telemetry task and the wire
  format. `init.cpp` and `constants.h` stay at the top because they belong to
  the image rather than to any module.

  Only `inc/` is on the include path, never `inc/<module>/`, so an include names
  the module it reaches into: `#include "bmp_app/app.h"`. Filenames inside a
  module lost the prefix the flat tree needed — `bmp.h` → `bmp180/driver.h`,
  `bmp_driver_app.*` → `bmp_app/app.*`, `bmp_app_snapshot.h` →
  `bmp_app/snapshot.h`, `telemetry.*` → `telemetry/wire.*`, `bmp_telemetry.*` →
  `telemetry/task.*`, `telem_fmt.h` → `telemetry/fmt.h`. Header guards follow.
  No behaviour changed: `.text` is identical across the move.

- `bmp_app_read` documents that a successful return does not mean a fresh
  reading. A failed cycle does not publish, so a dead sensor leaves the last
  good sample in place and the call keeps succeeding, plausibly, forever. The
  staleness is detectable through `seq` or `t_us` and the reason through
  `bmp_app_last_error()`, but nothing forces a caller to look, and the header
  and README now say so.

### Removed

- `telem_emitter_task`, `TelemRing`, `inc/telem_ring.h` and its host test. See
  the merge above for why a queue between one task and itself buys nothing.
- `-DBMP_APP_TEST`. It selected a demo consumer in place of the acquisition
  task; with the application layer as the only path there is no second build for
  it to choose. The demo reader it named had already been deleted.
- `src/sensor.cpp`. Its sweep profile lives on in `src/telemetry/task.cpp`,
  driven through the control surface instead of through its own fd.

### Fixed

- **A failing sampler starved every task below it.** On a successful cycle the
  sampler sleeps inside the conversion wait and yields the CPU; on a failing one
  the ioctl returns immediately and the loop retried at once, so a disconnected
  sensor turned the task into a busy-loop at priority 2. Measured by pulling SDA
  during a capture (`20260831-160509`): **2.70 s of complete console silence** —
  no samples, no errors, no drops — because nothing below it could run to report
  the fault. The premise of the layer is that a consumer can never delay
  acquisition; the inverse held far too well.

  The sampler now yields a tick after a failed cycle. That yield is also what
  lets the telemetry task report the fault: it sits one priority below the
  sampler, so it is the next task to run, and it emits on the edge — a
  disconnected sensor produces one record rather than hundreds a second.

  Re-measured (`20260831-160918`): the `E 5` lands 9 995 µs after the last good
  sample — one cycle — one record for a 6.34 s outage, the console stays alive
  throughout, and the stream resumes on reconnect with no reset. Taken under the
  `-DBMP_APP_TEST` build that preceded this change, with the same sampler and a
  different consumer, so it is the bar to re-hit rather than a result from this
  tree.

### Not yet verified on hardware

Everything above builds `-Werror` clean and passes the host suite, but no
capture has been taken since the merge. Re-run `E_analysis/capture.sh 150` and
check `median_interval_us` is still `5000 / 7000 / 10999 / 18999` with 0 `D`
records before treating the 1.5.0 gate figures as held.

## [1.6.0] - 2026-08-31

More than one task can use the sensor now. Everything measured stays where it
was: the driver, the bus and the acquisition timing are untouched.

### Added

- **An application layer over the driver (`bmp_app`).** The driver publishes a
  `/dev` node and nothing else, so every consumer had to open it, block for a
  full conversion — 5 ms at OSS0, 19 ms at OSS3 — and add its own traffic to the
  bus. Worse, changing the oversampling mode meant holding an fd, so two
  consumers would write `oss` against each other with nothing above the driver
  to arbitrate. That is workable for exactly one reader, which is what the tree
  had.

  One sampler task now owns `/dev/bmp180-0`. It applies any pending control
  change, takes a measurement, and publishes it into a snapshot. Consumers read
  that snapshot without blocking and without touching the bus, and set the mode
  or the temperature interval through calls the sampler applies at the top of
  its next cycle. Consumers never hold an fd, which is what gives `oss` a single
  writer.

  ```c
  bmp_app_sample_t s;
  if (bmp_app_read(&s)) { /* s.pressure_pa, s.t_us, s.oss, s.seq */ }

  bmp_app_set_oss(BMP180_OSS_ULTRA_HIGH_RES);   /* applied next cycle */
  ```

  The snapshot is a seqlock rather than a mutex: a reader must never be able to
  delay acquisition, and an RTEMS mutex held by a low-priority reader would
  block the sampler until the priority-inheritance handoff completed. Its
  counter is `uint32_t` and its payload is plain, because `std::atomic<uint64_t>`
  is not lock-free on ARMv7-M and would put a libatomic lock inside the one
  structure that exists to avoid one. Reads have a bounded retry budget, so a
  reader running at higher priority than the sampler cannot spin forever waiting
  for a writer it has preempted.

  A failed cycle does not publish. The snapshot keeps its previous values *and
  its previous timestamp*, so a consumer comparing `t_us` against the current
  uptime sees the true age of the data rather than a fresh timestamp on a stale
  reading — the same principle as B1.

- `-DBMP_APP_TEST` builds the sampler plus a demo consumer in place of
  `bmp180_telemetry_task`. It replaces the sweep rather than running beside it,
  because two owners of the device node would be two writers of `oss`, which is
  the race the layer removes.

### Verified

Nothing new runs in a default build, and the layer costs the shipped firmware
nothing. `20260831-120132`, default build, 150 s: 13 695 samples,
`median_interval_us` of `5000 / 7000 / 10999 / 18999`, zero errors, drops and
malformed lines — the v1.5.0 gate figures unchanged. `arm-rtems7-nm` on that
image finds no reference to the layer's tasks.

`20260831-120755`, built with `-DBMP_APP_TEST`, 150 s: **13 695 samples, the
same count**, the same four intervals, and 0/0/0. The demo consumer drove all
four modes through `bmp_app_set_oss` and read every sample through
`bmp_app_read`, at 500 samples per mode before the continuous run. So the whole
profile went through the layer at no measurable cost to acquisition.

The first attempt did not: 8797 samples, because the demo reader ended its sweep
at OSS3 and stayed there, running the 129 s free-run block at 19 ms per cycle
instead of 11 ms. It was the reader that was wrong, not the layer — it is fixed
to settle where `bmp180_telemetry_task` settles.

Host suite `test_bmp_app_snapshot` covers the seqlock against a concurrent
writer, 30 consecutive runs clean.

### Notes

- `bmp180_telemetry_task` still owns the device in a default build. Moving it
  onto the read surface, and making the sampler the only owner, is a separate
  change.

## [1.5.0] - 2026-08-30

Remediation round R4 complete, and with it the plan that started at 1.1.0. A
bus a slave is holding can now be recovered in software; everything else here is
cleanup with no behavioural change.

### Added

- **I14 — I2C bus recovery.** A CPU reset landing mid-transfer leaves the BMP180
  part-way through returning a byte: it holds SDA low waiting for clock pulses
  that never arrive, the peripheral latches BUSY, and every transfer afterwards
  returns `-EBUSY` — including the chip-id read at the next boot, so the board
  came up with no sensor and only a power cycle cleared it. Measured on the
  wedged board as `GPIOB IDR = 0x258` (PB6 high, PB7 low) with `I2C1 SR2` BUSY
  set. 1.4.0 made it matter: SCO polling and the temperature cache roughly
  tripled bus occupancy per second, so `capture.sh --reset-from` began hitting
  the window often enough to interrupt a capture session.

  Recovery is the sequence the I2C specification gives — disable the peripheral,
  take SCL and SDA over as GPIO outputs, drive up to nine SCL pulses until SDA
  releases, issue a STOP by hand, then hand the pins back and software-reset the
  peripheral, since BUSY is latched from the pin state. The configured bus clock
  is re-applied afterwards rather than the default, so a `set_clock` the
  application had done is not silently undone.

  It runs from two places: registration, when SDA reads low, and `transfer()`,
  when `wait_bus_idle` times out — this board has one master, so BUSY that
  outlasts the poll budget means a held bus, not contention. SDA still low after
  nine pulses returns `-EBUSY`: that is a short to ground or a dead slave, which
  clocking does not fix, and it must not look like a recoverable wedge.

- `-DI2C_RECOVERY_TEST` forces the sequence at boot so the pin handover, the
  manual STOP and the peripheral reset can be exercised on a healthy bus, where
  a bug would break every boot rather than only the wedged ones. Same pattern as
  `-DBMP180_TEARDOWN_TEST`; never define it in a real build.

### Fixed

- **B13 — strict-aliasing violation in the calibration sanity check.** The check
  walked `bmp180_calib_t` through a `reinterpret_cast<const uint16_t*>`, which is
  undefined behaviour, and bounded the loop with
  `sizeof(calib)/sizeof(uint16_t)`, which assumed the struct has no padding.
  Both held at `-O0`; neither was enforced anywhere. The check now runs over the
  raw 22-byte buffer as it came off the bus, with the same rejection criterion
  (`0x0000` or `0xFFFF`, neither of which the datasheet permits for a
  coefficient). A failed check also no longer leaves half-written calibration
  behind, since nothing is stored until it passes.

- **I16 — redundant `memset` after `calloc`.** `bmp180_register` cleared
  `dev->calib` on a block `i2c_dev_alloc_and_init` had already zeroed, implying
  a guarantee the caller had not checked.

### Changed

- **I12 — `ERROR` was an unprefixed all-caps macro in a shared header**, which
  collides with any vendor or system header defining the same identifier. All of
  `constants.h` is now `ES_`-prefixed, and the three macros with no caller since
  the heartbeat task was deleted are gone.
- **I6 — `bmp.h` included `<bits/stl_pair.h>`** for `std::pair`. That is a
  libstdc++ internal with no stability guarantee across versions and no
  equivalent on libc++. Now `<utility>`. `<cstring>` leaves with the last
  `memset` in the tree.
- **I13 — `BMP180_CHIP_ID_VALUE` deleted** from `bmp180_ioctls.h`. It duplicated
  `BMP180_CHIP_ID_EXPECTED` in `bmp_regs.h`, which sits with the register
  address it is read from and is the one actually used; the ioctl header defines
  the public ABI and should not carry a second name for a register constant.
- **I17 — header guard style is consistent.** `constants.h` used `#pragma once`
  against `ES2025_*` guards in the other five headers.
- **`-Werror` on all four build paths** — `compile.sh`, `C_src/Makefile`, the
  root `CMakeLists.txt` and the host test Makefile. The warning list has been
  empty since R1, so anything new is a regression from the change in front of
  you and should stop the build.

### Verified

Hardware gate, 2026-08-30, STLINK V2J40S0 at 2.915 V.

`20260830-130933`, 150 s, 13 683 samples, zero errors, drops and malformed
lines. **`median_interval_us` is bit-identical to the five 1.4.0 acceptance runs
in every mode** — `5000 / 7000 / 10999 / 18999` — which is the point: R4 rewrote
the pin configuration path and added a peripheral reset, and the measurement
path did not move by a microsecond.

| oss | segment_rms_pa | 1.4.0 | median_interval_us |
|-----|----------------|-------|--------------------|
| 0 | 5.32 Pa | 5.53 | 5000 µs (unchanged) |
| 1 | 5.52 | 4.12 | 7000 (unchanged) |
| 2 | 3.83 | 4.55 | 10999 (unchanged) |
| 3 | 3.49 | 3.75 | 18999 (unchanged) |
| free-run OSS2 | 4.09 | 3.66 | 10999 (unchanged) |

Noise is non-monotonic at OSS0→OSS1 in this run, the same known limitation the
1.4.0 gate hit at OSS1→OSS2: the datasheet step is 1 Pa against ±1 Pa
run-to-run scatter, and the sweep visits each mode once in sequence.

**Recovery on a healthy bus** (`20260830-131247`, `-DI2C_RECOVERY_TEST`): the
forced sequence prints `bus recovery on /dev/i2c-1 -> PASS`, registration
succeeds, 3 723 samples at the same four intervals, 0/0/0. The peripheral comes
back configured exactly as it was, which was the real risk in the change.

**Recovery on a real wedge** (`20260830-131424`, `-131457`): normal build,
`capture.sh 4` repeatedly so the ST-Link reset lands inside a transfer. Two of
seven boots found SDA low, recovered, registered and streamed normally; the
other five found it high and skipped the sequence.

**A/B against 1.4.0, same board, same session** (`20260830-131609` …
`-131611`): 1.4.0 rebuilt from `main`, flashed back, same reset hammering.
Three consecutive boots reported `BMP180 registration failed (RTEMS_IO_ERROR)`
with `E 19417 19` and zero samples, and stayed dead across further resets.
Flashing 1.5.0 onto that still-wedged board recovered it on the first boot
(`20260830-131640`), with no power cycle.

### Notes

- **The bug list is empty except B12**, which is unreachable until a second
  driver shares the I2C1 bus. On the improvements side only the deferred items
  remain: I8 is resolved, I9 and I17b are breaking ioctl changes held for a
  2.0.0, and I15 is a teardown path that now exists and is tested.
- Documentation drift (I10) is closed: the README described a 1 Hz printing loop
  that has not existed since 1.1.0 and an ioctl list missing three commands.

## [1.4.0] - 2026-08-07

Measurements are atomic on the bus, conversions are waited on rather than
guessed at, and temperature is no longer re-measured on every cycle.

### Fixed

- **I5 — the conversion wait was a fixed sleep of the datasheet maximum.** Every
  sample paid the worst case, and a sensor that never finished converting was
  read anyway: `0xF6` returns the previous result rather than an error, so the
  stale sample was indistinguishable from a fresh one.

  `bmp180_read_ut` and `bmp180_read_up` now sleep the datasheet *typical* time
  (3 / 3 / 5 / 9 / 17 ms) and then poll the SCO bit in `ctrl_meas` once per tick
  until the sensor clears it. The padded maxima from 1.2.1 become timeouts:
  reaching one returns `ETIMEDOUT` instead of a silently stale reading.

  `READ_MEASUREMENT` no longer flattens every failure to `-EIO` — a timeout and
  a NAKed bus are different faults, and the `E` records in the stream now say
  which one happened.

- **I19 — temperature was re-measured on every cycle.** Compensation needs a
  temperature, not a fresh one; the datasheet itself notes it can be sampled far
  more slowly than pressure. Every measurement was paying a second conversion
  (3-6 ms) for a quantity that moves in minutes.

  The device now caches the uncompensated temperature and re-reads it when the
  cached value is older than `temp_interval_ms`, default 1000 ms. At OSS0 a
  measurement drops from two conversions to one; the win shrinks as the pressure
  conversion grows, so expect roughly **+86% throughput at OSS0 and +21% at
  OSS3**. `SET_TEMP_INTERVAL` / `GET_TEMP_INTERVAL` ioctls configure it per
  device; 0 restores a temperature conversion per measurement.

  The session header's `temp_ms` field stops being 0 and reports the configured
  interval, which is what makes captures either side of the change comparable.

  **Consequence, measured:** every reading in an interval is compensated against
  one cached temperature, and compensation moves about 25 Pa per 0.1 °C, so a
  drifting temperature now leaves the stream as a staircase rather than a smooth
  curve. In the first seconds after boot, where self-heating runs ~0.08 °C/s, a
  sweep block spanning three refreshes measured 16.21 Pa whole-block against
  4.85 Pa within a segment. Thermally settled, the two agree. Noise comparisons
  must therefore use `segment_rms_pa`, which the host tooling now reports; the
  1000 ms default is kept deliberately, with the analysis adjusted to it rather
  than the interval shortened to flatter the statistic.

- **I8 — the I2C poll budget was an iteration count, not a timeout.** `100000`
  loop iterations bound nothing checkable: the wall-clock time it stood for
  depended on the compiler's output and the core clock, and it moved silently
  whenever the loop body changed.

  `wait_sr1` and `wait_bus_idle` now run against a 5 ms deadline taken from
  `rtems_clock_get_uptime_nanoseconds()` — about 50x the longest legitimate wait
  on this bus, where a byte and its ACK take ~90 us at 100 kHz. The timecounter
  is read once every 64 spins, so the check costs far less than the poll it
  guards. This is the prerequisite for I14 (bus recovery) being reviewable.

- **I1 — a measurement was not atomic on the I2C bus.** `i2c_bus_do_transfer`
  takes the bus mutex per *transfer*, but a BMP180 measurement is four transfers
  with two conversion sleeps between them. Two tasks reading concurrently
  triggered conversions into each other's sleep windows and read each other's
  results — silently, since the sensor returns the previous conversion rather
  than an error.

  `bmp180_do_measurement` now holds the bus across the whole sequence through an
  RAII `BusLock`. The mutex is recursive, so the per-transfer locks nest inside
  it. `self->oss` is serialised on the same lock, as are the `SET_OSS`,
  `GET_OSS` and `SOFT_RESET` handlers — a mode change must not land between a
  conversion trigger and its matching read.

  Measured with two concurrent readers: **unlocked, the second reader reports
  3118.92 Pa RMS and 186463 Pa peak-to-peak, with 55.6% of its samples
  byte-identical to the other task's. Locked, it reports 5.06 Pa against the
  primary reader's 5.02 Pa.** Combined throughput halves, which is the correct
  price for serialising. Single-reader behaviour is unchanged: intervals still
  13/16/22/34 ms, noise still monotonic.

- **B4 / I15 — there was no teardown path, so the destroy handlers were dead
  code.** `bmp180_destroy` and the bus destroy handler were installed and never
  reached, which is where the original use-after-free hid: freeing a device
  while its `/dev` node stayed published.

  `bmp180_unregister(dev_path)` goes through `unlink`, so the IMFS node runs the
  destroy handler in the only correct order. `-DBMP180_TEARDOWN_TEST` exercises
  register -> open -> unlink -> open at boot and prints PASS/FAIL, so the path
  stops being code nothing runs.

### Added

- `-DBMP180_CONCURRENCY_TEST` builds a second reader task that opens the device
  independently and tags its samples `oss=7`, so the two readers are separable
  host-side without a wire-format change. `-DBMP180_NO_BUS_LOCK` reduces
  `BusLock` to a no-op, so the failure can be reproduced from the same tree.
  Neither belongs in a real build.


### Verified

Five 150 s captures on hardware (`20260829-193314` … `-195123`), against the
three 1.3.0 baselines (`20260807-1832…-1837`). Noise is compared as
`segment_rms_pa` for 1.4.0 and `rms_pa` for 1.3.0, since only the former ran a
temperature cache.

| | 1.3.0 (n=3) | 1.4.0 (n=5) | interval 1.3.0 → 1.4.0 |
|---|---|---|---|
| OSS0 | 5.80 Pa | 5.53 Pa | 12999 → 5000 µs (2.60x) |
| OSS1 | 4.97 | 4.12 | 15999 → 7000 (2.29x) |
| OSS2 | 4.45 | 4.55 | 21999 → 10999 (2.00x) |
| OSS3 | 3.82 | 3.75 | 33998 → 18999 (1.79x) |
| free-run OSS2 | 4.70 | 3.66 | 21999 → 10999 (2.00x) |

Zero errors, drops and malformed lines across all five runs. Teardown and
compensation selftests pass. No mode is noisier than its 1.3.0 baseline.

**The R2 monotonicity criterion does not hold cleanly: `rms` falls with `oss` in
one run of five.** The four inversions are all at OSS1→OSS2 (5.4σ, 1.4σ, 6.0σ,
3.1σ), and in each the OSS1 block reads *low* rather than OSS2 reading high —
1.4.0 OSS1 spans 3.54-4.82 Pa against the baseline's 4.25-5.79.

Cause, as far as the data supports it: at 1.3.0 every sample carried its own
temperature conversion, so `ut` noise was injected into every compensated
pressure. Cached, that contribution is gone and pressure noise is the pressure
path alone — consecutive samples also repeat more often (OSS1 duplicates rise
from 9-12% to 12-26%). The datasheet step between OSS1 and OSS2 is 1 Pa, while
run-to-run scatter is about ±1 Pa on an environmental floor of 0.5-2.4 Pa, and
the sweep visits modes sequentially so drift lands on them unequally. The test
cannot resolve the step it is asked to resolve; interleaving the modes within a
sweep would fix that, and is a harness change rather than a driver one.

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

[1.6.0]: https://github.com/AlbertoFurlan20/ES2025/releases/tag/v1.6.0
[1.5.0]: https://github.com/AlbertoFurlan20/ES2025/releases/tag/v1.5.0
[1.4.0]: https://github.com/AlbertoFurlan20/ES2025/releases/tag/v1.4.0
[1.3.0]: https://github.com/AlbertoFurlan20/ES2025/releases/tag/v1.3.0
[1.2.1]: https://github.com/AlbertoFurlan20/ES2025/releases/tag/v1.2.1
[1.2.0]: https://github.com/AlbertoFurlan20/ES2025/releases/tag/v1.2.0
[1.1.0]: https://github.com/AlbertoFurlan20/ES2025/releases/tag/v1.1.0
[1.0.0]: https://github.com/AlbertoFurlan20/ES2025/releases/tag/v1.0.0
