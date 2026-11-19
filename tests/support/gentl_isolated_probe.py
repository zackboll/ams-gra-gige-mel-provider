#!/usr/bin/env python3
"""SDK-free native-probe test launcher. Network mutations only in fresh netns.

Not production preflight injection: tests configure real kernel state and run
the unchanged C++ check. PID 1 owns the probe. No proprietary producer allowed
by the tests that invoke this helper.
"""
import os
from pathlib import Path
import subprocess
import sys


def main():
    if sys.argv[1] != "--inside":
        with Path("/proc/self/ns/net").open("rb") as origin:
            return subprocess.run(["unshare", "--user", "--map-root-user", "--net", "--pid", "--fork",
                "--mount-proc", "--kill-child", "--", sys.executable, str(Path(__file__).resolve()),
                "--inside", str(origin.fileno()), *sys.argv[1:]], pass_fds=(origin.fileno(),), timeout=20).returncode
    fd, scenario, probe, producer, *args = sys.argv[2:]
    if os.getpid() != 1:
        raise RuntimeError("fresh PID namespace required")
    def ip(*arguments):
        subprocess.run(["ip", *arguments], check=True, timeout=2)
    ip("link", "set", "lo", "up")
    if scenario == "extra-interface":
        ip("link", "add", "unapproved", "type", "dummy")
    elif scenario == "default-route":
        ip("route", "add", "default", "dev", "lo")
    elif scenario == "external-route":
        ip("route", "add", "192.0.2.0/24", "dev", "lo", "table", "123")
    elif scenario == "unexpected-address":
        ip("address", "add", "127.0.0.2/8", "dev", "lo")
    elif scenario == "inspection-unavailable":
        # Remove proc namespace inspection ONLY in the launcher's private mount ns.
        subprocess.run(["umount", "/proc"], check=True, timeout=2)
    elif scenario == "no-contract":
        os.close(int(fd))
    elif scenario == "same-namespace":
        os.close(int(fd))
        replacement = os.open("/proc/self/ns/net", os.O_RDONLY)
        os.dup2(replacement, int(fd))
    elif scenario == "loopback-down":
        ip("link", "set", "lo", "down")
    elif scenario != "safe":
        raise ValueError(scenario)
    inherited = () if scenario == "no-contract" else (int(fd),)
    return subprocess.run([probe, producer, "wire-readonly", *args, "--origin-netns-fd", fd],
                          pass_fds=inherited, timeout=10).returncode


if __name__ == "__main__":
    sys.exit(main())