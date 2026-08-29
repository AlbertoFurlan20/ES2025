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


def test_intervals_split_discontiguous_runs_of_same_oss():
    """The same oss can appear in several separate runs.

    A sweep visits oss=2 and the continuous phase afterwards returns to it. A
    delta taken across the intervening oss=3 block is the length of that detour,
    not a cycle time, and would wreck the jitter figure. Regression test for a
    real defect seen in the v1.0.0 baseline capture, where jitter_std_us for
    oss=2 read 206533 us against a true value near zero.
    """
    session = build(
        [
            "S 1000 244 96820 2",
            "S 21000 244 96820 2",
            "S 41000 244 96820 3",   # detour to another mode
            "S 73000 244 96820 3",
            "S 900000 244 96820 2",  # back to oss=2, long gap
            "S 920000 244 96820 2",
        ]
    )
    intervals = metrics.intervals_us(session, oss=2)
    # Two runs of two samples each -> one delta per run, both 20000 us.
    # The 900000-41000 gap must NOT appear.
    assert sorted(intervals) == [20000, 20000]
    assert metrics.jitter_us(session, oss=2)["std_us"] == pytest.approx(0.0)


def test_per_block_summary_separates_repeat_visits():
    """oss=2 visited twice must produce two rows, not one pooled row.

    The standard capture profile sweeps 0-3 then returns to oss=2 for a long
    continuous run. Pooling those two visits mixes a 10 s block with a 100 s one,
    so the longer block's atmospheric drift inflates the noise figure for that
    mode alone. Regression test for a confound that made the v1.0.0 baseline
    report 4.71 Pa at oss=2 where the comparable sweep block was 5.53 Pa.
    """
    S = 1_000_000  # timestamps are microseconds
    rows = (
        # Short sweep block: 4 samples over 3 s, tight.
        [f"S {i * S} 244 {100 + (i % 3)} 2" for i in range(4)]
        # Detour to another setting.
        + [f"S {(10 + i) * S} 244 {200 + i * 50} 3" for i in range(4)]
        # Second visit to oss=2: 4 samples over 90 s, drifting hard.
        + [f"S {(100 + i * 30) * S} 244 {300 + i * 90} 2" for i in range(4)]
    )
    summary = metrics.per_block_summary(build(rows))

    assert list(summary.oss) == [2, 3, 2]
    assert list(summary.block) == [0, 1, 2]

    first, third = summary.iloc[0], summary.iloc[2]
    # Same setting, wildly different noise - because the third block drifts.
    assert first.rms_pa < third.rms_pa
    # span_s exposes why, so the contamination is visible rather than silent.
    assert first.span_s == pytest.approx(3.0)
    assert third.span_s == pytest.approx(90.0)


def test_per_oss_summary_still_pools_by_design():
    """per_oss_summary keeps pooling; the caveat is documented, not enforced."""
    rows = (
        [f"S {i * 1000} 244 100 2" for i in range(3)]
        + [f"S {10000 + i * 1000} 244 100 3" for i in range(3)]
        + [f"S {90000 + i * 1000} 244 100 2" for i in range(3)]
    )
    summary = metrics.per_oss_summary(build(rows))
    assert list(summary.oss) == [2, 3]
    assert int(summary[summary.oss == 2].n.iloc[0]) == 6


def test_temperature_segments_split_on_cache_refresh():
    """A block is one segment per cached temperature, not one segment overall."""
    session = build(
        [
            "S 1000 244 96820 0",
            "S 6000 244 96822 0",
            "S 11000 245 96860 0",
            "S 16000 245 96862 0",
        ]
    )
    frame = session.samples
    segments = list(metrics.temperature_segments(frame))
    assert [len(s) for s in segments] == [2, 2]
    assert [int(s.t_cdeg.iloc[0]) for s in segments] == [244, 245]


def test_segment_rms_excludes_the_cache_step():
    """The step between cached temperatures is not sensor noise.

    Two segments 40 Pa apart, each holding a 1 Pa spread: the block RMS sees the
    step and reports ~20 Pa, the segment RMS reports the 1 Pa that is real.
    """
    rows = [
        f"S {1000 + i * 5000} 244 {96820 + (i % 2) * 2} 0" for i in range(20)
    ] + [
        f"S {101000 + i * 5000} 245 {96860 + (i % 2) * 2} 0" for i in range(20)
    ]
    frame = build(rows).samples

    assert frame.p_pa.std(ddof=0) == pytest.approx(20.0, abs=0.5)
    assert metrics.segment_rms_pa(frame) == pytest.approx(1.0, abs=0.01)


def test_segment_rms_is_block_rms_when_temperature_is_constant():
    """With no cache refresh in the block the two measures must agree."""
    rows = [f"S {1000 + i * 5000} 244 {96820 + (i % 2) * 2} 0" for i in range(20)]
    frame = build(rows).samples
    assert metrics.segment_rms_pa(frame) == pytest.approx(
        float(frame.p_pa.std(ddof=0)), abs=1e-9
    )


def test_per_block_summary_reports_segments():
    """The summary carries the segment count so cache contamination is visible."""
    rows = [
        f"S {1000 + i * 5000} 244 {96820 + (i % 2) * 2} 0" for i in range(20)
    ] + [
        f"S {101000 + i * 5000} 245 {96860 + (i % 2) * 2} 0" for i in range(20)
    ]
    row = metrics.per_block_summary(build(rows)).iloc[0]
    assert row.n_segments == 2
    assert row.segment_rms_pa < row.rms_pa


def test_segment_rms_is_nan_without_a_temperature_cache():
    """Short segments mean the cache was off; the measure does not apply.

    With temp_ms=0 the temperature moves almost every sample, so segments are a
    couple of samples long and their spread understates the noise. Returning NaN
    stops such a capture being compared against a cached one on this column.
    """
    rows = [
        f"S {1000 + i * 5000} {244 + i} {96820 + (i % 2) * 8} 0" for i in range(40)
    ]
    frame = build(rows).samples
    assert math.isnan(metrics.segment_rms_pa(frame))
    assert frame.p_pa.std(ddof=0) > 0
