# Telemetry Design

> **Status: shipped in v1.1.0 (2026-08-04), except §2.1.**
>
> | Section | State |
> |---------|-------|
> | §1 wire format | ✅ shipped |
> | §2.1 temperature decoupling | ❌ **not implemented** — deferred, see below |
> | §2.2 ring buffer + emitter | ✅ shipped |
> | §2.3 supporting changes | ✅ shipped |
> | §3 host tooling | ✅ shipped, plus `capture.py` |
>
> §2.1 was deliberately excluded: Phase 0's purpose was to measure the v1.0.0
> measurement path, and decoupling temperature would have changed the very thing
> under measurement. It belongs to the remediation rounds in
> [`../REMEDIATION_PLAN.md`](../REMEDIATION_PLAN.md), after
> [B1](../KNOWN_ISSUES.md#b1--conversion-wait-can-expire-before-the-conversion-finishes-live)
> is fixed.
>
> Deviations found during implementation are marked **[as-built]**. Measured
> results are in [`../E_analysis/BASELINE.md`](../E_analysis/BASELINE.md).

Structured measurement stream from the BMP180 driver, plus host-side tooling to
parse it and derive metrics.

**Goal:** capture pressure, temperature and timing at the highest rate the
sensor supports, without the act of logging degrading the rate being measured.

**Rationale for the acquisition change:** see
[`A_report/fragments/01-temperature-decoupling-rationale.md`](../A_report/fragments/01-temperature-decoupling-rationale.md).

---

## 1. Wire format

Line-oriented text over the existing USART2 console (115200 8N1). One record per
line, tag first, space-separated.

```
#BMP180 v1 fw=1.1.0 temp_ms=0
S <t_us> <t_cdeg> <p_pa> <oss>
E <t_us> <errno>
D <t_us> <count>
```

| Tag | Meaning | Fields |
|-----|---------|--------|
| `#BMP180` | Session header, first line | schema version, firmware version, temp interval |
| `S` | Sample | monotonic µs, temperature (0.1 °C), pressure (Pa), oversampling 0–3 |
| `E` | Error | monotonic µs, errno |
| `D` | Dropped samples | monotonic µs, count lost since last `D` |

**Stability contract** — the reason for the `v1` tag:

1. A tag never changes meaning.
2. New fields append only to the end of a line.
3. Parsers ignore unknown trailing fields and unknown tags.

Any change violating these bumps the version to `v2`.

**Timestamps** are `rtems_clock_get_uptime_nanoseconds() / 1000` — monotonic
since boot, sub-tick resolution (reads the hardware timecounter, not the 1 ms
tick). No RTC is configured, so the device has no wall-clock; the host stamps
wall-clock once at capture start and derives absolute time by offset.

**Budget:** ~22 B/line. At the 128 Hz target that is 2.8 KB/s against an
11.5 KB/s UART ceiling — 4× headroom.

**[as-built] Line endings are CRLF.** The RTEMS console driver applies `ONLCR`,
so the `\n` the emitter writes reaches the host as `\r\n`. `parse.py` strips
whitespace per line, so this is transparent — but anything else consuming the
stream must not assume bare `\n`.

**[as-built] Measured rates**, v1.0.0 measurement path (temperature converted on
every sample), from the baseline captures:

| oss | median interval | achieved rate | line rate |
|-----|-----------------|---------------|-----------|
| 0 | 10999 µs | 90.9 Hz | ~2.0 KB/s |
| 1 | 13999 µs | 71.4 Hz | ~1.6 KB/s |
| 2 | 19999 µs | 50.0 Hz | ~1.1 KB/s |
| 3 | 31998 µs | 31.3 Hz | ~0.7 KB/s |

Well inside the ceiling at every mode; zero dropped records across 15000+
samples. Intervals were bit-identical between two independent runs, so the
budget has no measurable variance to absorb.

---

## 2. Firmware changes

Ordered by size. Prints are the last and smallest item.

### 2.1 Decouple temperature — NOT IMPLEMENTED, deferred

`bmp180_compensate` currently computes `B5` from `ut` internally and produces
both outputs in one call. Split it:

```
bmp180_compute_b5(cal, ut)            -> int32_t B5
bmp180_compensate_temp(B5)            -> int32_t t_cdeg
bmp180_compensate_press(cal, B5, up, oss) -> int32_t p_pa
```

Add to `bmp180_dev_t`: cached `B5`, timestamp of last temperature conversion,
and the temperature re-conversion interval. `bmp180_do_measurement` then
converts temperature only when the interval has elapsed, and otherwise performs
a pressure conversion alone against the cached `B5`.

New ioctl `BMP180_IOCTL_SET_TEMP_INTERVAL` (interval in ms; `0` = every sample)
so the interval can be swept on hardware. Appended after the existing four
commands — no renumbering, so the ABI is preserved.

### 2.2 Decouple emission from acquisition — shipped

Requirement: the UART must never stall the acquisition loop, or the timing being
measured is distorted by the act of measuring it.

- **Acquisition task** (higher priority): I²C transfer, timestamp, push a fixed
  24-byte binary record into a single-producer/single-consumer ring buffer.
  O(1), no formatting, never blocks.
- **Emitter task** (lower priority): pop, format to text, write. **[as-built]**
  No `printf`: newlib-nano mis-formats `%llu`, so lines are built with the
  integer helpers in `telem_fmt.h` and issued as one `write()`.
- **On ring full:** increment a drop counter and discard — never block the
  producer. The emitter flushes the counter as a `D` record when space returns.

Drop records are what make "I/O never throttles acquisition" verifiable instead
of assumed. Without them, overflow silently corrupts the rate metric.

**[as-built]** Ring is 128 slots x 24 B = 3 KB, over a second of slack at the
measured 90.9 Hz ceiling. Zero overflows across 15000+ samples.

### 2.3 Supporting changes — shipped

- Acquisition loop drops its fixed `rtems_task_wake_after` pacing and runs at the
  sensor's conversion limit.
- `CONFIGURE_MAXIMUM_TASKS` covers init + acquisition + emitter; both new tasks
  get explicit stack sizes rather than `RTEMS_MINIMUM_STACK_SIZE`.
- Session header emitted once at startup.

### 2.4 Dependency

[B1](../KNOWN_ISSUES.md#b1--conversion-wait-can-expire-before-the-conversion-finishes-live)
(conversion waits can expire early) must land first. At maximum rate every cycle
is a conversion wait, so the off-by-one goes from occasional to systematic.

---

## 3. Host tooling

Python package under `E_analysis/`, three separable pieces:

| Module | Responsibility |
|--------|----------------|
| `parse.py` | Text stream → DataFrame. Enforces the stability contract: unknown tags and trailing fields ignored, malformed lines counted not fatal. Handles session boundaries on reset. |
| `metrics.py` | DataFrame → figures. No plotting, no I/O. |
| `plot.py` | Figures → charts. No parsing, no computation. |
| `capture.py` | **[as-built]** Serial capture. Not in the original design — see below. |

Kept separate so metrics are testable without capture hardware or a display. 22
tests, all against synthetic streams; none need a board.

**[as-built] Capture is a Python module, not `stty` + `cat`.** The original
design assumed a shell one-liner. Two things broke it on macOS:

1. Termios settings applied to a `/dev/cu.*` node with `stty -f` are reset when
   the port is subsequently opened, so `cat` reads at the driver's default baud
   and records framing garbage. The settings must be applied to an
   already-open descriptor which then stays open for the run.
2. macOS ships no `timeout(1)`.

`capture.py` also pulses the target reset over the ST-Link once the port is
already draining, so a capture starts at the session header without anyone
pressing B2. A consequence worth knowing: the first lines of a capture are
pre-reset samples that queued while OpenOCD was connecting, and the header
therefore appears a few lines in. `parse.py` skips everything before the first
`#BMP180`, so this is harmless.

**[as-built] Interval segmentation is by contiguous run, not by oss value.** The
same oversampling setting appears in several disjoint runs — the sweep visits
`oss=2`, and the continuous phase returns to it — so filtering by value alone
takes an interval spanning the whole intervening detour. That produced a
reported jitter of 206533 µs against a true 0.43 µs before it was fixed.

**Metrics derived from the stream:**

- `read_freq` — achieved rate from `t_us` deltas, per `oss`. Distinct from the
  configured rate; they diverge exactly when I²C or the UART stalls.
- Jitter — spread of inter-sample intervals.
- RMS pressure noise per `oss`, against the datasheet Table 3 reference
  (0.06 / 0.05 / 0.04 / 0.03 hPa for OSS 0–3).
- Altitude via §3.6: `44330 * (1 - (p/p0)^(1/5.255))`.
- Temperature drift vs acquisition rate — tests the self-heating question.
- Loss accounting from `E` and `D` records.

**Capture** is `screen`/`cat` from `/dev/cu.usbserial-*` redirected to a file.
No on-device storage: the BSP has no SDIO/MMC driver and the Discovery has no SD
slot, so on-device buffering would mean a NOR block driver plus a filesystem
plus an export path — larger than the driver itself, on a board that is
USB-tethered anyway.

---

## 4. Out of scope

- On-device altitude computation — derivable in post from `p_pa`.
- Binary framing — text has 4× headroom at target rate and survives interleaving
  with existing `printf` debug output.
- Advanced-resolution mode (datasheet Table 4: OSS3 + software oversampling,
  76.5 ms, 0.02 hPa) — a later feature, noted so it is not lost.
- Interrupt-driven or DMA I²C. It would free CPU and remove the busy-wait in
  `wait_sr1` ([I8](../KNOWN_ISSUES.md#i8--i2c_poll_budget-is-an-iteration-count-not-a-timeout)), but it does not raise sample rate:
  the bus still clocks at 100 kHz and ~90 % of each cycle is the sensor's own
  ADC conversion. Worth doing for
  [I8](../KNOWN_ISSUES.md#i8--i2c_poll_budget-is-an-iteration-count-not-a-timeout)'s sake, not for throughput.
- Raising I²C to 400 kHz — a real ~12 % gain, but independent of telemetry.
