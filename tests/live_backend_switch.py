"""Opt-in GPU test. Run python3 tests/live_backend_switch.py with at least two installed backends."""
import ctypes as ct
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from live_framegen_smoke import ROOT, Source, preview_window


def exercise(log):
    binary = ROOT / "build/demo/dlss5-live"
    options = [line.split("\t") for line in subprocess.check_output(
        [str(binary), "--list-fg-backends"], text=True).splitlines()]
    assert len(options) >= 2, "install at least two interpolation backends to test switching"
    source = Source()
    x = ct.CDLL("libX11.so.6")
    xt = ct.CDLL("libXtst.so.6")
    x.XOpenDisplay.argtypes = [ct.c_char_p]
    x.XOpenDisplay.restype = ct.c_void_p
    x.XSetInputFocus.argtypes = [ct.c_void_p, ct.c_ulong, ct.c_int, ct.c_ulong]
    x.XKeysymToKeycode.argtypes = [ct.c_void_p, ct.c_ulong]
    x.XSync.argtypes = [ct.c_void_p, ct.c_int]
    x.XCloseDisplay.argtypes = [ct.c_void_p]
    xt.XTestFakeKeyEvent.argtypes = [ct.c_void_p, ct.c_uint, ct.c_int, ct.c_ulong]
    xt.XTestFakeMotionEvent.argtypes = [ct.c_void_p, ct.c_int, ct.c_int, ct.c_int, ct.c_ulong]
    xt.XTestFakeButtonEvent.argtypes = [ct.c_void_p, ct.c_uint, ct.c_int, ct.c_ulong]
    display = x.XOpenDisplay(None)
    argv = [str(binary), "--window", hex(source.canvas.winfo_id()), "--nr", "0", "--fps", "20",
            "--frame-gen", "1", "--fg-backend", options[0][0]]
    with log.open("w") as stream:
        child = subprocess.Popen(argv, stdout=stream, stderr=subprocess.STDOUT)
        try:
            def wait_log(expected, start=0):
                until = time.monotonic() + 30
                while time.monotonic() < until:
                    text = log.read_text()
                    if preview_window() and expected in text[start:]:
                        print(expected, flush=True)
                        return
                    assert child.poll() is None, text
                    source.step(.015)
                raise AssertionError(f"did not see {expected}: {log.read_text()}")

            def key(symbol):
                start = len(log.read_text())
                xid, _, _ = preview_window()
                x.XSetInputFocus(display, xid, 2, 0)
                code = x.XKeysymToKeycode(display, symbol)
                for down in (1, 0):
                    xt.XTestFakeKeyEvent(display, code, down, 0)
                x.XSync(display, 0)
                return start

            def click(inset, row):
                _, _, (x0, y0, width, _) = preview_window()
                xt.XTestFakeMotionEvent(display, -1, x0 + width - inset, y0 + row, 0)
                for down in (1, 0):
                    x.XSync(display, 0)
                    source.step(.07)
                    xt.XTestFakeButtonEvent(display, 1, down, 0)
                x.XSync(display, 0)
                source.step(.10)

            wait_log(f"{options[0][1]} frame generation 2x")
            wait_log(f"FG=off backend={options[0][0]}", key(ord("g")))
            wait_log(f"FG=off backend={options[1][0]}", key(ord("b")))
            wait_log(f"{options[1][1]} frame generation 2x", key(ord("g")))
            for i in range(1, len(options) + 1):
                # Keep the worker busy while switching to another engine.
                for j in range(6):
                    source.move(64 + 16 * j)
                    source.step(.065)
                wait_log(f"{options[(i + 1) % len(options)][1]} frame generation 2x", key(ord("b")))

            # Open the actual ImGui panel and operate its multiplier combo. The
            # window stays at the fixture's native size. The panel is right-aligned.
            key(0xffbe)  # F1
            source.step(.15)
            start = len(log.read_text())
            click(85, 144)
            key(0xff54)  # Down
            source.step(.08)
            key(0xff0d)  # Return
            wait_log("multiplier=3 fps=20", start)
            wait_log(f"{options[1][1]} frame generation 3x", start)
            start = len(log.read_text())
            click(85, 144)
            key(0xff54)
            source.step(.08)
            key(0xff0d)
            wait_log("multiplier=4 fps=20", start)
            wait_log(f"{options[1][1]} frame generation 4x", start)
            start = len(log.read_text())
            click(85, 173)
            key(0xff50)  # Home: select the first preset (24 FPS).
            source.step(.08)
            key(0xff0d)
            wait_log("multiplier=4 fps=24", start)
            key(0xffbe)
            key(0xff1b)  # Escape
            assert child.wait(timeout=10) == 0, log.read_text()
            text = log.read_text()
            assert "frame generation disabled" not in text, text
            generated = int(re.search(r"\bgenerated=(\d+)", text)[1])
            assert generated >= len(options), text
            print(f"Generated {generated} intermediate frames while switching.")
        finally:
            if child.poll() is None:
                child.kill()
            child.wait()
            x.XCloseDisplay(display)
            source.window.destroy()


if __name__ == "__main__":
    if sys.argv[1:] == ["--fixture"]:
        with tempfile.TemporaryDirectory(prefix="live-backend-switch-") as temp:
            exercise(Path(temp) / "live.log")
    else:
        subprocess.run(["xvfb-run", "-a", "-s", "-screen 0 1600x900x24", sys.executable,
                        str(Path(__file__).resolve()), "--fixture"], check=True)
