# Telemetry Design

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
#BMP180 v1 fw=1.1.0 temp_ms=1000
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

---

## 2. Firmware changes

Ordered by size. Prints are the last and smallest item.

### 2.1 Decouple temperature (driver — the substantial change)

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

### 2.2 Decouple emission from acquisition

Requirement: the UART must never stall the acquisition loop, or the timing being
measured is distorted by the act of measuring it.

- **Acquisition task** (higher priority): I²C transfer, timestamp, push a fixed
  16-byte binary record into a single-producer/single-consumer ring buffer.
  O(1), no formatting, never blocks.
- **Emitter task** (lower priority): pop, format to text, `printf`.
- **On ring full:** increment a drop counter and discard — never block the
  producer. The emitter flushes the counter as a `D` record when space returns.

Drop records are what make "I/O never throttles acquisition" verifiable instead
of assumed. Without them, overflow silently corrupts the rate metric.

Ring sized for ~1 s of samples at the fastest mode (256 entries ≈ 4 KB).

### 2.3 Supporting changes

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

Kept separate so metrics are testable without capture hardware or a display.

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
  `wait_sr1` ([I8](../KNOWN_ISSUES.md#i8)), but it does not raise sample rate:
  the bus still clocks at 100 kHz and ~90 % of each cycle is the sensor's own
  ADC conversion. Worth doing for
  [I8](../KNOWN_ISSUES.md#i8)'s sake, not for throughput.
- Raising I²C to 400 kHz — a real ~12 % gain, but independent of telemetry.
