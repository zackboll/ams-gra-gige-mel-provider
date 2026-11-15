#!/usr/bin/env python3
"""SDK-free GIGE-MEL-040-R2 orchestration tests. Not native qualification.

Exercise the fail-closed evaluation of the stage-selectable audit, runtime
identity and argument refusal, the gdb pthread-trace summarizer, and the
build/CTest isolation of the native executables. No SDK or network access.
"""
import argparse
import json
from pathlib import Path
import re
import socket
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import pleora_tsan_stage_audit as stage  # noqa: E402
import pthread_trace_summary as summary  # noqa: E402

# Literal shape of the retained R1/R2 fatal diagnostic (paths shortened).
CHECK_BODY = """ThreadSanitizer: CHECK failed: sanitizer_thread_registry.cpp:348 "((t)) != (0)" (0x0, 0x0) (tid=1)
    #0 __tsan::CheckUnwind() tsan_rtl.cpp:675 (libtsan.so.2+0x9f0b7)
    #1 __sanitizer::CheckFailed(char const*, int, char const*, unsigned long long, unsigned long long) (libtsan.so.2+0xd766d)
    #2 __sanitizer::ThreadRegistry::ConsumeThreadUserId(unsigned long) sanitizer_thread_registry.cpp:348 (libtsan.so.2+0xdb34c)
    #3 pthread_detach tsan_interceptors_posix.cpp:1094 (libtsan.so.2+0x50fab)
    #4 PtUtilsLib::Thread::WaitUntilSignaled() <null> (libPtUtilsLib.so.7.5+0x1d7bc)
    #5 PvStreamLib::Pipeline::Stop() <null> (libPvStream.so.6.5+0x2c75f)

"""


def complete_output(scenario):
    lines = [f"STAGE {name}" for name in stage.SCENARIOS[scenario]["stages"]]
    return "\n".join(lines + [f"SCENARIO_COMPLETE {scenario}"]) + "\n"


def through(scenario, last):
    expected = stage.SCENARIOS[scenario]["stages"]
    return "\n".join(f"STAGE {name}" for name in expected[:expected.index(last) + 1]) + "\n"


