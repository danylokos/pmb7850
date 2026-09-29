"""Check build reuse and failure safety without compiling upstream GDB."""

import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from tools import gdb_build


class GdbBuildTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.source = self.root / "source"
        self.source.mkdir()
        (self.source / "configure").write_text("configure fixture\n")
        self.build = self.root / "build with spaces"
        self.calls = []
        self.environments = []
        self.original_dependency_environment = gdb_build.dependency_environment
        discovery_patch = patch.object(gdb_build, "dependency_environment",
                                       return_value={})
        self.discovery = discovery_patch.start()
        self.addCleanup(discovery_patch.stop)

    def fake_run(self, command, *, cwd, extra_env=None):
        self.calls.append(command)
        self.environments.append(extra_env)
        if command[0] == str(self.source / "configure"):
            (cwd / "config.status").write_text("configured\n")
        else:
            (cwd / "gdb").mkdir(exist_ok=True)
            (cwd / "gdb/gdb").write_text("binary\n")
        return subprocess.CompletedProcess(command, 0)

    def invoke(self, jobs=4):
        with patch.object(gdb_build, "run", side_effect=self.fake_run):
            gdb_build.build(self.source, self.build, jobs)

    def test_reuse_and_jobs_override(self):
        self.invoke()
        self.invoke(jobs=2)
        self.assertEqual(len(self.calls), 3)
        self.assertEqual(self.calls[-1], ["make", "-j2", "all-gdb"])
        self.assertIn("--target=c166-unknown-none", self.calls[0])

    def test_unmanaged_build_preserved(self):
        self.build.mkdir()
        sentinel = self.build / "config.cache"
        sentinel.write_text("old configuration\n")
        with self.assertRaisesRegex(ValueError, "fresh GDB_BUILD_DIR"):
            self.invoke()
        self.assertEqual(sentinel.read_text(), "old configuration\n")
        self.assertEqual(self.calls, [])

    def test_changed_environment_rejected(self):
        self.invoke()
        with patch.dict(gdb_build.os.environ, {"CC": "different-compiler"}):
            with self.assertRaisesRegex(ValueError, "Incompatible"):
                self.invoke()
        self.assertEqual(len(self.calls), 2)

    def test_discovered_flags_used_for_configure_build_and_stamp(self):
        self.discovery.return_value = {
            "CPPFLAGS": "-I/brew/include", "LDFLAGS": "-L/brew/lib",
            "LIBS": "-lmpfr -lgmp -lexpat",
        }
        self.invoke()
        expected = self.discovery.return_value
        self.assertEqual(self.environments, [expected, expected])
        stamp = json.loads((self.build / gdb_build.STAMP).read_text())
        for name, value in expected.items():
            self.assertEqual(stamp["request"]["environment"][name], value)
        self.invoke()
        self.assertEqual(len(self.calls), 3)
        self.discovery.return_value = dict(expected, CPPFLAGS="-I/new-prefix")
        with self.assertRaisesRegex(ValueError, "Incompatible"):
            self.invoke()

    def test_partial_pkg_config_discovery_preserves_user_flags(self):
        def pkg_config(command, **_kwargs):
            option = command[1]
            if option == "--exists":
                return subprocess.CompletedProcess(command,
                                                   int(command[2] == "ncurses"))
            outputs = {
                "--cflags": "-I/brew/include\n",
                "--libs-only-L": "-L/brew/lib\n",
                "--libs-only-l": "-lmpfr -lgmp -lexpat\n",
            }
            self.assertEqual(command[2:], ["mpfr", "gmp", "expat"])
            return subprocess.CompletedProcess(command, 0, outputs[option])

        with patch.object(gdb_build.sys, "platform", "darwin"), \
             patch.object(gdb_build.shutil, "which", return_value="pkg-config"), \
             patch.object(gdb_build.subprocess, "run", side_effect=pkg_config), \
             patch.dict(gdb_build.os.environ, {
                 "CPPFLAGS": "-DUSER", "LDFLAGS": "-Wl,-headerpad_max_install_names",
                 "LIBS": "-lcustom",
             }):
            flags = self.original_dependency_environment()
        self.assertEqual(flags, {
            "CPPFLAGS": "-DUSER -I/brew/include",
            "LDFLAGS": "-Wl,-headerpad_max_install_names -L/brew/lib",
            "LIBS": "-lcustom -lmpfr -lgmp -lexpat",
        })

    def test_linux_does_not_inject_dependency_flags(self):
        with patch.object(gdb_build.sys, "platform", "linux"), \
             patch.object(gdb_build.subprocess, "run") as runner:
            self.assertEqual(self.original_dependency_environment(), {})
        runner.assert_not_called()

    def test_external_reconfigure_rejected(self):
        self.invoke()
        (self.build / "config.status").write_text("externally changed\n")
        with self.assertRaisesRegex(ValueError, "Incompatible"):
            self.invoke()

    def test_corrupt_stamp_rejected(self):
        self.invoke()
        (self.build / gdb_build.STAMP).write_text(json.dumps(None))
        with self.assertRaisesRegex(ValueError, "Incompatible"):
            self.invoke()

    def test_failed_configure_not_reused(self):
        def fail(command, *, cwd, extra_env=None):
            (cwd / "config.log").write_text("configure failed\n")
            return subprocess.CompletedProcess(command, 1)
        with patch.object(gdb_build, "run", side_effect=fail):
            with self.assertRaises(subprocess.CalledProcessError):
                gdb_build.build(self.source, self.build, 4)
        self.assertFalse((self.build / gdb_build.STAMP).exists())
        with self.assertRaisesRegex(ValueError, "incomplete"):
            self.invoke()

    def test_failed_build_can_resume(self):
        def fail_build(command, *, cwd, extra_env=None):
            result = self.fake_run(command, cwd=cwd, extra_env=extra_env)
            if command[0] == "make":
                result.returncode = 1
            return result
        with patch.object(gdb_build, "run", side_effect=fail_build):
            with self.assertRaises(subprocess.CalledProcessError):
                gdb_build.build(self.source, self.build, 4)
        self.invoke()
        self.assertEqual(len(self.calls), 3)

    def test_missing_smoke_binary_does_not_build(self):
        with patch.object(gdb_build, "run") as runner:
            with self.assertRaisesRegex(ValueError, "binary missing"):
                gdb_build.smoke(self.root / "missing")
        runner.assert_not_called()


if __name__ == "__main__":
    unittest.main()
