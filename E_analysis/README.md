# BMP180 telemetry analysis

Host-side tooling for the measurement stream defined in
[`../B_docs/TELEMETRY_DESIGN.md`](../B_docs/TELEMETRY_DESIGN.md).

## Install

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

## Capture

```bash
./capture.sh 300                      # 5 minutes from the default device
./capture.sh 300 /dev/cu.usbserial-XXXX
```

Press the black B2 reset button right after the script starts, so the session
header is captured. Output lands in `captures/YYYYmmdd-HHMMSS.log`.

The device has no RTC. Timestamps in the stream are monotonic microseconds since
the device booted; the capture file records the host wall-clock start on its
first line so absolute time can be reconstructed.

## Analyse

```python
from bmp180_analysis.parse import parse_file
from bmp180_analysis import metrics, plot

sessions = parse_file("captures/20260803-190000.log")
session = sessions[-1]          # last boot in the capture

print(metrics.per_oss_summary(session))
print(metrics.temperature_drift(session))

plot.report(session, "figures/")
```

## Layout

| Module | Responsibility |
|--------|----------------|
| `parse.py` | Text stream to DataFrames. Parsing only. |
| `metrics.py` | DataFrames to numbers. No I/O, no plotting. |
| `plot.py` | Numbers to charts. No parsing, no computation. |

Split this way so metrics are testable without hardware or a display.

## Tests

```bash
python3 -m pytest tests/ -v
```

All tests run against synthetic streams; none need a board.
