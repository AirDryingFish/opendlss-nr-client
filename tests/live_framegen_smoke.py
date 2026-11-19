"""Select an installed interpolator with --backend. Requires Xvfb, Tk, Pillow and local NR weights."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

from live_capture_smoke import ROOT, read_ppm


WIDTH, HEIGHT = 480, 320
TILE = 80
TOP = 110
BACKEND = "flow"


def preview_window():
    tree = subprocess.check_output(["xwininfo", "-root", "-tree"], text=True)
    match = re.search(
        r'(0x[0-9a-f]+) "(DLSS 5 Live[^"]*)".*?\s(\d+)x(\d+)[+-]\d+[+-]\d+\s+([+-]\d+)([+-]\d+)',
        tree,
    )
    if match:
        xid, title, w, h, x, y = match.groups()
        return int(xid, 16), title, tuple(map(int, (x, y, w, h)))
    return None


class Source:
    def __init__(self):
        import tkinter as tk

        self.window = tk.Tk()
        self.window.overrideredirect(True)
        self.window.geometry(f"{WIDTH}x{HEIGHT}+8+8")
        self.canvas = tk.Canvas(self.window, background="#171717", borderwidth=0, highlightthickness=0)
        self.canvas.pack(fill="both", expand=True)
        self.x = 64
        self.canvas.create_rectangle(self.x, TOP, self.x + TILE, TOP + TILE, fill="#f3c1eb",
                                     outline="", tags="tile")
        # Nonrepeating high-contrast detail gives the optical flow unit trackable features.
        for i in range(7):
            for j in range(7):
                if (7*i + 11*j + i*j) % 5 < 2:
                    x = self.x + i*10 + 4
                    y = TOP + j*10 + 4
                    self.canvas.create_rectangle(x, y, x + 5, y + 7, fill="#ac92dc", outline="", tags="tile")
        self.window.update()

    def move(self, x):
        self.canvas.move("tile", x - self.x, 0)
        self.x = x
        self.window.update()

    def step(self, delay=0.004):
        self.window.update()
        time.sleep(delay)


def run_capture(source, scratch, label, frames, nr, framegen, inspect=None, *, fps=16, multiplier=2):
    output = scratch / f"{label}.ppm"
    log = scratch / f"{label}.log"
    source_args = ["--region", f"8,8,{WIDTH},{HEIGHT}"] if nr else ["--window", hex(source.canvas.winfo_id())]
    argv = [str(ROOT / "scripts/run-client.sh"), "--live", *source_args,
            "--frame-gen", str(framegen), "--fg-backend", BACKEND, "--nr", str(nr), "--fps", str(fps),
            "--fg-multiplier", str(multiplier), "--frames", str(frames), "--output", str(output)]
    with log.open("w") as stream:
        child = subprocess.Popen(argv, cwd=scratch, stdout=stream, stderr=subprocess.STDOUT)
        try:
            start = time.monotonic()
            while child.poll() is None:
                if time.monotonic() - start > 60:
                    raise TimeoutError(f"{label} timed out")
                if inspect:
                    inspect()
                source.step()
        finally:
            if child.poll() is None:
                child.kill()
            child.wait()
    log_text = log.read_text()
    if child.returncode:
        raise RuntimeError(log_text)
    counts = {k: int(v) for k, v in re.findall(r'\b(captured|duplicates|generated|presented)=(\d+)\b', log_text)}
    assert counts.get("captured") == frames, log_text
    assert counts["presented"] == frames - counts["duplicates"] + counts["generated"], log_text
    return counts, read_ppm(output)


def moving_case(source, scratch, nr, framegen):
    from PIL import ImageGrab

    source.move(64)
    bounds = None
    position = 64
    direction = 1
    shifts = []
    midpoints = set()

    def inspect():
        nonlocal bounds, position, direction
        if not bounds:
            window = preview_window()
            if window:
                bounds = window[2]
            return
        x, y, w, h = bounds
        assert (w, h) == (WIDTH, HEIGHT)
        # Inspect the preview pixels, not a log or a downloaded image. At the
        # chosen threshold a direct dissolve has both end positions or neither.
        row_y = y + TOP + TILE // 2
        row = ImageGrab.grab(bbox=(x, row_y, x + w, row_y+1), xdisplay=os.environ["DISPLAY"])
        row = row.convert("RGB").tobytes()[::3]
        light = [i for i, r in enumerate(row) if r >= 175]
        if not light:
            return
        left, right = light[0], light[-1]
        if shifts:
            a, b = shifts[-1]
            halfway = (a + b) / 2
            if abs(left - halfway) <= 3 and abs(right - (halfway + TILE - 1)) <= 4:
                midpoints.add(len(shifts))
        if abs(left - position) <= 2 and abs(right - (position + TILE - 1)) <= 3:
            if position >= 304:
                direction = -1
            elif position <= 64:
                direction = 1
            next_x = position + direction * 16
            shifts.append((position, next_x))
            source.move(next_x)
            position = next_x

    stats, frame = run_capture(source, scratch, f"moving-nr-{nr}-fg-{framegen}", 36, nr, framegen, inspect)
    assert frame[:2] == (WIDTH, HEIGHT)
    assert len(shifts) >= 6, (stats, shifts)
    if framegen:
        assert stats["generated"] >= 6, (stats, shifts)
        assert len(midpoints) >= 3, f"expected motion-compensated midpoints: {midpoints}, {shifts}, {stats}"
    else:
        assert stats["generated"] == 0 and not midpoints, (stats, midpoints)
    print(f"NR={nr} FG={framegen}: {stats}, saw {len(midpoints)} distinct in-between positions.")


def sustained_motion(source, scratch):
    # Drive the source from an independent 30 Hz clock. Waiting for each
    # presented endpoint would hide a progressively slowing capture loop.
    rate = 30
    frames = 150
    start = time.monotonic()

    def advance():
        tick = int((time.monotonic() - start) * rate)
        # Ping-pong at 16 pixels / input frame with no wraparound jump.
        pos = 64 + 16 * (15 - abs(tick % 30 - 15))
        if pos != source.x:
            source.move(pos)

    stats, _ = run_capture(source, scratch, "pacing", frames, 0, 1, advance, fps=rate)
    log_text = (scratch / "pacing.log").read_text()
    seconds = float(re.search(r"\belapsed=([\d.]+) s", log_text)[1])
    changed = frames - stats["duplicates"]
    source_fps = changed / seconds
    generated_fps = stats["generated"] / seconds
    assert frames / seconds <= rate * 1.15, (seconds, stats)
    assert source_fps >= rate * .8, (source_fps, seconds, stats)
    assert generated_fps >= rate * .7, (generated_fps, seconds, stats)
    print(f"30 Hz motion over {seconds:.2f}s: {source_fps:.1f} changed + {generated_fps:.1f} generated FPS.")


def hard_cuts(source, scratch):
    from PIL import ImageGrab

    phase = 0
    bounds = None

    def inspect():
        nonlocal phase, bounds
        if phase == 2:
            return
        if not bounds:
            window = preview_window()
            if window:
                bounds = window[2]
            return
        x, y = bounds[:2]
        probe = ImageGrab.grab(bbox=(x+15, y+15, x+16, y+16), xdisplay=os.environ["DISPLAY"])
        red = probe.getpixel((0, 0))[0]
        if phase == 0 and red == 23:
            source.canvas.create_rectangle(0, 0, WIDTH, HEIGHT, fill="#eeeeee", outline="", tags="cut")
            phase = 1
        elif phase == 1 and red == 238:
            source.canvas.delete("cut")
            phase = 2

    stats, _ = run_capture(source, scratch, "cuts", 18, 0, 1, inspect)
    assert phase == 2 and stats["generated"] == 0 and stats["presented"] == 3, (phase, stats)


def equal_histogram_cut(source, scratch):
    # These two images have the same exposure and contrast. An ambiguous warp
    # can dissolve the stripes into a gray frame, invisible to a histogram cut detector.
    from PIL import ImageGrab

    canvas = source.canvas

    def stripes(horizontal):
        canvas.delete("stripes")
        canvas.create_rectangle(0, 0, WIDTH, HEIGHT, fill="#070707", outline="", tags="stripes")
        for pos in range(0, HEIGHT if horizontal else WIDTH, 4):
            rect = (0, pos, WIDTH, pos + 1) if horizontal else (pos, 0, pos + 1, HEIGHT)
            canvas.create_rectangle(*rect, fill="#fafafa", outline="", tags="stripes")
        source.window.update()

    stripes(False)
    switched = False

    def inspect():
        nonlocal switched
        if switched:
            return
        preview = preview_window()
        if preview:
            x, y = preview[2][:2]
            row = ImageGrab.grab(bbox=(x + 16, y + 15, x + 17, y + 16), xdisplay=os.environ["DISPLAY"])
            if row.getpixel((0, 0))[0] == 250:
                stripes(True)
                switched = True

    try:
        stats, frame = run_capture(source, scratch, "equal-histogram-cut", 20, 0, 1, inspect)
        assert switched and frame[2][3 * (15 * WIDTH + 16)] == 7
        assert stats["generated"] == 0 and stats["presented"] == 2, stats
    finally:
        canvas.delete("stripes")
        source.window.update()


def pause_and_resize(source, scratch):
    # Hide a stationary source and change its size/position before remapping.
    # Each side of the discontinuity is static. No cross-gap frame is needed.
    phase = "waiting_for_preview"
    paused = 0
    source.move(64)

    def inspect():
        nonlocal phase, paused
        if phase == "restored":
            return
        window = preview_window()
        if window and phase == "waiting_for_preview":
            # Allow a few captures at the initial size.
            phase = "initial"
            paused = time.monotonic()
        elif phase == "initial" and time.monotonic() - paused > .35:
            source.window.withdraw()
            source.window.update()
            phase = "hidden"
        elif phase == "hidden" and "waiting for source" in (scratch / "pause-resize.log").read_text().lower():
            source.window.geometry("512x328+8+8")
            source.move(260)
            source.window.deiconify()
            source.window.update()
            phase = "restored"

    stats, frame = run_capture(source, scratch, "pause-resize", 28, 0, 1, inspect)
    assert phase == "restored" and frame[:2] == (512, 328), (phase, frame[:2])
    assert stats["generated"] == 0, stats


def fixture(scratch):
    source = Source()
    try:
        for enabled in (0, 1):
            stats, frame = run_capture(source, scratch, f"static-{enabled}", 8, 0, enabled)
            assert stats["generated"] == 0, stats
            assert stats["duplicates"] == (7 if enabled else 0), stats
            assert frame[:2] == (WIDTH, HEIGHT)
        moving_case(source, scratch, nr=0, framegen=0)
        moving_case(source, scratch, nr=0, framegen=1)
        moving_case(source, scratch, nr=1, framegen=1)
        sustained_motion(source, scratch)
        hard_cuts(source, scratch)
        equal_histogram_cut(source, scratch)
        pause_and_resize(source, scratch)
    finally:
        source.window.destroy()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--backend", choices=("rife", "mako", "flow"), default="flow")
    parser.add_argument("--fixture", action="store_true")
    args = parser.parse_args()
    BACKEND = args.backend
    if args.fixture:
        with tempfile.TemporaryDirectory(prefix="nr-fg-") as scratch:
            fixture(Path(scratch))
    else:
        subprocess.run(["xvfb-run", "-a", "-s", "-screen 0 1440x760x24", sys.executable,
                        str(Path(__file__).resolve()), "--fixture", "--backend", BACKEND], check=True)
