"""Derive metrics from a parsed telemetry Session.

Pure computation: no file I/O, no plotting. Everything here is testable against
synthetic sessions without hardware.
"""

from __future__ import annotations

import math

import numpy as np
import pandas as pd

from .parse import Session

# BST-BMP180-DS000-09 Table 3, typical RMS pressure noise per oversampling
# mode, converted from hPa to Pa. Measured noise should land near these.
DATASHEET_RMS_PA: dict[int, float] = {0: 6.0, 1: 5.0, 2: 4.0, 3: 3.0}

# Datasheet 3.6 international barometric formula constants.
_ALT_SCALE_M = 44330.0
_ALT_EXPONENT = 1.0 / 5.255
_SEA_LEVEL_PA = 101325.0


def _block(session: Session, oss: int) -> pd.DataFrame:
    """Samples for one oversampling mode, in acquisition order."""
    if session.samples.empty:
        return session.samples
    return session.samples[session.samples.oss == oss]


def intervals_us(session: Session, oss: int) -> pd.Series:
    """Inter-sample intervals for one OSS setting.

    Deltas are taken only within a CONTIGUOUS run of samples at this setting.
    The same oss value can appear in several separate runs - the sweep visits
    oss=2, and the continuous phase afterwards returns to it - and a delta taken
    across the intervening runs is not a cycle time but the length of the whole
    detour. Splitting on contiguity also drops the mode-change-plus-warm-up gap
    at each run's start, which is the same class of artefact.
    """
    samples = session.samples
    if samples.empty:
        return pd.Series(dtype="int64")

    # Label maximal runs of identical oss, then keep only this setting's runs.
    run_id = (samples.oss != samples.oss.shift()).cumsum()

    out: list[pd.Series] = []
    for _, run in samples[samples.oss == oss].groupby(run_id, sort=True):
        if len(run) >= 2:
            out.append(run.t_us.diff().dropna())

    if not out:
        return pd.Series(dtype="int64")

    return pd.concat(out, ignore_index=True).astype("int64")


def read_freq_hz(session: Session, oss: int) -> float:
    """Achieved sample rate, from the median interval.

    Median rather than mean: a single console stall or scheduling hiccup should
    not move the reported rate, and the median is what characterises the
    sustained cycle time.
    """
    intervals = intervals_us(session, oss)
    if intervals.empty:
        return math.nan
    median = float(intervals.median())
    return 1_000_000.0 / median if median > 0 else math.nan


def jitter_us(session: Session, oss: int) -> dict:
    """Spread of the inter-sample interval."""
    intervals = intervals_us(session, oss)
    if intervals.empty:
        return {"std_us": math.nan, "p95_us": math.nan, "max_us": math.nan}
    return {
        "std_us": float(intervals.std(ddof=0)),
        "p95_us": float(intervals.quantile(0.95)),
        "max_us": float(intervals.max()),
    }


def pressure_noise(session: Session, oss: int) -> dict:
    """Mean, RMS deviation and peak-to-peak pressure, POOLED over one setting.

    .. warning::
       This pools every block at this setting. If a capture visits the same
       setting twice — as the standard sweep-then-continuous profile does for
       oss=2 — the longer block contributes real atmospheric drift, inflating
       ``rms_pa`` for that setting alone and making it look noisier than its
       neighbours for reasons that have nothing to do with the sensor.

       Use :func:`per_block_summary` for any mode-to-mode comparison.
    """
    block = _block(session, oss)
    if block.empty:
        return {"mean_pa": math.nan, "rms_pa": math.nan, "p2p_pa": math.nan, "n": 0}
    return {
        "mean_pa": float(block.p_pa.mean()),
        "rms_pa": float(block.p_pa.std(ddof=0)),
        "p2p_pa": int(block.p_pa.max() - block.p_pa.min()),
        "n": int(len(block)),
    }


def altitude_m(p_pa: float, p0_pa: float = _SEA_LEVEL_PA) -> float:
    """Altitude above the reference pressure.

    Datasheet 3.6: altitude = 44330 * (1 - (p / p0) ^ (1 / 5.255))
    """
    return _ALT_SCALE_M * (1.0 - (p_pa / p0_pa) ** _ALT_EXPONENT)


def temperature_drift(session: Session) -> dict:
    """Least-squares slope of temperature against time.

    A positive slope during a sustained high-rate run is the signature of die
    self-heating rather than a change in ambient temperature.

    Units: the wire field t_cdeg is a misnomer inherited from v1.0.0's
    bmp_types.h - it carries DECI-degrees (0.1 degC), not centi-degrees. Both
    returned values are converted to true degrees Celsius here so that callers
    never have to know that, and so no caller can get the factor wrong.
    """
    samples = session.samples
    if len(samples) < 2:
        return {"degc_per_min": math.nan, "span_degc": math.nan}

    seconds = (samples.t_us - samples.t_us.iloc[0]) / 1_000_000.0
    slope_units_per_s = float(np.polyfit(seconds, samples.t_cdeg.astype(float), 1)[0])

    return {
        "degc_per_min": slope_units_per_s * 60.0 / 10.0,
        "span_degc": float(samples.t_cdeg.max() - samples.t_cdeg.min()) / 10.0,
    }


