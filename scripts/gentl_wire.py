"""Test-only GVCP observation/analysis. No producer or camera SDK imports.

Wire constants agree with src/transport/gvcp_wire.hpp (Aravis 0.8.36).
This is an observer, not another GVCP transaction engine or production transport.
"""
import collections
import socket
import struct
import time

PORT = 3956
COMMANDS = {0x0002: "Discovery", 0x0080: "ReadRegister", 0x0082: "WriteRegister",
            0x0084: "ReadMemory", 0x0086: "WriteMemory"}


def classify(data):
    result = {"classification": "Unqualified", "addresses": [], "control_privilege": False}
    if len(data) < 8:
        return dict(result, error="truncated GVCP header")
    key, flags, command, length, transaction = struct.unpack("!BBHHH", data[:8])
    result.update(command=command, transaction=transaction, flags=flags)
    if key != 0x42 or length != len(data) - 8 or transaction == 0 or flags & ~0x11:
        return dict(result, error="invalid command header/length/flags/id")
    body = data[8:]
    kind = COMMANDS.get(command)
    if kind is None:
        return dict(result, error="unknown GVCP command")
    if command == 2:
        if body:
            return dict(result, error="discovery payload")
    elif command in (0x80, 0x82):
        stride = 4 if command == 0x80 else 8
        if not body or len(body) % stride or len(body) > 512:
            return dict(result, error="register payload boundary")
        result["addresses"] = [struct.unpack_from("!I", body, i)[0]
                               for i in range(0, len(body), stride)]
        if any(address % 4 for address in result["addresses"]):
            return dict(result, error="unaligned register")
    else:
        if len(body) < 8:
            return dict(result, error="truncated memory payload")
        address = struct.unpack_from("!I", body)[0]
        result["addresses"] = [address]
        if command == 0x84:
            reserved, size = struct.unpack_from("!HH", body, 4)
            if len(body) != 8 or reserved:
                return dict(result, error="read memory payload boundary")
        else:
            size = len(body) - 4
        result["size"] = size
        if not 0 < size <= 512 or size % 4 or address % 4 or address + size > 2**32:
            return dict(result, error="memory interval")
    result["classification"] = kind
    if command in (0x82, 0x86):
        result["write"] = True
        result["control_privilege"] = (0xa00 in result["addresses"] if command == 0x82 else
                                       address <= 0xa00 < address + size)
        if result["control_privilege"]:
            result["classification"] = "ControlPrivilegeModification"
    return result


def decode_ip(data):
    """AF_PACKET/SOCK_DGRAM supplies the IP packet, without an Ethernet cast."""
    if len(data) < 20 or data[0] >> 4 != 4:
        raise ValueError("truncated/non-IPv4 packet")
    header = (data[0] & 15) * 4
    length = struct.unpack_from("!H", data, 2)[0]
    if header < 20 or length != len(data) or header + 8 > length:
        raise ValueError("IP length boundary")
    if struct.unpack_from("!H", data, 6)[0] & 0x3fff or data[9] != 17:
        raise ValueError("fragmented or non-UDP traffic")
    source, destination, size, _ = struct.unpack_from("!HHHH", data, header)
    if size != length - header or size < 8:
        raise ValueError("UDP length boundary")
    return {"source": [socket.inet_ntoa(data[12:16]), source],
            "destination": [socket.inet_ntoa(data[16:20]), destination],
            "payload": data[header + 8:].hex(), "direction": "command" if destination == PORT else
            "ack" if source == PORT else "unexpected"}


class Observer:
    """Independent kernel packet socket, not producer callbacks or simulator logs.

    Linux loopback exposes outgoing and incoming copies. Observe incoming ONLY;
    real retransmissions remain separate records, never content-deduplicated.
    """
    def __init__(self):
        self.socket = socket.socket(socket.AF_PACKET, socket.SOCK_DGRAM, socket.htons(0x0003))
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
        self.socket.setsockopt(socket.SOL_SOCKET, 64, 1)  # SO_TIMESTAMPNS_NEW, kernel realtime
        self.socket.bind(("lo", 0))
        self.socket.setblocking(False)
        self.records = []

    def drain(self, phase):
        for _ in range(4096):
            try:
                data, ancillary, flags, address = self.socket.recvmsg(65535, 256)
            except BlockingIOError:
                return
            if address[2] == 4:  # PACKET_OUTGOING; not a retransmission
                continue
            record = {"monotonic_ns": time.monotonic_ns(), "phase": phase,
                      "ip_hex": data.hex(), "packet_type": address[2]}
            try:
                stamps = [struct.unpack("=qq", value) for level, kind, value in ancillary
                          if level == socket.SOL_SOCKET and kind == 64 and len(value) == 16]
                if len(stamps) != 1:
                    raise ValueError("missing kernel packet timestamp")
                record["timestamp_ns"] = stamps[0][0] * 1000000000 + stamps[0][1]
                if flags & socket.MSG_TRUNC:
                    raise ValueError("capture truncated")
                record.update(decode_ip(data))
                if record["direction"] == "command":
                    record.update(classify(bytes.fromhex(record["payload"])))
                elif record["direction"] == "unexpected":
                    raise ValueError("non-GVCP UDP traffic")
                else:
                    ack = bytes.fromhex(record["payload"])
                    if len(ack) < 8:
                        raise ValueError("truncated ACK")
                    status, command, length, transaction = struct.unpack("!HHHH", ack[:8])
                    record.update(ack_command=command, transaction=transaction, status=status)
                    if (length != len(ack) - 8 or transaction == 0 or command not in
                            (3, 0x81, 0x83, 0x85, 0x87)):
                        raise ValueError("unqualified ACK header")
            except ValueError as error:
                record.update(classification="Unqualified", error=str(error))
            self.records.append(record)
            if len(self.records) > 10000:
                raise RuntimeError("bounded trace record limit exceeded")
        raise RuntimeError("packet flood exceeded bounded drain")

    def finish(self):
        # PACKET_STATISTICS resets on read. Read once, after the final drain.
        packets, drops = struct.unpack("II", self.socket.getsockopt(263, 6, 8))
        self.socket.close()
        return {"active": True, "complete": True, "packets": packets, "drops": drops,
                "records": len(self.records), "implementation": "Linux AF_PACKET SOCK_DGRAM ETH_P_ALL lo"}


