#!/usr/bin/env python3
"""Fail-closed, one-test TSan exception for a fingerprinted GenApi lock cycle.

The native contract is first executed without suppression. Accept only its known
vendor lock-cycle report, then run it again with one function-specific suppression.
Any other diagnostic, changed runtime or test failure still fails the CTest.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys


# Pinned by the independent GIGE-MEL-028 investigation. No SDK content is copied.
EXPECTED_RUNTIME_HASHES = {
    "libGenApi_gcc48_v3_4.so": "9a2f6a5f2f6e85b22154ad467f75acafd1d1ec197122ea39e32d29df69ad0565",
    "libGCBase_gcc48_v3_4.so": "7534579ef0e4d513f569d97f26998c7378a322d2d58ea1d5d0d0ebced83604a4",
    "libLog_gcc48_v3_4.so": "935ded42d13cc143c73eb97a9b77f37ce4b4d6ae913370f148a33840b942a1a2",
    "liblog4cpp_gcc48_v3_4.so": "e053cc5f25f3ccea7aa573e6616565413cd1abc29e1b94fdd7f930830ce7544f",
}
SUPPRESSION = "deadlock:log4cpp_GenICam::Category::addAppender"
COMPLETED = "Passed 8 actual-runtime offline groups; no camera/stream/MEL hardware qualification."
GROUPS = (
    "eBUS parameter array and native status mapping",
    "integer type/access/current grid",
    "native float increment modes",
    "absence, native query errors and aliases",
    "strings, enums, booleans and entry availability",
    "cache freshness and dynamic write constraints",
    "unchanged shared profile and actual readback",
    "partial writes and terminal guards",
)
KNOWN_WARNING = "WARNING: ThreadSanitizer: lock-order-inversion (potential deadlock)"
KNOWN_SUMMARY = "SUMMARY: ThreadSanitizer: lock-order-inversion (potential deadlock)"
KNOWN_OFFSETS = ("0x1dba1", "0x1a75f", "0x1a47a", "0x1d765", "0x1d609")


class QualificationError(RuntimeError):
    """The sole exception could not be safely applied."""


def verify_runtime(directory: Path, expected: dict[str, str] = EXPECTED_RUNTIME_HASHES) -> None:
    if not directory.is_dir():
        raise QualificationError("GenApi runtime directory is missing")
    for filename, digest in expected.items():
        path = directory / filename
        if not path.is_file():
            raise QualificationError(f"Expected runtime library missing: {filename}")
        sha = hashlib.sha256(path.read_bytes()).hexdigest()
        if sha != digest:
            raise QualificationError(f"External runtime fingerprint differs: {filename}: {sha}")


def verify_suppression(path: Path) -> None:
    if not path.is_file():
        raise QualificationError("Scoped TSan suppression file is missing")
    directives = [line.strip() for line in path.read_text(encoding="utf-8").splitlines()
                  if line.strip() and not line.lstrip().startswith("#")]
    if directives != [SUPPRESSION]:
        raise QualificationError("Only the reviewed function-specific deadlock suppression is permitted")


def verify_environment(runtime: Path, environment: dict[str, str]) -> None:
    # Fail closed rather than allow a different GenApi/logging library to be loaded.
    parent = runtime.parent.parent.parent  # SDK/lib
    paths = [Path(p).resolve() for p in environment.get("LD_LIBRARY_PATH", "").split(":") if p]
    if len(paths) < 2 or paths[0] != parent.resolve() or paths[1] != runtime.resolve():
        raise QualificationError("LD_LIBRARY_PATH must begin with the selected SDK lib and GenApi runtime")
    if Path(environment.get("GENICAM_ROOT_V3_4", "/nonexistent")).resolve() != runtime.parent.parent.resolve():
        raise QualificationError("GENICAM_ROOT_V3_4 must identify the selected runtime")
    if any(environment.get(key) for key in ("LD_PRELOAD", "LD_AUDIT", "LD_DEBUG", "LD_DEBUG_OUTPUT")):
        raise QualificationError("Loader interposition is incompatible with pinned-runtime verification")
    # Never inherit user-provided suppressions, disabled reports, or alternate exits.
    for option in environment.get("TSAN_OPTIONS", "").split(":"):
        if not option:
            continue
        if option not in ("halt_on_error=1", "second_deadlock_stack=1", "exitcode=66"):
            raise QualificationError(f"Unreviewed TSAN_OPTIONS setting: {option.split('=')[0]}")


def verify_loaded_runtime(runtime: Path, output: str) -> None:
    # glibc's loader trace records actual initialization, including dlopen(Log/log4cpp).
    # Checking search paths or ldd alone does not establish those loaded identities.
    loaded = re.findall(r"^\s*\d+:\s+calling init: (.+?)\s*$", output, re.MULTILINE)
    for filename in EXPECTED_RUNTIME_HASHES:
        matches = [Path(path) for path in loaded if Path(path).name == filename]
        if len(matches) != 1 or matches[0].resolve() != (runtime / filename).resolve():
            raise QualificationError(f"Actual loaded runtime differs or is missing: {filename}")


def verify_completion(output: str) -> None:
    lines = output.splitlines()
    if lines.count(COMPLETED) != 1:
        raise QualificationError("Native contract did not complete all eight groups")
    if [line for line in lines if line.startswith("PASS ")] != [f"PASS {name}" for name in GROUPS]:
        raise QualificationError("Native contract must pass each of the eight distinct groups in order")
    if lines.index(COMPLETED) < lines.index(f"PASS {GROUPS[-1]}"):
        raise QualificationError("Native completion precedes the final group")


def verify_diagnostics(output: str, allowed: list[str]) -> None:
    diagnostics = [line.strip() for line in output.splitlines()
                   if re.search(r"WARNING:|ERROR:|FATAL:|SUMMARY:|Sanitizer:|runtime error:|^FAIL", line)]
    if diagnostics != allowed:
        raise QualificationError("Unexpected sanitizer or runtime diagnostic")


def verify_unsuppressed(returncode: int, output: str) -> None:
    warning_lines = re.findall(r"^WARNING: ThreadSanitizer:.*$", output, flags=re.MULTILINE)
    known_header = re.compile(r"^WARNING: ThreadSanitizer: lock-order-inversion "
                              r"\(potential deadlock\)(?: \(pid=\d+\))?$")
    if (returncode != 66 or len(warning_lines) != 1 or
            not known_header.fullmatch(warning_lines[0]) or output.count(KNOWN_SUMMARY) != 1):
        raise QualificationError("Unsuppressed native contract did not report exactly the expected deadlock")
    verify_completion(output)
    report = re.search(r"^==================\n(" + re.escape(warning_lines[0]) +
                       r"\n.*?)^==================$", output, re.MULTILINE | re.DOTALL)
    if report is None or output.splitlines().count("==================") != 2:
        raise QualificationError("Expected one complete sanitizer report")
    body = report.group(1)
    cycle = re.search(r"^  Cycle in lock order graph: (M\d+) \(0x[0-9a-f]+\) => "
                      r"(M\d+) \(0x[0-9a-f]+\) => \1$", body, re.MULTILINE)
    if cycle is None or cycle[1] == cycle[2]:
        raise QualificationError("Expected the documented two-mutex cycle")
    first, second = cycle.groups()
    headers = [
        f"  Mutex {second} acquired here while holding mutex {first} in main thread:",
        f"  Mutex {first} previously acquired by the same thread here:",
        f"  Mutex {first} acquired here while holding mutex {second} in main thread:",
        f"  Mutex {second} previously acquired by the same thread here:",
    ]
    actual_headers = re.findall(r"^  Mutex .*$", body, re.MULTILINE)
    if actual_headers != headers:
        raise QualificationError("Lock acquisition threads or order differ from the reviewed cycle")
    expected_frames = (
        (("<null> <null>", "0x1dba1"),),
        (("log4cpp_GenICam::Category::addAppender(log4cpp_GenICam::Appender&) <null>", "0x1a75f"),),
        (("log4cpp_GenICam::Category::removeAllAppenders() <null>", "0x1a47a"),
         ("<null> <null>", "0x1d609")),
        (("<null> <null>", "0x1d765"), ("<null> <null>", "0x1d609")),
    )
    for index, (header, frames) in enumerate(zip(headers, expected_frames)):
        block = body.split(header + "\n", 1)[1].split("\n\n", 1)[0]
        vendor_frames = re.findall(r"^    #(\d+) (.+) \(liblog4cpp_gcc48_v3_4.so\+(0x[0-9a-f]+)\)$",
                                   block, re.MULTILINE)
        expected = [(str(i), symbol, offset) for i, (symbol, offset) in enumerate(frames, 1)]
        if vendor_frames != expected:
            raise QualificationError(f"Vendor acquisition stack differs: {index + 1}")
    summaries = re.findall(r"^SUMMARY:.*$", output, re.MULTILINE)
    if len(summaries) != 1 or not re.fullmatch(
            re.escape(KNOWN_SUMMARY) + r" \((?:[^\n()]+/)?liblog4cpp_gcc48_v3_4\.so\+0x1dba1\) *", summaries[0]):
        raise QualificationError("Unexpected sanitizer summary frame")
    verify_diagnostics(output, [warning_lines[0], summaries[0].strip(), "ThreadSanitizer: reported 1 warnings"])


def verify_suppressed(returncode: int, output: str) -> None:
    if returncode != 0:
        raise QualificationError("Scoped native contract failed or did not finish")
    verify_completion(output)
    verify_diagnostics(output, [])


def run_contract(executable: Path, environment: dict[str, str], timeout: int) -> tuple[int, str]:
    result = subprocess.run([str(executable)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, errors="replace", env=environment, timeout=timeout, check=False)
    return result.returncode, result.stdout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-dir", required=True, type=Path)
    parser.add_argument("--suppression-file", required=True, type=Path)
    parser.add_argument("--contract", required=True, type=Path)
    args = parser.parse_args()
    last_output = ""
    try:
        runtime = args.runtime_dir.resolve(strict=True)
        if not args.contract.is_file():
            raise QualificationError("Native contract executable is missing")
        verify_runtime(runtime)
        print("PASS: four pinned external runtime SHA-256 fingerprints", flush=True)
        verify_suppression(args.suppression_file)
        verify_environment(runtime, dict(os.environ))
        base = dict(os.environ)
        base["LD_DEBUG"] = "libs"
        base["TSAN_OPTIONS"] = "halt_on_error=0:exitcode=66:second_deadlock_stack=1"
        original = run_contract(args.contract, base, timeout=50)
        last_output = original[1]
        verify_loaded_runtime(runtime, last_output)
        verify_unsuppressed(*original)
        print("PASS: actual loaded libraries and unsuppressed eight-group exact-cycle audit", flush=True)
        scoped = dict(base)
        scoped["TSAN_OPTIONS"] = ("halt_on_error=1:exitcode=66:second_deadlock_stack=1:"
                                  f"suppressions={args.suppression_file.resolve(strict=True)}")
        corrected = run_contract(args.contract, scoped, timeout=50)
        last_output = corrected[1]
        verify_loaded_runtime(runtime, last_output)
        verify_suppressed(*corrected)
        print("PASS: all eight native groups; unsuppressed vendor cycle matched exact locked report")
        print("PASS: reviewed deadlock-only exception; no other TSan findings")
        return 0
    except (QualificationError, OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: scoped native TSan qualification: {error}", file=sys.stderr)
        if isinstance(error, subprocess.TimeoutExpired) and error.output:
            last_output = error.output.decode(errors="replace") if isinstance(error.output, bytes) else error.output
        if last_output:
            print(last_output[-10000:], file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
