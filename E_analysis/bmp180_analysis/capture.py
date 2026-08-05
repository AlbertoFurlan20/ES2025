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
import errno
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


class PortBusy(Exception):
    """The serial device is held by another process."""


def _port_holder(device: str) -> str | None:
    """Best-effort description of whatever currently holds @device."""
    try:
        out = subprocess.run(
            ["lsof", "-F", "cp", device],
            capture_output=True, text=True, timeout=5,
        ).stdout
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return None

    pid = name = None
    for line in out.splitlines():
        if line.startswith("p"):
            pid = line[1:]
        elif line.startswith("c"):
            name = line[1:]
    return f"{name} (pid {pid})" if pid and name else None


def open_serial(device: str, baud: int) -> int:
    """Open @device and apply 8N1 raw termios at @baud to the open descriptor."""
    if baud not in BAUD_CONSTANTS:
        raise ValueError(f"unsupported baud {baud}; known: {sorted(BAUD_CONSTANTS)}")

    try:
        fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    except OSError as exc:
        # macOS gives exclusive access to /dev/cu.*, so a `screen` left open on
        # the console blocks every capture until it is quit. Say so, and name
        # the culprit, instead of raising a traceback at the operator.
        if exc.errno != errno.EBUSY:
            raise
        holder = _port_holder(device)
        held_by = f" held by {holder}" if holder else ""
        raise PortBusy(
            f"{device} is busy{held_by}.\n"
            f"  Close it and retry. If it is a screen session: "
            f"screen -X -S <pid> quit  (or Ctrl-A then \\ in that window)"
        ) from exc

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


PROGRESS_BAR_WIDTH = 28

# No `S` record for this long means the run is dead, not merely quiet: the
# slowest configured mode still emits ~30 samples/s.
DEFAULT_SILENCE_TIMEOUT = 10.0

# errno values in an `E` record that mean the run can never produce data, so
# there is nothing to wait for.
FATAL_ERRNOS = {
    19: "ENODEV - BMP180 registration failed (chip-id read did not answer)",
    2: "ENOENT - device node missing",
}


def _fatal_record(line: bytes) -> str | None:
    """Return a reason string if @line is an unrecoverable error, else None.

    Only `E` records are consulted. A registration failure is reported by the
    firmware exactly once, at boot, and no sample can follow it.
    """
    if not line.startswith(b"E "):
        return None
    parts = line.split()
    if len(parts) < 3:
        return None
    try:
        err = int(parts[2])
    except ValueError:
        return None
    detail = FATAL_ERRNOS.get(err)
    return f"device reported E {err}: {detail}" if detail else None


def _draw_progress(
    elapsed: float,
    total_seconds: float,
    total_bytes: int,
    samples: int,
    final: bool = False,
) -> None:
    """Redraw the in-place progress line on stderr.

    stderr, not stdout, so that `capture.sh > somewhere` keeps a clean output
    path. Skipped entirely when stderr is not a terminal, since a redirected log
    full of carriage returns is worse than no progress at all.
    """
    if not sys.stderr.isatty():
        return

    frac = 1.0 if total_seconds <= 0 else min(1.0, elapsed / total_seconds)
    filled = int(round(frac * PROGRESS_BAR_WIDTH))
    bar = "#" * filled + "-" * (PROGRESS_BAR_WIDTH - filled)
    remaining = max(0.0, total_seconds - elapsed)
    rate = samples / elapsed if elapsed > 0 else 0.0

    # \r returns to column 0 and \x1b[K erases to end of line, so each redraw
    # replaces the previous one in place and leaves no residue even if a later
    # line is shorter. Only the final draw emits a newline.
    sys.stderr.write(
        f"\r\x1b[K  [{bar}] {frac * 100:5.1f}%  "
        f"{elapsed:6.1f}/{total_seconds:.0f}s  "
        f"-{remaining:5.1f}s  "
        f"{total_bytes / 1024:7.1f} KiB  "
        f"{samples:6d} samples  {rate:5.1f}/s"
    )
    sys.stderr.write("\n" if final else "")
    sys.stderr.flush()


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
    silence_timeout: float = DEFAULT_SILENCE_TIMEOUT,
) -> tuple[int, int, str | None]:
    """Capture for @seconds into @out_path.

    Returns (bytes, sample lines, abort_reason). @abort_reason is None on a
    healthy run, otherwise a one-line description and the capture stopped early.

    A capture aborts rather than running to completion when the board is
    demonstrably not producing data. Sitting out a 150 s window recording a
    sensor that failed to register wastes the operator's time and leaves a log
    that looks like a result.
    """
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
                print("  resetting target over ST-Link...", file=sys.stderr, flush=True)
                if not reset_target(reset_cwd):
                    print("warning: reset failed; capturing mid-run", file=sys.stderr)

            total = 0
            live_samples = 0
            pending = b""        # incomplete trailing line, carried between reads
            start = time.time()
            deadline = start + seconds
            next_draw = 0.0
            last_sample_at = start
            abort: str | None = None

            while abort is None:
                now = time.time()
                if now >= deadline:
                    break

                # Wake often enough to keep the progress line moving even when
                # the board is silent, but never past the deadline.
                ready, _, _ = select.select([fd], [], [], min(0.2, deadline - now))
                if ready:
                    try:
                        chunk = os.read(fd, 65536)
                    except (BlockingIOError, OSError):
                        chunk = b""
                    if chunk:
                        sink.write(chunk)
                        sink.flush()
                        total += len(chunk)

                        pending += chunk
                        lines = pending.split(b"\n")
                        pending = lines.pop()
                        for line in lines:
                            if line.startswith(b"S "):
                                live_samples += 1
                                last_sample_at = time.time()
                            else:
                                abort = _fatal_record(line) or abort

                now = time.time()
                if abort is None and now - last_sample_at > silence_timeout:
                    abort = (
                        f"no sample records for {silence_timeout:g}s"
                        if live_samples
                        else f"no sample records at all within {silence_timeout:g}s"
                    )

                if now >= next_draw or abort is not None:
                    _draw_progress(min(now - start, seconds), seconds, total,
                                   live_samples, final=abort is not None)
                    next_draw = now + 0.1

            if abort is None:
                _draw_progress(seconds, seconds, total, live_samples, final=True)
    finally:
        os.close(fd)

    samples = sum(
        1 for line in out.read_text(errors="replace").splitlines()
        if line.startswith("S ")
    )
    return total, samples, abort


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("seconds", type=float, nargs="?", default=120)
    parser.add_argument("--device", default=DEFAULT_DEVICE)
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    parser.add_argument("--out", default=None)
    parser.add_argument(
        "--silence-timeout",
        type=float,
        default=DEFAULT_SILENCE_TIMEOUT,
        metavar="SECONDS",
        help="abort if no sample record arrives for this long "
             f"(default {DEFAULT_SILENCE_TIMEOUT:g})",
    )
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
    try:
        total, samples, abort = capture(
            args.device, args.seconds, out, args.baud, args.reset_from,
            args.silence_timeout,
        )
    except PortBusy as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    if abort is not None:
        print(f"ABORTED: {abort}", file=sys.stderr)
        print(f"  {total} bytes, {samples} sample records written to {out}",
              file=sys.stderr)
        print("  Check the sensor wiring (I2C1: PB6=SCL, PB7=SDA, addr 0x77) "
              "and that it is powered.", file=sys.stderr)
        return 1

    print(f"Done. {total} bytes, {samples} sample records.")
    print(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
