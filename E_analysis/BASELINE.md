# Measurement-Path Baseline

Reference dataset for the remediation rounds in
[`../REMEDIATION_PLAN.md`](../REMEDIATION_PLAN.md). Every R2/R3 gate is a
comparison against the numbers on this page.

The measurement path has been **unmodified since v1.0.0**. Only reporting was
instrumented (v1.1.0, per [`../B_docs/TELEMETRY_DESIGN.md`](../B_docs/TELEMETRY_DESIGN.md)),
and R1 (v1.2.0) deleted dead code and tightened the build. `bmp_regs.h` is
byte-identical across all four runs below — verified with `git diff v1.1.0`.

## Runs

Runs are numbered once, in capture order, and keep those numbers permanently.
The firmware version is an attribute of a run, never part of its name.

| Run | Capture file | Started (UTC) | `fw=` | Samples |
|-----|--------------|---------------|-------|---------|
| **1** | `captures/20260803-231306.log` | 2026-08-03T21:13:06Z | 1.0.0 | 7573 |
| **2** | `captures/20260804-003601.log` | 2026-08-03T22:36:01Z | 1.1.0 | 7576 |
| **3** | `captures/20260805-141413.log` | 2026-08-05T12:14:13Z | 1.2.0 | 7575 |
| **4** | `captures/20260805-141754.log` | 2026-08-05T12:17:54Z | 1.2.0 | 7573 |

Run 1 reads `fw=1.0.0` because it predates the version bump; runs 1 and 2 are
otherwise the same binary, which the identical timing columns confirm. Runs 3
and 4 were taken about three minutes apart, same binary, same board, same room.

Common to all four:

| | |
|---|---|
| Schema | `v1` |
| Duration | 150 s |
| Board | STM32F4-Discovery, ST-Link V2J40S0, target 2.92 V |
| Console | USART2, 115200 8N1, via CP2102 |
| I²C | I2C1 100 kHz, PB6/PB7, sensor `0x77` |
| Profile | 500 samples per OSS mode 0→3 (3 warm-up discarded), then continuous at OSS=2, back-to-back with no inter-sample sleep |

Ambient conditions were not independently measured. The temperature column is
the sensor's own die reading, which is the quantity the compensation uses.

## Instrumentation health

**Malformed lines, `E` records and `D` records are zero in all four runs**
(30 297 samples total).

Zero drops is the load-bearing result: it is the evidence that emission never
throttled acquisition, which is the premise every timing figure below rests on.
Had the console stalled the acquisition loop, the intervals would be measuring
the UART rather than the sensor.

## Reading the numbers

Use **`per_block_summary()`**, not `per_oss_summary()`, for anything comparing
modes. The profile visits `oss=2` twice — a ~10 s sweep block and a ~111 s
continuous block — and pooling them lets 111 s of real atmospheric drift inflate
the noise figure for that one mode. In run 2, pooled `oss=2` reads 4.93 Pa
against 4.82 Pa for the comparable sweep block; in run 1 the distortion is larger
(4.71 pooled vs 5.53 sweep-only). `per_block_summary()` reports `span_s` per
block precisely so this is visible rather than silent.

**All mode comparisons below use sweep blocks only** (n = 500 each). Block 4, the
continuous run, is the source for drift and long-term stability, never for
mode-to-mode noise.

### Run 4, the current reference

```
 block  oss    n     span_s       mean_pa   rms_pa  p2p_pa  datasheet_rms_pa  median_interval_us
     0    0  500   5.488657 101684.644000 4.961982      32               6.0             10999.0
     1    1  500   6.985563 101686.014000 5.375296      34               5.0             13999.0
     2    2  500   9.979376 101687.352000 3.991503      23               4.0             19999.0
     3    3  500  15.967002 101684.916000 3.552597      21               3.0             31998.0
     4    2 5573 111.433035 101685.485735 4.859078      32               4.0             19999.0
```

### Drift and absolute pressure

| Run | Drift °C/min | Span °C | Mean Pa (sweep) |
|-----|--------------|---------|-----------------|
| 1 | +0.019 | 0.2 | 101642 |
| 2 | +0.189 | 0.4 | 101700 |
| 3 | +0.027 | 0.1 | 101683 |
| 4 | −0.002 | 0.1 | 101685 |

Drift varies by two orders of magnitude between runs, so self-heating at this
duty cycle is **not characterised** — a longer run is needed before treating it
as settled either way. Runs 3 and 4 were thermally much calmer than run 2.

Mean pressure moved ~58 Pa between runs 1 and 2 across 83 minutes, and ~16 Pa
between runs 2 and 3 overnight. That is ordinary weather movement — roughly 5 m
and 1.5 m of equivalent altitude — and is why R2's mean-pressure criterion is a
tolerance, not a match.

Figures: `figures/baseline/`.

## Timing is deterministic; noise is not

RMS pressure noise, sweep blocks only:

| oss | Run 1 | Run 2 | Run 3 | Run 4 | datasheet | interval (all runs) |
|-----|-------|-------|-------|-------|-----------|---------------------|
| 0 | 5.305 | 5.627 | 5.154 | 4.962 | 6.0 | 10999 µs |
| 1 | **5.698** | 4.678 | 5.016 | **5.375** | 5.0 | 13999 µs |
| 2 | 5.525 | **4.824** | 3.496 | 3.992 | 4.0 | 19999 µs |
| 3 | 3.947 | 3.964 | 3.037 | 3.553 | 3.0 | 31998 µs |

**`median_interval_us` is bit-identical across all four runs**, including across
the v1.1.0 instrumentation release and the R1 refactor. Jitter stays below
0.5 µs in every mode. Acquisition timing is fully deterministic and
tick-quantised.

