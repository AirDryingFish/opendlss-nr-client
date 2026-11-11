#!/usr/bin/env python3
"""Linux equivalent of upstream build_shaders.ps1 / build_demo.ps1.

PTX arguments deliberately match upstream. No CUDA compiler is involved.
"""
from pathlib import Path
import subprocess
import sys
import importlib.util

_spec = importlib.util.spec_from_file_location('kernel_resources', Path(__file__).with_name('kernel_resources.py'))
_resources = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_resources)
ptx_jobs = _resources.ptx_jobs

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / 'build'
PTX = BUILD / 'ptx'
GLSLANG = ROOT / 'tools/glslang/bin/glslang'

def run(*args):
    subprocess.run([str(a) for a in args], check=True)

def emit(name, *args):
    # Each emitter accepts its output path at a distinct point; pass full argv.
    run(sys.executable, ROOT / 'scripts/ptx' / (name + '.py'), *args)

def ptx():
    PTX.mkdir(parents=True, exist_ok=True)
    for name, *args in ptx_jobs(PTX):
        emit(name, *args)

def shaders():
    for folder, output, recursive in ((ROOT/'shaders', BUILD/'shaders', False), (ROOT/'demo/shaders', BUILD/'data/shaders', True)):
        output.mkdir(parents=True, exist_ok=True)
        for src in sorted(folder.rglob('*.comp') if recursive else folder.glob('*.comp')):
            name = src.name + '.spv' if recursive else src.stem + '.spv'
            run(GLSLANG, '-V', '--target-env', 'vulkan1.3', '-I' + str(folder), src, '-o', output/name)

def materials():
    output = BUILD / 'data/materials'
    output.mkdir(parents=True, exist_ok=True)
    for src in sorted((ROOT/'demo/materials').glob('*.mat')):
        run(ROOT/'third_party/filament-install/bin/matc', '-a', 'vulkan', '-p', 'desktop', '-o', output/(src.stem+'.filamat'), src)

if __name__ == '__main__':
    mode = sys.argv[1] if len(sys.argv) > 1 else 'all'
    if mode in ('all', 'shaders'): shaders()
    if mode in ('all', 'ptx'): ptx()
    if mode in ('all', 'materials'): materials()
