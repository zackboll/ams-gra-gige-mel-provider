"""Internal helpers for gentl_qualification.py --wire. Never imported by production.

All camera/producer execution is inside the single supervisor's PID/network
namespace. Namespace PID 1 owns and reaps even orphaned vendor subprocesses.
"""
import ctypes
import json
import os
from pathlib import Path
import resource
import signal
import socket
import struct
import subprocess
import sys
import time

from gentl_wire import Observer, analyze, violations


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def network_snapshot():
    def ip(*args):
        return json.loads(subprocess.check_output(["ip", "-j", *args], timeout=2))
    return {"namespace": os.readlink("/proc/self/ns/net"),
            "user_namespace": os.readlink("/proc/self/ns/user"),
            "links": ip("link"), "addresses": ip("address"),
            "routes": ip("route", "show", "table", "all"),
            "ipv6_routes": ip("-6", "route", "show", "table", "all")}


def require_isolation(snapshot, host_namespace, configured=False):
    if snapshot["namespace"] == host_namespace:
        raise ValueError("host network namespace forbidden")
    if [link["ifname"] for link in snapshot["links"]] != ["lo"]:
        raise ValueError("physical NIC/veth/unqualified interface forbidden; only fresh lo allowed")
    if any(route.get("dev") != "lo" or route.get("gateway") or
           route.get("dst") == "default" for route in snapshot["routes"] + snapshot["ipv6_routes"]):
        raise ValueError("external or unqualified route")
    for interface in snapshot["addresses"]:
        if interface["ifname"] != "lo":
            raise ValueError("non-loopback address")
        for address in interface.get("addr_info", []):
            if address["local"] not in ("127.0.0.1", "::1"):
                raise ValueError("unqualified loopback address")
    if configured:
        if not all("UP" in link["flags"] for link in snapshot["links"]):
            raise ValueError("loopback not active")
    elif any("UP" in link["flags"] for link in snapshot["links"]) or snapshot["routes"]:
        raise ValueError("not a fresh network namespace")


def camera(config):
    """Pinned public Aravis simulator API via ctypes, in a dedicated native process.

    No provider/adapter linkage, no new responder, default embedded controlled XML.
    The observer records commands even when upstream rejects/ignores them.
    """
    output = Path(config["output"])
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    resource.setrlimit(resource.RLIMIT_FSIZE, (64 * 1024 * 1024, 64 * 1024 * 1024))
    library = ctypes.CDLL(config["aravis"])
    for name in ("arv_get_major_version", "arv_get_minor_version", "arv_get_micro_version"):
        getattr(library, name).restype = ctypes.c_uint
    version = ".".join(str(getattr(library, "arv_get_" + item + "_version")())
                       for item in ("major", "minor", "micro"))
    if version != "0.8.36":
        raise ValueError("requires pinned Aravis 0.8.36")
    library.arv_gv_fake_camera_new.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
    library.arv_gv_fake_camera_new.restype = ctypes.c_void_p
    library.arv_gv_fake_camera_is_running.argtypes = [ctypes.c_void_p]
    library.arv_gv_fake_camera_get_fake_camera.argtypes = [ctypes.c_void_p]
    library.arv_gv_fake_camera_get_fake_camera.restype = ctypes.c_void_p
    library.arv_fake_camera_get_genicam_xml.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t)]
    library.arv_fake_camera_get_genicam_xml.restype = ctypes.c_void_p
    library.arv_fake_camera_read_register.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                                     ctypes.POINTER(ctypes.c_uint32)]
    library.g_object_unref.argtypes = [ctypes.c_void_p]
    simulator = library.arv_gv_fake_camera_new(b"127.0.0.1", b"Wire003A")
    if not simulator or not library.arv_gv_fake_camera_is_running(simulator):
        raise RuntimeError("simulator startup failed")
    fake = library.arv_gv_fake_camera_get_fake_camera(simulator)
    size = ctypes.c_size_t()
    xml = library.arv_fake_camera_get_genicam_xml(fake, ctypes.byref(size))
    (output / "camera.xml").write_bytes(ctypes.string_at(xml, size.value))
    def state():
        result = {}
        for address in (0xa00, 0x1f0, 0x124):
            value = ctypes.c_uint32()
            if not library.arv_fake_camera_read_register(fake, address, ctypes.byref(value)):
                raise RuntimeError("simulator register read failed")
            result[hex(address)] = value.value
        return result
    write_json(output / "camera-ready.json", {"implementation": "ArvGvFakeCamera", "version": version,
                                             "initial_registers": state(), "xml_bytes": size.value})
    deadline = time.monotonic() + config["timeout"] + 15
    while not (output / "camera-stop").exists():
        if time.monotonic() > deadline:
            raise TimeoutError("simulator lifetime expired")
        time.sleep(0.01)
    final = state()
    library.g_object_unref(simulator)  # native finalizer joins thread and closes sockets
    library.arv_shutdown()
    write_json(output / "camera-closed.json", {"native_shutdown": "g_object_unref; arv_shutdown returned",
                                               "final_registers": final})
    return 0


