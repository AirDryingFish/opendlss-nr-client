"""Check that the preview actually presents each generated phase in time order (Xvfb + GPU)."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

from PIL import ImageGrab
import live_framegen_smoke as smoke


def verify(source, scratch, multiplier):
    source.move(64)
    step = 24
    bounds = None
    positions = []

    def inspect():
        nonlocal bounds
        if not bounds:
            window = smoke.preview_window()
            if window:
                bounds = window[2]
            return
        x, y, width, _ = bounds
        row = y + smoke.TOP + smoke.TILE // 2
        strip = ImageGrab.grab(bbox=(x, row, x + width, row + 1))
        red = strip.convert("RGB").tobytes()[::3]
        left = next((i for i, value in enumerate(red) if value >= 175), None)
        if left is None:
            return
        if not positions or abs(positions[-1] - left) > 2:
            positions.append(left)
        if abs(left - source.x) <= 2 and source.x < 64 + step * 8:
            source.move(source.x + step)

    stats, _ = smoke.run_capture(source, scratch, f"presentation-{multiplier}", 21, 0, 1,
                                 inspect, fps=10, multiplier=multiplier)
    complete = 0
    # A completed interval has its two real endpoints and every interior position in order.
    for interval in range(8):
        start = 64 + interval * step
        expected = [start + step * i / multiplier for i in range(multiplier + 1)]
        if any(all(abs(actual - wanted) <= 2 for actual, wanted in zip(positions[index:], expected))
               for index in range(len(positions) - multiplier)):
            complete += 1
    assert complete >= 4, (multiplier, complete, positions, stats)
    assert stats["generated"] == 8 * (multiplier - 1), stats
    print(f"{smoke.BACKEND} {multiplier}x: saw {complete}/8 complete ordered intervals; {stats}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--backend", choices=("flow", "rife", "mako"), default="flow")
    parser.add_argument("--fixture", action="store_true")
    args = parser.parse_args()
    if not args.fixture:
        subprocess.run(["xvfb-run", "-a", "-s", "-screen 0 1440x760x24", sys.executable,
                        str(Path(__file__).resolve()), "--fixture", "--backend", args.backend], check=True)
        return
    smoke.BACKEND = args.backend
    source = smoke.Source()
    try:
        with tempfile.TemporaryDirectory() as tmp:
            for multiplier in (3, 4):
                verify(source, Path(tmp), multiplier)
    finally:
        source.window.destroy()


if __name__ == "__main__":
    main()
