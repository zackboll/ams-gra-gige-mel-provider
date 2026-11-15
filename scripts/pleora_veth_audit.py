#!/usr/bin/env python3
"""Explicit two-netns diagnostic; never a CTest or production camera tool.

All addresses are fixed private fixture addresses. No host-network fallback.
Evidence is private. A zero exit qualifies neither GVSP reception nor gates B/C.
Requires Linux user/net namespaces, iproute2, ping and AF_PACKET privileges in the
new user namespace. SDK and optional Aravis library must already exist externally.
"""
import argparse
from collections import Counter
import ctypes
import hashlib
import json
import os
from pathlib import Path
import selectors
import socket
import struct
import subprocess
import sys
import time

from pleora_stream_audit import Capture, Responder, inventory
from provision_pleora import verify_sdk

RX = "10.64.40.1"
CAM = "10.64.40.2"
RX_MAC = "02:00:00:40:01:01"
CAM_MAC = "02:00:00:40:01:02"


def command(*args):
    return subprocess.check_output(args, text=True, timeout=5)


def network():
    return {"netns": os.readlink("/proc/self/ns/net"),
            "userns": os.readlink("/proc/self/ns/user"), "pid": os.getpid(),
            "links": json.loads(command("ip", "-j", "-d", "link", "show")),
            "addresses": json.loads(command("ip", "-j", "address", "show")),
            "routes": json.loads(command("ip", "-j", "route", "show", "table", "all")),
            "routes6": json.loads(command("ip", "-j", "-6", "route", "show", "table", "all"))}


def stable_network(value):
    # Preserve raw inventories; compare configuration, not ticking DHCP/bridge timers.
    if isinstance(value, dict):
        return {key: stable_network(item) for key, item in value.items()
                if key not in ("valid_life_time", "preferred_life_time", "gc_timer", "hello_timer")}
    if isinstance(value, list):
        return [stable_network(item) for item in value]
    return value


def check_network(state, endpoint):
    """Fail before SDK access unless exactly our endpoint and connected route exist."""
    expected = "veth-rx" if endpoint == RX else "veth-cam"
    if sorted(link["ifname"] for link in state["links"]) != ["lo", expected]:
        raise RuntimeError("ENVIRONMENT_BLOCKED: unexpected interface")
    if any(item["ifname"] not in ("lo", expected) for item in state["addresses"]):
        raise RuntimeError("ENVIRONMENT_BLOCKED: unexpected addressed interface")
    link = next(item for item in state["links"] if item["ifname"] == expected)
    mac = RX_MAC if endpoint == RX else CAM_MAC
    if (link.get("linkinfo", {}).get("info_kind") != "veth" or link["address"] != mac
            or link["mtu"] != 1500 or "UP" not in link["flags"]):
        raise RuntimeError("ENVIRONMENT_BLOCKED: endpoint is not the configured veth")
    addresses = [(item["local"], item["prefixlen"]) for iface in state["addresses"]
                 if iface["ifname"] == expected for item in iface["addr_info"] if item["family"] == "inet"]
    if addresses != [(endpoint, 30)]:
        raise RuntimeError("ENVIRONMENT_BLOCKED: endpoint IPv4 mismatch")
    for route in state["routes"] + state["routes6"]:
        if (route.get("dst", "default") == "default" or "gateway" in route
                or route.get("dev") not in ("lo", expected)):
            raise RuntimeError("ENVIRONMENT_BLOCKED: unexpected route")
    if not any(r.get("dst") == "10.64.40.0/30" for r in state["routes"]):
        raise RuntimeError("ENVIRONMENT_BLOCKED: connected route missing")


def validate_bootstrap(data):
    """Independent decoder, not acceptance evidence from the SDK."""
    if len(data) != 0xf8:
        raise RuntimeError("bootstrap length mismatch")
    words = struct.unpack("!62I", data)
    mac = (words[2] & 0xffff).to_bytes(2, "big") + words[3].to_bytes(4, "big")
    if (words[0] != 0x00020000 or words[1] != 0x80000000 or mac.hex(":") != CAM_MAC
            or socket.inet_ntoa(data[0x24:0x28]) != CAM
            or data[0x34:0x38] != bytes.fromhex("fffffffc") or words[17] != 0):
        raise RuntimeError("bootstrap endpoint/version/mode mismatch")
    for offset, size in ((0x48, 32), (0x68, 32), (0x88, 32), (0xa8, 48), (0xd8, 16), (0xe8, 16)):
        value = data[offset:offset + size]
        if b"\0" not in value:
            raise RuntimeError("unterminated bootstrap identity")
        value.split(b"\0")[0].decode("ascii")


