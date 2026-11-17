"""Summarize a logic analyzer's digital CSV capture of lcd-dma-benchmark."""

import argparse
import csv
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
from pathlib import Path
from statistics import mean

TIME_SCALE = {unit: Decimal(scale) for unit, scale in (
    ("s", "1"), ("ms", "1e-3"), ("us", "1e-6"), ("ns", "1e-9")
)}


@dataclass(frozen=True)
class Frame:
    mode: int
    duration: Decimal
    cpu_time: Decimal


def read_frames(path: Path, time_column: str, frame_column: str,
                cpu_column: str, mode_column: str, time_scale: Decimal) -> list[Frame]:
    frames = []
    previous_time = None
    previous_frame = previous_cpu = 0
    start = None
    cpu_time = Decimal(0)
    frame_mode = 0

    with path.open(newline="", encoding="utf-8-sig") as stream:
        rows = csv.DictReader(stream, skipinitialspace=True)
        required = (time_column, frame_column, cpu_column, mode_column)
        missing = [name for name in required if name not in (rows.fieldnames or [])]
        if missing:
            raise ValueError(f"CSV columns not found: {', '.join(missing)}")

        for row in rows:
            try:
                now = Decimal(row[time_column]) * time_scale
                frame, cpu, mode = (int(row[name]) for name in required[1:])
                if not now.is_finite() or any(x not in (0, 1) for x in (frame, cpu, mode)):
                    raise ValueError
            except (InvalidOperation, TypeError, ValueError) as error:
                raise ValueError(f"row {rows.line_num}: expected a numeric timestamp and digital 0/1 values") from error

            if previous_time is not None:
                if now < previous_time:
                    raise ValueError(f"row {rows.line_num}: timestamps must be in order")
                if start is not None:
                    if previous_cpu:
                        cpu_time += now - previous_time
                    if not frame:
                        if now <= start:
                            raise ValueError(f"row {rows.line_num}: empty frame interval")
                        frames.append(Frame(frame_mode, now - start, cpu_time))
                        start = None
                    elif mode != frame_mode:
                        raise ValueError(f"row {rows.line_num}: Mode changed during a frame")

                if frame and not previous_frame:
                    start = now
                    frame_mode = mode
                    cpu_time = Decimal(0)

            previous_time, previous_frame, previous_cpu = now, frame, cpu

    # The first and last frames may be clipped by the capture boundaries.
    return frames


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv_path", type=Path)
    parser.add_argument("--time", default="Time [s]", help="timestamp column")
    parser.add_argument("--time-unit", choices=TIME_SCALE, default="s", help="unit of timestamps")
    parser.add_argument("--frame", default="Frame", help="rising/falling edge marks a refresh")
    parser.add_argument("--cpu", default="CPU", help="high means LCD CPU activity")
    parser.add_argument("--mode", default="Mode", help="0 = polling, 1 = DMA")
    args = parser.parse_args()

    try:
        if len({args.time, args.frame, args.cpu, args.mode}) != 4:
            raise ValueError("time, frame, CPU and mode must be distinct columns")
        frames = read_frames(args.csv_path, args.time, args.frame, args.cpu, args.mode,
                             TIME_SCALE[args.time_unit])
        for mode, label in ((0, "polling"), (1, "DMA")):
            if not any(frame.mode == mode for frame in frames):
                raise ValueError(f"no complete {label} frame; capture more cycles")
    except (OSError, ValueError) as error:
        parser.error(str(error))

    print(f"{'Mode':<8} {'Frames':>6} {'Frame ms':>12} {'CPU ms':>12} {'CPU/frame':>11} {'CPU/500ms':>11}")
    cpu_means = []
    for mode, label in ((0, "Polling"), (1, "DMA")):
        selected = [frame for frame in frames if frame.mode == mode]
        frame_ms = mean(frame.duration for frame in selected) * 1000
        cpu_ms = mean(frame.cpu_time for frame in selected) * 1000
        cpu_means.append(cpu_ms)
        print(f"{label:<8} {len(selected):>6} {frame_ms:>12.4f} {cpu_ms:>12.4f} "
              f"{cpu_ms / frame_ms:>10.2%} {cpu_ms / 500:>10.2%}")

    if cpu_means[0]:
        saved = (cpu_means[0] - cpu_means[1]) / cpu_means[0]
        print(f"LCD CPU time saved per frame: {saved:.2%}")


if __name__ == "__main__":
    main()
