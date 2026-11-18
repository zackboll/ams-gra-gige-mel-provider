#!/usr/bin/env python3
"""Runs the real gige_mel_gentl_probe executable against the fixture producer.

Offline only (GIGE-MEL-GENTL-001): proves argument gating, exact selection,
read-only inspection, repeated load/unload and exit-status propagation. It is
not evidence for any real producer or camera.
"""
import os
import subprocess
import sys
import tempfile
import unittest

PROBE, MOCK, MISSING = sys.argv[1:4]
del sys.argv[1:4]


def read(path):
    with open(path) as handle:
        return handle.read()


def run(*args, env=None):
    environment = {k: v for k, v in os.environ.items() if not k.startswith("GENTL_MOCK")}
    environment.update(env or {})
    return subprocess.run([PROBE, *args], capture_output=True, text=True, timeout=30, env=environment)


class ProbeTests(unittest.TestCase):
    def test_usage_and_gating(self):
        self.assertEqual(run("--help").returncode, 0)
        self.assertEqual(run().returncode, 2)
        self.assertEqual(run(MOCK, "unknown").returncode, 2)
        self.assertEqual(run(MOCK, "inspect", "mock-if-0").returncode, 2)  # device ID required
        # Host acquisition is refused without the explicit flag, before loading anything.
        with tempfile.NamedTemporaryFile() as log:
            result = run(MOCK, "acquire", "mock-if-0", "mock-dev-A", "1", env={"GENTL_MOCK_LOG": log.name})
            self.assertEqual(result.returncode, 2)
            self.assertIn("--allow-host-acquisition", result.stderr)
            self.assertEqual(read(log.name), "")

    def test_relative_and_missing_paths(self):
        for path in ("relative.cti", "/nonexistent/x.cti"):
            result = run(path, "load")
            self.assertEqual(result.returncode, 1, result.stdout)
            self.assertIn("FAILED", result.stdout)

    def test_offline_qualification_guards(self):
        with tempfile.NamedTemporaryFile() as log:
            for command in ("discover", "inspect", "acquire"):
                result = run(MOCK, "--offline-qualification", command, "mock-if-0", "mock-dev-A",
                             env={"GENTL_MOCK_LOG": log.name})
                self.assertEqual(result.returncode, 2)
            self.assertEqual(read(log.name), "")
            failed = run(MOCK, "--offline-qualification", "interfaces", env={
                "GENTL_MOCK_LOG": log.name, "GENTL_MOCK_FAIL": "TLUpdateInterfaceList=-1011"})
            self.assertEqual(failed.returncode, 1, failed.stdout)
            self.assertIn("GC_ERR_TIMEOUT", failed.stdout)
            self.assertNotIn("BEGIN TLGetNumInterfaces", failed.stdout)
            self.assertIn("unloaded", read(log.name))  # conclusive failure-path cleanup

    def test_wire_readonly_fixture_and_one_attempt_close(self):
        # Fixture only: demonstrates native request and ownership diagnostics, not
        # independently observed wire safety. No network traffic is generated.
        with tempfile.NamedTemporaryFile() as log:
            result = run(MOCK, "wire-readonly", "mock-if-0", "100", env={"GENTL_MOCK_LOG": log.name})
            # Existing fixture intentionally enumerates two devices: refuse an
            # ambiguous target before ANY open, never select the first silently.
            self.assertEqual(result.returncode, 1, result.stdout)
            self.assertIn("expected exactly one simulated device", result.stdout)
            self.assertNotIn("IFOpenDevice", read(log.name))
        for args in (("mock-if-0", "0"), ("mock-if-0", "10001"), ("mock-if-0", "100", "--access", "control")):
            result = run(MOCK, "wire-readonly", *args)
            self.assertEqual(result.returncode, 2)
        with tempfile.NamedTemporaryFile() as log:
            result = run(MOCK, "wire-readonly", "mock-if-0", "100", env={
                "GENTL_MOCK_LOG": log.name, "GENTL_MOCK_SINGLE_DEVICE": "1"})
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn("IFOpenDevice mock-dev-A access=2", read(log.name))
            self.assertIn("native_closed", result.stdout)
            self.assertIn("quiet_complete", result.stdout)
            self.assertIn("unloaded", read(log.name))
        for operation in ("DevClose", "IFClose", "TLClose", "GCCloseLib"):
            with tempfile.NamedTemporaryFile() as log:
                result = run(MOCK, "wire-readonly", "mock-if-0", "100", env={
                    "GENTL_MOCK_LOG": log.name, "GENTL_MOCK_SINGLE_DEVICE": "1",
                    "GENTL_MOCK_FAIL": operation + "=-1011"})
                self.assertEqual(result.returncode, 1, result.stdout)
                self.assertEqual(read(log.name).splitlines().count("fail " + operation), 1)
                self.assertNotIn("unloaded", read(log.name))
                self.assertNotIn("native_closed", result.stdout)

    def test_offline_close_failure_never_retried(self):
        for operation, command, preceding in (("TLClose", "interfaces", ""),
                                              ("GCCloseLib", "load", ""),
                                              ("TLClose", "interfaces", "TLUpdateInterfaceList=-1011,"),
                                              ("GCCloseLib", "interfaces", "TLUpdateInterfaceList=-1011,"),
                                              ("TLClose", "interfaces", "TLGetNumInterfaces=-1011,"),
                                              ("GCCloseLib", "interfaces", "TLGetNumInterfaces=-1011,")):
            with tempfile.NamedTemporaryFile() as log:
                failed = run(MOCK, "--offline-qualification", command, env={
                    "GENTL_MOCK_LOG": log.name, "GENTL_MOCK_FAIL": preceding + operation + "=-1011"})
                self.assertEqual(failed.returncode, 1, failed.stdout)
                self.assertIn(operation + " returned GC_ERR_TIMEOUT", failed.stdout)
                self.assertEqual(read(log.name).splitlines().count("fail " + operation), 1)
                self.assertNotIn("unloaded", read(log.name))

    def test_missing_symbol_reports_and_never_initializes(self):
        result = run(MISSING, "load")
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing mandatory entry points: TLOpen", result.stdout)

    def test_load_reports_native_failures_verbatim(self):
        result = run(MOCK, "load")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("TL_INFO_ID = gige-mel-gentl-mock", result.stdout)
        self.assertIn("TL_INFO_NAME = <GenTL GCGetInfo returned GC_ERR_NOT_IMPLEMENTED (-1003)", result.stdout)
        failed = run(MOCK, "load", env={"GENTL_MOCK_FAIL": "GCInitLib=-1005"})
        self.assertEqual(failed.returncode, 1)
        self.assertIn("GCInitLib returned GC_ERR_ACCESS_DENIED (-1005)", failed.stdout)

    def test_interfaces_do_not_discover_devices(self):
        with tempfile.NamedTemporaryFile() as log:
            result = run(MOCK, "interfaces", env={"GENTL_MOCK_LOG": log.name})
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn("interface mock-if-0", result.stdout)
            calls = read(log.name)
            self.assertNotIn("IFUpdateDeviceList", calls)
            self.assertNotIn("IFOpenDevice", calls)

    def test_interface_info_can_be_skipped(self):
        result = run(MOCK, "--no-interface-info", "interfaces")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("interface mock-if-0", result.stdout)
        self.assertNotIn("INTERFACE_INFO_DISPLAYNAME", result.stdout)
        self.assertIn("INTERFACE_INFO_DISPLAYNAME = Mock interface", run(MOCK, "interfaces").stdout)

    def test_inspect_is_read_only_and_exact(self):
        with tempfile.NamedTemporaryFile() as log:
            result = run(MOCK, "inspect", "mock-if-0", "mock-dev-B", env={"GENTL_MOCK_LOG": log.name})
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn("declared port byte order: big", result.stdout)
            self.assertIn("xml head: <?xml", result.stdout)
            calls = read(log.name).splitlines()
            self.assertIn("IFOpenDevice mock-dev-B access=2", calls)  # DEVICE_ACCESS_READONLY
            self.assertFalse(any(c.startswith(("GCWritePort", "DSStartAcquisition", "IFOpenDevice mock-dev-A"))
                                 for c in calls))
            self.assertEqual(calls[-1], "unloaded")
        wrong = run(MOCK, "inspect", "mock-if-0", "mock-dev")
        self.assertEqual(wrong.returncode, 1)
        self.assertIn("no fallback is selected", wrong.stdout)

    def test_repeat_and_explicit_host_acquisition(self):
        with tempfile.NamedTemporaryFile() as log:
            result = run(MOCK, "--repeat", "3", "acquire", "mock-if-0", "mock-dev-A", "4", "--reopen", "2",
                         "--allow-host-acquisition", env={"GENTL_MOCK_LOG": log.name})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout.count("received=4 mono8=4"), 6)
            self.assertIn("clock domain UNKNOWN", result.stdout)
            calls = read(log.name).splitlines()
            self.assertEqual(calls.count("loaded"), 3)
            self.assertEqual(calls.count("unloaded"), 3)
            self.assertEqual(calls.count("DSAnnounceBuffer"), calls.count("DSRevokeBuffer"))

    def test_stalled_acquisition_fails_without_hanging(self):
        result = run(MOCK, "acquire", "mock-if-0", "mock-dev-A", "1", "--allow-host-acquisition",
                     env={"GENTL_MOCK_STALL": "1"})
        self.assertEqual(result.returncode, 1)
        self.assertIn("GC_ERR_TIMEOUT", result.stdout)
        self.assertIn("received=0", result.stdout)


if __name__ == "__main__":
    unittest.main()
