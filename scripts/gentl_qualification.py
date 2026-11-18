#!/usr/bin/env python3
"""Offline-only, uninstalled GenTL probe supervisor (Python 3.9+, Linux).

Default mode offers no discovery/open. Explicit --wire mode qualifies an isolated
simulator only. A fresh user/network namespace contains producer discovery.
No host interface is changed.
This is crash containment for diagnostics, not a hostile-code sandbox.
"""
import argparse
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import signal
import subprocess
import sys
import time


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def absolute_file(value):
    path = Path(value)
    if not path.is_absolute() or not path.is_file():
        raise argparse.ArgumentTypeError("requires an explicit absolute existing file")
    return path


def bounded_seconds(value):
    seconds = float(value)
    if not math.isfinite(seconds) or not 0.1 <= seconds <= 300:
        raise argparse.ArgumentTypeError("timeout must be finite and in [0.1, 300] seconds")
    return seconds


def run_stage(command, output, name, timeout):
    """One subprocess; preserve native status, kill its process group on timeout.

    Logs go directly to disk: no pipe deadlock, no unbounded in-memory capture.
    A killed process does NOT establish native cleanup.
    """
    start = time.monotonic()
    record = {"name": name, "command": [str(arg) for arg in command],
              "timeout_seconds": timeout, "timed_out": False}
    with (output / (name + ".stdout.log")).open("xb") as stdout, \
            (output / (name + ".stderr.log")).open("xb") as stderr:
        try:
            process = subprocess.Popen(command, stdout=stdout, stderr=stderr,
                                       start_new_session=True, stdin=subprocess.DEVNULL, cwd=output)
        except OSError as error:
            record.update(outcome="launch_error", error=str(error), returncode=None, signal=None)
        else:
            try:
                code = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                record["timed_out"] = True
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass  # exited between the timeout and process-group kill
                code = process.wait()
            record.update(returncode=code, signal=-code if code < 0 else None,
                          outcome="timeout" if record["timed_out"] else
                          "signal" if code < 0 else "success" if code == 0 else "failure")
    record["elapsed_seconds"] = round(time.monotonic() - start, 6)
    return record


def collect(args):
    args.output.mkdir(parents=True, exist_ok=False)
    report = {
        "schema": "gige-mel.gentl-offline-qualification", "version": 1,
        "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "host": platform.platform(), "python": platform.python_version(),
        "producer": {"path": str(args.producer), "sha256": digest(args.producer)},
        "probe": {"path": str(args.probe), "sha256": digest(args.probe)},
        "environment": {key: os.environ[key] for key in (
            "LD_LIBRARY_PATH", "MVIMPACT_ACQUIRE_DIR", "GENICAM_ROOT", "GENICAM_ROOT_V3_5",
            "GENICAM_CACHE_V3_5") if key in os.environ},
        "isolation": "unshare --user --map-root-user --net; loopback left down; no host NIC",
        "device_discovery_requested": False, "device_open_requested": False,
        "read_only_safety": "NOT_ASSESSED", "physical_camera": "NOT_AUTHORIZED_NOT_RUN",
        "native_cleanup": "NOT_ESTABLISHED", "stages": [],
    }

    def save():
        report["logs"] = {path.name: {"bytes": path.stat().st_size, "sha256": digest(path)}
                          for path in sorted(args.output.glob("*.log"))}
        (args.output / "result.json").write_text(json.dumps(report, indent=2) + "\n")

    # Fail closed before loading the SDK if namespaces are unavailable.
    namespace = ["unshare", "--user", "--map-root-user", "--net", "--"]
    commands = [("isolation", namespace + ["true"])]
    # Repeat successful load/init/close/unload in independently contained children.
    commands += [(f"load-{cycle}", namespace + [str(args.probe), str(args.producer),
                  "--offline-qualification", "load"]) for cycle in range(1, args.repeat + 1)]
    commands += [("interfaces", namespace + [str(args.probe), str(args.producer),
                  "--offline-qualification", "interfaces"])]
    save()
    for name, command in commands:
        stage = run_stage(command, args.output, name, args.timeout)
        report["stages"].append(stage)
        report["outcome"] = stage["outcome"]
        save()
        if stage["outcome"] != "success":
            print(f"STOP: {name}: {stage['outcome']}; evidence: {args.output}")
            return 1
    report["native_cleanup"] = "PROBE_REPORTED_SUCCESS_NOT_INDEPENDENTLY_TRACED"
    save()
    print(f"Offline probe sequence succeeded; camera compatibility NOT established: {args.output}")
    return 0


def main():
    if len(sys.argv) == 3 and sys.argv[1] in ("--wire-worker", "--wire-camera", "--wire-synthetic"):
        import gentl_wire_runner as wire
        config = json.loads(Path(sys.argv[2]).read_text())
        if sys.argv[1] == "--wire-worker":
            return wire.worker(config, Path(__file__).resolve())
        wire.require_isolation(wire.network_snapshot(), config["host_namespace"], configured=True)
        if os.readlink("/proc/self/ns/user") == config["host_user_namespace"]:
            raise ValueError("fresh user namespace required")
        return wire.camera(config) if sys.argv[1] == "--wire-camera" else wire.synthetic(config)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=absolute_file)
    parser.add_argument("--producer", type=Path)
    parser.add_argument("--wire", action="store_true", help="explicit isolated simulated-camera qualification")
    parser.add_argument("--aravis-library", type=absolute_file, help="absolute pinned libaravis-0.8.so for simulator")
    parser.add_argument("--interface", help="exact producer interface ID inside the isolated network")
    parser.add_argument("--quiet", type=bounded_seconds, default=0.5)
    parser.add_argument("--self-test", choices=("read", "control-write", "register-write", "memory-write",
                        "unknown", "duplicate", "crash", "timeout", "post-close"),
                        help="SDK-free injected traffic; never qualifies a producer")
    parser.add_argument("--output", type=Path, required=True,
                        help="new absolute directory outside the repository; logs may be sensitive")
    parser.add_argument("--timeout", type=bounded_seconds, default=20.0)
    parser.add_argument("--repeat", type=int, choices=range(1, 6), default=3)
    args = parser.parse_args()
    if args.wire:
        if not args.aravis_library or args.quiet > 10 or args.timeout < args.quiet + 1:
            parser.error("--wire requires --aravis-library, quiet <=10 and timeout >= quiet+1")
        if args.self_test and (args.producer or args.probe):
            parser.error("self-tests cannot execute a producer/probe")
        if not args.self_test and args.producer and (not args.probe or not args.interface):
            parser.error("real producer requires explicit --probe and --interface")
    elif not args.probe or not args.producer or args.self_test or args.aravis_library or args.interface:
        parser.error("offline mode requires --probe and --producer; wire options require --wire")
    if args.producer and not args.producer.is_absolute():
        parser.error("--producer must be absolute")
    if not args.output.is_absolute():
        parser.error("--output must be absolute")
    repository = Path(__file__).resolve().parent.parent
    if repository == args.output.resolve() or repository in args.output.resolve().parents:
        parser.error("evidence must remain outside the repository")
    try:
        if args.wire:
            import gentl_wire_runner as wire
            return wire.collect(args, run_stage, digest, Path(__file__).resolve())
        args.producer = absolute_file(str(args.producer))
        return collect(args)
    except (OSError, ValueError) as error:
        print(f"qualification failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())