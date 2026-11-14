#!/usr/bin/env python3
"""SDK-free negative contracts for the scoped GenApi TSan exception."""

from pathlib import Path
import hashlib
import sys
import tempfile
import unittest
from unittest import mock
import subprocess

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import pleora_tsan_exception as exception


PASS_LINES = "".join(f"PASS {name}\n" for name in exception.GROUPS)
SUCCESS = PASS_LINES + exception.COMPLETED + "\n"
# Original project-owned diagnostic structure, transcribed from the native run.
VENDOR_REPORT = ("""==================
WARNING: ThreadSanitizer: lock-order-inversion (potential deadlock) (pid=123)
  Cycle in lock order graph: M0 (0x7230000001d8) => M1 (0x7f01ab846a60) => M0

  Mutex M1 acquired here while holding mutex M0 in main thread:
    #0 pthread_mutex_lock interceptor.cpp:1341 (libtsan.so.2+0x558ce)
    #1 <null> <null> (liblog4cpp_gcc48_v3_4.so+0x1dba1)
    #2 pleora_node_map::Fixture::Fixture() pleora_node_map.cpp:143
    #3 main pleora_contract_tests.cpp:318

  Mutex M0 previously acquired by the same thread here:
    #0 pthread_mutex_lock interceptor.cpp:1341 (libtsan.so.2+0x558ce)
    #1 log4cpp_GenICam::Category::addAppender(log4cpp_GenICam::Appender&) <null> (liblog4cpp_gcc48_v3_4.so+0x1a75f)
    #2 pleora_node_map::Fixture::Fixture() pleora_node_map.cpp:143
    #3 main pleora_contract_tests.cpp:318

  Mutex M0 acquired here while holding mutex M1 in main thread:
    #0 pthread_mutex_lock interceptor.cpp:1341 (libtsan.so.2+0x558ce)
    #1 log4cpp_GenICam::Category::removeAllAppenders() <null> (liblog4cpp_gcc48_v3_4.so+0x1a47a)
    #2 <null> <null> (liblog4cpp_gcc48_v3_4.so+0x1d609)

  Mutex M1 previously acquired by the same thread here:
    #0 pthread_mutex_lock interceptor.cpp:1341 (libtsan.so.2+0x558ce)
    #1 <null> <null> (liblog4cpp_gcc48_v3_4.so+0x1d765)
    #2 <null> <null> (liblog4cpp_gcc48_v3_4.so+0x1d609)

SUMMARY: ThreadSanitizer: lock-order-inversion (potential deadlock) (/external/liblog4cpp_gcc48_v3_4.so+0x1dba1)
==================
""" + SUCCESS + "ThreadSanitizer: reported 1 warnings\n")


