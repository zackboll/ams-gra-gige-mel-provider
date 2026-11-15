#!/usr/bin/env python3
"""GIGE-MEL-040-R2 stage-selectable native lifecycle audit. Explicit only.

Never registered in CTest/CI, never run against a physical camera. Reuses the
R1 two-namespace veth topology, both packet observers and fail-closed cleanup.
Exit zero ONLY for a complete scenario with every completion marker, no
sanitizer diagnostic, successful cleanup and healthy observers. A fatal
sanitizer CHECK, report, timeout, missing marker or missing runtime identity is
never success. Private evidence must stay outside the repository.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import socket
import subprocess
import sys
import time

import pleora_veth_audit as veth
from pleora_stream_audit import Capture, inventory
from provision_pleora import verify_sdk
import pthread_trace_summary as pthread_summary

COMMON_OPEN = ["stream_constructed", "stream_opened"]
COMMON_CLOSE = ["before_close", "after_close", "stream_destroyed"]
PIPELINE = ["pipeline_constructed"]
START = ["before_pipeline_start", "pipeline_started"]
STOP = ["before_pipeline_stop", "after_pipeline_stop"]
DESTROY = ["before_pipeline_destroy", "pipeline_destroyed"]
DIRECT = ["buffers_queued", "receive_returned", "buffers_dequeued"]


def _sequence(*parts):
    return ["runtime_initialized", *[stage for part in parts for stage in part], "before_process_exit"]


# Expected completion markers, in order; fixture, frames and the operations.
SCENARIOS = {
    "s0": {"fixture": "validated", "frames": False, "stages": _sequence(),
           "operations": "SDK initialization and ordinary process exit"},
    "s1": {"fixture": "validated", "frames": False, "stages": _sequence(["stream_constructed", "stream_destroyed"]),
           "operations": "PvStreamGEV construct/destroy"},
    "s2": {"fixture": "validated", "frames": False, "stages": _sequence(COMMON_OPEN, COMMON_CLOSE),
           "operations": "Open/Close, no pipeline"},
    "s3": {"fixture": "validated", "frames": False, "stages": _sequence(COMMON_OPEN, PIPELINE, DESTROY, COMMON_CLOSE),
           "operations": "PvPipeline construct/destroy, no Start"},
    "s4": {"fixture": "validated", "frames": False,
           "stages": _sequence(COMMON_OPEN, PIPELINE, START, STOP, DESTROY, COMMON_CLOSE),
           "operations": "Pipeline Start/Stop, no receive"},
    "s5": {"fixture": "validated", "frames": False,
           "stages": _sequence(COMMON_OPEN, PIPELINE, START, ["receive_returned"], STOP, DESTROY, COMMON_CLOSE),
           "operations": "Start, timed receive, Stop"},
    "s6": {"fixture": "aravis", "frames": True,
           "stages": _sequence(COMMON_OPEN, PIPELINE, START, ["buffer_released"], STOP, DESTROY, COMMON_CLOSE),
           "operations": "Start, retrieve/release genuine buffer, Stop"},
    "s7": {"fixture": "validated", "frames": False,
           "stages": _sequence(COMMON_OPEN, PIPELINE, START, STOP, DESTROY, PIPELINE, START, STOP, DESTROY, COMMON_CLOSE),
           "operations": "Two supported Start/Stop lifecycles on one stream"},
    "s8": {"fixture": "validated", "frames": False,
           "stages": _sequence(COMMON_OPEN, PIPELINE, START, STOP, DESTROY, ["post_stop_quiescent"], COMMON_CLOSE),
           "operations": "Close/destruction after successful Stop and quiescence"},
    "d1": {"fixture": "validated", "frames": False, "stages": _sequence(COMMON_OPEN, DIRECT, COMMON_CLOSE, ["buffers_freed"]),
           "operations": "Direct PvStream queue/timed retrieve/abort/dequeue, no PvPipeline"},
    "d2": {"fixture": "aravis", "frames": True, "stages": _sequence(COMMON_OPEN, DIRECT, COMMON_CLOSE, ["buffers_freed"]),
           "operations": "Direct PvStream with genuine buffer retrieve/requeue, no PvPipeline"},
}
FRAME_STAGE = {"s6": "pipeline_started", "d2": "buffers_queued"}

# Exactly the R1 unsuppressed options. Nothing here may suppress or mask a CHECK.
SANITIZER_OPTIONS = {
    "gcc-debug": {}, "clang-debug": {},
    "gcc-asan": {"ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
                 "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"},
    "gcc-tsan": {"TSAN_OPTIONS": "halt_on_error=0:exitcode=66:second_deadlock_stack=1"},
    "clang-tsan": {"TSAN_OPTIONS": "halt_on_error=0:exitcode=66:second_deadlock_stack=1"},
}
TRACERS = {"none": ("gcc-debug", "clang-debug", "gcc-asan", "gcc-tsan", "clang-tsan"),
           "strace": ("gcc-debug", "clang-debug", "gcc-tsan", "clang-tsan"),
           # LeakSanitizer cannot run under ptrace; refuse instead of degrading ASan.
           "gdb": ("gcc-debug", "clang-debug", "gcc-tsan", "clang-tsan")}
CONTROLLED_ENVIRONMENT = ("LD_PRELOAD", "LD_AUDIT", "LD_DEBUG", "LD_DEBUG_OUTPUT", "TSAN_OPTIONS",
                          "ASAN_OPTIONS", "UBSAN_OPTIONS", "LSAN_OPTIONS", "MSAN_OPTIONS")
CLANG_TSAN_ARCHIVE = Path("/usr/lib/llvm-19/lib/clang/19/lib/linux/libclang_rt.tsan-x86_64.a")
REQUIRED_SDK = ("libPvStream.so", "libPtUtilsLib.so", "libPvBase.so", "libPvBuffer.so")

CHECK = re.compile(r"ThreadSanitizer: CHECK failed: (\S+):(\d+) \"(.*)\"")
DIAGNOSTICS = (
    ("tsan_report", re.compile(r"WARNING: ThreadSanitizer: (.*)")),
    ("asan_report", re.compile(r"ERROR: AddressSanitizer: (.*)")),
    ("lsan_report", re.compile(r"ERROR: LeakSanitizer: (.*)")),
    ("ubsan_report", re.compile(r"runtime error: (.*)")),
    ("registry_report", re.compile(r"ThreadSanitizer: ((Detach|Join) of non-existent thread.*)")),
    ("other_sanitizer", re.compile(r"((Thread|Address|Leak|UndefinedBehavior|Memory)Sanitizer.*)")),
)


def classify_output(text):
    """Classify every sanitizer diagnostic in the complete, unmodified output."""
    lines = text.splitlines()
    diagnostics = []
    fatal = None
    for index, line in enumerate(lines):
        match = CHECK.search(line)
        if match:
            stack = []
            for frame in lines[index + 1:]:
                if not frame.startswith("    #"):
                    break
                stack.append(frame.strip())
            fatal = {"file": match.group(1), "line": int(match.group(2)), "condition": match.group(3),
                     "text": line.strip(), "stack": stack}
            diagnostics.append({"kind": "fatal_check", "text": line.strip()})
            continue
        for kind, pattern in DIAGNOSTICS:
            if pattern.search(line):
                diagnostics.append({"kind": kind, "text": line.strip()})
                break
    return diagnostics, fatal


def parse_native(text):
    stages, results, markers = [], [], []
    for line in text.splitlines():
        if line.startswith("STAGE "):
            stages.append(line[6:].strip())
        elif line.startswith("RESULT "):
            fields = line[7:].split(" ", 3)
            entry = {"operation": fields[0]}
            for field in fields[1:]:
                key, _, value = field.partition("=")
                entry[key] = int(value) if key == "code" and value.lstrip("-").isdigit() else value
            results.append(entry)
        elif line.startswith(("SCENARIO_COMPLETE ", "SDK_STEP_FAILED", "STOP_RETURNED", "BUFFER ",
                              "PIPELINE_CYCLE ", "LOCAL ", "SCENARIO ", "DEQUEUED ", "QUEUED")):
            markers.append(line.strip())
    return stages, results, markers


def evaluate(scenario, returncode, text, timed_out=False, identity_ok=True, cleanup_ok=True, observers_ok=True):
    """Fail-closed outcome. Only a complete, diagnostic-free, cleaned-up run passes."""
    expected = SCENARIOS[scenario]["stages"]
    stages, results, markers = parse_native(text)
    diagnostics, fatal = classify_output(text)
    reasons = []
    if timed_out:
        reasons.append("CHILD_TIMEOUT")
    if returncode is None:
        reasons.append("EXIT_STATUS_MISSING")
    elif returncode != 0:
        reasons.append(f"NATIVE_EXIT_{returncode}")
    if stages != expected:
        reasons.append("STAGE_SEQUENCE_MISMATCH")
    if f"SCENARIO_COMPLETE {scenario}" not in markers:
        reasons.append("COMPLETION_MARKER_MISSING")
    if diagnostics:
        reasons.append("SANITIZER_DIAGNOSTIC")
    if not identity_ok:
        reasons.append("RUNTIME_IDENTITY_MISSING")
    if not cleanup_ok:
        reasons.append("CLEANUP_INCOMPLETE")
    if not observers_ok:
        reasons.append("OBSERVER_GATE_FAILED")
    if fatal:
        disposition = "FATAL_SANITIZER_CHECK"
    elif diagnostics:
        disposition = "SANITIZER_REPORT"
    elif any(marker.startswith("SDK_STEP_FAILED") for marker in markers):
        disposition = "SDK_STEP_FAILED"
    elif reasons:
        disposition = "INCOMPLETE"
    else:
        disposition = "SCENARIO_COMPLETE"
    stop_entered = "before_pipeline_stop" in stages
    stop_returned = any(marker.startswith("STOP_RETURNED") for marker in markers)
    return {"scenario": scenario, "disposition": disposition, "passed": not reasons,
            "reasons": reasons, "returncode": returncode, "timed_out": timed_out,
            "stages": stages, "expected_stages": expected,
            "last_stage": stages[-1] if stages else None,
            "executed_fraction": f"{len(stages)}/{len(expected)}",
            "stop_entered": stop_entered, "stop_returned": stop_returned,
            "failure_inside_stop": bool(fatal) and stop_entered and not stop_returned,
            "sdk_results": results, "markers": markers, "diagnostics": diagnostics, "fatal_check": fatal}


def negative_control_outcome(returncode, text):
    """Characterize a deliberately invalid SDK-free pthread control under TSan.

    The only characterized result is the exact registry CHECK with a nonzero exit.
    An unexpected success, a different diagnostic or a missing CHECK is reported as
    such. This never turns any result into a qualification pass.
    """
    diagnostics, fatal = classify_output(text)
    if fatal and returncode and fatal["line"] == 348 and fatal["condition"] == "((t)) != (0)" \
            and "sanitizer_thread_registry.cpp" in fatal["file"] and len(diagnostics) == 1:
        status = "EXPECTED_FATAL_CHECK"
    elif returncode == 0 and not diagnostics:
        status = "UNEXPECTED_SUCCESS"
    else:
        status = "DIFFERENT_DIAGNOSTIC"
    return {"status": status, "characterized": status == "EXPECTED_FATAL_CHECK", "returncode": returncode,
            "diagnostics": diagnostics, "fatal_check": fatal}


def reproducer_outcome(scenario, variant, outcome):
    """Characterize the minimal S2 (or any Close/Stop) TSan reproducer.

    The vendor defect is documented, not qualified: a TSan run of S2 is
    characterized only by the exact registry CHECK under PtUtilsLib
    WaitUntilSignaled while PvStream::Close() is in progress, with a nonzero exit
    and the completion marker absent. An unexpected success is reported as such
    (the reproducer changed and must be re-examined); it is never a qualification
    pass, and nothing here alters `outcome["passed"]`.
    """
    fatal = outcome.get("fatal_check") or {}
    frames = "\n".join(fatal.get("stack", []))
    other = [item for item in outcome.get("diagnostics", []) if item["kind"] != "fatal_check"]
    if not variant.endswith("tsan"):
        status = "NOT_A_TSAN_REPRODUCER"
    elif outcome.get("passed") or (outcome.get("returncode") == 0 and not fatal):
        status = "UNEXPECTED_SUCCESS"
    elif (fatal.get("line") == 348 and fatal.get("condition") == "((t)) != (0)"
          and "sanitizer_thread_registry.cpp" in fatal.get("file", "")
          and "pthread_detach" in frames and "PtUtilsLib::Thread::WaitUntilSignaled()" in frames
          and (scenario != "s2" or ("PvStream::Close()" in frames and outcome.get("last_stage") == "before_close"))
          and outcome.get("returncode") not in (0, None) and not other
          and "COMPLETION_MARKER_MISSING" in outcome.get("reasons", [])):
        status = "REPRODUCED_VENDOR_THREAD_LIFECYCLE_CHECK"
    else:
        status = "DIFFERENT_FAILURE"
    return {"status": status, "reproduced": status == "REPRODUCED_VENDOR_THREAD_LIFECYCLE_CHECK",
            "qualification_pass": False}


def trace_gate(outcome, summary):
    """Apply gdb pthread-trace completeness to a scenario outcome (fail closed)."""
    outcome = dict(outcome)
    trace = summary.get("trace", {})
    if not trace.get("complete"):
        outcome["reasons"] = [*outcome["reasons"], "TRACE_INCOMPLETE"]
        outcome["passed"] = False
        if outcome["disposition"] == "SCENARIO_COMPLETE":
            outcome["disposition"] = "INCOMPLETE"
    outcome["trace"] = trace
    outcome["join_then_detach"] = summary.get("join_then_detach", [])
    return outcome


def sanitizer_identity(binary, variant):
    """Record exact executable and sanitizer-runtime identity; inconsistency fails closed."""
    binary = Path(binary).resolve()
    identity = {"binary": str(binary), "sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                "variant": variant}
    notes = subprocess.run(["readelf", "-n", str(binary)], capture_output=True, text=True, timeout=10).stdout
    build = re.search(r"Build ID: ([0-9a-f]+)", notes)
    identity["build_id"] = build.group(1) if build else None
    comment = subprocess.run(["readelf", "-p", ".comment", str(binary)], capture_output=True, text=True, timeout=10).stdout
    identity["compilers"] = sorted(set(re.findall(r"(GCC: \(.*?\) [0-9.]+|Debian clang version [0-9.]+)", comment)))
    needed = subprocess.run(["readelf", "-d", str(binary)], capture_output=True, text=True, timeout=10).stdout
    identity["needed"] = re.findall(r"Shared library: \[(.*?)\]", needed)
    symbols = subprocess.run(["nm", str(binary)], capture_output=True, text=True, timeout=30).stdout
    static_tsan = bool(re.search(r" T __tsan_init$", symbols, re.M))
    identity["static_clang_tsan"] = static_tsan
    if variant == "gcc-tsan":
        ok = "libtsan.so.2" in identity["needed"] and not static_tsan
    elif variant == "clang-tsan":
        # Complete matching clang runtime: static archive, and no GCC libtsan mixed in.
        ok = static_tsan and "libtsan.so.2" not in identity["needed"] and CLANG_TSAN_ARCHIVE.is_file()
        if CLANG_TSAN_ARCHIVE.is_file():
            identity["clang_tsan_archive_sha256"] = hashlib.sha256(CLANG_TSAN_ARCHIVE.read_bytes()).hexdigest()
    elif variant == "gcc-asan":
        ok = "libasan.so.8" in identity["needed"] and "libubsan.so.1" in identity["needed"]
    else:
        ok = not static_tsan and not any(name.startswith(("libtsan", "libasan", "libubsan")) for name in identity["needed"])
    want = "clang" if variant.startswith("clang") else "GCC"
    identity["consistent"] = ok and bool(identity["build_id"]) and any(want in item for item in identity["compilers"])
    return identity


def runtime_inventory(maps_text, sdk):
    """Hash each SDK, sanitizer and C/C++ runtime library actually mapped in the native process."""
    loaded = {}
    for row in maps_text.splitlines():
        fields = row.split()
        if len(fields) >= 6 and fields[-1].startswith("/"):
            path = Path(fields[-1])
            if (str(path).startswith(str(sdk) + "/") or "san" in path.name
                    or path.name.startswith(("libc.so", "libstdc++", "ld-linux"))):
                loaded[str(path)] = None
    for path in loaded:
        try:
            data = Path(path).read_bytes()
        except OSError:
            continue
        notes = subprocess.run(["readelf", "-n", path], capture_output=True, text=True, timeout=10).stdout
        build = re.search(r"Build ID: ([0-9a-f]+)", notes)
        loaded[path] = {"sha256": hashlib.sha256(data).hexdigest(), "build_id": build.group(1) if build else None}
    names = {Path(path).name for path in loaded}
    complete = all(any(name.startswith(prefix) for name in names) for prefix in REQUIRED_SDK)
    return loaded, complete and all(value for value in loaded.values())


def native_argv(args, directory):
    binary = str(args.binary.resolve())
    argv = [binary, args.scenario, "veth"]
    if args.tracer == "strace":
        argv = [str(args.strace.resolve()), "--kill-on-exit", "-f", "-ttt", "-T", "-s", "128",
                "-e", "trace=clone,clone3,futex,exit,exit_group,munmap,mmap,set_robust_list,rseq,network",
                "-o", str(directory / "syscalls.log"), *argv]
    elif args.tracer == "gdb":
        argv = ["gdb", "-nx", "-q", "-batch",
                "-x", str(Path(__file__).resolve().parent / "pthread_lifecycle_trace_gdb.py"),
                "--args", *argv]
    return ["setpriv", "--pdeathsig", "KILL", *argv]


def native_environment(args, directory):
    sdk = args.sdk.resolve()
    environment = os.environ.copy()
    if any(environment.get(key) for key in CONTROLLED_ENVIRONMENT):
        # Never inherit interposition or sanitizer flags that could mask the CHECK.
        raise RuntimeError("AUDIT_INCOMPLETE: inherited loader/sanitizer environment refused")
    environment.update(LD_LIBRARY_PATH=f"{sdk}/lib:{sdk}/lib/genicam/bin/Linux64_x64",
                       GENICAM_ROOT_V3_4=f"{sdk}/lib/genicam", PUREGEV_ROOT=str(sdk),
                       GENICAM_CACHE_V3_4=str(directory / "cache"), **SANITIZER_OPTIONS[args.variant])
    if args.tracer == "gdb":
        environment["GIGE_MEL_PTHREAD_TRACE"] = str(directory / "pthread-trace.jsonl")
    return environment


def run_native(args, directory, camera):
    """Run one scenario in a fresh process; retain complete output and exit status."""
    sdk = args.sdk.resolve()
    verify_sdk(sdk)
    identity = sanitizer_identity(args.binary, args.variant)
    (directory / "sanitizer-identity.json").write_text(json.dumps(identity, indent=2) + "\n")
    environment = native_environment(args, directory)
    argv = native_argv(args, directory)
    (directory / "native-command.json").write_text(json.dumps({
        "argv": argv, "sanitizer_environment": SANITIZER_OPTIONS[args.variant]}, indent=2) + "\n")
    process = None
    timed_out = False
    identity_ok = identity["consistent"]
    runtime_ok = False
    try:
        with (directory / "native.log").open("xb") as log, (directory / "sockets.jsonl").open("x") as sockets:
            # stderr is merged so the CHECK stack is retained in order with stage markers.
            process = subprocess.Popen(argv, env=environment, stdin=subprocess.PIPE,
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            with selectors.DefaultSelector() as select:
                select.register(process.stdout, selectors.EVENT_READ)
                deadline = time.monotonic() + args.timeout
                pending = b""
                while True:
                    if time.monotonic() >= deadline:
                        timed_out = True
                        break
                    if not select.select(0.1):
                        continue
                    chunk = os.read(process.stdout.fileno(), 65536)
                    if not chunk:
                        break
                    log.write(chunk)  # raw bytes, never truncated or filtered
                    log.flush()
                    pending += chunk
                    while b"\n" in pending:
                        raw, pending = pending.split(b"\n", 1)
                        line = raw.decode(errors="replace")
                        if not line.startswith("STAGE "):
                            continue
                        stage = line[6:]
                        pid = native_pid(process.pid, args.tracer)
                        inventory(pid, stage, sockets)
                        maps = Path(f"/proc/{pid}/maps").read_text()
                        (directory / "loaded-maps.txt").write_text(maps)
                        (directory / f"threads-{len(list(directory.glob('threads-*.json'))):02d}-{stage}.json").write_text(
                            json.dumps(threads(pid), indent=2) + "\n")
                        loaded, runtime_ok = runtime_inventory(maps, sdk)
                        (directory / "runtime-identity.json").write_text(json.dumps(loaded, indent=2) + "\n")
                        if stage == FRAME_STAGE.get(args.scenario):
                            veth.send_message(camera, {"send_frames": 41040})
                            veth.read_message(camera)
                        process.stdin.write(b"continue\n")
                        process.stdin.flush()
            if timed_out:
                process.kill()
            returncode = process.wait(timeout=10)
            if args.tracer == "gdb":
                # gdb's own status is not the native status; use the traced inferior's.
                returncode = inferior_exit(directory / "pthread-trace.jsonl", returncode)
    finally:
        if process is not None:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            process.stdin.close()
            process.stdout.close()
    return returncode, timed_out, identity_ok and runtime_ok, identity


def inferior_exit(trace, tracer_returncode):
    """Exit status of the debugged native process; missing evidence is never success."""
    if tracer_returncode != 0 or not trace.exists():
        return None
    exits = [json.loads(line) for line in trace.read_text().splitlines()
             if line.strip() and '"inferior_exit"' in line]
    if len(exits) != 1 or exits[0].get("exit_code") is None:
        return None
    return exits[0]["exit_code"]


def native_pid(pid, tracer):
    """The SDK process: under strace/gdb it is the tracer's only child."""
    if tracer == "none":
        return pid
    current = pid
    for _ in range(3):
        children = Path(f"/proc/{current}/task/{current}/children").read_text().split()
        if not children:
            break
        if len(children) != 1:
            raise RuntimeError("AUDIT_INCOMPLETE: cannot identify traced native child")
        current = int(children[0])
        if Path(f"/proc/{current}/exe").resolve().name.startswith("pleora_"):
            return current
    raise RuntimeError("AUDIT_INCOMPLETE: cannot identify traced native child")


