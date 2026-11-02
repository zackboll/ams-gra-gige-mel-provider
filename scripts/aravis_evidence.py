#!/usr/bin/env python3
"""Diagnostic evidence, NOT camera qualification. Python 3.9+ standard library.
Linux collection; portable verification never uses SDK, executable or network.
"""
from __future__ import annotations
import argparse
import ctypes
import datetime as dt
import hashlib
import json
import os
import platform
import posixpath
import re
import signal
import stat
import subprocess
import sys
import time
from pathlib import Path

SCHEMA = "gige-mel.aravis-evidence"
VERSION = 1
SUCCESS = "EVIDENCE_COLLECTED_AND_VERIFIED"
INCOMPLETE = "INCOMPLETE_EVIDENCE"
# Wall-time bounds include discovery, SDK control and cleanup; receive bounds
# remain capture=5 s and time-probe=10 s overall, as in the production routines.
COMMAND_TIMEOUT = 120
TERMINATE_GRACE = 2
U64 = (1 << 64) - 1
U32 = (1 << 32) - 1
OPERATIONS = ("inspect", "capture", "time-probe")
QUALIFICATION = {"hardware": "NOT_QUALIFIED", "timestamp": "NOT_QUALIFIED",
                 "production_mel_ready": False}

class EvidenceError(ValueError):
    pass

def require(condition, message):
    if not condition:
        raise EvidenceError(message)

def uint(value, name, maximum=U64, minimum=0):
    require(type(value) is int and minimum <= value <= maximum,
            name + " must be an integer in [%d, %d]" % (minimum, maximum))
    return value

def text(value, name):
    require(isinstance(value, str) and bool(value) and "\0" not in value,
            name + " must be a nonempty string without NUL")
    return value

def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate JSON key: " + key)
        result[key] = value
    return result

def load_json(raw):
    def bad_constant(value):
        raise EvidenceError("non-JSON constant: " + value)
    try:
        return json.loads(raw, object_pairs_hook=unique_object, parse_constant=bad_constant)
    except (ValueError, UnicodeError) as error:
        raise EvidenceError("invalid JSON: " + str(error)) from error

def utc_now():
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="microseconds").replace("+00:00", "Z")

def check_utc(value):
    require(isinstance(value, str) and value.endswith("Z"), "invalid host UTC log time")
    try:
        return dt.datetime.fromisoformat(value[:-1] + "+00:00")
    except ValueError as error:
        raise EvidenceError("invalid host UTC log time") from error

def digest(path):
    h = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            size += len(chunk)
            h.update(chunk)
    return {"sha256": h.hexdigest(), "size": size}

