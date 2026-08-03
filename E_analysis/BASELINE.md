# v1.0.0 Baseline

Reference dataset for the remediation rounds in
[`../REMEDIATION_PLAN.md`](../REMEDIATION_PLAN.md). Captured from the unmodified
v1.0.0 measurement path with only reporting instrumented — see
[`../B_docs/TELEMETRY_DESIGN.md`](../B_docs/TELEMETRY_DESIGN.md).

Every R2/R3 gate is a comparison against the numbers on this page.

## Capture

| | |
|---|---|
| File | `captures/20260803-231306.log` |
| Host start (UTC) | 2026-08-03T21:13:06Z |
| Duration | 150 s |
| Firmware | `fw=1.0.0 temp_ms=0` (temperature converted on every sample) |
| Schema | `v1` |
| Board | STM32F4-Discovery, ST-Link V2J40S0, target 2.92 V |
| Console | USART2, 115200 8N1, via CP2102 |
| I²C | I2C1 100 kHz, PB6/PB7, sensor `0x77` |
| Sensor-reported temperature | 30.0–30.2 °C throughout |
| Sensor-reported pressure | ≈ 101642 Pa |

Ambient conditions were not independently measured; the temperature column is
the sensor's own die reading, which is the quantity the compensation uses.

Acquisition profile: 500 samples at each oversampling mode 0→3 (3 warm-up
samples discarded per mode), then a continuous run at OSS=2. Sampling is
back-to-back with no inter-sample sleep.

## Instrumentation health

| Metric | Value | Meaning |
|--------|-------|---------|
| Samples | 7573 | |
| Malformed lines | **0** | emitter never produced a bad line |
| Error records (`E`) | **0** | no I²C failure during the run |
| Dropped records (`D`) | **0** | ring never overflowed |

Zero drops is the important one: it is the evidence that emission never
throttled acquisition, which is the premise the timing figures below rest on.
Had the console stalled the acquisition loop, the intervals would be measuring
the UART rather than the sensor.

## Results

```
 oss     n       mean_pa   rms_pa  p2p_pa  datasheet_rms_pa  read_freq_hz  median_interval_us  jitter_std_us
   0   500  101643.346000 5.304553      36               6.0     90.917356             10999.0       0.467866
   1   500  101642.624000 5.697949      28               5.0     71.433674             13999.0       0.332134
   2  6073  101641.727482 4.713948      35               4.0     50.002500             19999.0       0.433036
   3   500  101642.178000 3.947444      21               3.0     31.251953             31998.0       0.000000
```

Temperature drift over the run: **+0.019 °C/min**, total span 0.2 °C. Negligible
self-heating at this duty cycle over 150 s — worth re-checking on a longer run
before treating it as settled.

Figures: `figures/baseline/`.

## B1 is present and measurable

[B1](../KNOWN_ISSUES.md#b1--conversion-wait-can-expire-before-the-conversion-finishes-live)
predicts that `rtems_task_wake_after` can return up to one tick early, so a read
occasionally lands before the conversion completes and returns the *previous*
result. The prediction was that noise would fail to fall cleanly with
oversampling. It does not fall cleanly:

- **`rms_pa` is not monotonic.** OSS1 (5.70) is *noisier* than OSS0 (5.30).
  More oversampling producing more noise is physically backwards.
- **`p2p_pa` is not monotonic.** OSS2 (35) exceeds OSS1 (28).
- **Three of four modes exceed the datasheet reference** (Table 3): OSS1 5.70
  vs 5.0, OSS2 4.71 vs 4.0, OSS3 3.95 vs 3.0. Only OSS0 sits under.

The interval column shows the mechanism. Measured medians are 11.0 / 14.0 /
20.0 / 32.0 ms against tick sums of 10 / 13 / 19 / 31 ms plus roughly 1 ms of
I²C traffic — everything quantised to the 1 ms tick, which is exactly where the
off-by-one lives.

Timing itself is otherwise excellent: `jitter_std_us` is below 0.5 µs in every
mode, so the scheduler is not the source of the noise. That isolates the defect
to the conversion wait rather than to task scheduling.

## R2 acceptance criteria

R2 fixes B1 by padding each conversion constant by one tick. It is accepted when
a capture taken under comparable conditions shows:

1. **`rms_pa` falls monotonically** as `oss` rises. This is the primary gate.
2. **`p2p_pa` falls monotonically** as `oss` rises.
3. **`rms_pa` lands at or below `datasheet_rms_pa`** in every mode.
4. **`mean_pa` stays within a few Pa of this baseline**, adjusted for genuine
   weather change between runs. A large shift means something other than the
   timing fix moved.
5. **`median_interval_us` rises by roughly 1–2 ms per mode.** This is expected —
   it is the added margin — and must not be read as a regression. Predicted:
   12.0 / 15.0 / 21.0 / 33.0 ms.
6. **Drops and errors stay at zero.**

Criterion 5 is the one most likely to be misread later, so it is stated
explicitly: R2 deliberately trades a small amount of throughput for correctness.
The throughput comes back in R3, where SCO polling replaces the fixed padding
and returns the typical case to near the datasheet conversion time.

## Reproducing

```bash
cd C_src && ./flash.sh          # program v1.0.0 + instrumentation
cd ../E_analysis && ./capture.sh 150
.venv/bin/python -c "
import glob
from bmp180_analysis.parse import parse_file
from bmp180_analysis import metrics, plot
s = parse_file(sorted(glob.glob('captures/*.log'))[-1])[-1]
print(metrics.per_oss_summary(s).to_string(index=False))
print(metrics.temperature_drift(s))
plot.report(s, 'figures/baseline')
"
```

`capture.sh` resets the target over the ST-Link at capture start, so no B2 press
is needed. The first few lines of a capture are pre-reset samples that queued
while OpenOCD was connecting; the parser skips everything before the
`#BMP180` header, so they are harmless.
