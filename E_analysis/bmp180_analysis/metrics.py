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
    """Inter-sample intervals within one OSS block.

    Deltas are taken only within the block, so the gap spanning the mode change
    and its warm-up samples never appears - that gap is not a cycle time.
    """
    block = _block(session, oss)
    if len(block) < 2:
        return pd.Series(dtype="int64")
    return block.t_us.diff().dropna().astype("int64")


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
    """Mean, RMS deviation and peak-to-peak pressure for one OSS block."""
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
