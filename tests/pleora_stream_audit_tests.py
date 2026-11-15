#!/usr/bin/env python3
"""SDK-free observer safety/codec tests. Not native traffic qualification."""
import argparse
import json
from pathlib import Path
import socket
import struct
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import pleora_stream_audit as audit
import pleora_veth_audit as veth


class ObserverTests(unittest.TestCase):
    def test_veth_bootstrap_independent_offsets_and_identity(self):
        data = veth.bootstrap(True)
        self.assertEqual(len(data), 0xf8)
        self.assertEqual(data[:8], bytes.fromhex("0002000080000000"))
        self.assertEqual(data[10:16], bytes.fromhex("020000400102"))
        self.assertEqual(data[0x10:0x18], bytes.fromhex("0000000100000001"))
        self.assertEqual(data[0x24:0x28], bytes.fromhex("0a402802"))
        self.assertEqual(data[0x34:0x38], bytes.fromhex("fffffffc"))
        self.assertEqual(data[0x44:0x48], bytes(4))
        for offset, size, expected in ((0x48, 32, b"GIGE-MEL project fixture"),
                                        (0x68, 32, b"SyntheticVeth040R1"),
                                        (0xd8, 16, b"AUDIT040R1"), (0xe8, 16, b"VethOnly")):
            self.assertEqual(data[offset:offset + size].split(b"\0")[0], expected)

    def test_veth_responder_correlates_bootstrap_reads_and_refuses_writes(self):
        with tempfile.TemporaryDirectory() as temporary:
            fake = Mock()
            packets = [bytes.fromhex(value) for value in (
                "4201000200001234", "420100800004123500000024",
                "42010084000812360000008800000020", "420100820008123700000a0000000002")]
            fake.recvfrom.side_effect = [(packet, (veth.RX, 45000)) for packet in packets] + [RuntimeError("end")]
            with patch.object(socket, "socket", return_value=fake):
                responder = veth.VethResponder(Path(temporary), True)
                responder.thread.join(2)
                responder.close()
            fake.bind.assert_called_once_with((veth.CAM, 3956))
            responses = [call.args[0] for call in fake.sendto.call_args_list]
            self.assertEqual(responses[0][:8], bytes.fromhex("0000000300f81234"))
            self.assertEqual(responses[1], bytes.fromhex("00000081000412350a402802"))
            self.assertEqual(responses[2][:12], bytes.fromhex("000000850024123600000088"))
            self.assertEqual(responses[3], bytes.fromhex("8001008300001237"))
            self.assertIn("unsupported command 0x0082", responder.errors[0])

    def test_veth_rejects_host_before_commands(self):
        with patch.object(socket, "if_nameindex", return_value=[(1, "lo"), (2, "physical")]), \
             patch.object(veth, "command") as command, patch.object(veth, "verify_sdk") as sdk:
            with self.assertRaisesRegex(RuntimeError, "ENVIRONMENT_BLOCKED"):
                veth.coordinator(argparse.Namespace())
            command.assert_not_called()
            sdk.assert_not_called()

    def test_capture_gate_rejects_loss_errors_and_early_termination(self):
        good = {"records": 5, "kernel_packets": 5, "kernel_drops": 0, "errors": []}
        self.assertTrue(veth.healthy(good))
        for changed in ({"kernel_drops": 1}, {"errors": ["end"]}, {"kernel_packets": 6},
                        {"responder_errors": ["unknown"]}):
            self.assertFalse(veth.healthy({**good, **changed}))

    def test_topology_validation_rejects_routes_mac_and_physical_interfaces(self):
        state = {"links": [{"ifname": "lo"}, {"ifname": "veth-rx", "address": veth.RX_MAC,
                          "mtu": 1500, "flags": ["UP"], "linkinfo": {"info_kind": "veth"}}],
                 "addresses": [{"ifname": "veth-rx", "addr_info": [
                     {"family": "inet", "local": veth.RX, "prefixlen": 30}]}],
                 "routes": [{"dst": "10.64.40.0/30", "dev": "veth-rx"}], "routes6": []}
        veth.check_network(state, veth.RX)
        import copy
        for mutation in (lambda s: s["routes"].append({"dst": "default", "dev": "veth-rx"}),
                         lambda s: s["links"][1].update(address="00:00:00:00:00:00"),
                         lambda s: s["links"].append({"ifname": "physical"}),
                         lambda s: s["routes6"].append({"dst": "default", "dev": "veth-rx"})):
            changed = copy.deepcopy(state)
            mutation(changed)
            with self.assertRaisesRegex(RuntimeError, "ENVIRONMENT_BLOCKED"):
                veth.check_network(changed, veth.RX)

    def test_bootstrap_validator_rejects_contradictory_identity(self):
        veth.validate_bootstrap(veth.bootstrap(True))
        for offset in (0, 4, 10, 0x24, 0x34, 0x44):
            data = bytearray(veth.bootstrap(True))
            data[offset] ^= 1
            with self.assertRaises(RuntimeError):
                veth.validate_bootstrap(data)

    def test_dual_capture_does_not_double_count_and_unknown_outgoing_fails(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / "camera").mkdir()
            row = {"packet_type": 4, "hex": "420100020000b001", "destination_port": 3956,
                   "gvcp_command": "0x0002", "request_id": 45057, "payload": ""}
            (directory / "packets.jsonl").write_text(json.dumps(row) + "\n")
            (directory / "camera/packets.jsonl").write_text(json.dumps({**row, "packet_type": 0}) + "\n")
            report = veth.traffic_summary(directory)
            self.assertEqual(report["transmitted_commands"], 1)
            self.assertEqual(report["commands_received_camera_capture"], 1)
            self.assertTrue(report["endpoint_gvcp_match"])
            self.assertEqual(report["unidentified_outgoing"], [])
            with (directory / "packets.jsonl").open("a") as log:
                log.write(json.dumps({**row, "gvcp_command": "0x0082", "payload": "00000a0000000002"}) + "\n")
            self.assertFalse(veth.traffic_summary(directory)["endpoint_gvcp_match"])
            self.assertEqual(len(veth.traffic_summary(directory)["unidentified_outgoing"]), 1)
            with (directory / "packets.jsonl").open("a") as log:
                log.write(json.dumps({"packet_type": 4, "hex": "unknown"}) + "\n")
            self.assertEqual(len(veth.traffic_summary(directory)["unidentified_outgoing"]), 2)

    def test_host_interfaces_refused_before_directory_or_sdk_access(self):
        with patch.object(socket, "if_nameindex", return_value=[(1, "lo"), (2, "physical")]), \
             patch.object(audit, "verify_sdk") as verify, patch.object(audit.subprocess, "run") as run:
            with self.assertRaisesRegex(RuntimeError, "only lo"):
                audit.execute(argparse.Namespace())
            verify.assert_not_called()
            run.assert_not_called()

    def test_capture_is_unfiltered_and_records_non_gvcp_and_command(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            # Independently literal Ethernet/IPv4/UDP/GVCP discovery request.
            packet = bytes.fromhex("0000000000000000000000000800450000240000000040110000"
                                   "7f0000017f000002a1230f7400100000420100020000b001")
            fake = Mock()
            fake.recvfrom.side_effect = [(packet, ("lo", 0, 4)), (b"unknown", ("lo", 0, 0)), RuntimeError("end")]
            fake.getsockopt.return_value = struct.pack("II", 2, 0)
            with patch.object(socket, "socket", return_value=fake) as factory:
                capture = audit.Capture(directory)
                capture.thread.join(2)
                coverage = capture.close()
            factory.assert_called_once_with(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3))
            fake.bind.assert_not_called()
            fake.setsockopt.assert_not_called()  # no filter excluding unknown traffic
            rows = [json.loads(line) for line in (directory / "packets.jsonl").read_text().splitlines()]
            self.assertEqual(rows[0]["gvcp_command"], "0x0002")
            self.assertEqual(rows[0]["request_id"], 0xb001)
            self.assertEqual(rows[0]["destination_port"], 3956)
            self.assertEqual(rows[1]["hex"], b"unknown".hex())
            self.assertEqual(coverage["errors"], ["end"])
            self.assertEqual(coverage["kernel_drops"], 0)
            data = (directory / "all-interfaces.pcap").read_bytes()
            self.assertEqual(data[:4], b"\xd4\xc3\xb2\xa1")
            self.assertIn(packet, data)

    def test_responder_is_exclusive_loopback_and_refuses_writes(self):
        with tempfile.TemporaryDirectory() as temporary:
            fake = Mock()
            commands = [bytes.fromhex(value) for value in (
                "420100020000b001", "420100800004001100000a00", "420100820008001200000a0000000002")]
            fake.recvfrom.side_effect = [(packet, ("127.0.0.1", 45000)) for packet in commands] + [RuntimeError("end")]
            with patch.object(socket, "socket", return_value=fake):
                responder = audit.Responder(Path(temporary))
                responder.thread.join(2)
                responder.close()
            fake.bind.assert_called_once_with(("127.0.0.2", 3956))
            fake.setsockopt.assert_not_called()
            responses = [call.args[0] for call in fake.sendto.call_args_list]
            self.assertEqual(responses[0][:8], bytes.fromhex("0000000300f8b001"))
            self.assertEqual(len(responses[0]), 256)
            self.assertEqual(responses[0][8 + 36:8 + 40], socket.inet_aton("127.0.0.2"))
            self.assertEqual(responses[1], bytes.fromhex("000000810004001100000000"))
            self.assertEqual(responses[2], bytes.fromhex("8001008300000012"))
            self.assertEqual(responder.errors, ["end"])


if __name__ == "__main__":
    unittest.main()