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


class ObserverTests(unittest.TestCase):
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