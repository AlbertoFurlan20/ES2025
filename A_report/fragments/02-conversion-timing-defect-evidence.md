# Detecting an intermittent timing defect by instrumenting before fixing

**Feeds:** Design and implementation; Problems encountered
**Status:** Defect confirmed, fix scheduled (R2)
**Sources:** `C_src/src/bmp180.cpp:104-106`, `C_src/src/bmp180.cpp:145-147`;
RTEMS Classic API, `rtems_task_wake_after`; BST-BMP180-DS000-09 §3.3 Table 3;
`E_analysis/captures/`, two runs of 2026-08-03/04

## Summary

A static audit of the driver predicted that every sensor conversion delay could
expire up to one millisecond early, causing the driver to occasionally read a
stale value. The prediction was made *before* any measurement, and it named a
specific, falsifiable symptom.

The project then built a telemetry path, captured two datasets, and found the
predicted symptom at 5.4 standard deviations in both — appearing at a *different*
oversampling mode each run, which is what distinguishes an intermittent fault
from a systematic one.

The methodological point is the reusable one: the defect is invisible to the
console output the driver originally produced, and would have been invisible to
any fix-first workflow.

## The defect

Each measurement triggers a conversion in the sensor, waits, then reads the
result:

```cpp
bmp180_write_reg(&self->base, BMP180_REG_CTRL_MEAS, BMP180_MEAS_CTRL_TEMP);
rtems_task_wake_after(RTEMS_MILLISECONDS_TO_TICKS(BMP180_CONV_TIME_TEMP_MS));
bmp180_read_regs(&self->base, BMP180_REG_OUT_MSB, buf, 2);
```

`rtems_task_wake_after(n)` blocks for **between `n-1` and `n` ticks**. The call
can land at any point inside the current tick, so the first tick is partial. With
the project's 1 ms tick, every delay is therefore up to 1 ms shorter than the
constant requests.

Every constant in the driver was set to exactly the datasheet maximum:

| Measurement | Datasheet max | Ticks requested | Worst-case actual |
|-------------|---------------|-----------------|-------------------|
| Temperature | 4.5 ms | 5 | **4.0 ms** |
| Pressure OSS0 | 4.5 ms | 5 | **4.0 ms** |
| Pressure OSS1 | 7.5 ms | 8 | **7.0 ms** |
| Pressure OSS2 | 13.5 ms | 14 | **13.0 ms** |
| Pressure OSS3 | 25.5 ms | 26 | **25.0 ms** |

All five can under-wait. Reading register `0xF6` while a conversion is still in
progress does not fail — the BMP180 returns the *previous* conversion result. So
the fault produces no error code, no NAK, and no missing sample: just an
occasional reading that is one sample stale.

## Why the original code could not reveal it

Before instrumentation the firmware computed mean, standard deviation and
peak-to-peak on the device and printed four summary lines. That output cannot
distinguish this defect from ordinary sensor noise, for three reasons:

1. **No timestamps**, so the acquisition interval could not be compared against
   the conversion time it was supposed to contain.
2. **No jitter figure**, so scheduling noise could not be separated from sensor
   noise — the two leading explanations for elevated variance.
3. **Statistics computed on-device and discarded**, so no re-analysis was
   possible without reflashing, and a suspicious result could not be
   re-interrogated at all.

Instrumenting first was therefore not overhead. It was the only way to make the
prediction testable.

## Prediction

Stated in the issue register before any data was taken: if the defect is real,
**pressure noise will fail to fall as oversampling rises**, because each
oversampling step doubles the sensor's internal averaging and should reduce RMS
noise monotonically (datasheet Table 3: 0.06, 0.05, 0.04, 0.03 hPa for OSS 0–3,
i.e. 6, 5, 4 and 3 Pa).

## Method

Two independent 150 s captures, taken 83 minutes apart. Each sweeps oversampling
0→3 with 500 back-to-back samples per mode after 3 discarded warm-up samples,
then runs continuously at OSS=2.

Statistics use **only the sweep blocks**. This matters: the profile visits OSS=2
twice, and the second visit spans 111 s rather than 10 s, so it carries real
atmospheric drift on top of sensor noise. Pooling the two — which the first
version of the analysis did — inflated the OSS=2 figure from 5.53 to 4.71 Pa and
would have corrupted the comparison. The tooling now reports per contiguous
block and exposes each block's wall-clock span so the contamination is visible
rather than silent.

For a sample standard deviation the standard error is `s / sqrt(2(n-1))`, giving
roughly ±0.17 Pa at n = 500.

## Results

RMS pressure noise, sweep blocks only, against the datasheet reference:

