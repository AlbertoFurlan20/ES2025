# BMP180 Sensor Module — Testing Guide

Validation ladder for the RTEMS BMP180 driver on the STM32F4-Discovery, from
hardware-independent math checks up to physical and fault tests.

## Setup

```bash
make flash                              # build + program the board
screen /dev/cu.usbserial-0001 115200    # serial console (cu.*, NOT tty.*)
# press the BLACK reset (B2) to (re)start; exit screen: Ctrl-A then \
```

For anything quantitative, prefer `E_analysis/capture.sh`, which resets the
target over the ST-Link and writes a timestamped log — no B2 press, and the
result is a file the Python tooling can parse.

Console is **USART2, 115200 8N1** on **PA2(TX)/PA3(RX)**. Sensor is on **I2C1,
PB6(SCL)/PB7(SDA)**, addr `0x77`, VCC 3.3 V.

Nominal output once running:
```
[SELFTEST] compensate: T=150 (exp 150)  P=69964 (exp 69964)  -> PASS
#BMP180 v1 fw=1.5.0 temp_ms=1000
[[DEBUG]] BMP180 registered on /dev/bmp180-0
S 1043221 244 96822 0
S 1054220 244 96825 0
```

Records are the telemetry wire format, not human prose. `S` is a sample
(µs, 0.1 °C, Pa, oversampling); `E` is an error (µs, errno); `D` is a drop
count.

---

## 1. Datasheet vector self-test (math correctness) — automated

Runs at boot, no hardware needed. Feeds the datasheet worked example
(BST-BMP180-DS000-09 §3.5) into `bmp180_compensate`.

- **Input:** datasheet calibration constants, raw `UT=27898`, `UP=23843`, `oss=0`
- **Expect:** `T = 150` (15.0 °C), `P = 69964 Pa`
- **PASS criteria:** boot line shows `-> PASS`

A FAIL means the Bosch fixed-point compensation is broken — independent of any
wiring or I2C issue.

## 2. Altitude test (quantitative pressure response)

Pressure falls ~**12 Pa per metre** of altitude gain near sea level.

| Move sensor | Expected ΔP |
|-------------|-------------|
| +1 m        | ≈ −12 Pa    |
| +3 m (one floor) | ≈ −36 Pa |
| +10 m       | ≈ −120 Pa   |

Hold it at desk height, note the value, lift it up a staircase, watch the
pressure drop and return. This proves the reading tracks real physics, not a
constant. (Pressing/squeezing the sensor does little — it is a barometer.)

## 3. Absolute accuracy cross-check (reference)

Compare against a nearby weather station's **station pressure**, or convert the
reading to sea-level and compare to the published **QNH**:

```
QNH = P_station / (1 - 0.0065 * h / 288.15) ^ 5.255
```

Worked example for **Velate (Varese), h ≈ 480 m**:
- factor `(1 - 0.0065·480/288.15)^5.255 = 0.9444`
- reading `968.2 hPa / 0.9444 ≈ 1025 hPa`
- → compare to **Malpensa (LIMC) METAR QNH**; within a few hPa = accurate.

## 4. OSS sweep + noise analysis (automated, host-side)

`bmp180_telemetry_task` runs at boot: for each oversampling mode it takes **500
back-to-back samples** after 3 discarded warm-up samples, then settles into a
continuous run at `OSS=HIGH_RESOLUTION`. Sampling has no inter-sample sleep, so
consecutive timestamps bound the true acquisition cycle time.

No statistics are computed on the device. The board emits raw records; mean,
RMS, peak-to-peak and achieved rate are all derived on the host, where they can
be recomputed without reflashing:

```bash
cd ../E_analysis && ./capture.sh 150
.venv/bin/python -c "
import glob
from bmp180_analysis.parse import parse_file
from bmp180_analysis import metrics
s = parse_file(sorted(glob.glob('captures/*.log'))[-1])[-1]
print(metrics.per_block_summary(s).to_string(index=False))
"
```

How to read it:

- Use **`per_block_summary()`**, not `per_oss_summary()`. The profile visits
  `oss=2` twice — a ~10 s sweep block and a ~111 s continuous block — and
  pooling them lets atmospheric drift inflate that one mode.
- **rms_pa / p2p_pa** should **decrease** as OSS rises (more oversampling = less
  noise). If they don't, see below.
- **median_interval_us** should **increase** with OSS (datasheet pressure
  conversion grows ~4.5 → 25.5 ms).
- **mean_pa** should stay ~constant across modes (same true pressure).

