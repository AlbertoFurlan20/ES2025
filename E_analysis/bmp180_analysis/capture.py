"""Serial capture for the BMP180 telemetry stream.

Why this is not `stty` plus `cat`:

On macOS, termios settings applied to a /dev/cu.* node with `stty -f` are reset
when the port is subsequently opened, so `cat` reads at whatever baud the driver
defaults to and the capture is framing garbage. The settings must be applied to
an already-open descriptor and that descriptor must stay open for the duration.
This module does that, and is the reason capture.sh is a thin wrapper rather
than a shell one-liner.

Optionally pulses the target reset over the ST-Link first, so a capture starts
at the session header without anyone pressing the board's B2 button.
"""

from __future__ import annotations

import argparse
import os
import select
import subprocess
import sys
import termios
import time
from datetime import datetime, timezone
from pathlib import Path

BAUD_CONSTANTS = {
    9600: termios.B9600,
    19200: termios.B19200,
    38400: termios.B38400,
    57600: termios.B57600,
    115200: termios.B115200,
    230400: termios.B230400,
}

DEFAULT_DEVICE = "/dev/cu.usbserial-0001"
DEFAULT_BAUD = 115200
OPENOCD_BOARD_CFG = "board/stm32f4discovery.cfg"


def open_serial(device: str, baud: int) -> int:
    """Open @device and apply 8N1 raw termios at @baud to the open descriptor."""
    if baud not in BAUD_CONSTANTS:
        raise ValueError(f"unsupported baud {baud}; known: {sorted(BAUD_CONSTANTS)}")

    fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)

    attrs = termios.tcgetattr(fd)
    attrs[0] = termios.IGNPAR                                  # iflag
    attrs[1] = 0                                               # oflag: raw
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL    # cflag: 8N1, no modem ctrl
    attrs[3] = 0                                               # lflag: non-canonical
    attrs[4] = BAUD_CONSTANTS[baud]                            # ispeed
    attrs[5] = BAUD_CONSTANTS[baud]                            # ospeed
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIFLUSH)

    return fd


def reset_target(cwd: str | Path) -> bool:
    """Pulse the target reset via OpenOCD. Returns True on success."""
    try:
        result = subprocess.run(
            ["openocd", "-f", OPENOCD_BOARD_CFG, "-c", "init; reset; exit"],
            cwd=str(cwd),
            capture_output=True,
            timeout=30,
        )
        return result.returncode == 0
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return False


def capture(
    device: str,
    seconds: float,
    out_path: str | Path,
    baud: int = DEFAULT_BAUD,
    reset_cwd: str | Path | None = None,
) -> tuple[int, int]:
    """Capture for @seconds into @out_path. Returns (bytes, sample lines)."""
    out = Path(out_path)
    out.parent.mkdir(parents=True, exist_ok=True)

    fd = open_serial(device, baud)
    try:
        # The device has no RTC, so its timestamps are microseconds since ITS
        # boot. Record the host wall-clock now so absolute time is recoverable.
        started = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

        with open(out, "wb") as sink:
            sink.write(f"# capture_start_utc={started}\n".encode())
            sink.flush()

            if reset_cwd is not None:
                # The port is already open and draining, so everything the board
                # emits from reset onward lands in this capture.
                #
                # Do NOT flush after the reset: OpenOCD takes seconds to run, the
                # board boots and prints its session header during that window,
                # and a flush here would discard exactly the line the parser
                # needs to start a session. The input buffer was already flushed
                # in open_serial(), which is the right place for it.
                if not reset_target(reset_cwd):
                    print("warning: reset failed; capturing mid-run", file=sys.stderr)

            total = 0
            deadline = time.time() + seconds
            while time.time() < deadline:
                ready, _, _ = select.select([fd], [], [], 0.5)
                if not ready:
                    continue
                try:
                    chunk = os.read(fd, 65536)
                except (BlockingIOError, OSError):
                    continue
                if chunk:
                    sink.write(chunk)
                    total += len(chunk)
    finally:
        os.close(fd)

    samples = sum(
        1 for line in out.read_text(errors="replace").splitlines()
        if line.startswith("S ")
    )
    return total, samples


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("seconds", type=float, nargs="?", default=120)
    parser.add_argument("--device", default=DEFAULT_DEVICE)
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    parser.add_argument("--out", default=None)
    parser.add_argument(
        "--reset-from",
        default=None,
        metavar="C_SRC_DIR",
        help="pulse target reset via OpenOCD from this directory, so the "
             "capture starts at the session header",
    )
    args = parser.parse_args()

    if not os.path.exists(args.device):
        print(f"No such device: {args.device}", file=sys.stderr)
        return 1

    out = args.out or (
        Path(__file__).resolve().parent.parent
        / "captures"
        / f"{datetime.now().strftime('%Y%m%d-%H%M%S')}.log"
    )

    print(f"Capturing {args.seconds:g}s from {args.device} at {args.baud} -> {out}")
    total, samples = capture(
        args.device, args.seconds, out, args.baud, args.reset_from
    )
    print(f"Done. {total} bytes, {samples} sample records.")
    print(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
