"""Compile and exercise the GPU-independent preset manifest boundary."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class EnvironmentPresetsTest(unittest.TestCase):
    def test_relative_hdr_discovery_rejects_unsafe_or_malformed_entries(self):
        with tempfile.TemporaryDirectory(prefix='nr-environment-presets-') as scratch:
            executable = Path(scratch) / 'environment-presets'
            command = shlex.split(os.environ.get('CXX', 'c++')) + [
                '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
                '-I', str(ROOT / 'demo'), '-I', str(ROOT / 'src'),
                str(ROOT / 'tests/environment_presets_test.cpp'), '-o', str(executable)]
            build = subprocess.run(command, text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            arguments = [str(executable)]
            manifest = ROOT / 'assets/environments/presets.json'
            if manifest.is_file():
                arguments.append(str(manifest))
            run = subprocess.run(arguments, text=True, capture_output=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == '__main__':
    unittest.main()