**Known result:** noise fell non-monotonically until R2 (B1, the conversion
wait expiring early). Since 1.2.1 it has been monotonic across three consecutive
captures. A non-monotonic sweep is now a result worth investigating, not the
expected state — but take two captures before believing it, because the original
defect was stochastic.

Tunables at the top of `bmp180_telemetry_task`: `SWEEP_SAMPLES`, `WARMUP`.

## 5. Fault injection (driver robustness)

With the read loop running, briefly disconnect **SDA**.

- **Expect:** reads fail and emit `E <t_us> <errno>` records rather than
  stopping. No system freeze, because every I2C poll is bounded by a 5 ms
  deadline (`I2C_POLL_TIMEOUT_US` in `i2c.cpp`), and every conversion wait by
  the datasheet maximum for its mode (`BMP180_CONV_TIME_*_MS`).
- The stream continues: `bmp180_telemetry_task` logs the error and retries
  indefinitely; it does not self-delete on consecutive failures.
- Reconnect and reset to resume.

Which errno appears says where the fault is. A disconnected SDA floats high on
the pull-up, so the sensor never acknowledges its address and reads fail with
`EIO` (5). SDA held *low* — shorted to ground, or a slave wedged mid-transfer —
leaves the peripheral BUSY instead, and that path now goes through recovery
(§9) before returning `EBUSY` (16).

Registration failure is also visible in the stream: if the chip-id read at boot
fails, `init.cpp` emits `E <t_us> 19` (`ENODEV`) before the tasks start. A
capture keeps the stream and nothing else, so a board that came up without its
sensor is identifiable after the fact.

Count errors and drops from a capture with:

```bash
grep -c '^E ' captures/<file>.log      # errors
grep -c '^D ' captures/<file>.log      # dropped records
```

Both should be **0** on a healthy run.

## 6. Temperature cross-check

Warm the chip with a finger / cool with canned air; temperature should track
within ~1 °C of a reference thermometer at steady state.

## 7. Teardown (R3, build flag)

```bash
# in C_src/
RTEMS_LOCAL_PATH=... ./compile.sh   # after adding -DBMP180_TEARDOWN_TEST
```

Registers the device, opens it, unlinks the node and opens it again, once at
boot, before the run's own registration.

- **Expect:** `[SELFTEST] teardown: unlink(/dev/bmp180-0) -> 0  -> PASS`
- A FAIL on the second open means the node outlived the device it points at,
  which is the use-after-free B4 describes.

## 8. R3 acceptance (SCO polling + temperature cache)

Two captures, same procedure as §4, compared against the 1.3.0 baseline.

| Criterion | Expectation |
|-----------|-------------|
| `temp_ms` in the header | `1000`, not `0` — the capture records the interval it ran with |
| `median_interval_us` | **falls** in every mode: the fixed worst-case sleep is gone (I5) and most cycles skip the temperature conversion entirely (I19) |
| `segment_rms_pa` | **unchanged** within noise against the 1.3.0 `rms_pa`. A conversion that is waited for properly must not be noisier than one that was slept through; if noise rises, SCO is being read too early |
| `segment_rms_pa` monotonic in `oss` | this is the R2 gate and it must not regress |
| Drops / errors / malformed | 0 / 0 / 0. An `E` record carrying `ETIMEDOUT` (116) means a conversion never reported finished, which is a sensor or bus fault, not a slow sample |
| Temperature trace | stepwise, changing about once per second rather than every sample |

**Read `segment_rms_pa`, not `rms_pa`, on any capture with `temp_ms` non-zero.**
The driver compensates every reading in an interval against one cached
temperature, and compensation moves about 25 Pa per 0.1 °C, so a drifting
temperature leaves the stream as a staircase: flat inside an interval, stepping
when the cache refreshes. `rms_pa` then measures thermal drift times the cache
interval, not the sensor. `segment_rms_pa` splits the block at each refresh and
reports the median within-segment RMS; `n_segments` shows how many refreshes the
block spanned, so contamination is visible rather than silent.

The effect is largest in the first seconds after boot, where self-heating runs
around 0.08 °C/s: a block spanning three cached temperatures has been measured at
16.21 Pa whole-block against 4.85 Pa within-segment. A thermally settled board
gives one segment per sweep block and the two figures agree.

Set `temp_ms` back to 0 through `BMP180_IOCTL_SET_TEMP_INTERVAL` to take a
comparison capture with the cache disabled; the header will report the change.
Note that `segment_rms_pa` reads NaN on such a capture by design — with the cache
off, temperature moves almost every sample, segments are two or three samples
long, and their spread understates the noise. Compare cache-off captures on
`rms_pa`.

