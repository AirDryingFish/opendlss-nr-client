"""Exercise the real installed gltfio archive on Filament's CPU-only NOOP backend.

Build/install Filament first. This test deliberately checks the linked archive, so
changing source metadata without rebuilding it cannot produce a passing result.
"""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MaterialArchiveTest(unittest.TestCase):
    def test_nontransmissive_materials_preserve_opaque_and_alpha_render_passes(self):
        sdk = Path(os.environ.get('FILAMENT_INSTALL_DIR', ROOT / 'third_party/filament-install'))
        if not (sdk / 'include/filament/Engine.h').is_file():
            self.skipTest('Filament SDK missing; run scripts/linux/build.sh first')
        lib = sdk / 'lib'
        if not (lib / 'libfilament.a').is_file():
            lib = lib / 'x86_64'
        # Resolve the same compiler / libc++ runtime as the production Linux build.
        setup = subprocess.run(
            ['bash', '-c', 'source "$1"; "$NR_PYTHON" -c '
             "'import json,os; print(json.dumps(dict(os.environ)))'",
             'material-archive-test', str(ROOT / 'scripts/linux/common.sh')],
            text=True, capture_output=True, check=True)
        env = json.loads(setup.stdout)
        names = ('gltfio_core uberarchive uberzlib ktxreader basis_transcoder dracodec '
                 'meshoptimizer image filament-iblprefilter filament backend bluevk smol-v '
                 'filabridge filaflat geometry mikktspace ibl utils abseil zstd stb perfetto').split()
        with tempfile.TemporaryDirectory(prefix='nr-material-archive-') as scratch:
            executable = Path(scratch) / 'material-archive-test'
            command = shlex.split(env.get('CXX', 'clang++')) + [
                '-std=c++20', '-stdlib=libc++', '-Wall', '-Wextra', '-Werror',
                '-isystem', str(sdk / 'include'), str(ROOT / 'tests/material_archive_test.cpp'),
                '-Wl,--start-group', *[str(lib / f'lib{name}.a') for name in names],
                '-Wl,--end-group', '-Wl,-z,noexecstack', '-pthread', '-ldl', '-lxcb', '-lX11',
                '-o', str(executable)] + shlex.split(env.get('CXXFLAGS', ''))
            build = subprocess.run(command, env=env, text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], env=env, text=True, capture_output=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == '__main__':
    unittest.main()