def configure(interface, address, mac):
    command("ip", "link", "set", "lo", "up")
    command("ip", "link", "set", interface, "address", mac, "mtu", "1500")
    command("ip", "address", "add", address + "/30", "dev", interface)
    command("ip", "link", "set", interface, "up")


def bootstrap(complete):
    data = bytearray(248)
    # Aravis 0.8.36 bootstrap offsets; no GenApi/ownership state is fabricated.
    values = {0: 0x00020000, 4: 0x80000000, 8: 0x0200, 12: 0x00400102,
              16: 1 if complete else 4, 20: 1 if complete else 4,
              36: 0x0a402802, 52: 0xfffffffc, 68: 0}
    for offset, value in values.items():
        struct.pack_into("!I", data, offset, value)
    for offset, text in {72: b"GIGE-MEL project fixture", 104: b"SyntheticVeth040R1",
                         136: b"1", 168: b"Isolated test only" if complete else b"",
                         216: b"AUDIT040R1", 232: b"VethOnly"}.items():
        data[offset:offset + len(text)] = text
    if not complete:
        # Keep the old fixture identity/protocol fields, changing only truthful
        # endpoint IP/mask/MAC. Never compare an ACK claiming a different device.
        for offset, size, text in ((72, 32, b"Project-owned test fixture"),
                                   (104, 32, b"SyntheticAudit040"),
                                   (216, 16, b"AUDIT040"), (232, 16, b"LoopbackOnly")):
            data[offset:offset + size] = text.ljust(size, b"\0")
    return bytes(data)


class VethResponder(Responder):
    def __init__(self, directory, complete):
        # Reuse ownership/retirement, not the old loopback fixture's wire behavior.
        import threading
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind((CAM, 3956))
        self.sock.settimeout(0.05)
        self.file = (directory / "responder.jsonl").open("x")
        self.stop = threading.Event()
        self.errors = []
        self.complete = complete
        validate_bootstrap(bootstrap(complete))
        self.thread = threading.Thread(target=self.run)
        self.thread.start()

    def serve(self):
        while not self.stop.is_set():
            try:
                data, source = self.sock.recvfrom(65535)
            except socket.timeout:
                continue
            record = {"source": source, "hex": data.hex(), "time_ns": time.time_ns()}
            if len(data) < 8:
                raise RuntimeError("unidentified responder input")
            flags, cmd, size, request = struct.unpack_from("!HHHH", data)
            record.update(command=f"0x{cmd:04x}", request_id=request)
            if source[0] != RX or flags >> 8 != 0x42 or size != len(data) - 8:
                raise RuntimeError("unexpected responder source/header/length")
            if cmd == 2 and size == 0:
                payload = bootstrap(self.complete)
                reply = struct.pack("!HHHH", 0, 3, len(payload), request) + payload
            elif self.complete and cmd == 0x80 and size == 4 and int.from_bytes(data[8:], "big") < 248:
                offset = int.from_bytes(data[8:], "big")
                if offset % 4:
                    raise RuntimeError("unaligned bootstrap read")
                reply = struct.pack("!HHHH", 0, 0x81, 4, request) + bootstrap(True)[offset:offset + 4]
            elif self.complete and cmd == 0x80 and size == 4 and int.from_bytes(data[8:], "big") in (0x600, 0x900, 0x904, 0x934, 0x93c, 0x940):
                # Actual model: one interface, no message/stream channels and no
                # optional capabilities. This fixture is not a GVSP transmitter.
                offset = int.from_bytes(data[8:], "big")
                reply = struct.pack("!HHHHI", 0, 0x81, 4, request, int(offset == 0x600))
            elif self.complete and cmd == 0x84 and size == 8:
                offset, length = struct.unpack_from("!II", data, 8)
                if offset % 4 or length % 4 or not 0 < length <= 248 or offset + length > 248:
                    raise RuntimeError("unsupported memory read range")
                reply = struct.pack("!HHHHI", 0, 0x85, length + 4, request, offset) + bootstrap(True)[offset:offset + length]
            else:
                # Unknown register reads must not pretend to expose meaningful state.
                reply = struct.pack("!HHHH", 0x8001, (cmd + 1) & 0xffff, 0, request)
                self.errors.append(f"unsupported command 0x{cmd:04x}: {data.hex()}")
            self.sock.sendto(reply, source)
            record["reply"] = reply.hex()
            self.file.write(json.dumps(record) + "\n")
            self.file.flush()