def blocks(session: Session):
    """Yield (block_index, oss, frame) for each contiguous run of one setting.

    A capture visits oss=2 twice — once in the sweep, once in the continuous
    phase — and those two runs are not comparable: the continuous one spans two
    orders of magnitude more wall time and therefore carries real atmospheric
    drift on top of sensor noise. Anything comparing modes to each other must
    work per block, not per oss value.
    """
    samples = session.samples
    if samples.empty:
        return

    run_id = (samples.oss != samples.oss.shift()).cumsum()
    for index, (_, frame) in enumerate(samples.groupby(run_id, sort=True)):
        yield index, int(frame.oss.iloc[0]), frame


def temperature_segments(frame: pd.DataFrame):
    """Yield each contiguous run of samples that share one reported temperature.

    Since 1.4.0 the driver caches the uncompensated temperature for
    `temp_interval_ms` and compensates every pressure reading in the interval
    against that one value. Compensation is sensitive to temperature at roughly
    25 Pa per 0.1 degC, so a smooth thermal drift leaves the stream as a
    staircase: flat inside an interval, stepping when the cache refreshes.

    A whole-block standard deviation therefore measures the drift times the
    cache interval, not the sensor. Segment the block first and the sensor is
    visible again.
    """
    if frame.empty:
        return

    run_id = (frame.t_cdeg != frame.t_cdeg.shift()).cumsum()
    for _, segment in frame.groupby(run_id, sort=True):
        yield segment


# Below this many samples a segment's standard deviation is a small-sample
# artefact rather than a noise figure, and pressure resolution alone can make it
# read near zero.
MIN_SEGMENT_SAMPLES = 10


def segment_rms_pa(frame: pd.DataFrame) -> float:
    """Median within-segment pressure RMS: sensor noise with cache steps removed.

    Use this, not `rms_pa`, whenever a capture ran with the temperature cache
    enabled (`temp_ms` non-zero in the session header). The median is taken over
    segments rather than pooling them, so one short segment at a block boundary
    cannot dominate.

    NaN when the block's segments are shorter than MIN_SEGMENT_SAMPLES, which is
    what a capture with the cache disabled looks like: temperature moves almost
    every sample, segments are two or three samples long, and their spread
    understates the noise instead of isolating it. Compare such a capture on
    `rms_pa` — with no cache there are no steps for this to remove.

    @see temperature_segments for why the steps exist.
    """
    lengths = []
    values = []
    for segment in temperature_segments(frame):
        lengths.append(len(segment))
        if len(segment) > 1:
            values.append(float(segment.p_pa.std(ddof=0)))

    if not values or pd.Series(lengths).median() < MIN_SEGMENT_SAMPLES:
        return math.nan

    return float(pd.Series(values).median())


def per_block_summary(session: Session) -> pd.DataFrame:
    """One row per contiguous block. Use this to compare oversampling modes.

    Prefer this over per_oss_summary() for noise comparisons: it never mixes two
    separate visits to the same setting, and it exposes each block's wall-clock
    span so drift contamination is visible rather than silent.
    """
    rows = []
    for index, oss, frame in blocks(session):
        span_s = float(frame.t_us.iloc[-1] - frame.t_us.iloc[0]) / 1_000_000.0
        intervals = frame.t_us.diff().dropna()
        rows.append(
            {
                "block": index,
                "oss": oss,
                "n": int(len(frame)),
                "span_s": span_s,
                "mean_pa": float(frame.p_pa.mean()),
                "rms_pa": float(frame.p_pa.std(ddof=0)),
                "segment_rms_pa": segment_rms_pa(frame),
                "n_segments": int(sum(1 for _ in temperature_segments(frame))),
                "p2p_pa": int(frame.p_pa.max() - frame.p_pa.min()),
                "datasheet_rms_pa": DATASHEET_RMS_PA.get(oss, math.nan),
                "median_interval_us": float(intervals.median())
                if not intervals.empty
                else math.nan,
            }
        )
    return pd.DataFrame(rows)


def per_oss_summary(session: Session) -> pd.DataFrame:
    """One row per oversampling mode present in the session."""
    rows = []
    for oss in sorted(session.samples.oss.unique()) if not session.samples.empty else []:
        noise = pressure_noise(session, int(oss))
        jitter = jitter_us(session, int(oss))
        rows.append(
            {
                "oss": int(oss),
                "n": noise["n"],
                "mean_pa": noise["mean_pa"],
                "rms_pa": noise["rms_pa"],
                "p2p_pa": noise["p2p_pa"],
                "datasheet_rms_pa": DATASHEET_RMS_PA.get(int(oss), math.nan),
                "read_freq_hz": read_freq_hz(session, int(oss)),
                "median_interval_us": float(intervals_us(session, int(oss)).median())
                if not intervals_us(session, int(oss)).empty
                else math.nan,
                "jitter_std_us": jitter["std_us"],
            }
        )
    return pd.DataFrame(rows)
