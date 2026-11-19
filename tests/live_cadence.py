"""Run with python3 tests/live_cadence.py. Needs a built Live client, NVIDIA flow, Xvfb, Tk and cc."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

from live_framegen_smoke import ROOT, Source


def check_refresh(source, clock, hz):
    # Ask for 120 FPS on both displays. The slower display must still show 30
    # real frames per second and use the remaining refreshes for interpolation.
    rate, frames = 30, 90
    argv = [str(ROOT / 'build/demo/dlss5-live'), '--window', hex(source.canvas.winfo_id()),
            '--nr', '0', '--frame-gen', '1', '--fg-backend', 'flow', '--fg-multiplier', '4',
            '--fps', str(rate), '--frames', str(frames)]
    env = dict(os.environ, SDL_RENDER_DRIVER='software', SDL_RENDER_VSYNC='0',
               LD_PRELOAD=str(clock), LIVE_TEST_REFRESH_HZ=str(hz))
    start = time.monotonic()
    with tempfile.TemporaryFile(mode='w+') as log, subprocess.Popen(
            argv, env=env, stdout=log, stderr=subprocess.STDOUT) as proc:
        try:
            while proc.poll() is None:
                elapsed = time.monotonic() - start
                if elapsed > 30:
                    raise TimeoutError(f'{hz} Hz capture timed out')
                tick = int(elapsed * rate)
                position = 64 + 16 * (15 - abs(tick % 30 - 15))
                if source.x != position:
                    source.move(position)
                source.step(.002)
        finally:
            if proc.poll() is None:
                proc.kill()
            proc.wait()
        log.seek(0)
        output = log.read()
    if proc.returncode:
        raise RuntimeError(output)
    summary = next(line for line in output.splitlines() if line.startswith('[live] frames='))
    counts = {k: int(v) for k, v in re.findall(r'\b(captured|duplicates|generated|presented)=(\d+)', summary)}
    seconds = float(re.search(r'\belapsed=([\d.]+)', summary)[1])
    real = counts['presented'] - counts['generated']
    assert counts['captured'] == frames and real == frames - counts['duplicates'], summary
    assert real / seconds >= rate * .8, summary
    assert counts['generated'] / seconds >= (hz - rate) * .65, summary
    assert counts['presented'] / seconds <= hz * 1.05, summary
    print(f'{hz} Hz: {real / seconds:.1f} real + {counts["generated"] / seconds:.1f} generated FPS')


def fixture():
    with tempfile.TemporaryDirectory(prefix='live-cadence-') as scratch:
        clock = Path(scratch) / 'refresh.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-o', str(clock),
                        str(Path(__file__).with_name('live_refresh_clock.c')), '-ldl'], check=True)
        source = Source()
        try:
            for hz in (60, 120):
                check_refresh(source, clock, hz)
        finally:
            source.window.destroy()


if __name__ == '__main__':
    if sys.argv[1:] == ['--fixture']:
        fixture()
    else:
        subprocess.run(['xvfb-run', '-a', '-s', '-screen 0 1440x760x24',
                        sys.executable, str(Path(__file__).resolve()), '--fixture'], check=True)