class EvaluationTests(unittest.TestCase):
    def test_only_complete_clean_run_passes(self):
        for scenario in stage.SCENARIOS:
            outcome = stage.evaluate(scenario, 0, complete_output(scenario))
            self.assertTrue(outcome["passed"], scenario)
            self.assertEqual(outcome["disposition"], "SCENARIO_COMPLETE")

    def test_fatal_check_is_never_success_even_with_zero_exit(self):
        text = through("s4", "before_pipeline_stop") + CHECK_BODY
        for returncode in (66, 0):  # a forced or zeroed exit status must still fail
            outcome = stage.evaluate("s4", returncode, text)
            self.assertFalse(outcome["passed"])
            self.assertEqual(outcome["disposition"], "FATAL_SANITIZER_CHECK")
            self.assertTrue(outcome["failure_inside_stop"])
            self.assertEqual(outcome["fatal_check"]["line"], 348)
            self.assertEqual(outcome["fatal_check"]["condition"], "((t)) != (0)")
            self.assertIn("PtUtilsLib::Thread::WaitUntilSignaled()", outcome["fatal_check"]["stack"][4])
        # Forged completion markers appended after a CHECK still cannot pass.
        self.assertFalse(stage.evaluate("s4", 0, text + complete_output("s4"))["passed"])

    def test_check_after_stop_returned_is_distinguished_from_inside_stop(self):
        text = through("s8", "before_close") + "STOP_RETURNED us=1\n" + CHECK_BODY
        outcome = stage.evaluate("s8", 66, text)
        self.assertFalse(outcome["failure_inside_stop"])
        self.assertEqual(outcome["last_stage"], "before_close")
        expected = stage.SCENARIOS["s8"]["stages"]
        self.assertEqual(outcome["executed_fraction"], f"{expected.index('before_close') + 1}/{len(expected)}")

    def test_missing_or_reordered_markers_fail(self):
        text = complete_output("s4")
        self.assertIn("COMPLETION_MARKER_MISSING",
                      stage.evaluate("s4", 0, text.replace("SCENARIO_COMPLETE s4", ""))["reasons"])
        self.assertIn("STAGE_SEQUENCE_MISMATCH",
                      stage.evaluate("s4", 0, text.replace("STAGE after_pipeline_stop\n", ""))["reasons"])
        swapped = text.replace("STAGE before_close", "STAGE tmp").replace(
            "STAGE pipeline_destroyed", "STAGE before_close").replace("STAGE tmp", "STAGE pipeline_destroyed")
        self.assertFalse(stage.evaluate("s4", 0, swapped)["passed"])
        other = text.replace("SCENARIO_COMPLETE s4", "SCENARIO_COMPLETE s2")
        self.assertFalse(stage.evaluate("s4", 0, other)["passed"])

    def test_unexpected_sanitizer_reports_fail(self):
        for line in ("WARNING: ThreadSanitizer: data race (pid=1)",
                     "WARNING: ThreadSanitizer: lock-order-inversion (potential deadlock) (pid=1)",
                     "==1==ERROR: AddressSanitizer: heap-use-after-free on address 0x1",
                     "==1==ERROR: LeakSanitizer: detected memory leaks",
                     "x.cpp:1:2: runtime error: signed integer overflow",
                     "ThreadSanitizer: Detach of non-existent thread"):
            outcome = stage.evaluate("s4", 0, complete_output("s4") + line + "\n")
            self.assertFalse(outcome["passed"], line)
            self.assertIn("SANITIZER_DIAGNOSTIC", outcome["reasons"])
            self.assertEqual(outcome["disposition"], "SANITIZER_REPORT")

    def test_timeout_identity_cleanup_and_observer_gates(self):
        text = complete_output("s2")
        for keyword, reason in (({"timed_out": True}, "CHILD_TIMEOUT"),
                                ({"identity_ok": False}, "RUNTIME_IDENTITY_MISSING"),
                                ({"cleanup_ok": False}, "CLEANUP_INCOMPLETE"),
                                ({"observers_ok": False}, "OBSERVER_GATE_FAILED")):
            outcome = stage.evaluate("s2", 0, text, **keyword)
            self.assertFalse(outcome["passed"])
            self.assertIn(reason, outcome["reasons"])
        missing = stage.evaluate("s2", None, text)
        self.assertIn("EXIT_STATUS_MISSING", missing["reasons"])
        self.assertFalse(missing["passed"])

    def test_sdk_results_and_step_failure_are_preserved(self):
        text = through("s6", "pipeline_started") + (
            "RESULT RetrieveNextBuffer code=0 name=OK description=\n"
            "RESULT Buffer.Operation code=1538 name=NO_LICENSE description=\n"
            "SDK_STEP_FAILED ReleaseBuffer\n")
        outcome = stage.evaluate("s6", 2, text)
        self.assertEqual(outcome["disposition"], "SDK_STEP_FAILED")
        self.assertEqual(outcome["sdk_results"][1], {"operation": "Buffer.Operation", "code": 1538,
                                                     "name": "NO_LICENSE", "description": ""})

    def test_negative_control_unexpected_success_or_other_diagnostic_rejected(self):
        expected = stage.negative_control_outcome(66, "STAGE before_invalid_second_detach\n" + CHECK_BODY)
        self.assertEqual(expected["status"], "EXPECTED_FATAL_CHECK")
        unexpected = stage.negative_control_outcome(0, "RESULT pthread_detach_second_INVALID rc=22\n")
        self.assertEqual(unexpected["status"], "UNEXPECTED_SUCCESS")
        self.assertFalse(unexpected["characterized"])
        for code, text in ((66, "WARNING: ThreadSanitizer: data race\n"), (0, CHECK_BODY),
                           (66, CHECK_BODY + "WARNING: ThreadSanitizer: data race\n"),
                           (66, CHECK_BODY.replace(":348", ":349"))):
            self.assertFalse(stage.negative_control_outcome(code, text)["characterized"])