def software_camera(library):
    """Use the real pinned library, confined to camera netns, with truthful identity."""
    lib = ctypes.CDLL(str(library.resolve()))
    lib.arv_get_major_version.restype = ctypes.c_uint
    lib.arv_get_minor_version.restype = ctypes.c_uint
    lib.arv_get_micro_version.restype = ctypes.c_uint
    if (lib.arv_get_major_version(), lib.arv_get_minor_version(), lib.arv_get_micro_version()) != (0, 8, 36):
        raise RuntimeError("AUDIT_INCOMPLETE: requires Aravis 0.8.36")
    lib.arv_gv_fake_camera_new.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
    lib.arv_gv_fake_camera_new.restype = ctypes.c_void_p
    lib.arv_gv_fake_camera_is_running.argtypes = [ctypes.c_void_p]
    lib.arv_gv_fake_camera_get_fake_camera.argtypes = [ctypes.c_void_p]
    lib.arv_gv_fake_camera_get_fake_camera.restype = ctypes.c_void_p
    lib.arv_fake_camera_write_memory.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                                ctypes.c_uint32, ctypes.c_void_p]
    camera = lib.arv_gv_fake_camera_new(b"veth-cam", b"AUDIT040R1")
    if not camera or not lib.arv_gv_fake_camera_is_running(camera):
        raise RuntimeError("AUDIT_INCOMPLETE: ArvGvFakeCamera did not start")
    fake = lib.arv_gv_fake_camera_get_fake_camera(camera)
    data = bootstrap(True)
    validate_bootstrap(data)
    # Set actual modeled memory before any receiver is launched, not ACK rewriting.
    if not lib.arv_fake_camera_write_memory(fake, 0, len(data), ctypes.c_char_p(data)):
        raise RuntimeError("AUDIT_INCOMPLETE: software camera bootstrap setup failed")
    return lib, camera


