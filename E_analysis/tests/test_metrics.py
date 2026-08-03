"""Metric tests. Synthetic sessions with known statistics."""

import math

import pytest

from bmp180_analysis import metrics
from bmp180_analysis.parse import parse_stream

HEADER = "#BMP180 v1 fw=1.0.0 temp_ms=0"


def build(rows):
    return parse_stream([HEADER] + rows)[0]


def test_intervals_drop_first_of_block():
    """The first delta of an OSS block spans the mode change, not a cycle."""
    session = build(
        [
            "S 1000 244 96820 0",
            "S 11000 244 96820 0",
            "S 21000 244 96820 0",
        ]
    )
    intervals = metrics.intervals_us(session, oss=0)
    # 3 samples -> 2 deltas, both 10000 us; nothing is discarded within a block.
    assert list(intervals) == [10000, 10000]


def test_intervals_exclude_cross_mode_gap():
    session = build(
        [
            "S 1000 244 96820 0",
            "S 11000 244 96820 0",
            "S 500000 244 96820 1",  # huge gap: mode change + warmup
            "S 515000 244 96820 1",
        ]
    )
    assert list(metrics.intervals_us(session, oss=0)) == [10000]
    assert list(metrics.intervals_us(session, oss=1)) == [15000]


def test_read_freq_hz():
    session = build(["S 0 244 96820 0", "S 10000 244 96820 0", "S 20000 244 96820 0"])
    assert metrics.read_freq_hz(session, oss=0) == pytest.approx(100.0)


def test_read_freq_uses_median_not_mean():
    """One stalled sample must not drag the reported rate down."""
    session = build(
        [
            "S 0 244 96820 0",
            "S 10000 244 96820 0",
            "S 20000 244 96820 0",
            "S 500000 244 96820 0",  # single long stall
        ]
    )
    assert metrics.read_freq_hz(session, oss=0) == pytest.approx(100.0)


def test_pressure_noise_known_values():
    # Population stddev of [10, 12, 10, 12] is 1.0; peak-to-peak is 2.
    session = build(
        [
            "S 0 244 10 0",
            "S 10000 244 12 0",
            "S 20000 244 10 0",
            "S 30000 244 12 0",
        ]
    )
    noise = metrics.pressure_noise(session, oss=0)
    assert noise["rms_pa"] == pytest.approx(1.0)
    assert noise["p2p_pa"] == 2
    assert noise["mean_pa"] == pytest.approx(11.0)


def test_datasheet_reference_values():
    """BST-BMP180-DS000-09 Table 3, converted from hPa to Pa."""
    assert metrics.DATASHEET_RMS_PA == {0: 6.0, 1: 5.0, 2: 4.0, 3: 3.0}


def test_altitude_at_sea_level_is_zero():
    assert metrics.altitude_m(101325.0) == pytest.approx(0.0, abs=1e-6)


def test_altitude_matches_datasheet_gradient():
    """Sea-level gradient of the datasheet 3.6 formula.

    The datasheet prints both a formula and, separately, the prose remark that
    "1 hPa corresponds to 8.43 m at sea level". They do not agree, and the
    formula is what we implement.

    Differentiating 44330 * (1 - (p/p0)^(1/5.255)) at p = p0 gives
    44330 / (5.255 * 101325) = 8.329 m/hPa. That is the ISA sea-level value
    (rho = 1.225 kg/m3 at 15 degC, i.e. 12.013 Pa/m). The 8.43 m remark implies
    rho ~ 1.21, i.e. roughly 20 degC air - a different reference condition, not
    a different formula. TESTING.md's "~12 Pa per metre" agrees with the
    formula, so the codebase is self-consistent; only the rule of thumb differs.
    """
    delta = metrics.altitude_m(101325.0 - 100.0) - metrics.altitude_m(101325.0)
    assert delta == pytest.approx(8.329, abs=0.01)


def test_temperature_drift_detects_ramp():
    """A 1 unit/s ramp in t_cdeg is 0.1 degC/s, i.e. 6.0 degC/min.

    Note that t_cdeg is a misnomer inherited from v1.0.0's bmp_types.h: the
    field holds DECI-degrees (0.1 degC), not centi-degrees. temperature_drift
    therefore converts to true degrees C so that no caller has to know this.
    """
    rows = [f"S {i * 1_000_000} {200 + i} 96820 2" for i in range(11)]
    drift = metrics.temperature_drift(build(rows))
    assert drift["degc_per_min"] == pytest.approx(6.0, rel=1e-3)


def test_per_oss_summary_shape():
    rows = []
    for oss in range(4):
        for i in range(5):
            rows.append(f"S {oss * 1_000_000 + i * 10000} 244 96820 {oss}")
    summary = metrics.per_oss_summary(build(rows))
    assert list(summary.oss) == [0, 1, 2, 3]
    assert "rms_pa" in summary.columns
    assert "read_freq_hz" in summary.columns
    assert "datasheet_rms_pa" in summary.columns


def test_empty_oss_block_is_safe():
    session = build(["S 0 244 96820 0"])
    assert math.isnan(metrics.read_freq_hz(session, oss=3))