# Literal shape of the retained minimal S2 diagnostic (inside PvStream::Close()).
S2_CHECK = """ThreadSanitizer: CHECK failed: sanitizer_thread_registry.cpp:348 "((t)) != (0)" (0x0, 0x0) (tid=1)
    #2 __sanitizer::ThreadRegistry::ConsumeThreadUserId(unsigned long) sanitizer_thread_registry.cpp:348
    #3 pthread_detach tsan_interceptors_posix.cpp:1094 (libtsan.so.2+0x50fab)
    #4 PtUtilsLib::Thread::WaitUntilSignaled() <null> (libPtUtilsLib.so.7.5+0x1d7bc)
    #5 PvStreamLib::DataReceiverUM2Thread::StopThread() <null> (libPvStream.so.6.5+0x3a604)
    #8 PvStream::Close() <null> (libPvStream.so.6.5+0x30181)

"""


class ReproducerTests(unittest.TestCase):
    def reproduce(self, returncode, text, variant="gcc-tsan", scenario="s2"):
        outcome = stage.evaluate(scenario, returncode, text)
        return outcome, stage.reproducer_outcome(scenario, variant, outcome)

    def test_minimal_s2_check_is_characterized_but_never_passes(self):
        for variant in ("gcc-tsan", "clang-tsan"):
            outcome, result = self.reproduce(66, through("s2", "before_close") + S2_CHECK, variant)
            self.assertEqual(result["status"], "REPRODUCED_VENDOR_THREAD_LIFECYCLE_CHECK")
            self.assertFalse(outcome["passed"])
            self.assertFalse(result["qualification_pass"])
            self.assertEqual(outcome["disposition"], "FATAL_SANITIZER_CHECK")

    def test_unexpected_success_of_fatal_reproducer_is_rejected(self):
        outcome, result = self.reproduce(0, complete_output("s2"))
        self.assertTrue(outcome["passed"])  # the scenario itself completed ...
        self.assertEqual(result["status"], "UNEXPECTED_SUCCESS")  # ... but the reproducer did not reproduce
        self.assertFalse(result["reproduced"])
        self.assertFalse(result["qualification_pass"])
        # A TSan run that exits 0 with no diagnostic but lost its markers is not a reproduction either.
        self.assertEqual(self.reproduce(0, through("s2", "before_close"))[1]["status"], "UNEXPECTED_SUCCESS")

    def test_altered_or_mixed_failures_are_not_characterized(self):
        base = through("s2", "before_close")
        cases = ((0, base + S2_CHECK),                                              # zero exit
                 (None, base + S2_CHECK),                                           # missing exit status
                 (66, base + S2_CHECK.replace(":348", ":349")),                     # different CHECK
                 (66, base + S2_CHECK.replace("WaitUntilSignaled", "Other")),       # different frame
                 (66, base + S2_CHECK.replace("PvStream::Close()", "Unrelated()")),  # not inside Close
                 (66, through("s2", "stream_opened") + S2_CHECK),                   # wrong stage
                 (66, base + "WARNING: ThreadSanitizer: data race (pid=1)\n" + S2_CHECK),
                 (66, base + S2_CHECK + complete_output("s2")))                     # forged completion
        for returncode, text in cases:
            outcome, result = self.reproduce(returncode, text)
            self.assertFalse(outcome["passed"], text[-120:])
            self.assertFalse(result["reproduced"], text[-120:])
        self.assertEqual(self.reproduce(0, complete_output("s2"), "gcc-debug")[1]["status"], "NOT_A_TSAN_REPRODUCER")
        # Observed under gdb: a tracer notice splitting the CHECK from its stack is never
        # guessed through; the stack is unproven, so the run is not characterized.
        lines = S2_CHECK.splitlines()
        split = "\n".join([lines[0], "[Detaching after fork from child process 1]", *lines[1:]]) + "\n"
        outcome, result = self.reproduce(66, base + split)
        self.assertEqual(outcome["fatal_check"]["stack"], [])
        self.assertEqual(result["status"], "DIFFERENT_FAILURE")

    def test_gdb_tracer_suppresses_its_own_inferior_event_notices(self):
        tracer = (ROOT / "scripts/pthread_lifecycle_trace_gdb.py").read_text()
        for setting in ("set print thread-events off", "set print inferior-events off", "set debuginfod enabled off"):
            self.assertIn(f'gdb.execute("{setting}")', tracer)
        # Observation only: no vendor-code breakpoints, writes or interposition.
        self.assertNotRegex(tracer, r"write_memory|LD_PRELOAD|disassemble|x/\d*i")

    def test_incomplete_trace_or_missing_completion_evidence_fails(self):
        good = [{"event": "trace_ready"}, {"event": "inferior_exit", "exit_code": 0}, {"event": "trace_finished"}]
        self.assertTrue(summary.completeness(good)["complete"])
        for broken in (good[:2], good[1:], [good[0], good[2]], good[:2] + good[1:],
                       [good[0], {"event": "inferior_exit", "exit_code": None}, good[2]], []):
            result = summary.completeness(broken)
            self.assertFalse(result["complete"], broken)
            outcome = stage.trace_gate(stage.evaluate("s2", 0, complete_output("s2")),
                                       {"trace": result, "join_then_detach": []})
            self.assertFalse(outcome["passed"])
            self.assertIn("TRACE_INCOMPLETE", outcome["reasons"])
            self.assertEqual(outcome["disposition"], "INCOMPLETE")
        kept = stage.trace_gate(stage.evaluate("s2", 0, complete_output("s2")),
                                {"trace": summary.completeness(good), "join_then_detach": []})
        self.assertTrue(kept["passed"])


