"""Render the figure set the report uses, from named captures.

    .venv/bin/python make_report_figures.py figures/v2.0.0 captures/a.log [captures/b.log ...]

The first capture drives the single-run charts (plot.report); every capture given
contributes to the cross-run noise chart, which is the only honest way to show a
per-mode noise figure whose run-to-run scatter is comparable to the datasheet
separation between adjacent modes.

Two more charts come from every capture in the same directory that recorded a
complete sweep, so the named run is shown against the project's whole measured
history rather than on its own.
"""

from __future__ import annotations

import glob
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd

from bmp180_analysis import metrics, plot
from bmp180_analysis.parse import parse_file

SWEEP_BLOCKS = 4
SWEEP_N = 500

# Where the temperature cache was introduced. Before it, temp_ms is 0, every
# sample carries its own temperature conversion, and segment_rms_pa is NaN by
# design - segments are two or three samples long and their spread understates
# the noise. Those captures are compared on rms_pa instead, per TESTING.md
# section 8, and the charts say which is which rather than blending them.
CACHE_FROM = (1, 4, 0)


def version_key(fw: str):
    return tuple(int(part) for part in fw.split("."))


def full_sweep(path: str):
    """Sweep-block summary for a capture, or None if it recorded no full sweep.

    Half the committed captures are deliberate failures - wedged buses, resets
    landing mid-transfer - and a partial sweep would otherwise contribute a
    block that is not comparable with anything.
    """
    sessions = parse_file(path)
    if not sessions:
        return None
    session = sessions[-1]
    if not len(session.samples):
        return None

    summary = metrics.per_block_summary(session).head(SWEEP_BLOCKS)
    if len(summary) < SWEEP_BLOCKS:
        return None
    if list(summary.oss) != list(range(SWEEP_BLOCKS)):
        return None
    if not all(summary.n == SWEEP_N):
        return None

    summary = summary.copy()
    summary["fw"] = session.fw
    summary["temp_ms"] = session.temp_ms
    summary["noise_pa"] = summary.segment_rms_pa.fillna(summary.rms_pa)
    return summary


def history(capture_dir: Path):
    """Every full sweep in capture_dir, oldest firmware first."""
    found = [full_sweep(p) for p in sorted(glob.glob(str(capture_dir / "*.log")))]
    found = [f for f in found if f is not None]
    if not found:
        return None
    table = pd.concat(found, ignore_index=True)
    order = sorted(table.fw.unique(), key=version_key)
    return table, order


def plot_interval_history(table, order, out: Path) -> Path:
    """Acquisition cycle time per mode, across every firmware measured."""
    fig, ax = plt.subplots(figsize=(9, 4.5))
    x = range(len(order))

    for oss in range(SWEEP_BLOCKS):
        per_fw = [table[(table.fw == fw) & (table.oss == oss)].median_interval_us.mean() / 1000.0
                  for fw in order]
        ax.plot(x, per_fw, marker="o", label=f"oss={oss}")

    ax.set_xticks(list(x))
    ax.set_xticklabels(order)
    ax.set_xlabel("firmware version")
    ax.set_ylabel("median acquisition interval [ms]")
    ax.set_title("Acquisition cycle time per mode, by firmware")
    ax.grid(axis="y", alpha=0.3)
    ax.legend()
    fig.tight_layout()

    path = out / "interval_history.png"
    fig.savefig(path, dpi=120)
    plt.close(fig)
    return path


def plot_noise_history(table, order, out: Path) -> Path:
    """Per-mode noise across every firmware, one panel per mode.

    Points are individual captures, the line joins the per-firmware mean, and the
    shaded span marks the releases whose temperature cache is on - the two eras
    are measured with different columns and must not be read as one series.
    """
    fig, axes = plt.subplots(2, 2, figsize=(11, 7), sharex=True)
    x = {fw: i for i, fw in enumerate(order)}
    cache_start = next((i for i, fw in enumerate(order)
                        if version_key(fw) >= CACHE_FROM), len(order))

    for oss, ax in zip(range(SWEEP_BLOCKS), axes.flat):
        rows = table[table.oss == oss]
        ax.axvspan(cache_start - 0.5, len(order) - 0.5, color="0.92",
                   label="temperature cache on")
        ax.plot([x[fw] for fw in rows.fw], rows.noise_pa, ".", color="0.4",
                markersize=8, label="captures")
        means = [rows[rows.fw == fw].noise_pa.mean() for fw in order]
        ax.plot(range(len(order)), means, marker="o", label="mean")
        ax.axhline(rows.datasheet_rms_pa.iloc[0], linestyle="--", color="crimson",
                   label="datasheet typ.")
        ax.set_title(f"oss={oss}")
        ax.set_ylabel("RMS noise [Pa]")
        ax.grid(axis="y", alpha=0.3)

    for ax in axes[1]:
        ax.set_xticks(range(len(order)))
        ax.set_xticklabels(order, rotation=45, ha="right")
        ax.set_xlabel("firmware version")

    axes[0][0].legend(fontsize=8)
    fig.suptitle("Pressure noise per mode, by firmware "
                 "(rms_pa before 1.4.0, segment_rms_pa from 1.4.0)")
    fig.tight_layout()

    path = out / "noise_history.png"
    fig.savefig(path, dpi=120)
    plt.close(fig)
    return path


def sweep_noise(path: str):
    """Per-mode noise for one capture's sweep blocks, drift-corrected."""
    session = parse_file(path)[-1]
    summary = metrics.per_block_summary(session).head(SWEEP_BLOCKS)
    noise = summary.segment_rms_pa.fillna(summary.rms_pa)
    return summary.oss.tolist(), noise.tolist(), summary.datasheet_rms_pa.tolist()


def plot_noise_across_runs(paths: list[str], out: Path) -> Path:
    fig, ax = plt.subplots(figsize=(7, 4))
    width = 0.8 / (len(paths) + 1)

    datasheet = None
    for i, path in enumerate(paths):
        oss, noise, ds = sweep_noise(path)
        datasheet = ds
        offset = (i - len(paths) / 2) * width
        ax.bar([o + offset for o in oss], noise, width=width,
               label=f"run {i + 1}")

    offset = (len(paths) - len(paths) / 2) * width
    ax.bar([o + offset for o in oss], datasheet, width=width,
           color="0.6", label="datasheet typ.")

    ax.set_xlabel("oversampling setting")
    ax.set_ylabel("RMS pressure noise [Pa]")
    ax.set_xticks(oss)
    ax.set_title("Pressure noise vs oversampling, three runs")
    ax.legend()
    fig.tight_layout()

    path = out / "noise_across_runs.png"
    fig.savefig(path, dpi=120)
    plt.close(fig)
    return path


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2

    out = Path(argv[1])
    captures = argv[2:]
    out.mkdir(parents=True, exist_ok=True)

    written = plot.report(parse_file(captures[0])[-1], out)
    written.append(plot_noise_across_runs(captures, out))

    found = history(Path(captures[0]).parent)
    if found is not None:
        table, order = found
        written.append(plot_interval_history(table, order, out))
        written.append(plot_noise_history(table, order, out))

    for path in written:
        print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
