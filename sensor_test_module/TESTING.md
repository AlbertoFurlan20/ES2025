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

## 4. Noise / repeatability

Leave it still, capture ~60 samples, compute mean ± stddev.

- At `OSS = ULTRA_HIGH_RES` (3): expect a few Pa of noise, ~0.1 °C on temp.
- Large jumps or drift = wiring/pull-up/power problem.

## 5. Oversampling (OSS) sweep

Cycle `BMP180_OSS_ULTRA_LOW_POWER → … → BMP180_OSS_ULTRA_HIGH_RES` via the
`BMP180_IOCTL_SET_OSS` ioctl. Expect: noise **decreases** and per-sample
conversion time **increases** with higher OSS (datasheet: 4.5→25.5 ms).

## 6. Fault injection (driver robustness)

With the read loop running, briefly disconnect **SDA**.

- **Expect:** `BMP180_IOCTL_READ_MEASUREMENT` returns IO errors; after >3
  consecutive failures the task prints "Too many consecutive failures" and
  deletes itself. The heartbeat `[f] alive` **keeps printing** — no system
  freeze (all I2C polls are bounded by `I2C_POLL_BUDGET`).
- Reconnect and reset to resume.

## 7. Temperature cross-check

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