class ScopedExceptionTests(unittest.TestCase):
    def test_known_exact_report_accepted_only_as_unsuppressed_baseline(self):
        exception.verify_unsuppressed(66, VENDOR_REPORT)
        exception.verify_suppressed(0, SUCCESS)
        with self.assertRaises(exception.QualificationError):
            exception.verify_suppressed(66, VENDOR_REPORT)

    def test_additional_race_remains_failure(self):
        with self.assertRaises(exception.QualificationError):
            exception.verify_unsuppressed(66, VENDOR_REPORT + "WARNING: ThreadSanitizer: data race\n")
        with self.assertRaises(exception.QualificationError):
            exception.verify_suppressed(0, SUCCESS + "WARNING: ThreadSanitizer: data race\n")

    def test_different_lock_cycle_remains_failure(self):
        changed = VENDOR_REPORT.replace("0x1dba1", "0x1dba2")
        with self.assertRaises(exception.QualificationError):
            exception.verify_unsuppressed(66, changed)
        with self.assertRaises(exception.QualificationError):
            exception.verify_unsuppressed(66, VENDOR_REPORT.replace("removeAllAppenders", "unrelatedFunction"))

    def test_missing_groups_and_wrong_exit_fail(self):
        for code, output in ((0, VENDOR_REPORT), (66, VENDOR_REPORT.replace(f"PASS {exception.GROUPS[1]}\n", "")),
                             (66, VENDOR_REPORT.replace(exception.COMPLETED, "not complete"))):
            with self.subTest(code=code, length=len(output)):
                with self.assertRaises(exception.QualificationError):
                    exception.verify_unsuppressed(code, output)

    def test_duplicate_group_cannot_replace_missing_group(self):
        for verify, code, output in ((exception.verify_unsuppressed, 66, VENDOR_REPORT),
                                     (exception.verify_suppressed, 0, SUCCESS)):
            with self.assertRaises(exception.QualificationError):
                verify(code, output.replace(exception.GROUPS[1], exception.GROUPS[0]))

    def test_additional_diagnostics_and_crashes_fail_in_both_phases(self):
        for verify, code, output in ((exception.verify_unsuppressed, 66, VENDOR_REPORT),
                                     (exception.verify_suppressed, 0, SUCCESS)):
            for diagnostic in ("WARNING: unrelated runtime", "ERROR: AddressSanitizer: bad access",
                               "FATAL: crash", "SUMMARY: unrelated", "runtime error: undefined behavior",
                               "  WARNING: ThreadSanitizer: data race", "FAIL native group"):
                with self.subTest(diagnostic=diagnostic, phase=verify.__name__):
                    with self.assertRaises(exception.QualificationError):
                        verify(code, output + diagnostic + "\n")
            for status in (-11, -6, 1, 124):
                with self.assertRaises(exception.QualificationError):
                    verify(status, output)

    def test_cycle_structure_and_frame_association_fail_closed(self):
        for old, new in (("in main thread:", "in thread T1:"),
                         ("=> M0\n", "=> M2\n"),
                         ("#1 <null> <null> (liblog4cpp_gcc48_v3_4.so+0x1dba1)",
                          "#1 unrelated (liblog4cpp_gcc48_v3_4.so+0x1dba1)"),
                         ("0x1a75f", "0x1a47a"),
                         ("reported 1 warnings", "reported 2 warnings"),
                         ("==================\n", "")):
            with self.subTest(old=old):
                with self.assertRaises(exception.QualificationError):
                    exception.verify_unsuppressed(66, VENDOR_REPORT.replace(old, new))
        # Merely placing the expected offsets outside the report must not rescue it.
        changed = VENDOR_REPORT.replace("0x1dba1", "0x1dba2")
        with self.assertRaises(exception.QualificationError):
            exception.verify_unsuppressed(66, changed + "liblog4cpp_gcc48_v3_4.so+0x1dba1\n")

    def test_actual_loaded_libraries_required_not_just_search_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            runtime = Path(temporary)
            loaded = "".join(f"  123:\tcalling init: {runtime / name}\n"
                             for name in exception.EXPECTED_RUNTIME_HASHES)
            exception.verify_loaded_runtime(runtime, loaded)
            for output in ("", loaded + loaded, loaded.replace(str(runtime), "/different"),
                           loaded.replace("calling init:", "trying file=")):
                with self.assertRaises(exception.QualificationError):
                    exception.verify_loaded_runtime(runtime, output)

    def test_subprocess_timeout_is_failure_not_partial_success(self):
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / "contract"
            executable.write_text(f"#!{sys.executable}\nimport time\ntime.sleep(10)\n", encoding="utf-8")
            executable.chmod(0o700)
            with self.assertRaises(subprocess.TimeoutExpired):
                exception.run_contract(executable, {}, timeout=0.01)

    def test_main_never_reruns_after_bad_baseline_or_timeout(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / "contract"
            executable.touch()
            argv = ["runner", "--runtime-dir", str(root), "--suppression-file", str(root / "rule"),
                    "--contract", str(executable)]
            for failure in ((66, VENDOR_REPORT + "WARNING: unrelated\n"),
                            subprocess.TimeoutExpired("contract", 50, output=b"partial execution")):
                with mock.patch.object(sys, "argv", argv), \
                     mock.patch.object(exception, "verify_runtime"), \
                     mock.patch.object(exception, "verify_suppression"), \
                     mock.patch.object(exception, "verify_environment"), \
                     mock.patch.object(exception, "verify_loaded_runtime"), \
                     mock.patch.object(exception, "run_contract") as run:
                    if isinstance(failure, Exception):
                        run.side_effect = failure
                    else:
                        run.return_value = failure
                    with mock.patch("sys.stdout"), mock.patch("sys.stderr"):
                        self.assertEqual(exception.main(), 1)
                    self.assertEqual(run.call_count, 1)

    def test_main_checks_second_process_and_keeps_exception_local(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable, rule = root / "contract", root / "rule"
            executable.touch()
            rule.touch()
            argv = ["runner", "--runtime-dir", str(root), "--suppression-file", str(rule),
                    "--contract", str(executable)]
            for result, expected in (((0, SUCCESS), 0), ((-11, SUCCESS), 1),
                                     ((0, SUCCESS.replace(exception.COMPLETED, "")), 1),
                                     ((0, SUCCESS + "WARNING: unrelated\n"), 1)):
                with self.subTest(result=result), mock.patch.object(sys, "argv", argv), \
                     mock.patch.object(exception, "verify_runtime"), \
                     mock.patch.object(exception, "verify_suppression"), \
                     mock.patch.object(exception, "verify_environment"), \
                     mock.patch.object(exception, "verify_loaded_runtime") as loaded, \
                     mock.patch.object(exception, "run_contract", side_effect=[(66, VENDOR_REPORT), result]) as run, \
                     mock.patch("sys.stdout"), mock.patch("sys.stderr"):
                    self.assertEqual(exception.main(), expected)
                    self.assertEqual(loaded.call_count, 2)
                    baseline = run.call_args_list[0].args[1]["TSAN_OPTIONS"]
                    scoped = run.call_args_list[1].args[1]["TSAN_OPTIONS"]
                    self.assertNotIn("suppressions=", baseline)
                    self.assertEqual(scoped, "halt_on_error=1:exitcode=66:second_deadlock_stack=1:"
                                     f"suppressions={rule}")

    def test_single_suppression_rule_only(self):
        with tempfile.TemporaryDirectory() as temporary:
            rule = Path(temporary) / "only.supp"
            rule.write_text("# Narrow scope\n" + exception.SUPPRESSION + "\n", encoding="utf-8")
            exception.verify_suppression(rule)
            for extra in ("race:*", "deadlock:*", "deadlock:other_function"):
                rule.write_text(exception.SUPPRESSION + "\n" + extra + "\n", encoding="utf-8")
                with self.assertRaises(exception.QualificationError):
                    exception.verify_suppression(rule)

    def test_vendor_hash_guard_rejects_changed_library(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            expected = {}
            for name in exception.EXPECTED_RUNTIME_HASHES:
                content = name.encode("utf-8")
                (root / name).write_bytes(content)
                expected[name] = hashlib.sha256(content).hexdigest()
            exception.verify_runtime(root, expected)
            changed = next(iter(expected))
            (root / changed).write_bytes(b"different")
            with self.assertRaises(exception.QualificationError):
                exception.verify_runtime(root, expected)

    def test_unreviewed_tsan_flags_or_library_overrides_fail(self):
        with tempfile.TemporaryDirectory() as temporary:
            sdk = Path(temporary) / "sdk"
            runtime = sdk / "lib/genicam/bin/Linux64_x64"
            runtime.mkdir(parents=True)
            safe = {
                "LD_LIBRARY_PATH": f"{sdk / 'lib'}:{runtime}",
                "GENICAM_ROOT_V3_4": str(sdk / "lib/genicam"),
                "TSAN_OPTIONS": "halt_on_error=1:second_deadlock_stack=1",
            }
            exception.verify_environment(runtime, safe)
            for update in ({"TSAN_OPTIONS": "suppressions=/tmp/other.supp"},
                           {"TSAN_OPTIONS": "detect_deadlocks=0"},
                           {"TSAN_OPTIONS": "halt_on_error=0"},
                           {"LD_PRELOAD": "other.so"},
                            {"LD_AUDIT": "other.so"},
                            {"LD_DEBUG_OUTPUT": "/tmp/hidden-loader-trace"},
                           {"LD_LIBRARY_PATH": "/tmp/other:" + safe["LD_LIBRARY_PATH"]}):
                with self.subTest(update=update):
                    with self.assertRaises(exception.QualificationError):
                        exception.verify_environment(runtime, {**safe, **update})


if __name__ == "__main__":
    unittest.main()