def synthetic(config):
    """SDK-free traffic fixture, NEVER a producer qualification."""
    def marker(name):
        print("WIRE", time.time_ns(), name, flush=True)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))
    sock.settimeout(1)
    marker("load")
    marker("discovery")
    def send(command, payload, transaction):
        sock.sendto(struct.pack("!BBHHH", 0x42, 1, command, len(payload), transaction) + payload,
                    ("127.0.0.1", 3956))
        sock.recvfrom(65535)
    send(2, b"", 1)
    marker("open_readonly")
    marker("inspect")
    send(0x80, struct.pack("!I", 0x1f0), 2)
    send(0x84, struct.pack("!IHH", 0x200, 0, 16), 3)
    scenario = config["self_test"]
    if scenario in ("control-write", "register-write", "memory-write"):
        command = 0x86 if scenario == "memory-write" else 0x82
        address = 0xa00 if scenario == "control-write" else 0x1f0
        send(command, struct.pack("!II", address, 2), 4)
    elif scenario == "unknown":
        sock.sendto(struct.pack("!BBHHH", 0x42, 1, 0x42, 0, 4), ("127.0.0.1", 3956))
    elif scenario == "duplicate":
        send(0x80, struct.pack("!I", 0x1f0), 2)
    elif scenario == "crash":
        os.kill(os.getpid(), signal.SIGSEGV)
    elif scenario == "timeout":
        time.sleep(60)
    marker("device_closed")
    marker("native_closed")
    if scenario == "post-close":
        send(0x80, struct.pack("!I", 0xa00), 5)
    time.sleep(config["quiet"])
    marker("quiet_complete")
    sock.close()
    return 0


def markers_from(path):
    result = {}
    for line in path.read_text(errors="replace").splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[0] == "WIRE":
            if fields[2] in result:
                raise ValueError("duplicate lifecycle marker")
            result[fields[2]] = int(fields[1])
    return result