def violations(records, closed_ns=None):
    reasons = []
    seen = set()
    for record in records:
        if record.get("phase") != "producer":
            continue
        if record.get("error") or record.get("classification") == "Unqualified":
            reasons.append("unqualified packet")
        if record.get("write"):
            reasons.append("control privilege write" if record["control_privilege"] else "configuration write")
        if record.get("direction") == "command":
            identity = (tuple(record["source"]), record.get("transaction"))
            if identity in seen:
                reasons.append("duplicate/retransmitted transaction")
            seen.add(identity)
            if closed_ns is not None and record["timestamp_ns"] >= closed_ns:
                reasons.append("command after successful device close")
    return sorted(set(reasons))


def analyze(records, observation, markers, native, synthetic=False):
    """Fail closed: neither exit(0), READONLY nor synthetic packets can qualify a producer."""
    commands = [r for r in records if r.get("phase") == "producer" and r.get("direction") == "command"]
    reasons = violations(records, markers.get("device_closed"))
    # A readable complete exchange requires a matching successful ACK for every
    # observed command, not just a request sent into a black hole. Keep unmatched
    # ACKs and failed ACKs explicit; never use producer success as a substitute.
    unmatched = []
    pending = {}
    for record in records:
        if record.get("phase") != "producer":
            continue
        if record.get("direction") == "command" and "command" in record:
            key = (tuple(record["source"]), record["transaction"], record["command"] + 1)
            pending[key] = record
        elif record.get("direction") == "ack":
            key = (tuple(record["destination"]), record.get("transaction"), record.get("ack_command"))
            request = pending.pop(key, None)
            if request is None or record.get("status") != 0:
                unmatched.append(key)
            elif "payload" in record:
                ack = bytes.fromhex(record["payload"])
                request_body = bytes.fromhex(request["payload"])[8:]
                command = request["command"]
                expected = {2: 256, 0x80: 8 + len(request_body), 0x82: 12,
                            0x84: 12 + request.get("size", 0), 0x86: 12}.get(command)
                if len(ack) != expected:
                    unmatched.append(key)
                elif command in (0x84, 0x86) and ack[8:12] != request_body[:4]:
                    unmatched.append(key)
    evidence_incomplete = bool(pending or unmatched)
    result = {"requested_access": "DEVICE_ACCESS_READONLY", "reasons": reasons,
              "command_counts": dict(collections.Counter(r.get("classification", "Unqualified") for r in commands)),
              "written_addresses": [a for r in commands if r.get("write") for a in r["addresses"]],
              "control_privilege_write": any(r.get("control_privilege") for r in commands),
              "markers": markers, "native": native, "synthetic": synthetic}
    result["unmatched_transactions"] = len(pending) + len(unmatched)
    if native.get("timed_out"):
        state = "PRODUCER_TIMEOUT"
    elif native.get("returncode") is not None and native["returncode"] < 0:
        state = "PRODUCER_CRASH"
    elif reasons:
        state = "READ_ONLY_WIRE_VIOLATION"
    elif (not observation.get("active") or not observation.get("complete") or native.get("error") or
          observation.get("drops", 1) != 0 or observation.get("records") != len(records) or
          not observation.get("calibrated") or not commands or evidence_incomplete):
        state = "WIRE_OBSERVATION_UNAVAILABLE"
    elif (native.get("returncode") != 0 or not native.get("simulator_shutdown") or
          not native.get("descendants_reaped") or
          not all(key in markers for key in ("open_readonly", "inspect", "device_closed", "native_closed", "quiet_complete")) or
          not markers["open_readonly"] < markers["inspect"] < markers["device_closed"] <=
          markers["native_closed"] < markers["quiet_complete"] or
          markers["quiet_complete"] - markers["native_closed"] < native.get("quiet_ns", 100000000)):
        state = "NATIVE_CLEANUP_UNRESOLVED"
    elif not any(r.get("command") == 2 for r in commands) or not any(r.get("command") in (0x80, 0x84) for r in commands):
        state = "WIRE_OBSERVATION_UNAVAILABLE"
    else:
        state = "HARNESS_SELF_TEST_PASS" if synthetic else "READ_ONLY_WIRE_PASS"
    result["result"] = state
    return result