class ArgumentAndEnvironmentTests(unittest.TestCase):
    base = ["--binary", "/x/b", "--sdk", "/x/sdk", "--output", "/tmp/out", "--scenario", "s4"]

    def test_unsupported_variants_tracers_and_fixtures_refused(self):
        refused = ([*self.base, "--variant", "gcc-msan"],
                   [*self.base, "--variant", "gcc-asan", "--tracer", "gdb"],      # LSan cannot run under ptrace
                   [*self.base, "--variant", "gcc-asan", "--tracer", "strace"],
                   [*self.base, "--variant", "gcc-tsan", "--tracer", "strace"],   # strace path missing
                   [*self.base[:-1], "s9", "--variant", "gcc-tsan"],
                   [*self.base[:-1], "s6", "--variant", "gcc-tsan"],             # needs pinned Aravis
                   [*self.base, "--variant", "gcc-tsan", "--timeout", "1"])
        for argv in refused:
            with self.assertRaises(SystemExit, msg=argv), patch("sys.stderr"):
                stage.parse(argv)
        args = stage.parse([*self.base, "--variant", "gcc-tsan", "--tracer", "gdb"])
        self.assertEqual((args.variant, args.tracer), ("gcc-tsan", "gdb"))

    def test_options_are_unsuppressed_and_never_mask_checks(self):
        for variant, options in stage.SANITIZER_OPTIONS.items():
            joined = ":".join(options.values())
            for forbidden in ("suppressions", "report_bugs=0", "report_thread_leaks=0", "detect_deadlocks=0",
                              "ignore_noninstrumented_modules", "exitcode=0", "die_after_fork=0", "verbosity"):
                self.assertNotIn(forbidden, joined, variant)
        self.assertEqual(stage.SANITIZER_OPTIONS["gcc-tsan"]["TSAN_OPTIONS"],
                         "halt_on_error=0:exitcode=66:second_deadlock_stack=1")
        self.assertEqual(stage.SANITIZER_OPTIONS["gcc-tsan"], stage.SANITIZER_OPTIONS["clang-tsan"])

    def test_inherited_interposition_or_sanitizer_environment_refused(self):
        args = argparse.Namespace(sdk=Path("/x/sdk"), variant="gcc-tsan", tracer="none")
        for key in ("LD_PRELOAD", "LD_AUDIT", "TSAN_OPTIONS", "ASAN_OPTIONS", "LSAN_OPTIONS"):
            with patch.dict(stage.os.environ, {key: "anything"}):
                with self.assertRaisesRegex(RuntimeError, "refused"):
                    stage.native_environment(args, Path("/tmp/out"))


    def test_namespace_isolation_required_before_sdk_or_topology(self):
        with patch.object(socket, "if_nameindex", return_value=[(1, "lo"), (2, "eth0")]), \
             patch.object(stage.veth, "command") as command, patch.object(stage, "verify_sdk") as sdk, \
             patch.object(stage.subprocess, "Popen") as popen:
            with self.assertRaisesRegex(RuntimeError, "ENVIRONMENT_BLOCKED"):
                stage.coordinator(argparse.Namespace(output=Path("/tmp/none"), scenario="s4"))
            command.assert_not_called()
            sdk.assert_not_called()
            popen.assert_not_called()

    def test_private_output_inside_repository_refused(self):
        args = stage.parse([*self.base[:4], "--output", str(ROOT / "build" / "evidence"), "--scenario", "s4",
                            "--variant", "gcc-tsan"])
        with patch.object(stage.veth, "network") as network:
            with self.assertRaisesRegex(RuntimeError, "outside the repository"):
                stage.outer(args)
            network.assert_not_called()

    def test_gdb_exit_status_requires_single_inferior_record(self):
        with tempfile.TemporaryDirectory() as temporary:
            trace = Path(temporary) / "pthread-trace.jsonl"
            self.assertIsNone(stage.inferior_exit(trace, 0))
            trace.write_text(json.dumps({"event": "inferior_exit", "exit_code": 66}) + "\n")
            self.assertEqual(stage.inferior_exit(trace, 0), 66)
            self.assertIsNone(stage.inferior_exit(trace, 1))  # gdb itself failed
            with trace.open("a") as log:
                log.write(json.dumps({"event": "inferior_exit", "exit_code": 0}) + "\n")
            self.assertIsNone(stage.inferior_exit(trace, 0))

    def test_runtime_inventory_requires_loaded_sdk_identities(self):
        with tempfile.TemporaryDirectory() as temporary:
            sdk = Path(temporary)
            names = ("libPvStream.so.6.5", "libPtUtilsLib.so.7.5", "libPvBase.so.6.5")
            for name in names:
                (sdk / name).write_bytes(b"x")
            maps = "\n".join(f"7f-80 r-xp 0 00:00 1 {sdk / name}" for name in names)
            self.assertFalse(stage.runtime_inventory(maps, sdk)[1])  # libPvBuffer never mapped
            (sdk / "libPvBuffer.so.6.5").write_bytes(b"y")
            loaded, complete = stage.runtime_inventory(maps + f"\n7f-80 r-xp 0 00:00 1 {sdk / 'libPvBuffer.so.6.5'}", sdk)
            self.assertTrue(complete)
            self.assertEqual(len(loaded), 4)
            self.assertTrue(all(len(item["sha256"]) == 64 for item in loaded.values()))


