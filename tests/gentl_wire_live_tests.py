#!/usr/bin/env python3
"""Explicit manual SDK-free integration matrix; NOT registered in ordinary CTest.

Uses the SAME supervisor/isolation/simulator/observer as real qualification.
All evidence goes to a new external directory. No GenTL producer is loaded.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--aravis-library", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not args.output.is_absolute() or ROOT in args.output.resolve().parents:
        parser.error("external absolute output required")
    args.output.mkdir(parents=True, exist_ok=False)
    summary = {}
    for scenario, expected in (("read", "HARNESS_SELF_TEST_PASS"),
                               ("control-write", "READ_ONLY_WIRE_VIOLATION"),
                               ("register-write", "READ_ONLY_WIRE_VIOLATION"),
                               ("memory-write", "READ_ONLY_WIRE_VIOLATION"),
                               ("unknown", "READ_ONLY_WIRE_VIOLATION"),
                               ("duplicate", "READ_ONLY_WIRE_VIOLATION"),
                               ("post-close", "READ_ONLY_WIRE_VIOLATION"),
                               ("crash", "PRODUCER_CRASH"), ("timeout", "PRODUCER_TIMEOUT")):
        command = [sys.executable, str(ROOT / "scripts/gentl_qualification.py"), "--wire",
                   "--self-test", scenario, "--aravis-library", str(args.aravis_library),
                   "--timeout", "3", "--quiet", "0.2", "--output", str(args.output / scenario)]
        native = subprocess.run(command, timeout=30, check=False)
        result = json.loads((args.output / scenario / "result.json").read_text())
        summary[scenario] = {"result": result["result"], "expected": expected,
                             "exit_status": native.returncode}
        if result["result"] != expected or native.returncode != int(scenario != "read"):
            raise RuntimeError(f"{scenario}: {summary[scenario]}")
        if result["result"] == "READ_ONLY_WIRE_PASS":
            raise RuntimeError("synthetic traffic must never qualify a producer")
    (args.output / "matrix.json").write_text(json.dumps(summary, indent=2) + "\n")
    print("9/9 isolated simulator scenarios matched expected outcomes; no real producer qualified")


if __name__ == "__main__":
    main()