# v1.0.0 Measurement-Path Baseline

Reference dataset for the remediation rounds in
[`../REMEDIATION_PLAN.md`](../REMEDIATION_PLAN.md). Captured with the v1.0.0
measurement path **unmodified** — only reporting was instrumented, per
[`../B_docs/TELEMETRY_DESIGN.md`](../B_docs/TELEMETRY_DESIGN.md).

Every R2/R3 gate is a comparison against the numbers on this page.

> **On the version label.** The `fw=` field records the release that produced the
> data. Release 1.1.0 is the instrumentation itself; the *measurement path* it
> observes is still v1.0.0, untouched. Run 1 is labelled `fw=1.0.0` because it
> was taken before the version bump — the binaries are otherwise identical, which
> the identical timing columns below confirm.

## Captures

Two independent runs. Both are kept because the noise result differs between
them in an informative way (see below).

| | Run 1 | Run 2 |
|---|---|---|
| File | `captures/20260803-231306.log` | `captures/20260804-003601.log` |
| Host start (UTC) | 2026-08-03T21:13:06Z | 2026-08-03T22:36:01Z |
| Firmware | `fw=1.0.0 temp_ms=0` | `fw=1.1.0 temp_ms=0` |
| Duration | 150 s | 150 s |
| Samples | 7573 | 7576 |

Common to both:

| | |
|---|---|
| Schema | `v1` |
| Board | STM32F4-Discovery, ST-Link V2J40S0, target 2.92 V |
| Console | USART2, 115200 8N1, via CP2102 |
| I²C | I2C1 100 kHz, PB6/PB7, sensor `0x77` |
| Profile | 500 samples per OSS mode 0→3 (3 warm-up discarded), then continuous at OSS=2, back-to-back with no inter-sample sleep |

Ambient conditions were not independently measured. The temperature column is
the sensor's own die reading, which is the quantity the compensation uses.

## Instrumentation health

| Metric | Run 1 | Run 2 |
|--------|-------|-------|
| Malformed lines | **0** | **0** |
| Error records (`E`) | **0** | **0** |
| Dropped records (`D`) | **0** | **0** |

Zero drops is the load-bearing result: it is the evidence that emission never
throttled acquisition, which is the premise every timing figure below rests on.
Had the console stalled the acquisition loop, the intervals would be measuring
the UART rather than the sensor.

## Results

**Run 2** (`fw=1.1.0`, the current reference):

```
 oss     n       mean_pa   rms_pa  p2p_pa  datasheet_rms_pa  read_freq_hz  median_interval_us  jitter_std_us
   0   500  101701.816000 5.626912      33               6.0     90.917356             10999.0       0.467866
   1   500  101699.204000 4.678289      27               5.0     71.433674             13999.0       0.332134
   2  6076  101699.518269 4.934145      32               4.0     50.002500             19999.0       0.433060
   3   500  101702.424000 3.964369      23               3.0     31.251953             31998.0       0.000000
```

Temperature drift: **+0.189 °C/min**, span 0.4 °C (Run 1: +0.019 °C/min, span
0.2 °C). The two runs differ by an order of magnitude, so drift at this duty
cycle is not yet characterised — a longer run is needed before treating
self-heating as settled either way.

Mean pressure rose ~58 Pa between runs (101642 → 101700) over about 83 minutes.
That is ordinary weather movement, roughly 5 m of equivalent altitude, and is
why R2's mean-pressure criterion is stated as a tolerance rather than a match.

Figures: `figures/baseline/`.

## Timing is deterministic; noise is not

Side by side:

| oss | rms Run 1 | rms Run 2 | p2p Run 1 | p2p Run 2 | interval (both) | jitter (both) |
|-----|-----------|-----------|-----------|-----------|-----------------|---------------|
| 0 | 5.30 | 5.63 | 36 | 33 | 10999 µs | 0.468 µs |
| 1 | **5.70** | 4.68 | 28 | 27 | 13999 µs | 0.332 µs |
| 2 | 4.71 | **4.93** | **35** | **32** | 19999 µs | 0.433 µs |
| 3 | 3.95 | 3.96 | 21 | 23 | 31998 µs | 0.000 µs |

`median_interval_us` and `jitter_std_us` are **bit-identical across the two
runs**. Acquisition timing is fully deterministic and tick-quantised. Noise, by
contrast, moves between runs — which is exactly the asymmetry that identifies
the defect.

## B1 is present and measurable

[B1](../KNOWN_ISSUES.md#b1--conversion-wait-can-expire-before-the-conversion-finishes-live)
predicts `rtems_task_wake_after` can return up to one tick early, so a read
occasionally lands before the conversion completes and returns the *previous*
result. Predicted symptom: noise fails to fall cleanly with oversampling.

**Both runs show non-monotonic noise, at different mode transitions:**

- Run 1: `rms` rises 0→1 (5.30 → 5.70).
- Run 2: `rms` rises 1→2 (4.68 → 4.93).
- Both runs: `p2p` at OSS2 exceeds OSS1 (35 > 28; 32 > 27).

More oversampling producing more noise is physically backwards. That it lands on
a *different* transition each run is the strongest part of the evidence: a
stochastic stale-sample effect randomises which mode it spoils, whereas a fixed
cause — a miswired pull-up, a bad OSS control byte, a arithmetic error — would
degrade the same mode every time.

The mechanism is visible in the interval column. Measured medians of 11.0 / 14.0
/ 20.0 / 32.0 ms sit against tick sums of 10 / 13 / 19 / 31 ms plus about 1 ms
of I²C traffic — everything quantised to the 1 ms tick, which is precisely where
the off-by-one lives.

Sub-microsecond jitter in every mode rules out the scheduler as the noise
source. That isolates the defect to the conversion wait rather than to task
scheduling, which is a distinction the old on-device statistics could not have
drawn.

## R2 acceptance criteria

R2 fixes B1 by padding each conversion constant by one tick. It is accepted when
a capture under comparable conditions shows:

1. **`rms_pa` falls monotonically** as `oss` rises. Primary gate.
2. **`p2p_pa` falls monotonically** as `oss` rises.
3. **`rms_pa` at or below `datasheet_rms_pa`** in every mode.
4. **`mean_pa` within ~100 Pa** of this baseline, allowing for genuine weather
   movement between runs (~58 Pa drifted across the 83 minutes separating the
   two baseline runs alone). A larger shift means something other than the
   timing fix moved.
5. **`median_interval_us` rises by 1–2 ms per mode.** Expected — it is the added
   margin — and must **not** be read as a regression. Predicted: 12.0 / 15.0 /
   21.0 / 33.0 ms.
6. **Drops, errors and malformed lines stay at zero.**

Because the defect is stochastic, a single post-R2 run showing monotonic noise
is suggestive but not conclusive. Take **at least two runs**, as here, and
require monotonicity in both.

Criterion 5 is the one most likely to be misread later, so it is stated
explicitly: R2 deliberately trades a little throughput for correctness. The
throughput returns in R3, where SCO polling replaces the fixed padding and
brings the typical case back toward the datasheet conversion time.

## Reproducing

```bash
cd C_src && ./flash.sh
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
is needed. The first lines of a capture are pre-reset samples that queued while
OpenOCD was connecting; the parser skips everything before the `#BMP180` header,
so they are harmless.
