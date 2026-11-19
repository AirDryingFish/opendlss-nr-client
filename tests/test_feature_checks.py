import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location(
    "feature_checks", Path(__file__).resolve().parents[1] / "scripts/check-features.py")
feature_checks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(feature_checks)


class FeatureChecksTest(unittest.TestCase):
    def setUp(self):
        self.workspace = tempfile.TemporaryDirectory()
        self.addCleanup(self.workspace.cleanup)
        self.root = Path(self.workspace.name)
        root = patch.object(feature_checks, "ROOT", self.root)
        root.start()
        self.addCleanup(root.stop)

    def run_checks(self, *args):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = feature_checks.main(["--json", "--only", *args])
        return status, json.loads(output.getvalue())

    def test_old_cmake_is_diagnosed_before_fetching_filament(self):
        capabilities = {"version": {"major": 3, "minor": 20, "patch": 5, "string": "3.20.5"}}
        with patch.object(feature_checks.shutil, "which", return_value="/usr/bin/tool"), \
                patch.object(feature_checks, "run", return_value=json.dumps(capabilities)):
            status, results = self.run_checks("build")
        self.assertEqual(status, 2)
        self.assertIn("CMake 3.22.1", results[0]["detail"])

    def test_fresh_clone_without_build(self):
        status, results = self.run_checks("window", "rife", "--multiplier", "4")
        self.assertEqual(status, 2)
        self.assertEqual([row["feature"] for row in results], ["window", "rife-4x"])
        self.assertTrue(all("build.sh" in row["detail"] for row in results))

    def test_debug_uses_its_own_launcher_and_external_weights(self):
        launcher = self.root / "scripts/run-client.sh"
        launcher.parent.mkdir()
        launcher.write_text("""#!/bin/sh
test "$1" = --debug && test "$8" = --model && test "$9" = "$PWD/external weights"
""")
        launcher.chmod(0o755)
        status, results = self.run_checks("debug", "--model", str(self.root / "external weights"))
        self.assertEqual(status, 0)
        self.assertEqual(results[0]["feature"], "debug")

    def test_real_probe_failures_are_reported_per_backend_and_factor(self):
        live = self.root / "build/demo/dlss5-live"
        live.parent.mkdir(parents=True)
        live.write_text("""#!/bin/sh
test "$1" = --probe-feature && test "$3" = --fg-multiplier || exit 3
if test "$2 $4" = 'rife 3'; then echo 'unsupported multiplier' >&2; exit 1; fi
echo processed
""")
        live.chmod(0o755)
        status, results = self.run_checks("rife", "flow")
        self.assertEqual(status, 2)
        self.assertEqual([r["feature"] for r in results],
                         ["rife-2x", "rife-3x", "rife-4x", "flow-2x", "flow-3x", "flow-4x"])
        self.assertEqual([r["feature"] for r in results if not r["ready"]], ["rife-3x"])
        self.assertIn("unsupported multiplier", results[1]["detail"])
        self.assertIn("prepare-rife.sh", results[1]["hint"])
        status, selected = self.run_checks("flow", "--multiplier", "4")
        self.assertEqual(status, 0)
        self.assertEqual(selected, [{"feature": "flow-4x", "ready": True, "detail": "processed"}])


if __name__ == "__main__":
    unittest.main()