class TraceSummaryTests(unittest.TestCase):
    @staticmethod
    def event(name, lwp, t, **fields):
        return {"event": name, "lwp": lwp, "t_ns": t, **fields}

    def test_join_then_detach_found_and_interceptor_nesting_counted_once(self):
        frames = [{"function": "pthread_detach", "object": "/lib/libtsan.so.2"},
                  {"function": "PtUtilsLib::Thread::WaitUntilSignaled()", "object": "/sdk/libPtUtilsLib.so.7.5"}]
        records = [self.event("pthread_create", 1, 1, stack=frames), self.event("pthread_create", 1, 2),
                   self.event("pthread_create_return", 1, 3, result=0, pthread_t="0xa"),
                   self.event("pthread_create_return", 1, 4, result=0, pthread_t="0xa"),
                   self.event("pthread_join", 1, 5, pthread_t="0xa", stack=frames),
                   self.event("pthread_join", 1, 6, pthread_t="0xa"),
                   self.event("pthread_join_return", 1, 7, result=0),
                   self.event("pthread_join_return", 1, 8, result=0),
                   self.event("pthread_detach", 1, 9, pthread_t="0xa", stack=frames,
                              implementation="/lib/libtsan.so.2"),
                   self.event("sanitizer_check_failed", 1, 10, stack=frames)]
        result = summary.analyze(records)
        thread = result["threads"]["0xa"]
        self.assertEqual(thread["created"], 1)
        self.assertEqual(len(thread["join"]), 1)
        self.assertTrue(thread["detach"][0]["unreturned"])
        self.assertEqual(thread["detach"][0]["prior"], ["pthread_join"])
        findings = {item["finding"] for item in result["findings"]}
        self.assertIn("consumed more than once", findings)
        self.assertIn("pthread_detach entered but never returned", findings)
        self.assertEqual(result["order"][-1]["event"], "sanitizer_check_failed")

    def test_create_join_detach_sequence_reported_with_actual_return_values(self):
        wait = [{"function": "___pthread_join", "object": "/lib/libc.so.6"},
                {"function": "PtUtilsLib::Thread::WaitUntilSignaled()", "object": "/sdk/libPtUtilsLib.so.7.5"}]
        lifecycle = [self.event("trace_ready", None, 0),
                     self.event("pthread_create", 1, 1), self.event("pthread_create_return", 1, 2, result=0, pthread_t="0xd"),
                     self.event("pthread_join", 1, 3, pthread_t="0xd", stack=wait),
                     self.event("pthread_join_return", 1, 4, result=0),
                     self.event("pthread_detach", 1, 5, pthread_t="0xd", stack=wait)]
        ordinary = summary.analyze(lifecycle + [self.event("pthread_detach_return", 1, 6, result=3),
                                                self.event("inferior_exit", None, 7, exit_code=0),
                                                self.event("trace_finished", None, 8)])
        self.assertTrue(ordinary["trace"]["complete"])
        [finding] = ordinary["join_then_detach"]
        self.assertEqual((finding["create"]["result"], finding["join"]["result"], finding["detach"]["result"]), (0, 0, 3))
        self.assertEqual(finding["detach"]["caller"], "PtUtilsLib::Thread::WaitUntilSignaled()@libPtUtilsLib.so.7.5")
        aborted = summary.analyze(lifecycle + [self.event("inferior_exit", None, 7, exit_code=66),
                                               self.event("trace_finished", None, 8)])
        [finding] = aborted["join_then_detach"]
        self.assertIsNone(finding["detach"]["result"])
        self.assertTrue(finding["detach"]["unreturned"])
        self.assertEqual(aborted["trace"]["inferior_exit"], 66)
        # A failed join or a detach before the join is not this sequence.
        failed = [dict(r, result=22) if r["event"] == "pthread_join_return" else r for r in lifecycle]
        self.assertEqual(summary.analyze(failed)["join_then_detach"], [])
        early = [self.event("pthread_create", 1, 1), self.event("pthread_create_return", 1, 2, result=0, pthread_t="0xe"),
                 self.event("pthread_detach", 1, 3, pthread_t="0xe"), self.event("pthread_detach_return", 1, 4, result=0),
                 self.event("pthread_join", 1, 5, pthread_t="0xe"), self.event("pthread_join_return", 1, 6, result=22)]
        self.assertEqual(summary.analyze(early)["join_then_detach"], [])

    def test_valid_lifecycle_has_no_findings_and_invisible_create_is_reported(self):
        valid = [self.event("pthread_create", 1, 1), self.event("pthread_create_return", 1, 2, result=0, pthread_t="0xb"),
                 self.event("pthread_detach", 1, 3, pthread_t="0xb"), self.event("pthread_detach_return", 1, 4, result=0)]
        self.assertEqual(summary.analyze(valid)["findings"], [])
        invisible = [self.event("pthread_detach", 1, 1, pthread_t="0xc"),
                     self.event("pthread_detach_return", 1, 2, result=0)]
        self.assertEqual(summary.analyze(invisible)["findings"][0]["finding"],
                         "detached/joined but no observed pthread_create")



