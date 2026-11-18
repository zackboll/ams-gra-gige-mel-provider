#!/usr/bin/env python3
"""SDK-free supervisor regressions; no real producer or network access."""
import argparse
import importlib.util
import json
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "qualification", Path(__file__).resolve().parents[1] / "scripts/gentl_qualification.py")
Q = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(Q)


class QualificationTests(unittest.TestCase):
    def test_status_signal_timeout_and_partial_logs(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            cases = [("ok", "print('complete')", "success", 0),
                     ("fail", "raise SystemExit(7)", "failure", 7),
                     ("signal", "import os,signal; os.kill(os.getpid(),signal.SIGTERM)",
                      "signal", -signal.SIGTERM),
                     ("hang", "import time; print('entered',flush=True); time.sleep(60)",
                      "timeout", -signal.SIGKILL)]
            for name, code, outcome, status in cases:
                result = Q.run_stage([sys.executable, "-c", code], output, name, 0.5)
                self.assertEqual(result["outcome"], outcome)
                self.assertEqual(result["returncode"], status)
            self.assertIn("entered", (output / "hang.stdout.log").read_text())
            result = Q.run_stage(["/nonexistent/gentl-probe"], output, "missing", 1)
            self.assertEqual(result["outcome"], "launch_error")

    def test_fail_closed_and_stop_after_first_failure(self):
        for fail_at in (0, 1, 3, None):
            with self.subTest(fail_at=fail_at), tempfile.TemporaryDirectory() as directory:
                args = argparse.Namespace(output=Path(directory) / "evidence",
                    producer=Path(__file__).resolve(), probe=Path(sys.executable), repeat=2, timeout=1)
                calls = []

                def stage(command, output, name, timeout):
                    calls.append(command)
                    return {"outcome": "failure" if len(calls) - 1 == fail_at else "success"}

                with patch.object(Q, "run_stage", side_effect=stage):
                    self.assertEqual(Q.collect(args), int(fail_at is not None))
                self.assertEqual(len(calls), 4 if fail_at is None else fail_at + 1)
                for command in calls:
                    self.assertEqual(command[:5], ["unshare", "--user", "--map-root-user", "--net", "--"])
                    self.assertFalse(set(command) & {"discover", "inspect", "acquire", "control"})
                report = json.loads((args.output / "result.json").read_text())
                self.assertFalse(report["device_open_requested"])
                self.assertEqual(report["read_only_safety"], "NOT_ASSESSED")
                if fail_at is not None:
                    self.assertEqual(report["native_cleanup"], "NOT_ESTABLISHED")
                with self.assertRaises(FileExistsError):
                    Q.collect(args)  # never overwrite an earlier evidence bundle

    def test_argument_bounds(self):
        for value in ("nan", "inf", "0", "301", "-2"):
            with self.assertRaises(argparse.ArgumentTypeError):
                Q.bounded_seconds(value)
        with self.assertRaises(argparse.ArgumentTypeError):
            Q.absolute_file("relative.cti")

    def test_cli_rejects_unsafe_requests_before_launch(self):
        script = Path(Q.__file__).resolve()
        common = [sys.executable, str(script), "--probe", sys.executable,
                  "--producer", str(Path(__file__).resolve())]
        for extra in (["--output", "relative"],
                      ["--output", str(script.parent.parent / "forbidden-evidence")],
                      ["--output", "/tmp/unused-gentl-evidence", "--command", "inspect"],
                      ["--output", "/tmp/unused-gentl-evidence", "--repeat", "0"],
                      ["--output", "/tmp/unused-gentl-evidence", "--timeout", "nan"]):
            result = subprocess.run(common + extra, capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 2, result.stderr)


if __name__ == "__main__":
    unittest.main()