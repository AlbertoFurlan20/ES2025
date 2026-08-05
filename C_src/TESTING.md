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
#BMP180 v1 fw=1.2.0 temp_ms=0
[[DEBUG]] BMP180 registered on /dev/bmp180-0
S 1043221 244 96822 0
S 1054220 244 96825 0
```

Records are the telemetry wire format, not human prose. `S` is a sample
(µs, 0.1 °C, Pa, oversampling); `E` is an error (µs, errno); `D` is a drop
count. Full contract in
[`../B_docs/TELEMETRY_DESIGN.md`](../B_docs/TELEMETRY_DESIGN.md).

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

**Known result:** noise does *not* currently fall monotonically. This is
[B1](../KNOWN_ISSUES.md#b1--conversion-wait-can-expire-before-the-conversion-finishes-live),
confirmed intermittent across two captures and scheduled for R2. Recorded
reference numbers and the acceptance criteria for the fix are in
[`../E_analysis/BASELINE.md`](../E_analysis/BASELINE.md). Do not treat a
non-monotonic sweep as a wiring fault until R2 has landed.

Tunables at the top of `bmp180_telemetry_task`: `SWEEP_SAMPLES`, `WARMUP`.

## 5. Fault injection (driver robustness)

With the read loop running, briefly disconnect **SDA**.

- **Expect:** reads fail and emit `E <t_us> <errno>` records rather than
  stopping. No system freeze, because every I2C poll is bounded by
  `I2C_POLL_BUDGET` in `i2c1.cpp`.
- The stream continues: `bmp180_telemetry_task` logs the error and retries
  indefinitely; it does not self-delete on consecutive failures.
- Reconnect and reset to resume.

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