class IsolationTests(unittest.TestCase):
    def test_native_targets_default_off_uninstalled_and_unregistered(self):
        top = (ROOT / "CMakeLists.txt").read_text()
        pleora = (ROOT / "cmake/GigeMelPleora.cmake").read_text()
        self.assertRegex(top, r'option\(GIGE_MEL_PLEORA_STREAM_TRAFFIC_AUDIT "[^"]*" OFF\)')
        block = pleora.split("if(GIGE_MEL_PLEORA_STREAM_TRAFFIC_AUDIT)", 1)[1].split("endif()", 1)[0]
        self.assertIn("add_executable(pleora_tsan_stage_audit", block)
        native = r"(?<![/\w])pleora_tsan_stage_audit(?![\w.])"  # the executable, not the SDK-free test
        for text in (top, pleora):
            for command in re.findall(r"(?:add_test|install)\([^)]*\)", text):
                self.assertNotRegex(command, native)
                self.assertNotIn("TARGET_FILE:pleora_tsan_stage_audit", command)
            self.assertNotIn("pthread_tsan_controls", text)
            self.assertNotIn("pthread_negative_controls", text)
        # The SDK-free orchestration test is registered and runs only Python.
        self.assertRegex(top, r"add_test\(NAME pleora_tsan_stage_audit_tooling_contract\s+"
                              r"COMMAND \$\{Python3_EXECUTABLE\} \$\{CMAKE_CURRENT_SOURCE_DIR\}/tests/"
                              r"pleora_tsan_stage_audit_tests\.py\)")
        controls = (ROOT / "tests/repro/pthread_tsan_controls/CMakeLists.txt").read_text()
        self.assertNotIn("add_test", controls)
        self.assertIn("PTHREAD_CONTROLS_NEGATIVE=1", controls)

    def test_no_new_suppression_or_exception_widening(self):
        supp = (ROOT / "tests/support/pleora_genapi_tsan.supp").read_text()
        directives = [line for line in supp.splitlines() if line.strip() and not line.lstrip().startswith("#")]
        self.assertEqual(directives, ["deadlock:log4cpp_GenICam::Category::addAppender"])
        for path in (ROOT / "scripts/pleora_tsan_stage_audit.py", ROOT / "tests/pleora_tsan_stage_audit.cpp",
                     ROOT / "tests/repro/pthread_tsan_controls/pthread_controls.cpp"):
            text = path.read_text()
            self.assertNotIn("__tsan_default_suppressions", text)
            self.assertNotIn("__tsan_default_options", text)
            self.assertIsNone(re.search(r"suppressions=", text), path)

    def test_findings_documented_without_waiver_and_vendor_record_is_unsent(self):
        doc = (ROOT / "docs/pleora-native-tsan-investigation.md").read_text()
        for required in ("VENDOR_THREAD_LIFECYCLE_FAILURE", "SANITIZER_RUNTIME_DEFECT_OR_LIMITATION",
                         "NATIVE_TSAN_BLOCKED", "PRODUCTION_INTEGRATION_BLOCKED", "READY_FOR_INDEPENDENT_REVIEW",
                         "1538 NO_LICENSE", "byte-identical", "not two independent", "**3 ESRCH**"):
            self.assertIn(required, doc)
        self.assertIn("pleora-native-tsan-investigation.md",
                      (ROOT / "docs/pleora-stream-only-audit.md").read_text())
        vendor = (ROOT / "docs/pleora-tsan-vendor-escalation.md").read_text()
        self.assertIn("NOT SENT", vendor)
        # Shareable record: symbols/versions only; no disassembly, captures or license material.
        self.assertNotRegex(vendor, r"(?i)\b(mov|call|jmp|push)\s+%|objdump|\.pcap|BEGIN [A-Z ]*KEY|"
                                    r"end\s+user\s+license\s+agreement")

    def test_r1_harness_and_findings_preserved(self):
        r1 = (ROOT / "tests/pleora_stream_only_audit.cpp").read_text()
        self.assertIn('result("Pipeline.Stop", pipeline->Stop())', r1)
        doc = (ROOT / "docs/tasks/GIGE-MEL-040-R1-validation.md").read_text()
        for finding in ("1538 NO_LICENSE", "sanitizer_thread_registry.cpp:348 ((t)) != (0)",
                        "Native exit 66", "This is **not** the known external lock-order cycle"):
            self.assertIn(finding, doc)


if __name__ == "__main__":
    unittest.main()