def threads(pid):
    """Observed native thread inventory at each barrier (names, LWPs)."""
    result = []
    for task in sorted(Path(f"/proc/{pid}/task").iterdir(), key=lambda item: int(item.name)):
        try:
            result.append({"lwp": int(task.name), "comm": (task / "comm").read_text().strip(),
                           "state": (task / "stat").read_text().split(") ", 1)[1].split()[0]})
        except (FileNotFoundError, ProcessLookupError, IndexError):
            continue
    return result


def camera_args(args):
    """The R1 camera worker, reused unchanged, with the scenario's fixture."""
    spec = SCENARIOS[args.scenario]
    return argparse.Namespace(output=args.output / "camera", scenario=spec["fixture"],
                              aravis_library=args.aravis_library, frames=spec["frames"])


def camera_argv(args):
    return ["setpriv", "--pdeathsig", "KILL", "unshare", "--net", sys.executable, str(Path(__file__).resolve()),
            *sys.argv[1:], "--camera"]


def coordinator(args):
    """Receiver namespace: topology, observers, native run, fail-closed cleanup."""
    if socket.if_nameindex() != [(1, "lo")]:
        raise RuntimeError("ENVIRONMENT_BLOCKED: coordinator namespace not fresh")
    directory = args.output
    child = capture = None
    report = {"scenario": args.scenario, "variant": args.variant, "tracer": args.tracer,
              "operations": SCENARIOS[args.scenario]["operations"],
              "fixture": SCENARIOS[args.scenario]["fixture"], "frames": SCENARIOS[args.scenario]["frames"]}
    outcome = None
    try:
        child = subprocess.Popen(camera_argv(args), stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        initial = veth.read_message(child)
        if initial["netns"] == os.readlink("/proc/self/ns/net") or initial["userns"] != os.readlink("/proc/self/ns/user"):
            raise RuntimeError("ENVIRONMENT_BLOCKED: namespace identity mismatch")
        report["camera_initial"] = initial
        veth.command("ip", "link", "add", "veth-rx", "type", "veth", "peer", "name", "veth-cam")
        veth.command("ip", "link", "set", "veth-cam", "netns", str(initial["pid"]))
        veth.configure("veth-rx", veth.RX, veth.RX_MAC)
        veth.send_message(child, "configure")
        report["camera"] = veth.read_message(child)
        report["receiver"] = veth.network()
        veth.check_network(report["camera"], veth.CAM)
        veth.check_network(report["receiver"], veth.RX)
        capture = Capture(directory)
        report["receiver_ping"] = veth.command("ping", "-n", "-c", "2", "-W", "1", "-I", "veth-rx", veth.CAM)
        veth.send_message(child, "ping")
        report["camera_ping"] = veth.read_message(child)
        returncode, timed_out, identity_ok, identity = run_native(args, directory, child)
        report.update(native_returncode=returncode, timed_out=timed_out, identity_ok=identity_ok,
                      sanitizer_identity=identity)
        time.sleep(0.3)
        veth.send_message(child, "stop")
        report["camera_capture"] = veth.read_message(child)
        report["camera_returncode"] = child.wait(timeout=5)
    finally:
        if child is not None:
            if child.poll() is None:
                child.kill()
                child.wait(timeout=3)
                report["forced_camera_cleanup"] = True
            child.stdin.close()
            child.stdout.close()
        time.sleep(0.3)
        if capture:
            report["receiver_capture"] = capture.close()
        report["packet_sockets_after_close"] = Path("/proc/self/net/packet").read_text()
        report["remaining_interfaces"] = socket.if_nameindex()
        cleanup_ok = (report["remaining_interfaces"] == [(1, "lo")] and not report.get("forced_camera_cleanup")
                      and len(report["packet_sockets_after_close"].splitlines()) == 1)
        observers_ok = False
        if (directory / "packets.jsonl").exists() and (directory / "camera/packets.jsonl").exists():
            report["traffic"] = veth.traffic_summary(directory)
            observers_ok = (veth.healthy(report.get("receiver_capture", {"errors": ["missing"]}))
                            and "camera_capture" in report and veth.healthy(report["camera_capture"])
                            and not report["traffic"]["unidentified_outgoing"]
                            and report["traffic"]["endpoint_gvcp_match"] and report.get("camera_returncode") == 0)
        if (directory / "native.log").exists():
            outcome = evaluate(args.scenario, report.get("native_returncode"),
                               (directory / "native.log").read_bytes().decode(errors="replace"),
                               timed_out=report.get("timed_out", False), identity_ok=report.get("identity_ok", False),
                               cleanup_ok=cleanup_ok, observers_ok=observers_ok)
            if args.tracer == "gdb":
                trace = directory / "pthread-trace.jsonl"
                records = ([json.loads(line) for line in trace.read_text().splitlines() if line.strip()]
                           if trace.exists() else [])
                result = pthread_summary.analyze(records)
                (directory / "pthread-summary.json").write_text(json.dumps(
                    {key: result[key] for key in ("trace", "order", "findings", "join_then_detach")},
                    indent=1, default=str) + "\n")
                outcome = trace_gate(outcome, result)
            outcome["reproducer"] = reproducer_outcome(args.scenario, args.variant, outcome)
            report["outcome"] = outcome
        report.update(cleanup_ok=cleanup_ok, observers_ok=observers_ok)
        (directory / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    if outcome is None or not outcome["passed"]:
        disposition = outcome["disposition"] if outcome else "INCOMPLETE"
        raise RuntimeError(f"NOT_PASSING {disposition}: see {directory}/report.json")


def outer(args):
    """Host side: refuse repository output, compare host inventory, prove retirement."""
    if args.output.is_relative_to(Path(__file__).resolve().parents[1]):
        raise RuntimeError("private evidence must be outside the repository")
    args.output.mkdir(mode=0o700)
    before = veth.network()
    (args.output / "host-before.json").write_text(json.dumps(before, indent=2))
    with (args.output / "coordinator.log").open("x") as log:
        process = subprocess.Popen(["setpriv", "--pdeathsig", "KILL", "unshare", "--user", "--map-root-user", "--net",
                                    sys.executable, str(Path(__file__).resolve()), *sys.argv[1:], "--coordinator"],
                                   stdout=log, stderr=subprocess.STDOUT)
        try:
            rc = process.wait(timeout=args.timeout + 60)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
            raise RuntimeError("AUDIT_INCOMPLETE: coordinator cleanup timeout")
    after = veth.network()
    (args.output / "host-after.json").write_text(json.dumps(after, indent=2))
    if veth.stable_network(before) != veth.stable_network(after):
        raise RuntimeError("AUDIT_INCOMPLETE: host network inventory changed")
    report_path = args.output / "report.json"
    if not report_path.exists():
        raise RuntimeError(f"AUDIT_INCOMPLETE: no report; see {args.output}/coordinator.log")
    report = json.loads(report_path.read_text())
    namespace_list = veth.command("lsns", "--type", "net", "--noheadings", "--output", "NS,PID")
    nsids = [state["netns"].split("[")[1].rstrip("]")
             for state in (report.get("camera_initial", {}), report.get("receiver", {})) if "netns" in state]
    listed = {line.split()[0] for line in namespace_list.splitlines()}
    owned = [process.pid, report.get("camera_initial", {}).get("pid")]
    closure = {"coordinator_returncode": rc, "pids": owned,
               "owned_children_retired": all(pid and not Path(f"/proc/{pid}").exists() for pid in owned),
               "namespace_ids": nsids,
               "namespaces_absent_from_lsns": len(nsids) == 2 and not listed.intersection(nsids),
               "host_configuration_unchanged": True}
    (args.output / "outer-cleanup.json").write_text(json.dumps(closure, indent=2) + "\n")
    if not closure["owned_children_retired"] or not closure["namespaces_absent_from_lsns"]:
        raise RuntimeError("AUDIT_INCOMPLETE: child or namespace retirement unproven")
    outcome = report.get("outcome") or {}
    summary = {"scenario": args.scenario, "variant": args.variant, "tracer": args.tracer,
               "disposition": outcome.get("disposition"), "passed": bool(outcome.get("passed")) and rc == 0,
               "returncode": outcome.get("returncode"), "last_stage": outcome.get("last_stage"),
               "executed": outcome.get("executed_fraction"), "reasons": outcome.get("reasons"),
               "reproducer": (outcome.get("reproducer") or {}).get("status")}
    print(json.dumps(summary))
    if not summary["passed"]:
        sys.exit(1)


def parse(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--scenario", choices=sorted(SCENARIOS), required=True)
    parser.add_argument("--variant", choices=sorted(SANITIZER_OPTIONS), required=True)
    parser.add_argument("--tracer", choices=sorted(TRACERS), default="none")
    parser.add_argument("--strace", type=Path, help="external observational strace (tracer=strace)")
    parser.add_argument("--aravis-library", type=Path, help="pinned Aravis 0.8.36 (s6/d2 fixture)")
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--coordinator", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--camera", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    args.output = args.output.resolve()
    if args.variant not in TRACERS[args.tracer]:
        parser.error(f"unsupported execution variant: {args.variant} under tracer {args.tracer}")
    if args.tracer == "strace" and args.strace is None:
        parser.error("tracer strace requires --strace")
    if SCENARIOS[args.scenario]["fixture"] == "aravis" and args.aravis_library is None:
        parser.error(f"scenario {args.scenario} requires --aravis-library")
    if not 5 <= args.timeout <= 600:
        parser.error("timeout must be 5..600 seconds")
    return args


def main():
    args = parse()
    if args.camera:
        veth.camera_worker(camera_args(args))
    elif args.coordinator:
        coordinator(args)
    else:
        outer(args)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        if "--camera" in sys.argv:
            veth.emit({"error": str(error)})
        else:
            print(str(error), file=sys.stderr)
        sys.exit(1)
