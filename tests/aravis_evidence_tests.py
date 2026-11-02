#!/usr/bin/env python3
"""SDK-free evidence regression tests. Fixtures are not camera qualification."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("evidence", ROOT / "scripts/aravis_evidence.py")
e = importlib.util.module_from_spec(spec)
spec.loader.exec_module(e)
DEVICE = 'Exact camera; $(never-execute)'
CFG = dict(width=3, height=2, offset_x=0, offset_y=0, frame_rate_hz=25, exposure_ns=10000000)


def probe(count=2):
    common = dict(device_id=DEVICE, interface="gige_vision", compiled_sdk_version="0.8.36",
                  runtime_sdk_version="0.8.36", actual_stream_timestamp_path="unverified", timestamp_qualified=False)
    header = dict(common, record="header", schema="gige-mel.aravis-time-probe", schema_version=2,
                  native_device_type="ArvGvDevice", identity={"DeviceID": "exact metadata α", "GenICamXmlSha256": "a" * 64},
                  diagnostic_tick_frequency_hz=e.U64, configuration=CFG, requested_frames=count,
                  receive_deadline_ms=10000, sdk_stream_timestamp_paths={"zero": "host fallback"})
    for key in ("diagnostic_tick_frequency_read", "actual_stream_timestamp_path_basis", "timing_origin",
                "sdk_buffer_timestamp_source", "sdk_system_timestamp_source", "timestamp_units", "qualification_reason"):
        header[key] = "unverified observation"
    frames = [dict(common, record="frame", index=i, stream_generation=e.U64, native_frame_id=e.U64 - i,
                   sdk_buffer_timestamp_ns=e.U64 - i, sdk_system_timestamp_ns=e.U64, descriptor_timestamp_valid=False,
                   qualification_reason="unqualified", **{k: CFG[k] for k in ("width", "height", "offset_x", "offset_y")})
              for i in range(count)]
    return [header] + frames + [dict(record="summary", frames=count, delivered_frames=count,
                                    incomplete_frames=0, timeout_count=0, pool_starvation_events=0, timestamp_qualified=False)]


def jsonl(path, records):
    path.write_text("".join(json.dumps(r, ensure_ascii=False) + "\n" for r in records), encoding="utf-8")


class EvidenceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="gige-evidence-test-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.fixture = self.root / "fixture"
        self.fixture.mkdir()
        jsonl(self.fixture / "probe", probe())
        self.pgm = self.root / "image.pgm"
        self.pgm.write_bytes(b"P5\n3 2\n255\n\x00\n\xff\x20\x09\x04")
        config = "  " + " ".join(k + "=" + str(v) for k, v in CFG.items()) + "\n"
        self.inspect = "Aravis runtime 0.8.36, compiled against 0.8.36\nDevice " + DEVICE + " via GigEVision (read-only inspection)\n  DeviceID: exact metadata α\n" + config
        self.capture = "Aravis runtime 0.8.36, compiled against 0.8.36\nCaptured device " + DEVICE + ": 3x2 Mono8, source row stride 3 bytes, native frame ID 9, stream generation 1\nFrameTimestamp invalid\n" + config
        (self.fixture / "inspect").write_text(self.inspect, encoding="utf-8")
        (self.fixture / "capture").write_text(self.capture, encoding="utf-8")
        self.diag = self.root / "diagnostic"
        self.diag.write_text("#!" + sys.executable + "\n" + '''import sys, json, pathlib
p = pathlib.Path(__file__).parent
args = sys.argv[1:]
with (p / 'calls').open('a') as f: f.write(json.dumps(args) + '\\n')
prefix = ''
if args[0] == '--discovery-interface':
    prefix = 'GigE Vision discovery restricted to interface ' + args[1] + ' (UP, IPv4 127.0.0.1); no fallback to other interfaces\\n'
    args = args[2:]
name = args[0]
if (p / 'fail').exists() or (p / ('fail-' + name)).exists():
    print('native failure', file=sys.stderr); sys.exit(7)
if (p / 'hang').exists():
    import subprocess, time, signal
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(300)'])
    (p / 'descendant').write_text(str(child.pid))
    time.sleep(300)
if name in ('inspect', 'capture'):
    print(prefix + (p / 'fixture' / name).read_text(), end='')
if name == 'capture': (pathlib.Path(args[2])).write_bytes((p / 'image.pgm').read_bytes())
if name == 'time-probe':
    print(prefix + 'Aravis runtime 0.8.36, compiled against 0.8.36\\nObserved 2 frame(s) from ' + args[1] + ' via ArvGvDevice; wrote fixture')
    pathlib.Path(args[3]).write_bytes((p / 'fixture' / 'probe').read_bytes())
''', encoding="utf-8")
        self.diag.chmod(0o755)

    def collect(self, acquire=True, interface="lo", name="bundle"):
        path = self.root / name
        e.collect(self.diag, DEVICE, path, acquire, 2, interface)
        return path

    def test_valid_p5_and_uint64_probe(self):
        self.assertEqual(e.validate_pgm(self.pgm, 3, 2)["payload_bytes"], 6)
        self.assertEqual(e.validate_probe(self.fixture / "probe", DEVICE, 2), probe())

    def test_invalid_pgm(self):
        original = self.pgm.read_bytes()
        for raw in (original[:-1], original + b"x", original.replace(b"3 2", b"2 3"),
                    original.replace(b"P5", b"P2"), original.replace(b"255", b"254"),
                    original.replace(b"3 2", b"0 2"), b"P5\n99999999999999999999999999999999 2\n255\n"):
            with self.subTest(raw=raw):
                self.pgm.write_bytes(raw)
                with self.assertRaises(e.EvidenceError): e.validate_pgm(self.pgm, 3, 2)
        for dimensions in ((0, 2), (1 << 65, 2), (3.0, 2), (True, 2)):
            with self.assertRaises(e.EvidenceError): e.validate_pgm(self.pgm, *dimensions)

    def test_invalid_probe(self):
        mutations = [lambda r: r.pop(1), lambda r: r[2].update(index=0), lambda r: r[1].update(index=8),
                     lambda r: r[0].update(schema="wrong"), lambda r: r[0].update(schema_version=1),
                     lambda r: r[1].update(timestamp_qualified=True), lambda r: r[1].update(descriptor_timestamp_valid=True),
                     lambda r: r[1].update(device_id="other"), lambda r: r[0].update(actual_stream_timestamp_path="qualified"),
                     lambda r: r[-1].update(frames=1), lambda r: r[1].update(width=4),
                     lambda r: r[2].update(stream_generation=1), lambda r: r.insert(1, r[0])]
        for value in (-1, 1 << 64, 1.5, True, "42", float(e.U64)):
            mutations.append(lambda r, v=value: r[1].update(sdk_buffer_timestamp_ns=v))
        for mutate in mutations:
            records = copy.deepcopy(probe()); mutate(records)
            jsonl(self.fixture / "probe", records)
            with self.assertRaises(e.EvidenceError): e.validate_probe(self.fixture / "probe", DEVICE, 2)
        for raw in ('{broken\n', '{"a":1,"a":2}\n', '{"x":NaN}\n', ' ' * 4097 + '\n'):
            (self.fixture / "probe").write_text(raw)
            with self.assertRaises(e.EvidenceError): e.validate_probe(self.fixture / "probe", DEVICE, 2)

    def test_acquisition_manifest_exact_metadata_and_interface(self):
        path = self.collect()
        m = e.verify(path)
        self.assertEqual(m["status"], e.SUCCESS)
        self.assertEqual(m["observations"]["time-probe"], probe())
        self.assertEqual(m["observations"]["inspect"]["identity_observations"]["DeviceID"], "exact metadata α")
        self.assertEqual(m["discovery_interface"], "lo")
        calls = [json.loads(line) for line in (self.root / "calls").read_text().splitlines()]
        self.assertEqual([call[2] for call in calls], list(e.OPERATIONS))
        for call in calls:
            self.assertEqual(call[:2], ["--discovery-interface", "lo"])
            self.assertEqual(call[3], DEVICE)
        for name, identity in m["artifacts"].items(): self.assertEqual(identity, e.digest(path / name))

    def test_inspect_only_default_and_portable_offline(self):
        path = self.collect(acquire=False, interface=None)
        m = e.verify(path)
        self.assertEqual(m["commands"]["capture"], {"status": "not_executed"})
        self.assertFalse((path / "capture.pgm").exists())
        self.assertEqual(json.loads((self.root / "calls").read_text()), ["inspect", DEVICE])
        moved = self.root / "different-directory"
        shutil.copytree(path, moved)
        self.diag.unlink()
        # Offline verification must not execute, query interfaces or create sockets.
        with patch.object(subprocess, "Popen", side_effect=AssertionError("subprocess")), \
             patch.object(socket, "socket", side_effect=AssertionError("network")):
            self.assertEqual(e.verify(moved), m)

    def test_integrity_and_manifest_negative_controls(self):
        path = self.collect()
        original = (path / "manifest.json").read_bytes()
        m = json.loads(original)
        for raw in (b"broken", b"[]", b"{}", original.replace(b'evidence"', b'wrong"'),
                    json.dumps(dict(m, qualification={"hardware": "QUALIFIED"})).encode()):
            (path / "manifest.json").write_bytes(raw)
            with self.assertRaises((e.EvidenceError, KeyError, TypeError)): e.verify(path)
        (path / "manifest.json").write_bytes(original)
        pgm = (path / "capture.pgm").read_bytes()
        (path / "capture.pgm").unlink()
        with self.assertRaises(e.EvidenceError): e.verify(path)
        (path / "capture.pgm").write_bytes(pgm + b"x")
        with self.assertRaises(e.EvidenceError): e.verify(path)
        # Even an updated hash cannot conceal an invalid payload.
        m["artifacts"]["capture.pgm"] = e.digest(path / "capture.pgm")
        e.write_manifest(path, m)
        with self.assertRaises(e.EvidenceError): e.verify(path)
        (path / "capture.pgm").write_bytes(pgm)
        (path / "manifest.json").write_bytes(original)
        (path / "extra").write_text("unmanifested")
        with self.assertRaises(e.EvidenceError): e.verify(path)
        (path / "extra").unlink()
        (path / "capture.pgm").unlink()
        (path / "capture.pgm").symlink_to(self.pgm)
        with self.assertRaises(e.EvidenceError): e.verify(path)

    def test_existing_output_invalid_count_and_executable(self):
        path = self.collect(False)
        before = (path / "manifest.json").read_bytes()
        with self.assertRaises(FileExistsError): e.collect(self.diag, DEVICE, path)
        self.assertEqual((path / "manifest.json").read_bytes(), before)
        for value in (0, 33, -1, True, 2.0):
            with self.assertRaises(e.EvidenceError): e.collect(self.diag, DEVICE, self.root / "bad", frames=value)
        for value in ("0", "33", "x", "1.0", "-1"):
            with self.assertRaises(Exception): e.frame_count(value)
        self.assertFalse((self.root / "bad").exists())
        self.diag.chmod(0o644)
        with self.assertRaises(e.EvidenceError): e.collect(self.diag, DEVICE, self.root / "bad")

    def test_failed_restricted_command_no_retry(self):
        (self.root / "fail").touch()
        with self.assertRaises(e.EvidenceError): self.collect()
        path = self.root / "bundle"
        m = json.loads((path / "manifest.json").read_text())
        self.assertEqual(m["status"], e.INCOMPLETE)
        self.assertEqual(m["commands"]["inspect"]["exit_code"], 7)
        self.assertEqual(m["commands"]["capture"]["status"], "not_executed")
        self.assertEqual(len((self.root / "calls").read_text().splitlines()), 1)
        self.assertIn("native failure", (path / "inspect.stderr.txt").read_text())
        with self.assertRaises(e.EvidenceError): e.verify(path)

    def test_capture_failure_stops_before_probe(self):
        (self.root / "fail-capture").touch()
        with self.assertRaises(e.EvidenceError): self.collect()
        path = self.root / "bundle"
        m = json.loads((path / "manifest.json").read_text())
        self.assertEqual(m["commands"]["inspect"]["status"], "succeeded")
        self.assertEqual(m["commands"]["capture"]["status"], "failed")
        self.assertEqual(m["commands"]["time-probe"]["status"], "not_executed")
        self.assertEqual(len((self.root / "calls").read_text().splitlines()), 2)
        before = {p.name: p.read_bytes() for p in path.iterdir()}
        with self.assertRaises(e.EvidenceError): e.verify(path)
        self.assertEqual(before, {p.name: p.read_bytes() for p in path.iterdir()})

    def assert_incomplete(self, path, calls, acquired=True):
        m = json.loads((path / "manifest.json").read_bytes())
        self.assertEqual(m["status"], e.INCOMPLETE)
        self.assertEqual(m["validation"]["status"], "incomplete")
        self.assertEqual(len((self.root / "calls").read_text().splitlines()), calls)
        names = ["inspect.stdout.txt", "inspect.stderr.txt"]
        if acquired:
            names += ["capture.stdout.txt", "capture.stderr.txt", "capture.pgm",
                      "time-probe.stdout.txt", "time-probe.stderr.txt", "time-probe.jsonl"]
            self.assertEqual((path / "capture.pgm").read_bytes(), self.pgm.read_bytes())
            self.assertEqual((path / "time-probe.jsonl").read_bytes(), (self.fixture / "probe").read_bytes())
        for name in names:
            self.assertTrue((path / name).is_file(), name)
        before = {p.name: p.read_bytes() for p in path.iterdir()}
        with self.assertRaises(e.EvidenceError): e.verify(path)
        self.assertEqual(before, {p.name: p.read_bytes() for p in path.iterdir()})
        self.assertFalse(list(path.glob(".manifest-*.tmp")))
        return m

    def test_final_validation_failure_is_incomplete(self):
        with patch.object(e, "validate_manifest", side_effect=e.EvidenceError("final control")):
            with self.assertRaisesRegex(e.EvidenceError, "final control"): self.collect()
        m = self.assert_incomplete(self.root / "bundle", 3)
        self.assertIn("final control", m["validation"]["errors"])

    def test_final_candidate_rejects_changed_checklist(self):
        real_validate = e.validate_manifest
        for rehash in (False, True):
            def corrupt(root, candidate):
                path = root / "qualification-checklist.md"
                path.write_bytes(path.read_bytes() + b"changed\n")
                if rehash:
                    candidate["artifacts"][path.name] = e.digest(path)
                return real_validate(root, candidate)
            path = self.root / ("checklist-" + str(rehash))
            message = "checklist mismatch" if rehash else "hash/size mismatch"
            with patch.object(e, "validate_manifest", side_effect=corrupt):
                with self.assertRaisesRegex(e.EvidenceError, message): self.collect(name=path.name)
            m = self.assert_incomplete(path, 6 if rehash else 3)
            self.assertIn(message, m["validation"]["errors"][0])

    def test_moved_acquisition_without_diagnostic_or_network(self):
        path = self.collect()
        moved = self.root / "moved"
        path.rename(moved)
        self.diag.unlink()
        with patch.object(subprocess, "Popen", side_effect=AssertionError("subprocess")), \
             patch.object(socket, "socket", side_effect=AssertionError("network")):
            self.assertEqual(e.verify(moved)["status"], e.SUCCESS)

    def test_interrupt_before_final_validation(self):
        # R1-A: the reviewed code persisted success at the equivalent boundary.
        for acquire in (True, False):
            path = self.root / ("interrupt-" + str(acquire))
            with patch.object(e, "validate_manifest", side_effect=KeyboardInterrupt("before validation")) as validate:
                with self.assertRaises(KeyboardInterrupt): self.collect(acquire=acquire, name=path.name)
            self.assertEqual(validate.call_count, 1)
            self.assert_incomplete(path, 3 if acquire else 4, acquired=acquire)

    def test_interrupt_after_validation_before_commit(self):
        # R1-C: run every real check, then interrupt before publication starts.
        real_validate = e.validate_manifest
        def validated_then_interrupt(root, candidate):
            real_validate(root, candidate)
            self.assertEqual(json.loads((root / "manifest.json").read_bytes())["status"], e.INCOMPLETE)
            raise SystemExit("validated, not committed")
        with patch.object(e, "validate_manifest", side_effect=validated_then_interrupt):
            with self.assertRaises(SystemExit): self.collect()
        self.assert_incomplete(self.root / "bundle", 3)

    def test_failure_before_atomic_replace_preserves_manifest(self):
        # R1-D: inject at the actual commit boundary, after bytes are fsynced.
        real_replace = e.os.replace
        for index, failure in enumerate((KeyboardInterrupt, SystemExit, OSError)):
            previous = []
            def fail_success(source, destination):
                candidate = json.loads(Path(source).read_bytes())
                if candidate["status"] == e.SUCCESS:
                    previous.append(Path(destination).read_bytes())
                    self.assertEqual(json.loads(previous[-1])["status"], e.INCOMPLETE)
                    self.assertEqual(Path(source).parent, Path(destination).parent)
                    raise failure("before replace")
                return real_replace(source, destination)
            path = self.root / ("before-replace-" + str(index))
            with patch.object(e.os, "replace", side_effect=fail_success):
                with self.assertRaises(failure): self.collect(name=path.name)
            self.assertEqual(len(previous), 1)
            self.assertEqual((path / "manifest.json").read_bytes(), previous[0])
            self.assert_incomplete(path, 3 * (index + 1))

    def test_atomic_success_acquisition_and_inspect_only(self):
        # R1-E/F: old canonical and complete staged bytes at the commit boundary.
        real_replace, real_fsync, real_validate = e.os.replace, e.os.fsync, e.validate_manifest
        for acquire in (True, False):
            validated, synced, committed = [], [], []
            def validate(root, candidate):
                self.assertEqual(json.loads((root / "manifest.json").read_bytes())["status"], e.INCOMPLETE)
                result = real_validate(root, candidate)
                validated.append(copy.deepcopy(candidate))
                return result
            def fsync(fd):
                real_fsync(fd)
                synced.append(os.fstat(fd).st_ino)
            def replace(source, destination):
                source, destination = Path(source), Path(destination)
                candidate = json.loads(source.read_bytes())
                self.assertIn(source.stat().st_ino, synced)
                self.assertEqual(source.stat().st_mode & 0o777, 0o600)
                self.assertEqual(source.parent, destination.parent)
                if candidate["status"] == e.SUCCESS:
                    self.assertEqual(validated, [candidate])
                    self.assertEqual(json.loads(destination.read_bytes())["status"], e.INCOMPLETE)
                    self.assertEqual((destination.parent / "qualification-checklist.md").read_bytes(),
                                     e.checklist(candidate).encode("utf-8"))
                    self.assertEqual(candidate["artifacts"]["qualification-checklist.md"],
                                     e.digest(destination.parent / "qualification-checklist.md"))
                    committed.append(candidate)
                real_replace(source, destination)
                self.assertEqual(json.loads(destination.read_bytes()), candidate)
            with patch.object(e, "validate_manifest", side_effect=validate), \
                 patch.object(e.os, "fsync", side_effect=fsync), patch.object(e.os, "replace", side_effect=replace):
                path = self.collect(acquire=acquire, name="atomic-" + str(acquire))
            self.assertEqual(len(committed), 1)
            self.assertEqual(e.verify(path), committed[0])
            self.assertEqual(committed[0]["validation"], {"status": "verified", "errors": []})
            self.assertEqual(path.stat().st_mode & 0o777, 0o700)
            for name, identity in committed[0]["artifacts"].items():
                self.assertEqual(identity, e.digest(path / name))
            self.assertFalse(list(path.glob(".manifest-*.tmp")))

    def test_timeout_group_cleanup_no_retry(self):
        (self.root / "hang").touch()
        real_run = e.run_command
        def bounded(argv, root, name): return real_run(argv, root, name, timeout=0.3)
        unrelated = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"], start_new_session=True)
        try:
            with patch.object(e, "run_command", side_effect=bounded):
                with self.assertRaises(e.EvidenceError): self.collect()
            self.assertIsNone(unrelated.poll(), "timeout must not kill unrelated processes")
        finally:
            unrelated.terminate()
            unrelated.wait()
        m = json.loads((self.root / "bundle/manifest.json").read_text())
        self.assertEqual(m["commands"]["inspect"]["status"], "timed_out")
        self.assertEqual(m["status"], e.INCOMPLETE)
        self.assertEqual(m["validation"]["status"], "incomplete")
        with self.assertRaises(e.EvidenceError): e.verify(self.root / "bundle")
        self.assertEqual(len((self.root / "calls").read_text().splitlines()), 1)
        pid = int((self.root / "descendant").read_text())
        with self.assertRaises(ProcessLookupError): os.kill(pid, 0)

    def test_checklist_tracks_existing_thirteen_items(self):
        section = (ROOT / "docs/aravis-integration.md").read_text().split("### Camera-timing qualification checklist", 1)[1].split("None of the following is evidence", 1)[0]
        items = []
        for line in section.splitlines():
            match = re.match(r"^\d+\. (.*)$", line)
            if match: items.append(match[1])
            elif items and line.startswith("   "): items[-1] += " " + line.strip()
        self.assertEqual(tuple(items), e.TIMING_ITEMS)
        self.assertEqual(len(items), 13)
        path = self.collect(False)
        self.assertEqual((path / "qualification-checklist.md").read_text().count("**NOT QUALIFIED**"), 13)


if __name__ == "__main__":
    unittest.main(verbosity=2)
