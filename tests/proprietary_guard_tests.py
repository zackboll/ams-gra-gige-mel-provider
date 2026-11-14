#!/usr/bin/env python3
"""SDK-free negative fixtures, synthesized without any vendor file contents."""
import importlib.util
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("guard", ROOT / "scripts/check_proprietary.py")
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)


class GuardTests(unittest.TestCase):
    def test_content(self):
        for path, data in [
            ("vendor/PvExample.h", b""),
            ("renamed.txt", b"\x7fELFfake"),
            ("renamed.cpp", b"class " + b"PvDevice {"),
            ("renamed.txt", b"Copyright 2099 " + b"Pleora " + b"Technologies"),
            ("renamed.txt", b"#ifndef " + b"__PVEXAMPLE_H__"),
            ("download.tar.xz", b""),
            ("machine.lic", b""),
            ("private.txt", b"-----BEGIN " + b"OPENSSH PRIVATE KEY-----"),
        ]:
            with self.subTest(path=path, data=data):
                self.assertIsNotNone(guard.violation(path, "100644", data))
        self.assertIsNone(guard.violation("adapter.cpp", "100644", b"#include <PvDevice.h>\n"))
        self.assertIsNotNone(guard.violation("alias", "120000", b"/external/sdk"))
        self.assertIsNotNone(guard.violation("vendor", "160000", b""))

    def test_index_and_deleted_history(self):
        with tempfile.TemporaryDirectory() as directory:
            def git(*args):
                return subprocess.check_output(["git", "-C", directory, *args], stderr=subprocess.STDOUT).decode().strip()
            git("init", "-q")
            git("config", "user.email", "fixture@example.invalid")
            git("config", "user.name", "Fixture")
            path = pathlib.Path(directory) / "safe.cpp"
            path.write_text("#include <PvDevice.h>\n")
            git("add", ".")
            git("commit", "-qm", "base")
            base = git("rev-parse", "HEAD")
            path.write_bytes(b"\x7fELFcontrolled fixture")
            git("add", ".")
            # Working-tree sanitization must not hide an unsafe staged blob.
            path.write_text("safe\n")
            command = ["python3", str(ROOT / "scripts/check_proprietary.py")]
            self.assertNotEqual(subprocess.run(command, cwd=directory, capture_output=True).returncode, 0)
            git("commit", "-qm", "unsafe intermediate")
            git("rm", "-f", "safe.cpp")
            git("commit", "-qm", "deleted")
            self.assertEqual(subprocess.run(command, cwd=directory, capture_output=True).returncode, 0)
            self.assertNotEqual(subprocess.run(command + ["--history", base], cwd=directory, capture_output=True).returncode, 0)


if __name__ == "__main__":
    unittest.main()
