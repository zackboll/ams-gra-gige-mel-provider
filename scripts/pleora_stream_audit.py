#!/usr/bin/env python3
"""Explicit isolated native audit; raw artifacts are private, not publication input.

Requires Linux unprivileged user/network namespaces, iproute2 and AF_PACKET.
No automatic CTest registration, physical interface, discovery or SDK provisioning.
Exit zero means API observation completed, NOT that traffic/reception gates passed.
Native refusal or sanitizer failure exits nonzero with its diagnostic preserved.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import selectors
import socket
import struct
import subprocess
import sys
import threading
import time

from provision_pleora import verify_sdk


class Capture:
    """Independent kernel packet observer, no filter, all namespace interfaces."""
    def __init__(self, directory):
        self.sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3))
        self.sock.settimeout(0.05)
        self.file = (directory / "all-interfaces.pcap").open("xb")
        self.file.write(struct.pack("<IHHIIII", 0xa1b2c3d4, 2, 4, 0, 0, 262144, 1))
        self.log = (directory / "packets.jsonl").open("x")
        self.stop = threading.Event()
        self.errors = []
        self.count = 0
        self.begin_ns = time.time_ns()
        self.thread = threading.Thread(target=self.run)
        self.thread.start()

    def run(self):
        try:
            while not self.stop.is_set():
                try:
                    data, address = self.sock.recvfrom(262144)
                except socket.timeout:
                    continue
                now = time.time_ns()
                self.file.write(struct.pack("<IIII", now // 10**9, now % 10**9 // 1000, len(data), len(data)))
                self.file.write(data)
                item = {"time_ns": now, "interface": address[0], "packet_type": address[2], "hex": data.hex()}
                if len(data) >= 34 and data[12:14] == b"\x08\x00":
                    offset = 14 + (data[14] & 15) * 4
                    item.update(source=socket.inet_ntoa(data[26:30]), destination=socket.inet_ntoa(data[30:34]),
                                protocol=data[23])
                    if data[23] == 17 and len(data) >= offset + 8:
                        source, dest, length, _ = struct.unpack_from("!HHHH", data, offset)
                        payload = data[offset + 8:offset + length]
                        item.update(source_port=source, destination_port=dest)
                        if (source == 3956 or dest == 3956) and len(payload) >= 8:
                            flags, command, size, request = struct.unpack_from("!HHHH", payload)
                            item.update(gvcp_command=f"0x{command:04x}", request_id=request,
                                        flags_status=f"0x{flags:04x}", declared_size=size, payload=payload[8:].hex())
                self.log.write(json.dumps(item) + "\n")
                self.count += 1
        except Exception as error:
            self.errors.append(str(error))

    def close(self):
        self.stop.set()
        self.thread.join(2)
        if self.thread.is_alive():
            self.errors.append("capture thread did not stop")
        # Linux SOL_PACKET / PACKET_STATISTICS: resets counters when read.
        packets, drops = struct.unpack("II", self.sock.getsockopt(263, 6, 8))
        self.sock.close()
        self.file.close()
        self.log.close()
        return {"records": self.count, "kernel_packets": packets, "kernel_drops": drops, "errors": self.errors,
                "capture_begin_ns": self.begin_ns, "capture_end_ns": time.time_ns()}


class Responder:
    """Minimal read-only bootstrap fixture, NOT a camera or 039 owner identity."""
    def __init__(self, directory):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.2", 3956))  # exclusive, no reuse
        self.sock.settimeout(0.05)
        self.file = (directory / "responder.jsonl").open("x")
        self.stop = threading.Event()
        self.errors = []
        self.thread = threading.Thread(target=self.run)
        self.thread.start()

    def run(self):
        try:
            self.serve()
        except Exception as error:
            self.errors.append(str(error))

    def serve(self):
        while not self.stop.is_set():
            try:
                data, source = self.sock.recvfrom(65535)
            except socket.timeout:
                continue
            record = {"time_ns": time.time_ns(), "source": source, "hex": data.hex()}
            if len(data) >= 8:
                _, command, size, request = struct.unpack_from("!HHHH", data)
                record.update(command=f"0x{command:04x}", request_id=request)
                if command == 0x2 and size == 0 and len(data) == 8:
                    # GVCP discovery bootstrap layout independently cross-checked
                    # with pinned Aravis 0.8.36 arvgvcpprivate.h. Synthetic identity,
                    # never a vendor identity or license/entitlement workaround.
                    bootstrap = bytearray(248)
                    for offset, value in {0: 0x00020000, 4: 0x80000000, 8: 0x0200,
                                          12: 0x00000040, 16: 4, 20: 4,
                                          36: 0x7f000002, 52: 0xff000000}.items():
                        struct.pack_into("!I", bootstrap, offset, value)
                    for offset, text in {72: b"Project-owned test fixture", 104: b"SyntheticAudit040",
                                         136: b"1", 216: b"AUDIT040", 232: b"LoopbackOnly"}.items():
                        bootstrap[offset:offset + len(text)] = text
                    reply = struct.pack("!HHHH", 0, 3, len(bootstrap), request) + bootstrap
                elif command == 0x80 and size == 4 and len(data) == 12:
                    address = struct.unpack_from("!I", data, 8)[0]
                    record["address"] = f"0x{address:08x}"
                    # Read-only fixture reports no optional capabilities or owner.
                    reply = struct.pack("!HHHHI", 0, 0x81, 4, request, 0)
                else:
                    # Unsupported command: never acknowledge a write as applied.
                    reply = struct.pack("!HHHH", 0x8001, command + 1, 0, request)
                self.sock.sendto(reply, source)
                record["reply"] = reply.hex()
            self.file.write(json.dumps(record) + "\n")

    def close(self):
        self.stop.set()
        self.thread.join(2)
        if self.thread.is_alive():
            self.errors.append("responder did not stop")
        self.sock.close()
        self.file.close()


def inventory(pid, stage, file):
    base = Path(f"/proc/{pid}")
    sockets = {}
    for descriptor in (base / "fd").iterdir():
        try:
            target = descriptor.readlink().as_posix()
        except FileNotFoundError:
            continue
        if target.startswith("socket:"):
            sockets[descriptor.name] = target
    file.write(json.dumps({"time_ns": time.time_ns(), "stage": stage, "pid": pid, "sockets": sockets,
                           "fdinfo": {fd: (base / "fdinfo" / fd).read_text() for fd in sockets},
                           "net": {name: (base / "net" / name).read_text()
                                   for name in ("udp", "udp6", "tcp", "tcp6", "raw", "packet", "netlink", "unix")}}) + "\n")
    file.flush()


def execute(args):
    # sysfs can remain mounted for the host namespace; query the current netns.
    interfaces = sorted(name for _, name in socket.if_nameindex())
    if interfaces != ["lo"]:
        raise RuntimeError("BLOCKED: namespace must contain only lo")
    subprocess.run(["ip", "link", "set", "lo", "up"], check=True)
    directory = args.output.resolve()
    directory.mkdir(mode=0o700)  # never overwrite prior evidence
    sdk = args.sdk.resolve()
    verify_sdk(sdk)
    environment = os.environ.copy()
    if any(environment.get(key) for key in ("LD_PRELOAD", "LD_AUDIT", "LD_DEBUG", "LD_DEBUG_OUTPUT")):
        raise RuntimeError("BLOCKED: loader interposition is incompatible with runtime identity audit")
    environment.update(LD_LIBRARY_PATH=f"{sdk}/lib:{sdk}/lib/genicam/bin/Linux64_x64",
                       GENICAM_ROOT_V3_4=f"{sdk}/lib/genicam", PUREGEV_ROOT=str(sdk),
                       GENICAM_CACHE_V3_4=str(directory / "cache"))
    network = {"interfaces": interfaces, "namespace": os.readlink("/proc/self/ns/net"),
               "addresses": subprocess.check_output(["ip", "-j", "address"], text=True),
               "routes": subprocess.check_output(["ip", "-j", "route", "show", "table", "all"], text=True)}
    (directory / "network.json").write_text(json.dumps(network, indent=2) + "\n")
    # Confirm reserved port free even in no-responder mode.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.2", 3956))
    capture = Capture(directory)
    responder = None
    process = None
    coverage = {}
    try:
        if args.responder:
            responder = Responder(directory)
        with (directory / "native.log").open("x") as log, (directory / "sockets.jsonl").open("x") as sockets:
            process = subprocess.Popen([str(args.binary.resolve()), args.mode], env=environment,
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, text=True, bufsize=1)
            coverage.update(pid=process.pid, binary=str(args.binary.resolve()),
                            binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),
                            mode=args.mode, responder=args.responder)
            selector = selectors.DefaultSelector()
            selector.register(process.stdout, selectors.EVENT_READ)
            deadline = time.monotonic() + 40
            pending = b""
            while True:
                if time.monotonic() > deadline:
                    raise RuntimeError("AUDIT_INCOMPLETE: native lifecycle exceeded 40 seconds")
                if not selector.select(0.1):
                    continue
                chunk = os.read(process.stdout.fileno(), 65536)
                if not chunk:
                    if pending:
                        log.write(pending.decode(errors="replace"))
                    break
                pending += chunk
                while b"\n" in pending:
                    raw, pending = pending.split(b"\n", 1)
                    line = raw.decode(errors="replace")
                    log.write(line + "\n")
                    log.flush()
                    if line.startswith("STAGE "):
                        stage = line.split(" ", 1)[1]
                        inventory(process.pid, stage, sockets)
                        maps = Path(f"/proc/{process.pid}/maps").read_text()
                        (directory / "loaded-maps.txt").write_text(maps)
                        runtime = {}
                        for mapped in maps.splitlines():
                            fields = mapped.split()
                            if len(fields) >= 6 and fields[-1].startswith(str(sdk)):
                                path = Path(fields[-1])
                                runtime[str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
                        (directory / "runtime-sha256.json").write_text(json.dumps(runtime, indent=2) + "\n")
                        time.sleep(0.1)
                        process.stdin.write("continue\n")
                        process.stdin.flush()
            coverage["native_returncode"] = process.wait(timeout=3)
            selector.close()
        time.sleep(0.3)  # packet observation after process/static destruction
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait()
            coverage["incomplete"] = "native process killed"
        if responder:
            responder.close()
            coverage["responder_errors"] = responder.errors
        coverage.update(capture.close())
        (directory / "coverage.json").write_text(json.dumps(coverage, indent=2) + "\n")
    if (coverage.get("incomplete") or coverage["errors"] or coverage["kernel_drops"]
            or coverage.get("responder_errors")):
        raise RuntimeError("AUDIT_INCOMPLETE: capture/lifecycle failure")
    print(json.dumps(coverage))
    if coverage["native_returncode"] != 0:
        raise RuntimeError(f"AUDIT_INCOMPLETE: native return code {coverage['native_returncode']}; see native.log")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--sdk", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--mode", choices=("default", "disabled"), default="default")
    parser.add_argument("--responder", action="store_true")
    parser.add_argument("--inside-namespace", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not args.inside_namespace:
        os.execvp("unshare", ["unshare", "--user", "--map-root-user", "--net", sys.executable,
                             str(Path(__file__).resolve()), *sys.argv[1:], "--inside-namespace"])
    execute(args)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)