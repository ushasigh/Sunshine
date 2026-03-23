#!/usr/bin/env python3
"""Plot per-frame VMAF scores from a VMAF log file."""

from __future__ import annotations

import argparse
import ast
import re
from pathlib import Path

FONT_SIZE = 20

HEADER_RE = re.compile(r"^(?P<key>[a-zA-Z0-9_]+)=(?P<value>.+)$")
FRAME_RE = re.compile(
    r"frame=(?P<frame>\d+),\s*vmaf=(?P<vmaf>-?\d+(?:\.\d+)?)(?:,\s*ssim=-?\d+(?:\.\d+)?)?(?:,\s*frame_nr=(?P<frame_nr>\d+))?"
)


def parse_vmaf_log(log_path: Path) -> tuple[dict[str, str], list[int], list[int | None], list[float]]:
    runs: list[tuple[dict[str, str], list[int], list[int | None], list[float]]] = []
    metadata: dict[str, str] = {}
    frames: list[int] = []
    frame_nrs: list[int | None] = []
    scores: list[float] = []

    with log_path.open("r", encoding="utf-8") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            header_match = HEADER_RE.match(line)
            if header_match:
                key = header_match.group("key")
                value = header_match.group("value")
                if key == "timestamp" and (metadata or frames):
                    runs.append((metadata, frames, frame_nrs, scores))
                    metadata = {}
                    frames = []
                    frame_nrs = []
                    scores = []
                metadata[key] = value

            match = FRAME_RE.search(raw_line)
            if match:
                frames.append(int(match.group("frame")))
                frame_nrs.append(int(match.group("frame_nr")) if match.group("frame_nr") else None)
                scores.append(float(match.group("vmaf")))

    if metadata or frames:
        runs.append((metadata, frames, frame_nrs, scores))

    valid_runs = [run for run in runs if run[1]]
    if not valid_runs:
        raise ValueError(f"No per-frame VMAF entries found in {log_path}")

    return valid_runs[-1]


def build_summary(metadata: dict[str, str], frames: list[int], scores: list[float]) -> str:
    mean_score = sum(scores) / len(scores)
    variance = sum((score - mean_score) ** 2 for score in scores) / len(scores)
    first_line_parts = [
        f"frames={len(frames)}",
        f"min={min(scores):.4f}",
        f"max={max(scores):.4f}",
        f"mean={mean_score:.4f}",
        f"variance={variance:.4f}",
    ]
    second_line_parts: list[str] = []
    if "vmaf_p5" in metadata:
        second_line_parts.append(f"p5={float(metadata['vmaf_p5']):.4f}")
    if "vmaf_p25" in metadata:
        second_line_parts.append(f"p25={float(metadata['vmaf_p25']):.4f}")

    lines = ["  ".join(first_line_parts)]
    if second_line_parts:
        lines.append("  ".join(second_line_parts))
    return "\n".join(lines)


def get_drop_summary(metadata: dict[str, str]) -> str | None:
    matched = int(metadata.get("alignment_matched", "0"))
    dropped = int(metadata.get("alignment_dropped", "0"))
    total = matched + dropped
    if total <= 0:
        return None
    drop_rate = dropped / total * 100.0
    return f"Frame drop rate: {dropped}/{total} ({drop_rate:.2f}%)"


def get_x_values(frames: list[int], frame_nrs: list[int | None]) -> list[int]:
    if frame_nrs and all(frame_nr is not None for frame_nr in frame_nrs):
        return [int(frame_nr) for frame_nr in frame_nrs]
    return frames


def get_drop_positions(metadata: dict[str, str], x_values: list[int]) -> list[int]:
    drop_positions: set[int] = set()

    if x_values:
        for prev_value, next_value in zip(x_values, x_values[1:]):
            if next_value > prev_value + 1:
                drop_positions.update(range(prev_value + 1, next_value))

    raw_dropped = metadata.get("dropped_frame_nrs", "")
    if raw_dropped:
        try:
            parsed = ast.literal_eval(raw_dropped)
        except (SyntaxError, ValueError):
            parsed = []
        if isinstance(parsed, list):
            for value in parsed:
                if isinstance(value, int):
                    drop_positions.add(value)

    return sorted(drop_positions)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Read per-frame VMAF scores from a log and plot frame vs VMAF."
    )
    parser.add_argument(
        "log_file",
        nargs="?",
        default="vmaf_same_4k_v0.6.1_results.log",
        help="Path to the VMAF log file.",
    )
    parser.add_argument(
        "-o",
        "--output",
        default="",
        help="Output image path. Defaults to <log file stem>.png.",
    )
    parser.add_argument(
        "--title",
        default="VMAF per Frame",
        help="Plot title.",
    )
    parser.add_argument(
        "--show",
        action="store_true",
        help="Show the plot window after saving.",
    )
    return parser


def configure_matplotlib(show: bool):
    import matplotlib

    if not show:
        matplotlib.use("Agg")

    import matplotlib.pyplot as plt

    return plt


def main() -> None:
    args = build_parser().parse_args()
    log_path = Path(args.log_file)
    output_path = Path(args.output) if args.output else log_path.with_suffix(".png")
    plt = configure_matplotlib(args.show)

    metadata, frames, frame_nrs, scores = parse_vmaf_log(log_path)
    x_values = get_x_values(frames, frame_nrs)
    drop_positions = get_drop_positions(metadata, x_values)

    fig, ax = plt.subplots(figsize=(14, 8))
    ax.set_zorder(1)
    ax.patch.set_alpha(0)
    ax.set_title(args.title, fontsize=FONT_SIZE)
    ax.set_xlabel("frame_nr", fontsize=FONT_SIZE)
    ax.set_ylabel("VMAF", fontsize=FONT_SIZE)
    x_max = max(drop_positions) if drop_positions else max(x_values)
    ax.set_xlim(min(x_values), x_max)
    ax.set_ylim(0, 100)
    ax.grid(True, linestyle="--", alpha=0.35)
    ax.tick_params(axis="both", labelsize=FONT_SIZE)

    loss_ax = ax.twinx()
    loss_ax.set_zorder(2)
    loss_ax.patch.set_alpha(0)
    if drop_positions:
        loss_ax.vlines(drop_positions, 0, 1, color="#d62728", linewidth=1.0, alpha=0.35, zorder=3)
    loss_ax.set_ylim(0, 1.0)
    loss_ax.set_ylabel("Frame loss", fontsize=FONT_SIZE)
    loss_ax.set_yticks([0, 1])
    loss_ax.tick_params(axis="y", labelsize=FONT_SIZE)
    ax.fill_between(x_values, scores, color="#1f77b4", alpha=0.15, zorder=1)
    ax.plot(x_values, scores, color="#1f77b4", linewidth=1.5, zorder=2)

    summary = build_summary(metadata, frames, scores)
    drop_summary = get_drop_summary(metadata)
    annotation = summary if not drop_summary else f"{summary}\n{drop_summary}"
    fig.subplots_adjust(bottom=0.28)
    fig.text(
        0.5,
        0.015,
        annotation,
        transform=fig.transFigure,
        ha="center",
        va="bottom",
        multialignment="left",
        fontsize=FONT_SIZE,
        zorder=4,
        bbox={"boxstyle": "round", "facecolor": "white", "alpha": 0.85, "edgecolor": "#cccccc"},
    )

    fig.savefig(output_path, dpi=160)
    print(f"Saved plot to {output_path}")
    if drop_summary:
        print(drop_summary)

    if args.show:
        plt.show()
    else:
        plt.close(fig)


if __name__ == "__main__":
    main()
