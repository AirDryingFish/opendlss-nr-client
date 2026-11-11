"""Compile and exercise the file browser's real filesystem policy."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ModelFileBrowserTest(unittest.TestCase):
    def test_lists_supported_files_and_rechecks_open_selection(self):
        with tempfile.TemporaryDirectory(prefix='nr-model-file-browser-') as scratch:
            executable = Path(scratch) / 'model-file-browser'
            command = shlex.split(os.environ.get('CXX', 'c++')) + [
                '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
                '-I', str(ROOT / 'demo'), str(ROOT / 'tests/model_file_browser_test.cpp'),
                '-o', str(executable)]
            build = subprocess.run(command, text=True, capture_output=True, timeout=30)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], text=True, capture_output=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == '__main__':
    unittest.main()
