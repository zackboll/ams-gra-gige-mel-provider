#!/usr/bin/env python3
"""Deterministic SDK-free wire regressions; no namespace privilege or camera."""
import copy
import importlib.util
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import gentl_wire as W
import gentl_wire_runner as R


def command(code=0x80, payload=struct.pack("!I", 0x1f0), transaction=2):
    return struct.pack("!BBHHH", 0x42, 1, code, len(payload), transaction) + payload


def record(data, stamp=3):
    return dict(W.classify(data), timestamp_ns=stamp, phase="producer", direction="command",
                source=["127.0.0.1", 10000], destination=["127.0.0.1", 3956], payload=data.hex())


def response(request, stamp=4):
    code = request["command"] + 1
    body = (bytes(248) if code == 3 else struct.pack("!I", 123) if code == 0x81 else
            struct.pack("!I", request["addresses"][0]) + bytes(request["size"]))
    return dict(phase="producer", direction="ack", source=request["destination"][:],
                destination=request["source"][:], transaction=request["transaction"], ack_command=code,
                status=0, timestamp_ns=stamp,
                payload=(struct.pack("!HHHH", 0, code, len(body), request["transaction"]) + body).hex())


class WireTests(unittest.TestCase):
    def setUp(self):
        self.records = [record(command(2, b"", 1)), record(command())]
        self.records += [response(r) for r in self.records]
        self.observation = {"active": True, "complete": True, "drops": 0, "calibrated": True, "records": 2}
        self.markers = {"open_readonly": 1, "inspect": 2, "required_read_attempt": 2,
                        "required_read_success": 5, "device_closed": 6,
                        "native_closed": 7, "quiet_complete": 500000007}
        self.native = {"returncode": 0, "timed_out": False, "simulator_shutdown": True,
                       "descendants_reaped": True, "quiet_ns": 500000000}

    def result(self, synthetic=True):
        self.observation["records"] = len(self.records)
        return W.analyze(self.records, self.observation, self.markers, self.native, synthetic)

    def test_success_and_never_qualify_synthetic(self):
        self.assertEqual(self.result()["result"], "HARNESS_SELF_TEST_PASS")
        # Production state machine requires an actual launch in the runner. This
        # pure analyzer test verifies spelling, not real-producer qualification.
        self.assertEqual(self.result(False)["result"], "READ_ONLY_WIRE_PASS")
        self.assertEqual(self.result()["command_counts"], {"Discovery": 1, "ReadRegister": 1})

    def test_every_write_violates_even_when_requested_readonly(self):
        for code, address, control in ((0x82, 0xa00, True), (0x82, 0x1f0, False),
                                       (0x86, 0x200, False), (0x86, 0x9fc, True)):
            with self.subTest(code=code, address=address):
                data = struct.pack("!II", address, 2)
                if address == 0x9fc:
                    data += b"abcd"  # range crosses CCP without starting there
                self.records = self.records[:2] + [record(command(code, data, 3))]
                result = self.result()
                self.assertEqual(result["result"], "READ_ONLY_WIRE_VIOLATION")
                self.assertEqual(result["requested_access"], "DEVICE_ACCESS_READONLY")
                self.assertEqual(result["control_privilege_write"], control)
                self.assertEqual(result["written_addresses"], [address])

    def test_unknown_malformed_truncated_boundaries(self):
        good = command()
        packets = [good[:n] for n in range(len(good))]
        packets += [command(0x42, b""), good + b"extra", command(0x80, b"x"),
                    command(0x82, b"abcd"), command(2, b"abcd"),
                    command(0x80, struct.pack("!I", 3)), command(transaction=0),
                    command(0x84, struct.pack("!IHH", 0xfffffffc, 0, 8)),
                    command(0x84, struct.pack("!IHH", 0, 1, 4)),
                    command(0x84, struct.pack("!IHH", 0, 0, 0)),
                    command(0x84, struct.pack("!IHH", 0, 0, 516)),
                    command(0x86, struct.pack("!I", 0xfffffffc) + b"abcdefgh"),
                    b"\x43" + good[1:], good[:1] + b"\x80" + good[2:]]
        for packet in packets:
            with self.subTest(packet=packet.hex()):
                self.assertEqual(W.classify(packet)["classification"], "Unqualified")
                self.records = self.records[:2] + [record(packet)]
                self.assertEqual(self.result()["result"], "READ_ONLY_WIRE_VIOLATION")

    def test_valid_memory_and_batched_registers(self):
        for packet in (command(0x84, struct.pack("!IHH", 0x200, 0, 512)),
                       command(0x80, struct.pack("!II", 0xa00, 0x1f0))):
            self.assertTrue(W.classify(packet)["classification"].startswith("Read"))
        result = W.classify(command(0x82, struct.pack("!IIII", 0x1f0, 1, 0xa00, 2)))
        self.assertTrue(result["control_privilege"])

    def test_duplicate_retransmission_and_id_reuse_not_deduplicated(self):
        for packet in (command(), command(0x80, struct.pack("!I", 0xa00), 2)):
            self.records = self.records[:2] + [record(packet)]
            result = self.result()
            self.assertEqual(result["result"], "READ_ONLY_WIRE_VIOLATION")
            self.assertIn("duplicate/retransmitted transaction", result["reasons"])

    def test_crash_timeout_and_cleanup_uncertainty(self):
        self.native["returncode"] = -11
        self.assertEqual(self.result()["result"], "PRODUCER_CRASH")
        self.native["timed_out"] = True
        self.assertEqual(self.result()["result"], "PRODUCER_TIMEOUT")
        self.native.update(returncode=1, timed_out=False)
        self.assertEqual(self.result()["result"], "NATIVE_CLEANUP_UNRESOLVED")
        self.native["returncode"] = 0
        for key in ("simulator_shutdown", "descendants_reaped"):
            self.native[key] = False
            self.assertEqual(self.result()["result"], "NATIVE_CLEANUP_UNRESOLVED")
            self.native[key] = True
        for key in ("open_readonly", "inspect", "device_closed", "native_closed", "quiet_complete"):
            saved = self.markers.pop(key)
            self.assertEqual(self.result()["result"], "NATIVE_CLEANUP_UNRESOLVED")
            self.markers[key] = saved
        self.markers["quiet_complete"] = 8
        self.assertEqual(self.result()["result"], "NATIVE_CLEANUP_UNRESOLVED")

    def test_post_close_heartbeat_and_configuration(self):
        for packet in (command(0x80, struct.pack("!I", 0xa00), 3),
                       command(0x82, struct.pack("!II", 0xa00, 0), 3)):
            self.records = self.records[:2] + [record(packet, 8)]
            result = self.result()
            self.assertEqual(result["result"], "READ_ONLY_WIRE_VIOLATION")
            self.assertIn("command after successful device close", result["reasons"])

    def test_missing_incomplete_dropped_evidence(self):
        self.assertEqual(W.analyze(self.records[:2], dict(self.observation, records=2), self.markers,
                                  self.native)["result"], "WIRE_OBSERVATION_UNAVAILABLE")
        for field, value in (("active", False), ("complete", False), ("drops", 1),
                             ("calibrated", False)):
            original = self.observation[field]
            self.observation[field] = value
            self.assertEqual(self.result()["result"], "WIRE_OBSERVATION_UNAVAILABLE")
            self.observation[field] = original
        self.assertEqual(W.analyze(self.records, {}, self.markers, self.native)["result"],
                         "WIRE_OBSERVATION_UNAVAILABLE")
        self.observation["records"] = 3
        self.assertEqual(W.analyze(self.records, self.observation, self.markers, self.native)["result"],
                         "WIRE_OBSERVATION_UNAVAILABLE")
        self.records.clear()
        self.assertEqual(self.result()["result"], "WIRE_OBSERVATION_UNAVAILABLE")

    def test_ip_udp_boundaries(self):
        payload = command()
        udp = struct.pack("!HHHH", 10000, 3956, 8 + len(payload), 0) + payload
        ip = bytearray(20)
        ip[0] = 0x45
        ip[9] = 17
        struct.pack_into("!H", ip, 2, 20 + len(udp))
        ip[12:16] = ip[16:20] = socket.inet_aton("127.0.0.1")
        data = bytes(ip) + udp
        self.assertEqual(W.decode_ip(data)["payload"], payload.hex())
        for n in range(len(data)):
            with self.assertRaises(ValueError):
                W.decode_ip(data[:n])
        for offset, value in ((0, 0x65), (0, 0x44), (6, 0x20), (9, 6), (25, 255)):
            malformed = bytearray(data)
            malformed[offset] = value
            with self.assertRaises(ValueError):
                W.decode_ip(malformed)

    def test_full_endpoint_ack_correlation_adversarial(self):
        for field, value in (("source", ["127.0.0.2", 3956]), ("source", ["127.0.0.1", 3957]),
                             ("destination", ["127.0.0.2", 10000]), ("destination", ["127.0.0.1", 10001]),
                             ("transaction", 7), ("ack_command", 0x85), ("status", 0x8001)):
            with self.subTest(field=field, value=value):
                saved = copy.deepcopy(self.records)
                self.records[-1][field] = value
                result = self.result(False)
                self.assertNotEqual(result["result"], "READ_ONLY_WIRE_PASS")
                self.assertGreater(result["unmatched_transactions"], 0)
                self.assertTrue(result["unexpected_acks"])
                self.records = saved
        self.records.append(copy.deepcopy(self.records[-1]))
        self.assertEqual(self.result()["result"], "WIRE_OBSERVATION_UNAVAILABLE")
        self.records = self.records[2:4]
        self.assertGreater(self.result()["unmatched_transactions"], 0)

    def test_parallel_client_ports_and_memory_echo(self):
        request = record(command(0x84, struct.pack("!IHH", 0x1ec, 0, 8)))
        request["source"][1] = 10001  # same ID as another client, independent transport identity
        ack = response(request)
        self.records += [request, ack]
        self.assertEqual(self.result()["result"], "HARNESS_SELF_TEST_PASS")
        self.records[-1]["payload"] = ack["payload"][:16] + struct.pack("!I", 0x200).hex() + ack["payload"][24:]
        self.assertEqual(self.result()["result"], "WIRE_OBSERVATION_UNAVAILABLE")

    def test_required_read_minimum_evidence(self):
        for key in ("required_read_attempt", "required_read_success"):
            saved = self.markers.pop(key)
            self.assertEqual(self.result()["result"], "WIRE_OBSERVATION_UNAVAILABLE")
            self.markers[key] = saved
        self.markers["required_read_attempt"] = 0
        self.assertEqual(self.result()["result"], "WIRE_OBSERVATION_UNAVAILABLE")
        self.markers["required_read_attempt"] = 2
        # Earlier optional metadata reads can succeed without satisfying TestRegister.
        self.records[1] = record(command(0x80, struct.pack("!I", 0x200)))
        self.records[3] = response(self.records[1])
        self.assertEqual(self.result()["required_read"]["state"], "SUCCESS_WITHOUT_WIRE")
        self.assertEqual(self.result()["result"], "WIRE_OBSERVATION_UNAVAILABLE")
        self.markers.pop("required_read_success")
        self.markers["required_read_failed"] = 5
        self.native["returncode"] = 1
        self.native["required_read_diagnostics"] = ["native=-1011 function=GCReadPort diagnostic=timeout"]
        self.assertEqual(self.result()["result"], "REQUIRED_INSPECTION_FAILED")
        self.assertEqual(self.result()["required_read"]["state"], "FAILED")
        self.native["descendants_reaped"] = False
        self.assertEqual(self.result()["result"], "NATIVE_CLEANUP_UNRESOLVED")

    def test_required_memory_read_and_missing_or_forged_ack(self):
        request = record(command(0x84, struct.pack("!IHH", 0x1ec, 0, 8)))
        self.records[1], self.records[3] = request, response(request)
        self.assertEqual(self.result()["result"], "HARNESS_SELF_TEST_PASS")
        self.records[3]["source"] = ["127.0.0.2", 3956]
        self.assertEqual(self.result()["result"], "WIRE_OBSERVATION_UNAVAILABLE")
        self.records.pop()
        self.assertEqual(self.result()["result"], "WIRE_OBSERVATION_UNAVAILABLE")
        self.records = self.records[:1] + self.records[2:]
        self.assertEqual(self.result()["result"], "WIRE_OBSERVATION_UNAVAILABLE")

    def test_success_marker_with_only_earlier_optional_wire_is_not_inspection(self):
        # Wire read completed before post-open inspection; even a native success
        # marker cannot repurpose it as evidence of the designated read operation.
        self.records[1]["timestamp_ns"] = 0
        self.records[3]["timestamp_ns"] = 1
        self.assertEqual(self.result()["required_read"]["state"], "SUCCESS_WITHOUT_WIRE")
        self.assertEqual(self.result(False)["result"], "WIRE_OBSERVATION_UNAVAILABLE")

    def test_ack_payload_constraints_and_unsolicited_ack(self):
        saved = copy.deepcopy(self.records)
        for payload in ("", self.records[3]["payload"][:-2], self.records[3]["payload"] + "00",
                        "0000008100080002" + "0000007b"):
            self.records = copy.deepcopy(saved)
            self.records[3]["payload"] = payload
            self.assertEqual(self.result()["result"], "WIRE_OBSERVATION_UNAVAILABLE")
        self.records = copy.deepcopy(saved)
        unsolicited = record(command(transaction=99))
        self.records.append(response(unsolicited))
        self.assertTrue(self.result()["unexpected_acks"])
        self.assertNotEqual(self.result(False)["result"], "READ_ONLY_WIRE_PASS")

    def test_fresh_namespace_only_no_physical_nic_or_routes(self):
        safe = {"namespace": "net:[new]", "links": [{"ifname": "lo", "flags": []}],
                "addresses": [{"ifname": "lo", "addr_info": []}], "routes": [], "ipv6_routes": []}
        R.require_isolation(safe, "net:[host]")
        for mutate in (lambda s: s.update(namespace="net:[host]"),
                       lambda s: s["links"].append({"ifname": "eth0"}),
                       lambda s: s["links"].append({"ifname": "veth0"}),
                       lambda s: s["routes"].append({"dst": "default", "dev": "lo"}),
                       lambda s: s["routes"].append({"dst": "192.0.2.0/24", "dev": "eth0"}),
                       lambda s: s["addresses"][0]["addr_info"].append({"local": "192.0.2.1"}),
                       lambda s: s["links"][0]["flags"].append("UP")):
            changed = copy.deepcopy(safe)
            mutate(changed)
            with self.assertRaises(ValueError):
                R.require_isolation(changed, "net:[host]")

    def test_no_producer_if_namespace_establishment_fails(self):
        spec = importlib.util.spec_from_file_location("qualification", ROOT / "scripts/gentl_qualification.py")
        q = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(q)
        with tempfile.TemporaryDirectory() as directory:
            args = type("Args", (), dict(output=Path(directory) / "bundle", aravis_library=Path(sys.executable),
                        probe=Path(sys.executable), producer=Path(sys.executable), interface="lo",
                        quiet=0.1, timeout=2, self_test=None))()
            with patch.object(q, "run_stage", return_value={"outcome": "failure", "returncode": 1}) as stage:
                self.assertEqual(R.collect(args, q.run_stage, q.digest, Path(q.__file__)), 1)
                self.assertEqual(stage.call_count, 1)
                self.assertNotIn("wire-readonly", stage.call_args.args[0])
            result = json.loads((args.output / "result.json").read_text())
            self.assertEqual(result["result"], "WIRE_OBSERVATION_UNAVAILABLE")

    def test_tooling_default_off_and_not_linked_or_installed(self):
        for path in (ROOT / "src").rglob("*.cpp"):
            self.assertNotIn("gentl_wire", path.read_text())
        cmake = (ROOT / "CMakeLists.txt").read_text()
        self.assertNotIn("install(PROGRAMS", cmake)
        self.assertNotIn("gentl_wire_runner.py", cmake)


if __name__ == "__main__":
    unittest.main()