def worker(config, script):
    output = Path(config["output"])
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    resource.setrlimit(resource.RLIMIT_FSIZE, (64 * 1024 * 1024, 64 * 1024 * 1024))
    if os.getpid() != 1:
        raise ValueError("wire worker requires fresh PID namespace init")
    before = network_snapshot()
    require_isolation(before, config["host_namespace"])
    if before["user_namespace"] == config["host_user_namespace"]:
        raise ValueError("fresh user namespace required")
    # The only network mutation: lo inside this already verified fresh namespace.
    subprocess.run(["ip", "link", "set", "dev", "lo", "up"], check=True, timeout=2)
    after = network_snapshot()
    require_isolation(after, config["host_namespace"], configured=True)
    write_json(output / "network.json", {"before": before, "configured": after})
    observer = None
    children = []
    native = {"returncode": None, "timed_out": False, "simulator_shutdown": False,
              "descendants_reaped": False, "quiet_ns": int(config["quiet"] * 1e9)}
    observation = {}
    markers = {}
    records = []
    logs = []
    def launch(command, name):
        stdout = (output / (name + ".stdout.log")).open("xb")
        stderr = (output / (name + ".stderr.log")).open("xb")
        logs.extend([stdout, stderr])
        child = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
                                 cwd=output)
        children.append(child)
        return child
    camera_process = None
    producer = None
    try:
        camera_process = launch([sys.executable, str(script), "--wire-camera", str(output / "config.json")], "camera")
        deadline = time.monotonic() + 5
        while not (output / "camera-ready.json").exists():
            if camera_process.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError("camera failed/timed out before ready")
            time.sleep(0.01)
        observer = Observer()
        # Active readiness calibration: real discovery AND ACK independently seen
        # before a producer can start. Discard neither: retain as calibration phase.
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as calibration:
            calibration.settimeout(1)
            calibration.sendto(bytes.fromhex("420100020000fffe"), ("127.0.0.1", 3956))
            calibration.recvfrom(65535)
        observer.drain("calibration")
        observation["calibrated"] = (sum(r.get("command") == 2 for r in observer.records) == 1 and
                                       sum(r.get("direction") == "ack" for r in observer.records) == 1 and
                                       not any(r.get("error") for r in observer.records))
        if not observation["calibrated"]:
            raise RuntimeError("independent packet calibration failed")
        command = ([sys.executable, str(script), "--wire-synthetic", str(output / "config.json")]
                   if config["self_test"] else
                   [config["probe"], config["producer"], "wire-readonly", config["interface"],
                    str(int(config["quiet"] * 1000))])
        write_json(output / "producer-command.json", command)
        native["started_ns"] = time.time_ns()
        producer = launch(command, "producer")
        deadline = time.monotonic() + config["timeout"]
        while producer.poll() is None:
            observer.drain("producer")
            markers = markers_from(output / "producer.stdout.log")
            if violations(observer.records, markers.get("device_closed")):
                native["stopped_on_violation"] = True
                producer.kill()
                break
            if camera_process.poll() is not None:
                raise RuntimeError("simulator died during producer sequence")
            if time.monotonic() >= deadline:
                native["timed_out"] = True
                producer.kill()
                break
            time.sleep(0.005)
        native["returncode"] = producer.wait(timeout=2)
        # Retain original signal status, but a supervisor violation kill is not
        # misreported as a spontaneous producer crash.
        if native.get("stopped_on_violation"):
            native["supervisor_kill_status"] = native["returncode"]
            native["returncode"] = 1
        native["exited_ns"] = time.time_ns()
        observer.drain("producer")
        markers = markers_from(output / "producer.stdout.log")
        # Additional bounded quiet observation after process exit: detects helpers
        # surviving a successful producer close, before terminating/reaping them.
        deadline = time.monotonic() + (config["quiet"] if native["returncode"] == 0 else 0)
        while time.monotonic() < deadline:
            observer.drain("producer")
            if violations(observer.records, markers.get("device_closed")):
                break
            time.sleep(0.005)
        (output / "camera-stop").touch(exist_ok=False)
        native["camera_returncode"] = camera_process.wait(timeout=5)
        native["simulator_shutdown"] = (native["camera_returncode"] == 0 and
                                         (output / "camera-closed.json").is_file())
        observer.drain("producer")
        # Prove the camera released its native GVCP listener.
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as check:
            check.bind(("127.0.0.1", 3956))
        try:
            orphan, status = os.waitpid(-1, os.WNOHANG)
        except ChildProcessError:
            native["descendants_reaped"] = True
        else:
            native["unexpected_descendant"] = {"pid": orphan, "status": status}
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        native["error"] = str(error)
    finally:
        # PID namespace init owns every remaining process. No process can escape
        # into the host PID namespace. Kill all, then reap all adopted descendants.
        try:
            os.kill(-1, signal.SIGKILL)
        except ProcessLookupError:
            pass
        for child in children:
            child.wait(timeout=2)
        while True:
            try:
                os.waitpid(-1, 0)
            except ChildProcessError:
                break
        for handle in logs:
            handle.close()
        if observer:
            try:
                observer.drain("producer")
                observation.update(observer.finish())
            except (OSError, ValueError, RuntimeError) as error:
                observation.update(complete=False, error=str(error))
            records = observer.records
        write_json(output / "wire.json", {"observation": observation, "packets": records})
        result = analyze(records, observation, markers, native, bool(config["self_test"]))
        write_json(output / "wire-result.json", result)
    return int(result["result"] not in ("READ_ONLY_WIRE_PASS", "HARNESS_SELF_TEST_PASS"))