| oss | Run 1 | Run 2 | datasheet | z, Run 1 | z, Run 2 |
|-----|-------|-------|-----------|----------|----------|
| 0 | 5.305 | 5.627 | 6.0 | −4.14 | −2.09 |
| 1 | 5.698 | 4.678 | 5.0 | +3.87 | −2.17 |
| 2 | 5.525 | 4.824 | 4.0 | **+8.72** | **+5.40** |
| 3 | 3.947 | 3.964 | 3.0 | +7.58 | +7.68 |

The sharper test is per transition. Each oversampling step should reduce noise by
about 1.0 Pa:

| Transition | Run 1 observed | z vs expected | Run 2 observed | z vs expected |
|------------|----------------|---------------|----------------|---------------|
| 0 → 1 | **+0.393** | **+5.65** | −0.949 | +0.22 |
| 1 → 2 | −0.173 | +3.29 | **+0.146** | **+5.39** |
| 2 → 3 | −1.578 | −2.69 | −0.860 | +0.71 |

## Interpretation

**In each run exactly one transition fails catastrophically, and it is a
different transition each time.** Run 1 breaks at 0→1 (z = +5.65); Run 2 breaks
at 1→2 (z = +5.39). Both are around 5.4 standard deviations — not marginal.

Equally important, **the transitions that are not spoiled match the datasheet
almost exactly**: Run 2's 0→1 lands at z = +0.22 and its 2→3 at z = +0.71. The
sensor is capable of meeting specification. Something intermittently prevents it.

That asymmetry is the core of the argument. A *systematic* cause — a miswired
pull-up, a wrong control byte, an arithmetic error in the compensation — would
degrade the same mode in every run. Only a *stochastic* cause moves between
runs. An intermittent stale read is exactly such a cause: whether a given sample
is spoiled depends on where the wake-up lands inside the current tick, which is
uncorrelated with the oversampling setting.

Two alternative explanations are ruled out by the same dataset:

- **Scheduling noise.** Inter-sample jitter is below 0.5 µs in every mode, and
  the median intervals are bit-identical across the two runs (10999, 13999,
  19999, 31998 µs). Acquisition timing is deterministic; the variance is not
  coming from the scheduler.
- **Telemetry back-pressure.** Zero dropped records across more than 15000
  samples, so emission never stalled acquisition and the intervals measure the
  sensor rather than the UART.

The interval figures also corroborate the mechanism directly. Measured medians of
11.0, 14.0, 20.0 and 32.0 ms sit against tick sums of 10, 13, 19 and 31 ms plus
roughly 1 ms of I²C traffic — every value quantised to the 1 ms tick, which is
precisely the granularity at which the off-by-one occurs.

## An open question

OSS=3 sits about 1 Pa above the datasheet figure in both runs (z = +7.58 and
+7.68) with very little variation between them. That consistency is unlike the
intermittent signature described above and is therefore probably a separate
effect — plausibly an environmental noise floor from building vibration or
mains-frequency interference, which would bound the achievable RMS regardless of
oversampling. It is recorded here as unexplained rather than folded into the
timing argument, and it should be re-examined after the timing fix lands: if
OSS=3 remains elevated once the intermittent component is removed, the residual
is environmental.

## Fix and acceptance

The fix adds one tick of margin to each constant, so the worst case still clears
the datasheet maximum, at a cost of 1–2 ms per measurement. That trade is
deliberate and is reversed later by polling the sensor's start-of-conversion bit
instead of sleeping a fixed time, which removes the class of defect rather than
padding against it.

Because the defect is intermittent, **a single passing run does not demonstrate
a fix**: Run 2 already contains two transitions that match the datasheet
perfectly while the run as a whole is still defective. Acceptance therefore
requires at least two independent captures, both monotonic, both within the
datasheet reference. The full criteria are recorded in
`E_analysis/BASELINE.md`.

## What generalises

- **Predict before measuring.** The prediction named a specific symptom in
  advance, so finding it was confirmation rather than post-hoc pattern-matching
  on noisy data.
- **Instrument before fixing.** Had the constants been padded first, the change
  would have been unfalsifiable — there would have been no before-picture, and
  the fix would have rested on the argument alone.
- **Repeat the experiment.** One run showed the anomaly at 0→1 and would have
  supported a wrong, mode-specific hypothesis. The second run is what identified
  the fault as intermittent.
- **Watch what the analysis silently pools.** The first analysis merged two
  visits to the same setting whose durations differed by a factor of eleven,
  contaminating a mode comparison with atmospheric drift. Reporting each block's
  time span made the error self-evident.