def validate_pgm(path, width, height):
    """Parse exactly the binary P5 representation written by capture_one()."""
    uint(width, "width", U32, 1)
    uint(height, "height", U32, 1)
    require(width <= U64 // height, "PGM dimension product overflow")
    with path.open("rb") as stream:
        header = stream.readline(128) + stream.readline(128) + stream.readline(128)
        match = re.fullmatch(rb"P5\n([1-9][0-9]*) ([1-9][0-9]*)\n255\n", header)
        require(match is not None, "PGM must have capture_one's binary P5/Mono8 header")
        require(tuple(int(v) for v in match.groups()) == (width, height), "PGM dimensions disagree")
        require(path.stat().st_size == len(header) + width * height,
                "PGM payload truncated or has unexpected extra bytes")
    return {"width": width, "height": height, "payload_bytes": width * height}

CONFIG_RE = re.compile(r"^  width=([0-9]+) height=([0-9]+) offset_x=([0-9]+) offset_y=([0-9]+) "
                       r"frame_rate_hz=([0-9]+) exposure_ns=([0-9]+)$", re.M)
CONFIG_KEYS = ("width", "height", "offset_x", "offset_y", "frame_rate_hz", "exposure_ns")

def configuration(value):
    require(isinstance(value, dict) and set(value) == set(CONFIG_KEYS), "invalid configuration members")
    for key in CONFIG_KEYS:
        uint(value[key], key, U64 if key == "exposure_ns" else U32,
             1 if key in ("width", "height", "frame_rate_hz", "exposure_ns") else 0)
    return value

def parse_diagnostic(raw, device, operation, interface):
    versions = re.findall(r"^Aravis runtime ([^,\n]+), compiled against ([^\n]+)$", raw, re.M)
    require(len(versions) == 1, operation + ": missing/duplicate SDK versions")
    configs = CONFIG_RE.findall(raw)
    require(len(configs) == 1, operation + ": missing/duplicate configuration")
    result = {"runtime_sdk_version": versions[0][0], "compiled_sdk_version": versions[0][1],
              "configuration": configuration(dict(zip(CONFIG_KEYS, map(int, configs[0]))))}
    if interface is not None:
        require(raw.count("GigE Vision discovery restricted to interface " + interface + " (UP, IPv4 ") == 1,
                "diagnostic did not report requested discovery restriction")
    if operation == "inspect":
        require("Device " + device + " via GigEVision (read-only inspection)\n" in raw,
                "inspect exact device ID/interface mismatch")
        require("PROBLEM:" not in raw, "inspect reported profile problems")
        result["identity_observations"] = dict(re.findall(r"^  (Device[^:\n]+): ([^\n]*)$", raw, re.M))
    else:
        matches = re.findall(r"^Captured device (.*): ([0-9]+)x([0-9]+) Mono8, source row stride ([0-9]+) bytes, "
                             r"native frame ID ([0-9]+), stream generation ([0-9]+)$", raw, re.M)
        require(len(matches) == 1 and matches[0][0] == device, "capture device/metadata mismatch")
        values = matches[0]
        result.update(width=int(values[1]), height=int(values[2]), source_row_stride=int(values[3]),
                      native_frame_id=int(values[4]), stream_generation=int(values[5]))
        uint(result["native_frame_id"], "capture native_frame_id")
        uint(result["stream_generation"], "capture stream_generation", minimum=1)
        require(result["source_row_stride"] >= result["width"], "invalid capture stride")
        require((result["width"], result["height"]) ==
                (result["configuration"]["width"], result["configuration"]["height"]), "capture geometry mismatch")
        require("FrameTimestamp invalid" in raw, "capture timestamp validity claim changed")
    return result


def validate_probe(path, device, count, config=None, sdk=None):
    uint(count, "COUNT", 32, 1)
    records = []
    with path.open("rb") as stream:
        while True:
            line = stream.readline(4097)
            if not line:
                break
            require(len(line) <= 4096 and line.endswith(b"\n"), "oversized/unterminated JSONL line")
            require(len(records) < count + 2, "extra JSONL record")
            record = load_json(line)
            require(isinstance(record, dict), "JSONL record must be an object")
            require(record.get("timestamp_qualified") is False, "timestamp must remain unqualified")
            records.append(record)
    require(len(records) == count + 2, "JSONL frame count mismatch")
    header, summary = records[0], records[-1]
    require(header.get("record") == "header" and summary.get("record") == "summary", "JSONL record order")
    require(header.get("schema") == "gige-mel.aravis-time-probe" and
            type(header.get("schema_version")) is int and header["schema_version"] == 2, "wrong time-probe schema/version")
    uint(header.get("requested_frames"), "requested_frames", 32, 1)
    require(header["requested_frames"] == count, "requested frame count mismatch")
    uint(header.get("receive_deadline_ms"), "receive_deadline_ms")
    require(header["receive_deadline_ms"] == 10000, "unexpected time-probe receive bound")
    cfg = configuration(header.get("configuration"))
    if config is not None:
        require(cfg == config, "separate acquisition configurations disagree")
    require(header.get("interface") == "gige_vision" and header.get("native_device_type") == "ArvGvDevice",
            "collector requires production GigE interface")
    uint(header.get("diagnostic_tick_frequency_hz"), "diagnostic_tick_frequency_hz")
    require(isinstance(header.get("identity"), dict), "missing probe identity")
    for key, value in header["identity"].items():
        text(key, "identity key")
        text(value, "identity value")
    for field in ("diagnostic_tick_frequency_read", "actual_stream_timestamp_path_basis", "timing_origin",
                  "sdk_buffer_timestamp_source", "sdk_system_timestamp_source", "timestamp_units", "qualification_reason"):
        text(header.get(field), field)
    require(isinstance(header.get("sdk_stream_timestamp_paths"), dict), "missing stream path observations")
    generation = None
    for record in records[:-1]:
        require(record.get("device_id") == device, "JSONL exact device ID mismatch")
        require(record.get("interface") == "gige_vision", "JSONL interface mismatch")
        require(record.get("actual_stream_timestamp_path") == "unverified", "GigE timestamp path must stay unverified")
        for key in ("compiled_sdk_version", "runtime_sdk_version"):
            text(record.get(key), key)
            require(record[key] == header[key] and (sdk is None or record[key] == sdk[key]), "SDK version mismatch")

    for index, frame in enumerate(records[1:-1]):
        require(frame.get("record") == "frame", "expected frame record")
        uint(frame.get("index"), "index", 31)
        require(frame["index"] == index, "duplicate/incorrect frame index")
        for key in ("stream_generation", "native_frame_id", "sdk_buffer_timestamp_ns", "sdk_system_timestamp_ns"):
            uint(frame.get(key), key, minimum=1 if key == "stream_generation" else 0)
        if generation is None:
            generation = frame["stream_generation"]
        require(frame["stream_generation"] == generation, "stream generation changed")
        for key in ("width", "height", "offset_x", "offset_y"):
            uint(frame.get(key), key, U32, 1 if key in ("width", "height") else 0)
            require(frame[key] == cfg[key], "frame/configuration geometry mismatch")
        require(frame.get("descriptor_timestamp_valid") is False, "descriptor timestamp must remain invalid")
        text(frame.get("qualification_reason"), "qualification_reason")
    for key in ("frames", "delivered_frames", "incomplete_frames", "timeout_count", "pool_starvation_events"):
        uint(summary.get(key), key)
    require(summary["frames"] == count and summary["delivered_frames"] == count, "summary count mismatch")
    return records

# Normalized snapshot of docs/aravis-integration.md, tested against its source.
TIMING_ITEMS = (
    'Camera manufacturer, model, serial, firmware and GenICam XML identity (`time-probe` records the XML size and SHA-256).',
    'The SDK version and the actual loaded library: the path and the runtime/compiled version.',
    "The native timestamp field (GVSP leader, chunk `ChunkTimestamp`, or event data) and the camera's documented acquisition event for it.",
    'Proof that the event denotes **start of sensor integration**: not frame end, readout, trigger or transmission.',
    'Tick frequency, units and checked conversion rules. The SDK conversion (`src/arvgvspprivate.h:576-591`) is unchecked and is not a qualification.',
    'Clock origin and epoch, and whether the domain is device-local, host-monotonic or synchronized UTC.',
    'PTP synchronization and lock-status evidence, where relevant.',
    'Timestamp behavior at frame-ID wrap, stream restart, camera reset, exposure change and network reconnection.',
    'The continuity/discontinuity policy and the rules for changing `clock_generation`.',
    'A hardware observation or vendor-backed qualification relating the timestamp event to actual exposure start.',
    'The maximum supported uncertainty, and its implications for the intended MEL consumer.',
    'Behavior when any required fact becomes unavailable. For example, with a zero tick frequency the SDK silently substitutes host time (`src/arvgvstream.c:484`). This must be fail-closed (invalid timestamps), never a fallback.',
    'Proof of the timestamp path **actually used by the qualified stream** (GIGE-MEL-010-R1). The SDK stream silently ignores its own stream-construction frequency read error (`src/arvgvstream.c:1613`). A separate pre-stream read, including `time-probe`\'s `diagnostic_tick_frequency_hz`, is therefore not evidence of the path. A qualified profile needs a mechanism that fixes or observes the value the stream itself used, for example chunk timestamps with a camera-documented frequency, or a stream-side value it can verify. Until then, `actual_stream_timestamp_path` stays `"unverified"`.',
)

def checklist(manifest):
    observations = manifest["observations"]
    lines = ["# Camera qualification evidence checklist", "", "## A. Evidence collected", "",
             "Collection status: " + manifest["status"],
             "Host UTC start/end are HOST LOG TIME ONLY, never camera UTC/PTP evidence.",
             "Device ID requested (JSON): `" + json.dumps(manifest["device_id"], ensure_ascii=True) + "`."]
    for name in OPERATIONS:
        lines.append("- " + name + ": " + manifest["commands"][name]["status"])
    lines += ["", "SDK identity/configuration observations: " + ("retained" if observations else "missing") + ".",
              "Capture and time-probe are separate acquisition runs; frame IDs need not be continuous.",
              "", "## B. Evidence missing", "",
              "All thirteen selected-physical-camera timing gates remain open (NOT QUALIFIED).",
              "Loaded SDK library path is not reported by this diagnostic; independently retain it later.",
              "Physical NIC/MTU, real packet loss/resend, cable removal and camera power-loss: NOT QUALIFIED.",
              "", "## C. Independent human/vendor/hardware review required", ""]
    for index, item in enumerate(TIMING_ITEMS, 1):
        lines.append(str(index) + ". **NOT QUALIFIED** — " + item)
    lines += ["", "Physical manufacturer/model/serial/firmware must be checked against the selected hardware.",
              "Start-of-integration meaning, epoch/domain, PTP lock/accuracy, clock generation continuity,",
              "hardware uncertainty and internal SDK frequency/fallback path: NOT QUALIFIED.",
              "A separate nonzero diagnostic_tick_frequency_hz does not identify ArvGvStream's own read.",
              "Increasing/epoch-like/equal timestamps or stable intervals prove none of these requirements.",
              "Matching image geometry proves no calibration, scene identity, sensitivity or timing.",
              "", "Retain vendor timing documents separately with title, revision, source, SHA-256 and",
              "relevant pages; correlate them to the exact physical camera and firmware. Do not edit this",
              "bundle to add them: create a separate reviewed record referencing its manifest hash.",
              "", "Success means EVIDENCE_COLLECTED_AND_VERIFIED only, never HARDWARE_QUALIFIED,",
              "TIMESTAMP_QUALIFIED or PRODUCTION_MEL_READY. No production timestamp admission changed.",
              "", "Read-only means no intentional feature reconfiguration. Opening may take GVCP control",
              "and create an SDK heartbeat thread; inspect starts no acquisition. Capture/time-probe",
              "create native streams and issue AcquisitionStart/Stop; SDK stream setup can write stream",
              "destination/port and a default packet size when absent. Normal cleanup stops, destroys",
              "streams and closes/releases control. A killed process cannot guarantee native cleanup.",
              "", "Keep this bundle private: camera identity, host and network details may be sensitive.",
              "Do not commit evidence to the repository. Hashes detect corruption, not authenticity.", ""]
    return "\n".join(lines)


def command_argv(manifest, name):
    argv = [manifest["diagnostic"]["path"]]
    if manifest["discovery_interface"] is not None:
        argv += ["--discovery-interface", manifest["discovery_interface"]]
    argv += [name, manifest["device_id"]]
    if name == "capture":
        argv += [posixpath.join(manifest["collection_directory"], "capture.pgm")]
    if name == "time-probe":
        argv += [str(manifest["parameters"]["frames"]),
                 posixpath.join(manifest["collection_directory"], "time-probe.jsonl")]
    return argv


def validate_content(root, m):
    observations = {}
    for name in ("inspect", "capture"):
        if m["commands"][name]["status"] == "succeeded":
            observations[name] = parse_diagnostic((root / (name + ".stdout.txt")).read_text(encoding="utf-8"),
                                                 m["device_id"], name, m["discovery_interface"])
    if "capture" in observations:
        cfg = observations["capture"]["configuration"]
        require(cfg == observations["inspect"]["configuration"], "capture/inspect configurations disagree")
        for key in ("compiled_sdk_version", "runtime_sdk_version"):
            require(observations["capture"][key] == observations["inspect"][key], "capture SDK version mismatch")
        observations["pgm"] = validate_pgm(root / "capture.pgm", cfg["width"], cfg["height"])
    if m["commands"]["time-probe"]["status"] == "succeeded":
        records = validate_probe(root / "time-probe.jsonl", m["device_id"], m["parameters"]["frames"],
                                 observations["inspect"]["configuration"], observations["inspect"])
        # Preserve the exact parsed observations, including uint64 values and identity.
        raw = (root / "time-probe.stdout.txt").read_text(encoding="utf-8")
        versions = re.findall(r"^Aravis runtime ([^,\n]+), compiled against ([^\n]+)$", raw, re.M)
        sdk = observations["inspect"]
        require(versions == [(sdk["runtime_sdk_version"], sdk["compiled_sdk_version"])],
                "time-probe stdout SDK version mismatch")
        require("Observed %d frame(s) from %s via ArvGvDevice; wrote " %
                (m["parameters"]["frames"], m["device_id"]) in raw, "time-probe stdout count/device mismatch")
        if m["discovery_interface"] is not None:
            require("GigE Vision discovery restricted to interface " + m["discovery_interface"] + " (UP, IPv4 " in raw,
                    "time-probe missing discovery restriction")
        observations["time-probe"] = records
    return observations


def verify(root):
    """Read-only, offline. Never execute or locate the recorded diagnostic."""
    root = Path(root)
    require(not (root / "manifest.json").is_symlink(), "manifest symlink forbidden")
    require((root / "manifest.json").stat().st_size <= 1024 * 1024, "oversized manifest")
    m = load_json((root / "manifest.json").read_bytes())
    require(isinstance(m, dict) and m.get("schema") == SCHEMA and
            type(m.get("schema_version")) is int and m["schema_version"] == VERSION, "invalid evidence schema/version")
    text(m["device_id"], "device_id")
    text(m["collection_directory"], "collection_directory")
    require(m["discovery_interface"] is None or isinstance(m["discovery_interface"], str), "invalid interface")
    if m["discovery_interface"] is not None:
        text(m["discovery_interface"], "discovery_interface")
    text(m["host"]["operating_system"], "operating_system")
    text(m["host"]["kernel"], "kernel")
    check_utc(m["start_utc"])
    check_utc(m["end_utc"])
    require(m["time_basis"] == "HOST LOG TIME ONLY", "invalid time claim")
    require(m["qualification"] == QUALIFICATION, "qualification claim changed")
    require(type(m["parameters"]["acquire"]) is bool, "invalid acquisition request")
    uint(m["parameters"]["frames"], "frames", 32, 1)
    uint(m["parameters"]["capture_frames"], "capture_frames", 1, 1)
    uint(m["timeout_seconds"], "timeout_seconds", COMMAND_TIMEOUT, COMMAND_TIMEOUT)
    diagnostic = m["diagnostic"]
    text(diagnostic["path"], "diagnostic path")
    require(re.fullmatch(r"[0-9a-f]{64}", diagnostic["sha256"]) is not None, "invalid executable hash")
    uint(diagnostic["size"], "diagnostic size", minimum=1)
    require(set(m["commands"]) == set(OPERATIONS), "invalid command set")
    expected = {"qualification-checklist.md"}
    stopped = False
    complete = True
    for name in OPERATIONS:
        cmd = m["commands"][name]
        requested = name == "inspect" or m["parameters"]["acquire"]
        status = cmd["status"]
        require(status in ("not_executed", "succeeded", "failed", "timed_out"), "invalid command status")
        if status == "not_executed":
            require(cmd == {"status": "not_executed"}, "unexecuted command has execution data")
            require(not requested or stopped, "requested command unexpectedly skipped")
            continue
        require(requested and not stopped, "unexpected/retried command")
        require(cmd["argv"] == command_argv(m, name), "command argument mismatch")
        check_utc(cmd["start_utc"])
        check_utc(cmd["end_utc"])
        require(type(cmd["exit_code"]) is int, "invalid exit status")
        require((status == "succeeded") == (cmd["exit_code"] == 0), "exit/status inconsistency")
        if status != "succeeded":
            stopped = True
            complete = False
        expected.update((name + ".stdout.txt", name + ".stderr.txt"))
        if name != "inspect":
            filename = "capture.pgm" if name == "capture" else "time-probe.jsonl"
            if status == "succeeded" or (root / filename).exists():
                expected.add(filename)
    require(set(m["artifacts"]) == expected, "missing/unexpected manifest artifacts")
    require({p.name for p in root.iterdir()} == expected | {"manifest.json"}, "missing/unmanifested files")
    for filename in expected:
        path = root / filename
        require(stat.S_ISREG(path.lstat().st_mode), "artifact must be a regular file: " + filename)
        uint(m["artifacts"][filename]["size"], "artifact size")
        require(m["artifacts"][filename] == digest(path), "artifact hash/size mismatch: " + filename)
    observations = validate_content(root, m)
    require(json.dumps(observations, sort_keys=True) == json.dumps(m["observations"], sort_keys=True),
            "retained observations inconsistent with artifacts")
    require((root / "qualification-checklist.md").read_text(encoding="utf-8") == checklist(m), "checklist mismatch")
    require(complete and m["status"] == SUCCESS and m["validation"] == {"status": "verified", "errors": []},
            "incomplete/invalid evidence (failure logs retained)")
    return m

class Subreaper:
    """Adopt/reap command descendants on Linux, without waiting on other groups."""
    def __enter__(self):
        require(sys.platform.startswith("linux"), "collection requires Linux process-group cleanup")
        self.libc = ctypes.CDLL(None, use_errno=True)
        self.old = ctypes.c_int()
        require(self.libc.prctl(37, ctypes.byref(self.old), 0, 0, 0) == 0, "PR_GET_CHILD_SUBREAPER failed")
        require(self.libc.prctl(36, 1, 0, 0, 0) == 0, "PR_SET_CHILD_SUBREAPER failed")
        return self

    def __exit__(self, *unused):
        self.libc.prctl(36, self.old.value, 0, 0, 0)


def kill_group(group, sig):
    try:
        os.killpg(group, sig)
    except ProcessLookupError:
        pass


def reap_group(group):
    while True:
        try:
            os.waitpid(-group, 0)
        except ChildProcessError:
            return
        except InterruptedError:
            continue


def run_command(argv, root, name, timeout=COMMAND_TIMEOUT):
    result = {"argv": argv, "start_utc": utc_now(), "exit_code": -1, "status": "failed"}
    with (root / (name + ".stdout.txt")).open("xb") as out, (root / (name + ".stderr.txt")).open("xb") as err:
        try:
            with Subreaper():
                child = subprocess.Popen(argv, stdout=out, stderr=err, start_new_session=True)
                try:
                    result["exit_code"] = child.wait(timeout=timeout)
                    result["status"] = "succeeded" if child.returncode == 0 else "failed"
                except subprocess.TimeoutExpired:
                    result["status"] = "timed_out"
                    kill_group(child.pid, signal.SIGTERM)
                    # Leave grace for every member, even if the leader exits first.
                    time.sleep(TERMINATE_GRACE)
                    kill_group(child.pid, signal.SIGKILL)
                    result["exit_code"] = child.wait()
                finally:
                    # A rogue descendant must not survive a failed or completed
                    # diagnostic. Only this newly-created command group is targeted.
                    kill_group(child.pid, signal.SIGKILL)
                    reap_group(child.pid)
        except OSError as error:
            err.write(("could not launch diagnostic: " + str(error) + "\n").encode())
    result["end_utc"] = utc_now()
    return result


def write_manifest(root, manifest):
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True, ensure_ascii=True) + "\n",
                                        encoding="utf-8")


