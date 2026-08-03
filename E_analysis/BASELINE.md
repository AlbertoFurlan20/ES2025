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

Use **`per_block_summary()`**, not `per_oss_summary()`, for anything comparing
modes. The profile visits `oss=2` twice — a 10 s sweep block and a 111 s
continuous block — and pooling them lets 111 s of real atmospheric drift inflate
the noise figure for that one mode. Pooled, `oss=2` reads 4.93 Pa; the comparable
sweep block is 4.82 Pa, and in run 1 the distortion is larger still (4.71 pooled
vs 5.53 sweep-only). `per_block_summary()` reports `span_s` per block precisely so
this is visible rather than silent.

**Run 2** (`fw=1.1.0`, the current reference), per contiguous block:

```
 block  oss    n     span_s       mean_pa   rms_pa  p2p_pa  datasheet_rms_pa  median_interval_us
     0    0  500   5.488657 101701.816000 5.626912      33               6.0             10999.0
     1    1  500   6.985564 101699.204000 4.678289      27               5.0             13999.0
     2    2  500   9.979376 101703.114000 4.824210      24               4.0             19999.0
     3    3  500  15.967002 101702.424000 3.964369      23               3.0             31998.0
     4    2 5576 111.493031 101699.195839 4.814421      32               4.0             19999.0
```

Block 4 is the continuous run and is **not** comparable with blocks 0–3; it is
the source for drift and long-term stability, not for mode-to-mode noise.

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

Sweep blocks only (n = 500 each), so the two runs are directly comparable:

| oss | rms Run 1 | rms Run 2 | datasheet | interval (both) | jitter (both) |
|-----|-----------|-----------|-----------|-----------------|---------------|
| 0 | 5.305 | 5.627 | 6.0 | 10999 µs | 0.468 µs |
| 1 | **5.698** | 4.678 | 5.0 | 13999 µs | 0.332 µs |
| 2 | 5.525 | **4.824** | 4.0 | 19999 µs | 0.433 µs |
| 3 | 3.947 | 3.964 | 3.0 | 31998 µs | 0.000 µs |

`median_interval_us` and `jitter_std_us` are **bit-identical across the two
runs**. Acquisition timing is fully deterministic and tick-quantised. Noise, by
contrast, moves between runs — which is exactly the asymmetry that identifies
the defect.

## B1 is present and measurable

[B1](../KNOWN_ISSUES.md#b1--conversion-wait-can-expire-before-the-conversion-finishes-live)
predicts `rtems_task_wake_after` can return up to one tick early, so a read
occasionally lands before the conversion completes and returns the *previous*
result. Predicted symptom: noise fails to fall cleanly with oversampling.

Each oversampling step should cut RMS noise by about 1.0 Pa. Per-transition,
against a standard error of `s/sqrt(2(n-1))` ≈ 0.17 Pa at n = 500:

| Transition | Run 1 observed | z vs expected | Run 2 observed | z vs expected |
|------------|----------------|---------------|----------------|---------------|
| 0 → 1 | **+0.393** | **+5.65** | −0.949 | +0.22 ✓ |
| 1 → 2 | −0.173 | +3.29 | **+0.146** | **+5.39** |
| 2 → 3 | −1.578 | −2.69 | −0.860 | +0.71 ✓ |

**In each run exactly one transition fails at ~5.4σ, and it is a different one
each time.** Run 1 breaks at 0→1; run 2 at 1→2. Equally telling, the transitions
that are *not* spoiled match the datasheet almost exactly (run 2: z = +0.22 and
+0.71) — the sensor is demonstrably capable of meeting specification, and
something intermittently prevents it.

That asymmetry is the argument. A systematic cause — a miswired pull-up, a bad
OSS control byte, an arithmetic error — would degrade the same mode every run.
Only a stochastic cause moves. An intermittent stale read is exactly that:
whether a sample is spoiled depends on where the wake-up lands inside the current
tick, which is uncorrelated with the oversampling setting.

Full derivation and the ruled-out alternatives:
[`../A_report/fragments/02-conversion-timing-defect-evidence.md`](../A_report/fragments/02-conversion-timing-defect-evidence.md).

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