def send_frames(port, directory):
    """Deterministic test-only GVSP, separately attributed, not camera control."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender, (directory / "transmitter.jsonl").open("x") as log:
        sender.bind((CAM, 0))
        for frame in range(401, 409):
            header = lambda kind, packet: struct.pack("!HHI", 0, frame, kind << 24 | packet)
            # Layout independently checked against pinned Aravis GVSP structures.
            leader = struct.pack("!HHQIIIIIHH", 0, 1, frame * 1000000, 0x01080001,
                                 32, 16, 0, 0, 0, 0)
            pixels = bytes((frame + index) % 256 for index in range(512))
            packets = [header(1, 0) + leader, header(3, 1) + pixels,
                       header(2, 2) + struct.pack("!II", 1, 16)]
            for packet in packets:
                sender.sendto(packet, (RX, port))
                log.write(json.dumps({"source": sender.getsockname(), "destination": [RX, port],
                                      "frame": frame, "hex": packet.hex(), "time_ns": time.time_ns()}) + "\n")
            time.sleep(0.02)


def emit(value):
    print(json.dumps(value), flush=True)


def read_message(process, timeout=10):
    # Only line-oriented control messages, with one outstanding request at a time.
    with selectors.DefaultSelector() as select:
        select.register(process.stdout, selectors.EVENT_READ)
        if not select.select(timeout):
            raise RuntimeError("AUDIT_INCOMPLETE: camera control timeout")
        line = process.stdout.readline()
    if not line:
        raise RuntimeError("AUDIT_INCOMPLETE: camera terminated unexpectedly")
    message = json.loads(line)
    if "error" in message:
        raise RuntimeError(message["error"])
    return message


def send_message(process, value):
    process.stdin.write(json.dumps(value) + "\n")
    process.stdin.flush()


def camera_worker(args):
    if socket.if_nameindex() != [(1, "lo")]:
        raise RuntimeError("ENVIRONMENT_BLOCKED: camera namespace not fresh")
    emit(network())
    if json.loads(sys.stdin.readline()) != "configure":
        raise RuntimeError("camera configuration barrier missing")
    configure("veth-cam", CAM, CAM_MAC)
    state = network()
    check_network(state, CAM)
    args.output.mkdir(mode=0o700)
    capture = Capture(args.output)
    responder = None
    software = None
    try:
        if args.scenario in ("minimal", "validated"):
            responder = VethResponder(args.output, args.scenario == "validated")
        elif args.scenario == "aravis":
            software = software_camera(args.aravis_library)
            (args.output / "software-camera.json").write_text(json.dumps({
                "library": str(args.aravis_library.resolve()),
                "sha256": hashlib.sha256(args.aravis_library.read_bytes()).hexdigest(),
                "version": "0.8.36", "interface": "veth-cam", "bootstrap": bootstrap(True).hex()}, indent=2))
        emit(state)
        if json.loads(sys.stdin.readline()) != "ping":
            raise RuntimeError("camera ping barrier missing")
        emit({"ping": command("ping", "-n", "-c", "2", "-W", "1", "-I", "veth-cam", RX)})
        request = json.loads(sys.stdin.readline())
        if isinstance(request, dict) and "send_frames" in request:
            if args.scenario != "aravis":
                raise RuntimeError("synthetic frames require software-device scenario")
            send_frames(request["send_frames"], args.output)
            emit({"frames_sent": 8})
            request = json.loads(sys.stdin.readline())
        if request != "stop":
            raise RuntimeError("camera stop barrier missing")
    finally:
        if responder:
            responder.close()
        if software:
            lib, camera = software
            lib.g_object_unref.argtypes = [ctypes.c_void_p]
            lib.g_object_unref(camera)
        time.sleep(0.3)
        coverage = capture.close()
        coverage["responder_errors"] = responder.errors if responder else []
        (args.output / "coverage.json").write_text(json.dumps(coverage, indent=2) + "\n")
    emit(coverage)


def run_native(args, directory, camera):
    sdk = args.sdk.resolve()
    verify_sdk(sdk)
    environment = os.environ.copy()
    if any(environment.get(key) for key in ("LD_PRELOAD", "LD_AUDIT", "LD_DEBUG", "LD_DEBUG_OUTPUT")):
        raise RuntimeError("AUDIT_INCOMPLETE: loader interposition refused")
    environment.update(LD_LIBRARY_PATH=f"{sdk}/lib:{sdk}/lib/genicam/bin/Linux64_x64",
                       GENICAM_ROOT_V3_4=f"{sdk}/lib/genicam", PUREGEV_ROOT=str(sdk),
                       GENICAM_CACHE_V3_4=str(directory / "cache"))
    argv = [str(args.binary.resolve()), args.mode, "veth-" + args.variant, "capacity16"]
    if args.strace:
        argv = [str(args.strace.resolve()), "--kill-on-exit", "-f", "-ttt", "-s", "512",
                "-e", "trace=network,ioctl,openat,close", "-o", str(directory / "syscalls.log"), *argv]
    argv = ["setpriv", "--pdeathsig", "KILL", *argv]
    process = None
    stages = []
    local_port = None
    buffers = 0
    try:
        with (directory / "native.log").open("x") as log, (directory / "sockets.jsonl").open("x") as sockets:
            process = subprocess.Popen(argv, env=environment, stdin=subprocess.PIPE,
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            (directory / "native-identity.json").write_text(json.dumps({
                "pid": process.pid, "stat": Path(f"/proc/{process.pid}/stat").read_text(),
                "argv": argv, "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest()}, indent=2))
            with selectors.DefaultSelector() as select:
                select.register(process.stdout, selectors.EVENT_READ)
                deadline = time.monotonic() + 40
                pending = b""
                while True:
                    if time.monotonic() >= deadline:
                        raise RuntimeError("AUDIT_INCOMPLETE: native deadline")
                    if not select.select(0.1):
                        continue
                    chunk = os.read(process.stdout.fileno(), 65536)
                    if not chunk:
                        log.write(pending.decode(errors="replace"))
                        break
                    pending += chunk
                    while b"\n" in pending:
                        raw, pending = pending.split(b"\n", 1)
                        line = raw.decode(errors="replace")
                        log.write(line + "\n")
                        log.flush()
                        if line.startswith("LOCAL "):
                            local_port = int(line.rsplit(":", 1)[1])
                        if line.startswith("BUFFER "):
                            buffers += 1
                        if line.startswith("STAGE "):
                            stage = line[6:]
                            stages.append(stage)
                            pid = process.pid
                            if args.strace:
                                children = Path(f"/proc/{pid}/task/{pid}/children").read_text().split()
                                if len(children) != 1:
                                    raise RuntimeError("AUDIT_INCOMPLETE: cannot identify traced native child")
                                pid = int(children[0])
                            inventory(pid, stage, sockets)
                            (directory / "native-process.json").write_text(json.dumps({
                                "pid": pid, "stat": Path(f"/proc/{pid}/stat").read_text(),
                                "exe": os.readlink(f"/proc/{pid}/exe"),
                                "netns": os.readlink(f"/proc/{pid}/ns/net")}, indent=2))
                            maps = Path(f"/proc/{pid}/maps").read_text()
                            (directory / "loaded-maps.txt").write_text(maps)
                            paths = {Path(row.split()[-1]) for row in maps.splitlines()
                                     if len(row.split()) >= 6 and row.split()[-1].startswith(str(sdk) + "/")}
                            (directory / "runtime-sha256.json").write_text(json.dumps(
                                {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in paths}, indent=2))
                            if stage == "pipeline_started" and args.frames:
                                if not local_port:
                                    raise RuntimeError("native receiver local port missing")
                                send_message(camera, {"send_frames": local_port})
                                read_message(camera)
                            process.stdin.write(b"continue\n")
                            process.stdin.flush()
            rc = process.wait(timeout=3)
            return {"native_returncode": rc, "stages": stages,
                    "buffers_observed": buffers,
                    "completed": "before_process_exit" in stages}
    finally:
        if process is not None:
            # Popen retains ownership until wait: an unreaped child PID cannot be reused.
            if process.poll() is None:
                process.kill()
                process.wait(timeout=3)
            process.stdin.close()
            process.stdout.close()


def healthy(coverage):
    return (not coverage["errors"] and not coverage["kernel_drops"]
            and not coverage.get("responder_errors")
            and coverage["records"] == coverage["kernel_packets"])


def traffic_summary(directory):
    """Count originating outgoing observations, never sum both veth observations."""
    commands = Counter()
    classes = Counter()
    unknown = []
    observations = {}
    for endpoint in (directory, directory / "camera"):
        rows = [json.loads(line) for line in (endpoint / "packets.jsonl").read_text().splitlines()]
        observations[endpoint] = rows
        for row in rows:
            if row["packet_type"] != 4:
                continue
            origin = "receiver" if endpoint == directory else "camera"
            if row.get("ether_type") == "0x0806" and "arp_operation" in row:
                kind = "ARP"
            elif row.get("ether_type") == "0x86dd" and row.get("icmpv6_type") in (130, 131, 132, 133, 134, 135, 136, 143):
                kind = "IPv6 control"  # ICMPv6 or hop-by-hop MLD; raw bytes retained
            elif row.get("protocol") == 1:
                kind = "ICMP"
            elif row.get("protocol") == 2 and row.get("destination") == "224.0.0.22":
                kind = "IGMPv3 membership"
            elif row.get("gvcp_command"):
                kind = "GVCP"
                commands[(origin, row["gvcp_command"], row["request_id"], row["payload"])] += 1
                permitted = ("0x0002", "0x0080", "0x0084") if origin == "receiver" else ("0x0003", "0x0081", "0x0085")
                if row["gvcp_command"] not in permitted:
                    unknown.append(row)
            elif row.get("destination_port") == 514 and row.get("destination") == "127.0.0.1":
                kind = "SDK syslog"
            elif (row.get("source") == RX and row.get("destination") == RX
                  and row.get("payload_length") == 1 and row["hex"].endswith("00")):
                # Corroborated by syscall trace: SDK wakes its own receive socket
                # during Stop. This is not GVSP and is not a network command.
                kind = "SDK local receive wakeup"
            elif origin == "camera" and row.get("source") == CAM and row.get("destination") == RX and (directory / "camera/transmitter.jsonl").exists():
                wire = bytes.fromhex(row["hex"])[42:]
                sent = [json.loads(line) for line in (directory / "camera/transmitter.jsonl").read_text().splitlines()]
                if not any(item["hex"] == wire.hex() and item["destination"][1] == row.get("destination_port") for item in sent):
                    raise RuntimeError("unattributed synthetic GVSP packet")
                kind = "synthetic GVSP"
            else:
                kind = "unidentified"
                unknown.append(row)
            classes[(origin, kind)] += 1
    receiver_tx = Counter(row["hex"] for row in observations[directory]
                          if row["packet_type"] == 4 and row.get("destination_port") == 3956)
    camera_rx = Counter(row["hex"] for row in observations[directory / "camera"]
                        if row["packet_type"] != 4 and row.get("destination_port") == 3956)
    camera_tx = Counter(row["hex"] for row in observations[directory / "camera"]
                        if row["packet_type"] == 4 and row.get("source_port") == 3956)
    receiver_rx = Counter(row["hex"] for row in observations[directory]
                          if row["packet_type"] != 4 and row.get("source_port") == 3956)
    records = directory / "camera/responder.jsonl"
    responder = [json.loads(line) for line in records.read_text().splitlines()] if records.exists() else []
    return {"endpoint_gvcp_match": receiver_tx == camera_rx and camera_tx == receiver_rx,
            "transmitted_commands": sum(receiver_tx.values()),
            "commands_received_camera_capture": sum(camera_rx.values()),
            "commands_received_responder": len(responder) if records.exists() else None,
            "responder_acks": sum("reply" in row for row in responder) if records.exists() else None,
            "outgoing_classes": [{"origin": origin, "class": kind, "count": count}
                                  for (origin, kind), count in sorted(classes.items())],
            "gvcp": [{"origin": origin, "command": cmd, "request_id": request,
                      "payload": payload, "count": count}
                     for (origin, cmd, request, payload), count in sorted(commands.items())],
            "unidentified_outgoing": unknown}


def coordinator(args):
    if socket.if_nameindex() != [(1, "lo")]:
        raise RuntimeError("ENVIRONMENT_BLOCKED: coordinator namespace not fresh")
    directory = args.output
    child = None
    capture = None
    report = {"scenario": args.scenario, "variant": args.variant, "mode": args.mode}
    try:
        child = subprocess.Popen(["setpriv", "--pdeathsig", "KILL", "unshare", "--net", sys.executable, str(Path(__file__).resolve()),
                                  *sys.argv[1:], "--camera"], stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, text=True, bufsize=1)
        initial = read_message(child)
        if initial["netns"] == os.readlink("/proc/self/ns/net") or initial["userns"] != os.readlink("/proc/self/ns/user"):
            raise RuntimeError("ENVIRONMENT_BLOCKED: namespace identity mismatch")
        report["camera_initial"] = initial
        command("ip", "link", "add", "veth-rx", "type", "veth", "peer", "name", "veth-cam")
        command("ip", "link", "set", "veth-cam", "netns", str(initial["pid"]))
        configure("veth-rx", RX, RX_MAC)
        send_message(child, "configure")
        report["camera"] = read_message(child)
        report["receiver"] = network()
        check_network(report["camera"], CAM)
        check_network(report["receiver"], RX)
        capture = Capture(directory)
        report["receiver_ping"] = command("ping", "-n", "-c", "2", "-W", "1", "-I", "veth-rx", CAM)
        send_message(child, "ping")
        report["camera_ping"] = read_message(child)
        report.update(run_native(args, directory, child))
        time.sleep(0.3)
        send_message(child, "stop")
        report["camera_capture"] = read_message(child)
        report["camera_returncode"] = child.wait(timeout=5)
    finally:
        if child is not None:
            if child.poll() is None:
                child.kill()
                child.wait(timeout=3)
                report["forced_camera_cleanup"] = True
            child.stdin.close()
            child.stdout.close()
        time.sleep(0.3)
        if capture:
            report["receiver_capture"] = capture.close()
        report["packet_sockets_after_close"] = Path("/proc/self/net/packet").read_text()
        # The last camera netns reference is gone: the peer must disappear too.
        report["remaining_interfaces"] = socket.if_nameindex()
        report["cleanup_ok"] = report["remaining_interfaces"] == [(1, "lo")]
        if (directory / "native.log").exists():
            text = (directory / "native.log").read_text()
            report["sanitizer_diagnostics"] = any(term in text for term in (
                "ThreadSanitizer:", "AddressSanitizer:", "LeakSanitizer:", "runtime error:"))
        if (directory / "packets.jsonl").exists() and (directory / "camera/packets.jsonl").exists():
            report["traffic"] = traffic_summary(directory)
        (directory / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    if (not report["cleanup_ok"] or report.get("forced_camera_cleanup")
            or report.get("sanitizer_diagnostics")
            or (args.frames and not report.get("buffers_observed"))
            or report["traffic"]["unidentified_outgoing"]
            or not report["traffic"]["endpoint_gvcp_match"]
            or len(report["packet_sockets_after_close"].splitlines()) != 1
            or report["camera_returncode"] or not report["completed"]
            or not healthy(report["camera_capture"]) or not healthy(report["receiver_capture"])):
        raise RuntimeError("AUDIT_INCOMPLETE: capture, process or cleanup gate failed")
    if report["native_returncode"]:
        raise RuntimeError(f"AUDIT_INCOMPLETE / NATIVE_RECEIVE_PENDING: native exit {report['native_returncode']}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--scenario", choices=("absent", "minimal", "validated", "aravis"), required=True)
    parser.add_argument("--variant", choices=("fixed", "auto", "simple", "multicast"), default="fixed")
    parser.add_argument("--mode", choices=("default", "disabled"), default="default")
    parser.add_argument("--aravis-library", type=Path)
    parser.add_argument("--strace", type=Path, help="optional external observational strace executable")
    parser.add_argument("--frames", action="store_true", help="explicit deterministic synthetic GVSP (aravis scenario only)")
    parser.add_argument("--coordinator", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--camera", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    args.output = args.output.resolve()
    if args.scenario == "aravis" and args.aravis_library is None:
        parser.error("aravis scenario requires --aravis-library")
    if args.frames and (args.scenario != "aravis" or args.variant == "multicast"):
        parser.error("frames require unicast aravis scenario")
    if args.camera:
        args.output = args.output / "camera"
        camera_worker(args)
    elif args.coordinator:
        coordinator(args)
    else:
        if args.output.is_relative_to(Path(__file__).resolve().parents[1]):
            raise RuntimeError("private evidence must be outside the repository")
        args.output.mkdir(mode=0o700)
        before = network()
        (args.output / "host-before.json").write_text(json.dumps(before, indent=2))
        with (args.output / "coordinator.log").open("x") as log:
            process = subprocess.Popen(["setpriv", "--pdeathsig", "KILL", "unshare", "--user", "--map-root-user", "--net",
                                        sys.executable, str(Path(__file__).resolve()),
                                        *sys.argv[1:], "--coordinator"], stdout=log, stderr=subprocess.STDOUT)
            try:
                rc = process.wait(timeout=70)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)
                raise RuntimeError("AUDIT_INCOMPLETE: coordinator cleanup timeout")
        after = network()
        (args.output / "host-after.json").write_text(json.dumps(after, indent=2))
        if stable_network(before) != stable_network(after):
            raise RuntimeError("AUDIT_INCOMPLETE: host network inventory changed")
        report_path = args.output / "report.json"
        if report_path.exists():
            report = json.loads(report_path.read_text())
            pids = [process.pid, report.get("camera_initial", {}).get("pid")]
            retired = all(pid is not None and not Path(f"/proc/{pid}").exists() for pid in pids)
            namespace_list = command("lsns", "--type", "net", "--noheadings", "--output", "NS,PID")
            nsids = [state["netns"].split("[")[1].rstrip("]")
                     for state in (report.get("camera_initial", {}), report.get("receiver", {})) if "netns" in state]
            listed_ids = {line.split()[0] for line in namespace_list.splitlines()}
            namespaces_retired = len(nsids) == 2 and not listed_ids.intersection(nsids)
            # PIDs are observation only here, never signal targets after reaping.
            (args.output / "outer-cleanup.json").write_text(json.dumps({
                "owned_children_retired": retired, "pids": pids,
                "namespace_ids": nsids, "namespace_inventory": namespace_list,
                "namespaces_absent_from_lsns": namespaces_retired,
                "host_configuration_unchanged": True, "coordinator_returncode": rc}, indent=2))
            if not retired or not namespaces_retired:
                raise RuntimeError("AUDIT_INCOMPLETE: child retirement unproven")
        if rc:
            raise RuntimeError(f"AUDIT_INCOMPLETE or ENVIRONMENT_BLOCKED: see {args.output}/coordinator.log")
        print("API lifecycle observed; native reception and traffic gates require separate evidence review")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        if "--camera" in sys.argv:
            emit({"error": str(error)})
        else:
            print(str(error), file=sys.stderr)
        sys.exit(1)