def collect(args, run_stage, digest, script):
    args.output.mkdir(parents=True, exist_ok=False)
    config = {"output": str(args.output), "aravis": str(args.aravis_library),
              "probe": str(args.probe) if args.probe else None,
              "producer": str(args.producer) if args.producer else None,
              "interface": args.interface, "quiet": args.quiet, "timeout": args.timeout,
              "self_test": args.self_test, "host_namespace": os.readlink("/proc/self/ns/net"),
              "host_user_namespace": os.readlink("/proc/self/ns/user")}
    write_json(args.output / "config.json", config)
    report = {"schema": "gige-mel.gentl-offline-qualification", "version": 2,
              "mode": "isolated-wire", "requested_access": "DEVICE_ACCESS_READONLY",
              "physical_camera": "NOT_AUTHORIZED_NOT_RUN", "synthetic": bool(args.self_test),
              "started_ns": time.time_ns(), "files": {}, "result": "WIRE_OBSERVATION_UNAVAILABLE"}
    for key in ("producer", "probe", "aravis_library"):
        path = getattr(args, key)
        report[key] = {"path": str(path), "sha256": digest(path)} if path and path.is_file() else None
    if not args.self_test and (not args.producer or not args.producer.is_file()):
        report["result"] = "PRODUCER_NOT_AVAILABLE"
    else:
        command = ["unshare", "--user", "--map-root-user", "--net", "--pid", "--fork",
                   "--mount-proc", "--kill-child", "--", sys.executable, str(script),
                   "--wire-worker", str(args.output / "config.json")]
        report["stages"] = [run_stage(command, args.output, "wire-namespace", args.timeout + 20)]
        if (args.output / "wire-result.json").is_file():
            report.update(json.loads((args.output / "wire-result.json").read_text()))
        stage = report["stages"][0]
        if stage.get("timed_out"):
            report["result"] = "PRODUCER_TIMEOUT"
        elif stage.get("returncode", 0) < 0:
            report["result"] = "NATIVE_CLEANUP_UNRESOLVED"
        elif stage["outcome"] != "success" and report["result"] in ("READ_ONLY_WIRE_PASS", "HARNESS_SELF_TEST_PASS"):
            report["result"] = "NATIVE_CLEANUP_UNRESOLVED"
    report["files"] = {p.name: {"sha256": digest(p), "bytes": p.stat().st_size}
                       for p in sorted(args.output.iterdir()) if p.is_file()}
    producer_log = args.output / "producer.stdout.log"
    if producer_log.exists():
        report["producer_info"] = [line for line in producer_log.read_text(errors="replace").splitlines()
                                   if line.startswith("producer TL_INFO_")]
    camera_info = args.output / "camera-ready.json"
    if camera_info.exists():
        report["simulated_camera"] = json.loads(camera_info.read_text())
    report["tool_sources"] = {p.name: digest(p) for p in
                              (script, script.parent / "gentl_wire.py", script.parent / "gentl_wire_runner.py")}
    write_json(args.output / "result.json", report)
    print(report["result"], "evidence:", args.output)
    return int(report["result"] not in ("READ_ONLY_WIRE_PASS", "HARNESS_SELF_TEST_PASS"))