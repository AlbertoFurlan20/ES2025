"""Charts for a parsed telemetry Session.

Plotting only: every number shown here comes from metrics.py, so the figures can
be checked without rendering anything.
"""

from __future__ import annotations

from pathlib import Path

import matplotlib

matplotlib.use("Agg")  # file output; no display needed
import matplotlib.pyplot as plt

from . import metrics
from .parse import Session


def plot_noise_vs_oss(session: Session, ax=None):
    """Measured RMS pressure noise per mode against the datasheet reference."""
    if ax is None:
        _, ax = plt.subplots(figsize=(6, 4))

    summary = metrics.per_oss_summary(session)
    ax.bar(summary.oss - 0.2, summary.rms_pa, width=0.4, label="measured")
    ax.bar(summary.oss + 0.2, summary.datasheet_rms_pa, width=0.4,
           label="datasheet typ.")
    ax.set_xlabel("oversampling setting")
    ax.set_ylabel("RMS pressure noise [Pa]")
    ax.set_xticks(summary.oss)
    ax.set_title("Pressure noise vs oversampling")
    ax.legend()
    return ax


def plot_pressure_series(session: Session, ax=None):
    """Pressure against time, coloured by oversampling mode."""
    if ax is None:
        _, ax = plt.subplots(figsize=(10, 4))

    samples = session.samples
    seconds = (samples.t_us - samples.t_us.iloc[0]) / 1_000_000.0
    for oss in sorted(samples.oss.unique()):
        mask = samples.oss == oss
        ax.plot(seconds[mask], samples.p_pa[mask], ".", markersize=2,
                label=f"oss={oss}")

    ax.set_xlabel("time since first sample [s]")
    ax.set_ylabel("pressure [Pa]")
    ax.set_title("Pressure")
    ax.legend()
    return ax


def plot_interval_hist(session: Session, oss: int, ax=None):
    """Distribution of inter-sample interval for one mode."""
    if ax is None:
        _, ax = plt.subplots(figsize=(6, 4))

    intervals = metrics.intervals_us(session, oss) / 1000.0
    ax.hist(intervals, bins=50)
    ax.set_xlabel("inter-sample interval [ms]")
    ax.set_ylabel("count")
    ax.set_title(f"Acquisition interval, oss={oss}")
    return ax


def plot_temperature(session: Session, ax=None):
    """Temperature against time - the self-heating check."""
    if ax is None:
        _, ax = plt.subplots(figsize=(10, 3))

    samples = session.samples
    seconds = (samples.t_us - samples.t_us.iloc[0]) / 1_000_000.0
    ax.plot(seconds, samples.t_cdeg / 10.0, ".", markersize=2)
    ax.set_xlabel("time since first sample [s]")
    ax.set_ylabel("temperature [degC]")

    drift = metrics.temperature_drift(session)
    ax.set_title(f"Temperature (drift {drift['degc_per_min']:+.3f} degC/min)")
    return ax


def report(session: Session, out_dir: str | Path) -> list[Path]:
    """Write every chart to out_dir and return the paths written."""
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []

    for name, fn in (
        ("noise_vs_oss.png", plot_noise_vs_oss),
        ("pressure_series.png", plot_pressure_series),
        ("temperature.png", plot_temperature),
    ):
        fig, ax = plt.subplots(figsize=(10, 4))
        fn(session, ax=ax)
        fig.tight_layout()
        path = out / name
        fig.savefig(path, dpi=120)
        plt.close(fig)
        written.append(path)

    for oss in sorted(session.samples.oss.unique()):
        fig, ax = plt.subplots(figsize=(6, 4))
        plot_interval_hist(session, int(oss), ax=ax)
        fig.tight_layout()
        path = out / f"interval_oss{int(oss)}.png"
        fig.savefig(path, dpi=120)
        plt.close(fig)
        written.append(path)

    return written
