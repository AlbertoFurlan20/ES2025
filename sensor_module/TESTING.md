# BMP180 Sensor Module — Testing Guide

Validation ladder for the RTEMS BMP180 driver on the STM32F4-Discovery, from
hardware-independent math checks up to physical and fault tests.

## Setup

```bash
make flash                              # build + program the board
screen /dev/cu.usbserial-0001 115200    # serial console (cu.*, NOT tty.*)
# press the BLACK reset (B2) to (re)start; exit screen: Ctrl-A then \
```

Console is **USART2, 115200 8N1** on **PA2(TX)/PA3(RX)**. Sensor is on **I2C1,
PB6(SCL)/PB7(SDA)**, addr `0x77`, VCC 3.3 V.

Nominal output once running:
```
[SELFTEST] compensate: T=150 (exp 150)  P=69964 (exp 69964)  -> PASS
[[DEBUG]] BMP180 registered on /dev/bmp180-0
Temperature: 24.4 degC   Pressure: 96822 Pa
```

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

## 4. OSS sweep + noise logger (automated)

The `SWEP` task (`bmp180_oss_sweep_task`, wired in `init.cpp`) runs once at
boot: for each oversampling mode it takes **32 back-to-back samples** and prints
a summary row, then drops into a normal 1 Hz read loop at `OSS=HIGH_RESOLUTION`.
Sampling is back-to-back (no inter-sample sleep) so the timing column reflects
the sensor's conversion time and the noise is not aliased by the 1 s tick.

Example output:

```
=== BMP180 OSS sweep + noise (32 samples/mode, back-to-back) ===
OSS  mean_Pa  std_Pa  p2p_Pa  mean_degC  ms/smp
  0    96820       5      18    24.4          5
  1    96819       3      11    24.4          8
  2    96820       2       7    24.4         14
  3    96821       1       4    24.4         26
=== sweep done; resuming 1 Hz read at OSS=HIGH_RESOLUTION ===
```

How to read it:

- **std_Pa / p2p_Pa** should **decrease** as OSS rises (more oversampling = less
  noise). If they don't, suspect pull-ups, wiring or power.
- **ms/smp** should **increase** with OSS (datasheet pressure conversion grows
  ~4.5 → 25.5 ms).
- **mean_Pa** should stay ~constant across modes (same true pressure).
- Stats are integer-only (`isqrt32`) — no float / `printf("%f")` dependency.

Tunables at the top of `bmp180_oss_sweep_task`: `N` (samples/mode), `WARMUP`
(discarded settling samples). To run the plain 1 Hz reader without the sweep,
swap `bmp180_oss_sweep_task` back to `bmp180_task` in `init.cpp`.

## 5. Fault injection (driver robustness)

With the read loop running, briefly disconnect **SDA**.

- **Expect:** reads return IO errors (`perror` logs them) and the heartbeat
  `[f] alive` **keeps printing** — no system freeze, because every I2C poll is
  bounded by `I2C_POLL_BUDGET` in `i2c1.cpp`.
- The plain `bmp180_task` reader additionally self-deletes after >3 consecutive
  failures ("Too many consecutive failures"); the `SWEP` reader logs and retries.
- Reconnect and reset to resume.

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
| Serial (macOS) | `/dev/cu.usbserial-*` (never `tty.*`) |
| Reset | black B2 button |
