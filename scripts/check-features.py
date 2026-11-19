#!/usr/bin/env python3
"""Check Linux build prerequisites or exercise individual client features."""
import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
BACKENDS = ("flow", "rife", "mako")
RUNTIME = ("chooser", "region", "window", "nr", "debug", *BACKENDS)
BUILD = "Build the client with ./scripts/linux/build.sh."
X11 = "Run in the target X11 desktop session and check DISPLAY/XAUTHORITY."
HINT = {
    "build": "Install the Ubuntu development packages in README.md.",
    "chooser": "Install python3-gi and gir1.2-gtk-3.0, then check desktop access.",
    "region": X11,
    "window": X11 + " Window capture also requires XComposite 0.2+.",
    "nr": "Check ./scripts/check-gpu.sh and extract the compatible NR weights into models/nr (or pass --model).",
    "debug": "Check chooser/display access, NR weights, bundled GLB and ./scripts/check-gpu.sh.",
    "flow": "Build with nvcc on PATH and install a compatible NVIDIA driver with CUDA and optical-flow support.",
    "rife": "Run ./scripts/linux/prepare-rife.sh and check the Vulkan GPU driver.",
    "mako": "Run ./scripts/linux/prepare-mako.sh and check external-memory/timeline support on the Vulkan GPU.",
}


def run(args, *, success=""):
    """Return stdout or raise with a short diagnostic, including loader and timeout errors."""
    try:
        result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, errors="replace", timeout=90)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise RuntimeError(str(error)) from error
    if result.returncode:
        output = (result.stderr.strip() or result.stdout.strip()).splitlines()
        tail = " | ".join(line.strip() for line in output[-3:])[-700:]
        raise RuntimeError(f"exit {result.returncode}: {tail or 'no diagnostics'}")
    return success or result.stdout.strip()


def check_build():
    if (platform.system(), platform.machine()) != ("Linux", "x86_64"):
        raise RuntimeError("The documented build targets x86_64 Linux.")
    tools = ("git", "curl", "unzip", "cmake", "ninja", "pkg-config", "cc", "c++")
    missing = [tool for tool in tools if shutil.which(tool) is None]
    if missing:
        raise RuntimeError("Missing commands: " + ", ".join(missing))
    cmake_version = json.loads(run(["cmake", "-E", "capabilities"]))["version"]
    if tuple(cmake_version[key] for key in ("major", "minor", "patch")) < (3, 22, 1):
        raise RuntimeError(f"Filament needs CMake 3.22.1 or newer (found {cmake_version['string']}).")
    run(["pkg-config", "--print-errors", "--exists", "x11", "xcomposite", "xcb", "xi", "xxf86vm",
         "gl", "vulkan", "sdl2 >= 2.0.19"])
    return "Build commands, Vulkan, X11 and SDL2 development packages found."


def check(feature, multiplier, model):
    if feature == "build":
        return check_build()
    if feature == "debug":
        args = [ROOT / "scripts/run-client.sh", "--debug", "--width", "320", "--height", "240", "--frames", "3"]
        if model:
            args.extend(["--model", model])
        return run(args, success="Loaded the bundled GLB and rendered three frames using the normal Debug launcher.")
    live = ROOT / "build/demo/dlss5-live"
    if not os.access(live, os.X_OK):
        raise RuntimeError(BUILD)
    if feature == "chooser":
        if not (live.parent / "client_start.py").is_file():
            raise RuntimeError("Missing built mode chooser. " + BUILD)
        run([live, "--list-fg-backends"])
        return run(["/usr/bin/python3", "-c",
                    "import gi; gi.require_version('Gtk', '3.0'); from gi.repository import Gtk; "
                    "import sys; sys.exit(0 if Gtk.init_check()[0] else 'GTK could not open the desktop display')"],
                   success="GTK opened the desktop and the Live backend list is readable.")
    args = [live, "--probe-feature", feature]
    if feature in BACKENDS:
        args.extend(["--fg-multiplier", str(multiplier)])
    if feature == "nr" and model:
        args.extend(["--model", model])
    return run(args)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", nargs="+", choices=("build", *RUNTIME),
                        help="check the selected features; default: all runtime features")
    parser.add_argument("--multiplier", type=int, choices=(2, 3, 4),
                        help="test just this multiplier; default: 2x, 3x and 4x for each interpolation backend")
    parser.add_argument("--model", type=Path, help="override the local NR model directory")
    parser.add_argument("--json", action="store_true", help="emit machine-readable results")
    args = parser.parse_args(argv)
    model = str(args.model.resolve()) if args.model else None
    checks = []
    for feature in dict.fromkeys(args.only or RUNTIME):
        rates = (args.multiplier,) if args.multiplier else (2, 3, 4)
        for rate in (rates if feature in BACKENDS else (None,)):
            name = feature if rate is None else f"{feature}-{rate}x"
            try:
                detail = check(feature, rate, model)
                entry = {"feature": name, "ready": True, "detail": detail}
            except RuntimeError as error:
                entry = {"feature": name, "ready": False, "detail": str(error), "hint": HINT[feature]}
            checks.append(entry)
            if not args.json:
                print(f"[{'OK' if entry['ready'] else 'FAIL'}] {name}  {entry['detail']}", flush=True)
                if "hint" in entry:
                    print(f"       {entry['hint']}", flush=True)
    if args.json:
        print(json.dumps(checks, ensure_ascii=False, indent=2))
    return 0 if all(entry["ready"] for entry in checks) else 2


if __name__ == "__main__":
    sys.exit(main())
