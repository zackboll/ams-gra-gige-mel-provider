#!/usr/bin/env python3
"""SDK-free negative contracts for the scoped GenApi TSan exception."""

from pathlib import Path
import hashlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import pleora_tsan_exception as exception


PASS_LINES = "".join(f"PASS group {i}\n" for i in range(8))
SUCCESS = PASS_LINES + exception.COMPLETED + "\n"
VENDOR_REPORT = (SUCCESS +
    exception.KNOWN_WARNING + " (pid=123)\n" +
    "log4cpp_GenICam::Category::addAppender\n" +
    "log4cpp_GenICam::Category::removeAllAppenders\n" +
    "\n".join(f"liblog4cpp_gcc48_v3_4.so+{address}" for address in exception.KNOWN_OFFSETS) + "\n" +
    exception.KNOWN_SUMMARY + "\n" +
    "ThreadSanitizer: reported 1 warnings\n")


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
        for code, output in ((0, VENDOR_REPORT), (66, VENDOR_REPORT.replace("PASS group 1\n", "")),
                             (66, VENDOR_REPORT.replace(exception.COMPLETED, "not complete"))):
            with self.subTest(code=code, length=len(output)):
                with self.assertRaises(exception.QualificationError):
                    exception.verify_unsuppressed(code, output)

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
                           {"LD_LIBRARY_PATH": "/tmp/other:" + safe["LD_LIBRARY_PATH"]}):
                with self.subTest(update=update):
                    with self.assertRaises(exception.QualificationError):
                        exception.verify_environment(runtime, {**safe, **update})


if __name__ == "__main__":
    unittest.main()
