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
COMPLETED = "Passed 8 actual-runtime offline groups;"
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
    if environment.get("LD_PRELOAD") or environment.get("LD_AUDIT"):
        raise QualificationError("Loader interposition is incompatible with pinned-runtime verification")
    # Never inherit user-provided suppressions, disabled reports, or alternate exits.
    for option in environment.get("TSAN_OPTIONS", "").split(":"):
        if not option:
            continue
        if option not in ("halt_on_error=1", "second_deadlock_stack=1", "exitcode=66"):
            raise QualificationError(f"Unreviewed TSAN_OPTIONS setting: {option.split('=')[0]}")


def verify_unsuppressed(returncode: int, output: str) -> None:
    warning_lines = re.findall(r"^WARNING: ThreadSanitizer:.*$", output, flags=re.MULTILINE)
    known_header = re.compile(r"^WARNING: ThreadSanitizer: lock-order-inversion "
                              r"\(potential deadlock\)(?: \(pid=\d+\))?$")
    if (returncode != 66 or len(warning_lines) != 1 or
            not known_header.fullmatch(warning_lines[0]) or output.count(KNOWN_SUMMARY) != 1):
        raise QualificationError("Unsuppressed native contract did not report exactly the expected deadlock")
    if output.count(COMPLETED) != 1:
        raise QualificationError("Native contract did not complete all eight groups")
    if len(re.findall(r"^PASS ", output, flags=re.MULTILINE)) != 8:
        raise QualificationError("Native contract must pass all eight groups")
    for symbol in ("log4cpp_GenICam::Category::addAppender", "log4cpp_GenICam::Category::removeAllAppenders"):
        if symbol not in output:
            raise QualificationError(f"Expected vendor call frame missing: {symbol}")
    for offset in KNOWN_OFFSETS:
        if f"liblog4cpp_gcc48_v3_4.so+{offset}" not in output:
            raise QualificationError(f"Known vendor runtime frame missing: {offset}")
    if "ThreadSanitizer: reported 1 warnings" not in output:
        raise QualificationError("Unsuppressed native run did not report exactly one warning")
    if "FATAL: ThreadSanitizer" in output or "ERROR: ThreadSanitizer" in output:
        raise QualificationError("Additional TSan failure in unsuppressed contract")


def verify_suppressed(returncode: int, output: str) -> None:
    if returncode != 0 or output.count(COMPLETED) != 1:
        raise QualificationError("Scoped native contract failed or did not finish")
    if len(re.findall(r"^PASS ", output, flags=re.MULTILINE)) != 8:
        raise QualificationError("Scoped native contract did not pass all eight groups")
    if "ThreadSanitizer:" in output or "WARNING:" in output or "FATAL:" in output:
        raise QualificationError("Sanitizer or runtime warning remains in scoped contract")


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
        verify_suppression(args.suppression_file)
        verify_environment(runtime, dict(os.environ))
        base = dict(os.environ)
        base["TSAN_OPTIONS"] = "halt_on_error=0:exitcode=66:second_deadlock_stack=1"
        original = run_contract(args.contract, base, timeout=50)
        last_output = original[1]
        verify_unsuppressed(*original)
        scoped = dict(base)
        scoped["TSAN_OPTIONS"] = ("halt_on_error=1:exitcode=66:second_deadlock_stack=1:"
                                  f"suppressions={args.suppression_file.resolve(strict=True)}")
        corrected = run_contract(args.contract, scoped, timeout=50)
        last_output = corrected[1]
        verify_suppressed(*corrected)
        print("PASS: all eight native groups; unsuppressed vendor cycle matched exact locked report")
        print("PASS: reviewed deadlock-only exception; no other TSan findings")
        return 0
    except (QualificationError, OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: scoped native TSan qualification: {error}", file=sys.stderr)
        if last_output:
            print(last_output[-10000:], file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