**Known limitation of this ladder.** Criterion 4 held in one of the five 1.4.0
acceptance runs. Every failure is at the OSS1→OSS2 step, and in each the OSS1
block reads low rather than OSS2 reading high. The datasheet difference between
those modes is 1 Pa, run-to-run scatter is about ±1 Pa, and the sweep visits each
mode once in sequence, so slow environmental drift lands on the modes unequally.
Interleaving the modes within a sweep — a few hundred samples of A, then B, then
A again — would let the two be compared against the same drift, and is the change
worth making before this criterion is trusted at that step.

## 9. I2C bus recovery (R4, build flag + fault injection)

A CPU reset landing mid-transfer leaves the BMP180 holding SDA low, waiting for
clock pulses that never arrive. The peripheral latches BUSY, every transfer
returns `-EBUSY`, and before v1.5.0 only a power cycle cleared it — the boot
after such a reset came up with no sensor at all. Recovery clocks the slave free
instead: up to nine SCL pulses driven by hand, then a manual STOP and a
peripheral reset.

**a. On a healthy bus — does recovery leave a working bus working?**

This is the real risk in the change: the sequence takes the pins away from the
peripheral and software-resets it, so a bug here breaks every boot, not only the
wedged ones.

```bash
# in C_src/, after adding -DI2C_RECOVERY_TEST to compile.sh
RTEMS_LOCAL_PATH=... ./compile.sh && make flash
```

The flag forces the sequence to run at registration regardless of the pin state.

- **Expect:** `[[DEBUG]] bus recovery on /dev/i2c-1 -> PASS`, then a completely
  normal run — the header line, `BMP180 registered on /dev/bmp180-0`, and `S`
  records at the usual rate.
- A `FAIL` here with nothing attached to the bus means the pin handover is
  wrong, not that the bus is stuck.
- Any change in `median_interval_us` against a capture without the flag means
  the peripheral was not brought back with the timing it had.

**b. On a wedged bus — does recovery actually free it?**

Reproduce the wedge, which is what `capture.sh --reset-from` hits:

```bash
cd ../E_analysis
./capture.sh 5     # resets over the ST-Link mid-transfer; repeat until it wedges
```

Bus occupancy since 1.4.0 is high enough that a few attempts usually land inside
a transfer. On a pre-1.5.0 build the wedged boot shows `E <t_us> 19` (`ENODEV`)
and no samples, and stays that way across every further reset; captures
`20260829-192310`, `-192344`, `-192436` and `-192535` are that failure recorded.

- **Expect on 1.5.0:** the next boot prints
  `[[DEBUG]] bus recovery on /dev/i2c-1 -> PASS` and then streams samples
  normally, with no power cycle.
- Shorting SDA to ground while a capture runs is the deliberate version: nine
  pulses cannot free a pin that is wired low, so recovery reports
  `FAIL (SDA still held)` and transfers return `EBUSY` (16) until the short is
  removed. That distinction is the point — an interrupted transfer is
  recoverable, a shorted line is not, and the two must not look alike.

The recovery attempt from inside a transfer is silent by design: it either
succeeds, in which case the sample is simply taken, or it fails and the errno
reaches the stream as an `E` record.

**Result, 2026-08-30.** Both halves pass. Forced recovery on a healthy bus:
`20260830-131247`, PASS line then an ordinary run at unchanged intervals. Real
wedges: two of seven reset-boots (`20260830-131424`, `-131457`) found SDA low and
recovered. The same reset hammering on 1.4.0 killed three consecutive boots
(`20260830-131609` … `-131611`, `RTEMS_IO_ERROR` and `E ... 19`, zero samples)
and the board stayed dead until 1.5.0 was flashed onto it, which cleared it on
the first boot. Roughly two boots in seven is how often this reproduces, so
expect to repeat `capture.sh 4` a handful of times.

---

## Quick reference

| Item | Value |
|------|-------|
| Console | USART2, 115200 8N1, PA2/PA3 |
| I2C bus | I2C1, PB6=SCL / PB7=SDA, 100 kHz |
| Sensor addr | `0x77` |
| Chip ID reg/val | `0xD0` → `0x55` |
| Flash | `make flash` (OpenOCD, stm32f4discovery.cfg) |
| Capture | `E_analysis/capture.sh <seconds>` (resets via ST-Link) |
| Serial (macOS) | `/dev/cu.usbserial-*` (never `tty.*`) |
| Reset | black B2 button, or ST-Link via `capture.sh` |
| Host tests | `make -C tests run` |
| Build flags | `BMP180_TEARDOWN_TEST` (§7), `BMP180_CONCURRENCY_TEST` / `BMP180_NO_BUS_LOCK` (I1), `I2C_RECOVERY_TEST` (§9) |
