"""Run with python3 tests/live_drag_smoke.py. Requires Xvfb, xwininfo and a built Live client."""
import ctypes as ct
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

from live_framegen_smoke import ROOT, preview_window


GREEN, BLUE, WHITE = 0x20AB56, 0x2457B4, 0xFFFFFF
WIDTH, HEIGHT = 320, 192


class Display:
    """Minimal X11 window manager and desktop pixel sampler for the private X server."""

    def __init__(self):
        x = self.x = ct.CDLL("libX11.so.6")
        ptr, xid, i, u = ct.c_void_p, ct.c_ulong, ct.c_int, ct.c_uint
        for name, result, args in [
            ("XOpenDisplay", ptr, [ct.c_char_p]),
            ("XDefaultRootWindow", xid, [ptr]),
            ("XCreateSimpleWindow", xid, [ptr, xid, i, i, u, u, u, xid, xid]),
            ("XMapWindow", i, [ptr, xid]),
            ("XSetWindowBackground", i, [ptr, xid, xid]),
            ("XClearWindow", i, [ptr, xid]),
            ("XReparentWindow", i, [ptr, xid, xid, i, i]),
            ("XMoveWindow", i, [ptr, xid, i, i]),
            ("XSync", i, [ptr, i]),
            ("XGetImage", ptr, [ptr, xid, i, i, u, u, xid, i]),
            ("XGetPixel", xid, [ptr, i, i]),
            ("XDestroyImage", i, [ptr]),
            ("XCloseDisplay", i, [ptr]),
        ]:
            fn = getattr(x, name)
            fn.restype, fn.argtypes = result, args
        self.d = x.XOpenDisplay(None)
        assert self.d
        self.root = x.XDefaultRootWindow(self.d)

    def window(self, x, y, w, h, rgb):
        xid = self.x.XCreateSimpleWindow(self.d, self.root, x, y, w, h, 0, 0, rgb)
        self.x.XMapWindow(self.d, xid)
        self.x.XSync(self.d, 0)
        return xid

    def paint(self, xid, color):
        self.x.XSetWindowBackground(self.d, xid, color)
        self.x.XClearWindow(self.d, xid)
        self.x.XSync(self.d, 0)

    def move(self, xid, x, y):
        self.x.XMoveWindow(self.d, xid, x, y)
        self.x.XSync(self.d, 0)

    def pixel(self, x, y):
        image = self.x.XGetImage(self.d, self.root, x, y, 1, 1, 0xFFFFFF, 2)
        assert image
        color = self.x.XGetPixel(image, 0, 0)
        self.x.XDestroyImage(image)
        return color & 0xFFFFFF


class DragTest(unittest.TestCase):
    def setUp(self):
        self.x = Display()
        self.addCleanup(self.x.x.XCloseDisplay, self.x.d)
        self.x.paint(self.x.root, WHITE)
        self.source = self.x.window(40, 40, WIDTH, HEIGHT, GREEN)

    def wait_for(self, inspect):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.client.poll() is not None:
                self.log.seek(0)
                self.fail(f"Live client exited: {self.log.read()[-2000:]}")
            result = inspect()
            if result:
                return result
            time.sleep(.01)
        self.fail(f"timeout waiting for {inspect.__name__}")

    def start(self, source_type):
        command = os.environ.get("NR_LIVE_BINARY", str(ROOT / "build/demo/dlss5-live"))
        source = ["--region", f"40,40,{WIDTH},{HEIGHT}"] if source_type == "region" else [
            "--window", hex(self.source)]
        self.log = tempfile.TemporaryFile(mode="w+")
        self.addCleanup(self.log.close)
        self.client = subprocess.Popen([command, *source, "--fps", "30", "--nr", "0", "--frame-gen", "0"],
                                       stdout=self.log, stderr=subprocess.STDOUT)
        self.addCleanup(self.client.wait, 8)
        self.addCleanup(self.client.terminate)
        xid, _, (x, y, w, h) = self.wait_for(preview_window)
        self.assertEqual((w, h), (WIDTH, HEIGHT))
        self.wait_for(lambda: self.x.pixel(x + 20, y + 50) == GREEN)
        return xid

    def drag_outside_source(self, source_type):
        win = self.start(source_type)
        self.x.move(win, 500, 350)
        self.wait_for(lambda: self.x.pixel(520, 400) == GREEN)
        self.x.window(760, 350, 180, HEIGHT, WHITE)

        # At y=350 the whole trajectory stays clear of the capture rectangle.
        # Read the desktop immediately after the move, before an Expose callback
        # has time to repaint a blank area uncovered by the X server.
        for label, x in [("unobstructed", 450), ("behind another window", 780), ("off-screen", -160)]:
            with self.subTest(trajectory=label):
                invalid = []
                for i in range(35):
                    if i % 10 == 0:
                        self.x.paint(self.source, BLUE if i % 20 == 0 else GREEN)
                    self.x.move(win, x, 350)
                    time.sleep(.002)
                    self.x.move(win, 500, 350)
                    color = self.x.pixel(520, 400)
                    if color not in (GREEN, BLUE):
                        invalid.append(f"#{color:06x}")
                    time.sleep(.006)
                self.assertEqual(len(invalid), 0, f"{len(invalid)}/35 invalid samples, first {invalid[:3]}")
        self.x.paint(self.source, BLUE)
        self.wait_for(lambda: self.x.pixel(520, 400) == BLUE)

    def test_window_drag_outside_source(self):
        self.drag_outside_source("window")

    def test_region_drag_outside_source(self):
        self.drag_outside_source("region")

    def test_only_titlebar_overlaps_region(self):
        win = self.start("region")
        frame = self.x.window(650, 350, WIDTH + 40, HEIGHT + 64, WHITE)
        self.x.x.XReparentWindow(self.x.d, win, frame, 20, 44)
        self.x.x.XMapWindow(self.x.d, win)
        self.x.x.XSync(self.x.d, 0)
        self.wait_for(lambda: self.x.pixel(690, 440) == GREEN)

        # A reparenting WM can move only the frame while SDL still knows the old
        # client origin. Only the top 12 rows of decoration overlap the source.
        self.x.move(frame, 40, 220)
        self.x.paint(self.source, BLUE)
        colors = []
        for _ in range(35):
            colors.append(self.x.pixel(110, 449))  # Corresponds to source (90, 225), under the titlebar.
            time.sleep(.01)
        self.assertEqual(set(colors), {GREEN}, f"expected last valid frame, got {set(map(hex, colors))}")
        self.x.move(frame, 650, 350)
        self.wait_for(lambda: self.x.pixel(690, 440) == BLUE)


if __name__ == "__main__":
    if sys.argv[1:] == ["--fixture"]:
        unittest.main(argv=[sys.argv[0]], verbosity=2)
    else:
        with tempfile.TemporaryDirectory(prefix="live-drag-") as scratch:
            subprocess.run(["xvfb-run", "-a", "-s", "-screen 0 1480x900x24", sys.executable,
                            str(Path(__file__).resolve()), "--fixture"], check=True, cwd=scratch)
