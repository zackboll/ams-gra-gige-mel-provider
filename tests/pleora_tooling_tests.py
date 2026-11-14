#!/usr/bin/env python3
"""SDK-free safety contracts for the explicit external provisioning/launcher."""
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import provision_pleora as provision
import run_pleora as launcher


class Tooling(unittest.TestCase):
    def test_bad_archive_rejected_before_extraction(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "untrusted.zip"
            archive.write_bytes(b"not authorized header content")
            destination = root / "headers"
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                provision.provision_headers(archive, destination)
            self.assertFalse(destination.exists())

    def test_incomplete_prefix_never_overwritten(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            marker = root / "keep"
            marker.write_text("unchanged")
            with patch.object(sys, "argv", ["provision", str(root)]):
                with self.assertRaises((ValueError, OSError)):
                    provision.main()
            self.assertEqual(marker.read_text(), "unchanged")
            self.assertEqual(list(root.iterdir()), [marker])

    def test_repository_destination_refused_without_download(self):
        with patch.object(sys, "argv", ["provision", str(ROOT / "build" / "forbidden-sdk")]):
            with self.assertRaisesRegex(ValueError, "outside"):
                provision.main()

    def test_launcher_changes_only_child_environment_and_preserves_arguments(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sdk = root / "external"
            old = os.environ.copy()
            with patch.dict(os.environ, {"XDG_CACHE_HOME": str(root / "cache"), "LD_LIBRARY_PATH": "/existing"}):
                snapshot = os.environ.copy()
                with patch.object(sys, "argv", ["launch", str(sdk), "program", "inspect", "aa:bb:cc:dd:ee:ff"]), \
                     patch.object(launcher, "verify_sdk") as verify, patch.object(os, "execvpe") as execute:
                    launcher.main()
                    verify.assert_called_once_with(sdk)
                    program, arguments, child = execute.call_args.args
                    self.assertEqual(program, "program")
                    self.assertEqual(arguments, ["program", "inspect", "aa:bb:cc:dd:ee:ff"])
                    self.assertEqual(child["GENICAM_ROOT_V3_4"], str(sdk / "lib/genicam"))
                    self.assertTrue(child["LD_LIBRARY_PATH"].endswith(":/existing"))
                    self.assertEqual(dict(os.environ), snapshot)
            self.assertEqual(dict(os.environ), old)


if __name__ == "__main__":
    unittest.main()