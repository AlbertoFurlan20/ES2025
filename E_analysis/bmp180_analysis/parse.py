"""Parse the BMP180 telemetry wire format into DataFrames.

Format is frozen at v1:

    #BMP180 v1 fw=1.0.0 temp_ms=0
    S <t_us> <t_cdeg> <p_pa> <oss>
    E <t_us> <errno>
    D <t_us> <count>

The stability contract this module must honour:
  1. A tag never changes meaning.
  2. New fields append only to the end of a line.
  3. Unknown tags and unknown trailing fields are ignored, not errors.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

import pandas as pd

SAMPLE_COLUMNS = ["t_us", "t_cdeg", "p_pa", "oss"]
ERROR_COLUMNS = ["t_us", "errno"]
DROP_COLUMNS = ["t_us", "count"]


@dataclass
class Session:
    """One boot-to-reset run of the device."""

    def __init__(self):
        pass

    schema: str = ""
    fw: str = ""
    temp_ms: int = 0
    samples: pd.DataFrame = field(
        default_factory=lambda: pd.DataFrame(columns=SAMPLE_COLUMNS)
    )
    errors: pd.DataFrame = field(
        default_factory=lambda: pd.DataFrame(columns=ERROR_COLUMNS)
    )
    drops: pd.DataFrame = field(
        default_factory=lambda: pd.DataFrame(columns=DROP_COLUMNS)
    )
    malformed: int = 0


class _Accumulator:
    """Collects rows for one session before they are frozen into DataFrames."""

    def __init__(self, schema: str, fw: str, temp_ms: int) -> None:
        self.schema = schema
        self.fw = fw
        self.temp_ms = temp_ms
        self.samples: list[tuple[int, int, int, int]] = []
        self.errors: list[tuple[int, int]] = []
        self.drops: list[tuple[int, int]] = []
        self.malformed = 0

    def finish(self) -> Session:
        return Session(
            schema=self.schema,
            fw=self.fw,
            temp_ms=self.temp_ms,
            samples=pd.DataFrame(self.samples, columns=SAMPLE_COLUMNS),
            errors=pd.DataFrame(self.errors, columns=ERROR_COLUMNS),
            drops=pd.DataFrame(self.drops, columns=DROP_COLUMNS),
            malformed=self.malformed,
        )


def _parse_header(line: str) -> tuple[str, str, int] | None:
    """Return (schema, fw, temp_ms) or None if this is not a header line."""
    parts = line.split()
    if not parts or parts[0] != "#BMP180":
        return None

    schema = parts[1] if len(parts) > 1 else ""
    fw = ""
    temp_ms = 0
    for token in parts[2:]:
        key, _, value = token.partition("=")
        if key == "fw":
            fw = value
        elif key == "temp_ms":
            try:
                temp_ms = int(value)
            except ValueError:
                temp_ms = 0
    return schema, fw, temp_ms


def parse_stream(lines: Iterable[str]) -> list[Session]:
    """Parse a telemetry stream into one Session per device boot."""
    sessions: list[Session] = []
    current: _Accumulator | None = None

    for raw in lines:
        line = raw.strip()
        if not line:
            continue

        header = _parse_header(line)
        if header is not None:
            if current is not None:
                sessions.append(current.finish())
            current = _Accumulator(*header)
            continue

        # Anything before the first header is boot noise from the BSP.
        if current is None:
            continue

        parts = line.split()
        tag = parts[0]

        # Rule 3: unknown tags are ignored, not counted as malformed.
        if tag not in ("S", "E", "D"):
            continue

        try:
            # Rule 2: read only the fields this version knows; ignore the rest.
            if tag == "S":
                current.samples.append(
                    (int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4]))
                )
            elif tag == "E":
                current.errors.append((int(parts[1]), int(parts[2])))
            else:
                current.drops.append((int(parts[1]), int(parts[2])))
        except (ValueError, IndexError):
            current.malformed += 1

    if current is not None:
        sessions.append(current.finish())

    return sessions


def parse_file(path: str | Path) -> list[Session]:
    """Parse a captured telemetry file."""
    with open(path, "r", errors="replace") as handle:
        return parse_stream(handle)
    return None
