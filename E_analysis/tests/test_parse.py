"""Parser tests. Synthetic streams only - no hardware required."""

from bmp180_analysis.parse import parse_stream

HEADER = "#BMP180 v1 fw=1.0.0 temp_ms=0"


def test_header_fields():
    (session,) = parse_stream([HEADER])
    assert session.schema == "v1"
    assert session.fw == "1.0.0"
    assert session.temp_ms == 0


def test_samples_parsed():
    (session,) = parse_stream([HEADER, "S 1000 244 96820 2", "S 2000 245 96825 2"])
    assert len(session.samples) == 2
    assert list(session.samples.t_us) == [1000, 2000]
    assert list(session.samples.t_cdeg) == [244, 245]
    assert list(session.samples.p_pa) == [96820, 96825]
    assert list(session.samples.oss) == [2, 2]


def test_negative_temperature():
    (session,) = parse_stream([HEADER, "S 1000 -244 96820 2"])
    assert session.samples.t_cdeg.iloc[0] == -244


def test_errors_and_drops():
    (session,) = parse_stream([HEADER, "E 1500 5", "D 1600 17"])
    assert list(session.errors.t_us) == [1500]
    assert list(session.errors.errno) == [5]
    # NOTE: deviation from the plan's literal test, which wrote
    # `session.drops.count`. `count` is a DataFrame method, so attribute access
    # can never reach a column of that name in any pandas version. Subscript
    # access asserts exactly the same thing.
    assert list(session.drops["count"]) == [17]


def test_unknown_tag_ignored():
    """Stability contract: an unknown tag must not break the parse."""
    (session,) = parse_stream([HEADER, "X 1 2 3", "S 1000 244 96820 2"])
    assert len(session.samples) == 1
    assert session.malformed == 0


def test_trailing_fields_ignored():
    """Stability contract: appended fields must not break an old parser."""
    (session,) = parse_stream([HEADER, "S 1000 244 96820 2 27898 23843"])
    assert len(session.samples) == 1
    assert session.samples.p_pa.iloc[0] == 96820


def test_malformed_counted_not_fatal():
    (session,) = parse_stream([HEADER, "S 1000 notanumber 96820 2", "S 2000 244 96820 2"])
    assert len(session.samples) == 1
    assert session.malformed == 1


def test_boot_noise_before_header_skipped():
    lines = ["[SELFTEST] compensate: PASS", "[[DEBUG]] registered", HEADER, "S 1 2 3 0"]
    (session,) = parse_stream(lines)
    assert len(session.samples) == 1


def test_reset_starts_new_session():
    lines = [HEADER, "S 1000 244 96820 2", HEADER, "S 10 244 96830 2"]
    first, second = parse_stream(lines)
    assert len(first.samples) == 1
    assert len(second.samples) == 1
    assert second.samples.t_us.iloc[0] == 10


def test_empty_stream_yields_no_sessions():
    assert parse_stream([]) == []
