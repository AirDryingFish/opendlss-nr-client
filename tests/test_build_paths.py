"""Build cache paths work in a standalone clone without ancestor directories."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class BuildPathsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='NR standalone clone ')
        self.addCleanup(self.tmp.cleanup)
        self.directory = Path(self.tmp.name)
        self.clone = self.directory / 'checkout' / 'client'
        scripts = self.clone / 'scripts' / 'linux'
        scripts.mkdir(parents=True)
        for name in ('common.sh', 'local-toolchain.sh'):
            shutil.copyfile(ROOT / 'scripts' / 'linux' / name, scripts / name)
        self.home = self.directory / 'user home'
        self.home.mkdir()
        self.env = dict(os.environ, HOME=str(self.home), CXX='/bin/false')
        for name in ('XDG_CACHE_HOME', 'NR_DEPS_DIR', 'NR_BUILD_DIR'):
            self.env.pop(name, None)

    def paths(self, **environment):
        result = subprocess.run(
            ['bash', '-c', 'source "$1"; printf "%s\\n" "$NR_ROOT" "$NR_DEPS_DIR" "$NR_BUILD_DIR"',
             'test', str(self.clone / 'scripts' / 'linux' / 'common.sh')],
            cwd=self.directory, env=dict(self.env, **environment),
            check=True, capture_output=True, text=True,
        )
        root, deps, build = map(Path, result.stdout.splitlines())
        self.assertEqual(root, self.clone)
        self.assertTrue(deps.is_dir())
        self.assertTrue((build / 'logs').is_dir())
        return deps, build

    def test_standalone_clone_defaults_to_home_cache(self):
        cache = self.home / '.cache' / 'opendlss-nr-client'
        self.assertEqual(self.paths(), (cache / 'native-build-deps', cache / 'native-build'))
        self.assertFalse((self.directory / 'work').exists())

    def test_xdg_cache_home_is_respected(self):
        cache = self.directory / 'custom cache' / 'opendlss-nr-client'
        self.assertEqual(
            self.paths(XDG_CACHE_HOME=str(cache.parent)),
            (cache / 'native-build-deps', cache / 'native-build'),
        )

    def test_explicit_existing_cache_overrides_are_preserved(self):
        deps = self.directory / 'existing deps'
        build = self.directory / 'existing build'
        deps.mkdir()
        marker = deps / 'cached-download'
        marker.write_text('preserve')
        self.assertEqual(
            self.paths(NR_DEPS_DIR=str(deps), NR_BUILD_DIR=str(build)), (deps, build),
        )
        self.assertEqual(marker.read_text(), 'preserve')
        self.assertFalse((self.home / '.cache').exists())


if __name__ == '__main__':
    unittest.main()