def collect(diagnostic, device, output, acquire=False, frames=8, discovery_interface=None):
    text(device, "device_id")
    uint(frames, "frames", 32, 1)
    if discovery_interface is not None:
        text(discovery_interface, "discovery_interface")
    executable = Path(diagnostic).resolve(strict=True)
    require(executable.is_file() and os.access(executable, os.X_OK), "diagnostic must be an executable file")
    identity = digest(executable)
    require(identity["size"] > 0, "diagnostic is empty")
    root = Path(output).absolute()
    root.mkdir(mode=0o700, exist_ok=False)  # never overwrite an earlier collection
    m = {"schema": SCHEMA, "schema_version": VERSION, "device_id": device,
         "discovery_interface": discovery_interface, "collection_directory": str(root),
         "host": {"operating_system": platform.system(), "kernel": platform.release()},
         "start_utc": utc_now(), "end_utc": utc_now(), "time_basis": "HOST LOG TIME ONLY",
         "diagnostic": dict(identity, path=str(executable)),
         "parameters": {"acquire": acquire, "frames": frames, "capture_frames": 1},
         "timeout_seconds": COMMAND_TIMEOUT,
         "commands": {name: {"status": "not_executed"} for name in OPERATIONS},
         "observations": {}, "artifacts": {}, "status": INCOMPLETE,
         "validation": {"status": "incomplete", "errors": []}, "qualification": QUALIFICATION}
    # Initial manifest survives an unexpected collector interruption.
    write_manifest(root, m)
    try:
        for name in OPERATIONS:
            if name != "inspect" and not acquire:
                break
            m["commands"][name] = run_command(command_argv(m, name), root, name)
            write_manifest(root, m)
            if m["commands"][name]["status"] != "succeeded":
                raise EvidenceError(name + " " + m["commands"][name]["status"] + "; no further camera commands issued")
            # Validate before issuing another camera command.
            m["observations"] = validate_content(root, m)
        require(digest(executable) == identity, "diagnostic changed during collection")
        m["status"] = SUCCESS
        m["validation"] = {"status": "verified", "errors": []}
    except (EvidenceError, OSError, UnicodeError, KeyError, TypeError) as error:
        m["validation"] = {"status": "incomplete", "errors": [str(error)]}
    m["end_utc"] = utc_now()
    (root / "qualification-checklist.md").write_text(checklist(m), encoding="utf-8")
    for path in root.iterdir():
        if path.name != "manifest.json":
            m["artifacts"][path.name] = digest(path)
    write_manifest(root, m)
    if m["status"] != SUCCESS:
        raise EvidenceError("incomplete evidence retained at " + str(root) + ": " + "; ".join(m["validation"]["errors"]))
    try:
        verify(root)
    except (EvidenceError, OSError, UnicodeError, KeyError, TypeError) as error:
        m["status"] = INCOMPLETE
        m["validation"] = {"status": "incomplete", "errors": [str(error)]}
        (root / "qualification-checklist.md").write_text(checklist(m), encoding="utf-8")
        m["artifacts"]["qualification-checklist.md"] = digest(root / "qualification-checklist.md")
        write_manifest(root, m)
        raise EvidenceError("final offline validation failed; incomplete evidence retained: " + str(error)) from error
    return m


def frame_count(value):
    if not re.fullmatch(r"[0-9]+", value):
        raise argparse.ArgumentTypeError("frames must be a decimal integer in [1, 32]")
    try:
        return uint(int(value), "frames", 32, 1)
    except EvidenceError as error:
        raise argparse.ArgumentTypeError(str(error)) from error


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="operation", required=True)
    c = commands.add_parser("collect", help="inspect only unless --acquire is explicit")
    c.add_argument("--diagnostic", required=True)
    c.add_argument("--device-id", required=True)
    c.add_argument("--discovery-interface", default=None)
    c.add_argument("--output", required=True)
    c.add_argument("--acquire", action="store_true")
    c.add_argument("--frames", type=frame_count, default=8)
    v = commands.add_parser("verify", help="offline; no executable, SDK or network required")
    v.add_argument("--input", required=True)
    args = parser.parse_args(argv)
    try:
        if args.operation == "collect":
            collect(args.diagnostic, args.device_id, args.output, args.acquire, args.frames, args.discovery_interface)
        else:
            verify(args.input)
        print(SUCCESS + " (NOT hardware/timestamp qualification)")
        return 0
    except (EvidenceError, OSError, UnicodeError, KeyError, TypeError, OverflowError) as error:
        print("evidence error: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
