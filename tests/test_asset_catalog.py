"""Compile and exercise the filesystem-only startup asset catalog."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AssetCatalogTest(unittest.TestCase):
    def test_recursive_discovery_preserves_view_metadata_and_deduplicates_aliases(self):
        with tempfile.TemporaryDirectory(prefix='nr-asset-catalog-') as scratch:
            executable = Path(scratch) / 'asset-catalog'
            command = shlex.split(os.environ.get('CXX','c++')) + [
                '-std=c++17','-Wall','-Wextra','-Werror','-pedantic',
                '-I',str(ROOT / 'demo'),str(ROOT / 'tests/asset_catalog_test.cpp'),
                '-o',str(executable)]
            build = subprocess.run(command,text=True,capture_output=True,timeout=30)
            self.assertEqual(build.returncode,0,build.stdout + build.stderr)
            run = subprocess.run([str(executable)],text=True,capture_output=True,timeout=10)
            self.assertEqual(run.returncode,0,run.stdout + run.stderr)


if __name__ == '__main__':
    unittest.main()
