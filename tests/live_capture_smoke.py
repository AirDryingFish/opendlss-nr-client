"""Opt-in integration test. Requires Python/Tk, Xvfb, xwininfo, the client and NR weights."""
import ctypes
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
COLORS = ("#a02040", "#10c080", "#3040e0", "#f0d040")


def read_ppm(path):
    with path.open("rb") as stream:
        assert stream.readline().strip() == b"P6"
        width, height = map(int, stream.readline().split())
        assert stream.readline().strip() == b"255"
        rgb = stream.read()
    assert len(rgb) == width * height * 3
    return width, height, rgb


def run_inside_private_display(scratch):
    import tkinter as tk

    app = tk.Tk()
    app.overrideredirect(True)
    app.geometry("320x192+8+8")
    canvas = tk.Canvas(app, borderwidth=0, highlightthickness=0)
    canvas.pack(fill="both", expand=True)
    app.update()

    def paint():
        w, h = canvas.winfo_width(), canvas.winfo_height()
        canvas.delete("all")
        for i, color in enumerate(COLORS):
            x, y = (i % 2) * (w // 2), (i // 2) * (h // 2)
            canvas.create_rectangle(x, y, x + w // 2, y + h // 2, fill=color, outline="")
        app.update()

    def capture(source, mode, frames=8, during=None):
        path = scratch / "result.ppm"
        path.unlink(missing_ok=True)
        command = [str(ROOT / "scripts/run-client.sh"), "--live", *source, "--nr", str(mode),
                   "--frames", str(frames), "--fps", "30", "--output", str(path)]
        with (scratch / "run.log").open("w") as log:
            child = subprocess.Popen(command, cwd=scratch, stdout=log, stderr=subprocess.STDOUT)
            start = time.monotonic()
            try:
                while child.poll() is None:
                    elapsed = time.monotonic() - start
                    assert elapsed < 90, "live capture timed out"
                    if during:
                        during(elapsed)
                    app.update()
                    time.sleep(0.01)
            finally:
                if child.poll() is None:
                    child.kill()
                child.wait()
        if child.returncode:
            raise RuntimeError((scratch / "run.log").read_text())
        return read_ppm(path)

    def check_quadrants(frame):
        w, h, rgb = frame
        swatches = [bytes.fromhex(color[1:]) for color in COLORS]
        top = swatches[0] * (w // 2) + swatches[1] * (w - w // 2)
        bottom = swatches[2] * (w // 2) + swatches[3] * (w - w // 2)
        assert rgb == top * (h // 2) + bottom * (h - h // 2), "capture differs from source pixels"

    paint()
    window = ["--window", hex(canvas.winfo_id())]
    region = ["--region", "8,8,320,192"]
    original = capture(region, 0)
    assert original[:2] == (320, 192)
    check_quadrants(original)

    # Move the preview into its own crop after the first frame. The viewer
    # must pause instead of capturing its previous output, then resume outside.
    x11 = ctypes.CDLL("libX11.so.6")
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XMoveWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int]
    x11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
    x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
    display = x11.XOpenDisplay(None)
    assert display
    overlap = "initial"

    def move_preview(elapsed):
        nonlocal overlap
        if elapsed < 0.35 or overlap == "restored":
            return
        tree = subprocess.check_output(["xwininfo", "-root", "-tree"], text=True)
        match = re.search(r'(0x[0-9a-f]+) "DLSS 5 Live"', tree)
        if not match:
            return
        if overlap == "initial":
            position = (40, 40)
            overlap = "inside"
        elif "[live] Capture paused:" in (scratch / "run.log").read_text():
            position = (720, 32)
            overlap = "restored"
        else:
            return
        x11.XMoveWindow(display, int(match[1], 16), *position)
        x11.XSync(display, 0)

    try:
        assert capture(region, 0, frames=32, during=move_preview) == original
        assert overlap == "restored", "overlapping preview was not detected"
    finally:
        x11.XCloseDisplay(display)
    # Capture the exact same pixels via a named window and the desktop crop.
    assert capture(window, 0) == original
    enhanced = capture(window, 1)
    assert enhanced[:2] == original[:2]
    assert enhanced[2] != original[2] and len(set(enhanced[2])) > 10

    # Change the running source. The final capture must follow its new contents
    # and size even when another window covers it.
    cover = tk.Toplevel(app)
    cover.overrideredirect(True)
    cover.geometry("320x192+8+8")
    cover.configure(background="#000000")
    cover.lift()
    changed = False

    def resize(elapsed):
        nonlocal changed
        if elapsed > 0.4 and not changed:
            changed = True
            app.geometry("416x256+8+8")
            app.update()
            paint()
            cover.lift()

    resized = capture(window, 0, frames=45, during=resize)
    assert changed and resized[:2] == (416, 256)
    check_quadrants(resized)
    app.destroy()
    print("Live region/window, exact RGB bypass, neural output and covered-window resize passed.")


if __name__ == "__main__":
    # Use a separate X server so the test reads only its own generated pixels.
    if sys.argv[1:] != ["--fixture"]:
        subprocess.run(["xvfb-run", "-a", "-s", "-screen 0 1600x900x24", sys.executable,
                        str(Path(__file__).resolve()), "--fixture"], check=True)
    else:
        with tempfile.TemporaryDirectory(prefix="nr-live-") as directory:
            run_inside_private_display(Path(directory))