Noise, by contrast, moves run to run. That asymmetry is what identifies the
defect.

## B1 is present, and it is stochastic

[B1](../KNOWN_ISSUES.md#b1--conversion-wait-can-expire-before-the-conversion-finishes-live)
predicts `rtems_task_wake_after` can return up to one tick early, so a read
occasionally lands before the conversion completes and returns the *previous*
result. Predicted symptom: noise fails to fall cleanly with oversampling.

Each oversampling step should cut RMS noise by about 1.0 Pa. Against a standard
error on the difference of two sample standard deviations of ≈ 0.247 Pa at
n = 500:

| Transition | Run 1 | Run 2 | Run 3 | Run 4 |
|------------|-------|-------|-------|-------|
| 0 → 1 | **+5.65** | +0.22 | +3.49 | **+5.73** |
| 1 → 2 | +3.29 | **+5.39** | −2.11 | −1.56 |
| 2 → 3 | −2.69 | +0.71 | +2.19 | +2.28 |

*(z-scores; a well-behaved transition sits near 0.)*

**The failure moves.** Run 1 breaks at 0→1, run 2 at 1→2, run 4 at 0→1 again,
and run 3 has no catastrophic failure at all. The transitions that are *not*
spoiled match the datasheet closely — the sensor is demonstrably capable of
meeting specification, and something intermittently prevents it.

That asymmetry is the argument. A systematic cause — a miswired pull-up, a bad
OSS control byte, an arithmetic error — would degrade the same mode every run.
Only a stochastic cause moves. An intermittent stale read is exactly that:
whether a sample is spoiled depends on where the wake-up lands inside the current
tick, which is uncorrelated with the oversampling setting.

Two alternatives are excluded by the same data: sub-microsecond jitter rules out
the scheduler, and zero dropped records across 30 297 samples rules out telemetry
back-pressure.

The mechanism is visible in the interval column. Measured medians of 11.0 / 14.0
/ 20.0 / 32.0 ms sit against tick sums of 10 / 13 / 19 / 31 ms plus about 1 ms
of I²C traffic — everything quantised to the 1 ms tick, which is precisely where
the off-by-one lives.

Full derivation:
[`../A_report/fragments/02-conversion-timing-defect-evidence.md`](../A_report/fragments/02-conversion-timing-defect-evidence.md).

### Run 3 would have passed the R2 gate with the defect unfixed

This is the most important result on the page, and it was obtained by accident.

**Run 3 is monotonic** — 5.154 > 5.016 > 3.496 > 3.037 — and every mode sits at
or under the datasheet figure. It satisfies R2's primary acceptance criterion.

The conversion constants were **not fixed**. `bmp_regs.h` still reads 5/5/8/14/26
ms, every one at the datasheet maximum, byte-identical to `v1.1.0`. Run 4, taken
three minutes later from the same binary, fails 0→1 at +5.73σ.

So a single-capture acceptance test would have certified an unfixed defect as
fixed. The two-run requirement below was written on theoretical grounds; run 3
is a concrete instance proving it is load-bearing rather than cautious.

### One open question

OSS3 sat ~1 Pa above the datasheet figure in runs 1 and 2 (3.95, 3.96) with
little variation, which did not match the intermittent signature. Runs 3 and 4
came in at 3.04 and 3.55, and were also thermally far calmer (0.1 °C span
against 0.4 °C). That is consistent with the earlier guess that the OSS3 excess
is an **environmental noise floor** — building vibration or mains interference —
rather than a timing artifact. Still not proven; re-examine after R2.

## R1 verification (runs 3 and 4)

R1 changed no runtime behaviour, so the gate is that nothing moved:

| Criterion | Result |
|-----------|--------|
| `median_interval_us` unchanged | ✅ bit-identical to runs 1 and 2 |
| `mean_pa` within tolerance | ✅ 16 Pa from run 2, well inside ±100 |
| Drops / errors / malformed | ✅ 0 / 0 / 0 in both |
| `fw=` reports the new build | ✅ `1.2.0` |

Identical timing across a refactor that rewrote the I2C bring-up path is the
substantive result: the parameterised `stm32f4_i2c_hw` configuration produces
exactly the same peripheral setup as the hardcoded constants it replaced.

## R2 acceptance criteria

R2 fixes B1 by padding each conversion constant by one tick. It is accepted when
captures under comparable conditions show:

1. **`rms_pa` falls monotonically** as `oss` rises. Primary gate.
2. **`p2p_pa` falls monotonically** as `oss` rises.
3. **`rms_pa` at or below `datasheet_rms_pa`** in every mode.
4. **`mean_pa` within ~100 Pa** of this baseline, allowing for genuine weather
   movement between runs. A larger shift means something other than the timing
   fix moved.
5. **`median_interval_us` rises by 1–2 ms per mode.** Expected — it is the added
   margin — and must **not** be read as a regression. Predicted: 12.0 / 15.0 /
   21.0 / 33.0 ms.
6. **Drops, errors and malformed lines stay at zero.**

**Take at least two runs and require every criterion in both.** Run 3 is the
proof this is necessary: it passes criteria 1–3 with the defect fully present.
Three runs would be better still — run 3 shows the false-pass rate is not small.

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
print(metrics.per_block_summary(s).to_string(index=False))
print(metrics.temperature_drift(s))
plot.report(s, 'figures/baseline')
"
```

`capture.sh` resets the target over the ST-Link at capture start, so no B2 press
is needed. The first lines of a capture are pre-reset samples that queued while
OpenOCD was connecting; the parser skips everything before the `#BMP180` header,
so they are harmless.
