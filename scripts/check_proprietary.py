#!/usr/bin/env python3
"""Reject proprietary SDK material in Git blobs; never traverse an SDK directory.

Default: indexed files (including unchanged files). --history BASE additionally
scans every tree reachable in BASE..HEAD, including intermediate deleted files.
This is a conservative defense in depth, not proof of authorship or a license.
"""

import argparse
import pathlib
import re
import subprocess
import sys


def git(*args):
    return subprocess.check_output(["git", *args])


def violation(path, mode, data):
    name = pathlib.PurePosixPath(path).name.lower()
    lower = path.lower()
    if mode == "160000":
        return "submodules are not permitted (could hide an SDK)"
    if mode == "120000":
        return "new symlinks are not permitted (could expose an external SDK)"
    if re.fullmatch(r"(?:pv|eb|pt)[a-z0-9_]+\.(?:h|hpp)", name):
        return "recognizable vendor header name"
    if re.search(r"(?:\.so(?:\..*)?|\.a|\.o|\.bin|\.exe|\.zip|\.tar(?:\..*)?|\.tgz|\.deb|\.rpm|\.run|\.key|\.lic)$", name):
        return "binary, archive, installer or license-key file"
    if any(part in {"ebus_sdk", "pleora-sdk", "sdk-downloads", "license-keys"} for part in lower.split("/")):
        return "SDK/credential directory"
    if data.startswith((b"\x7fELF", b"!<arch>\n", b"MZ", b"PK\x03\x04", b"\x1f\x8b", b"\xfd7zXZ\x00")):
        return "binary/archive signature"
    # Construct signatures so this independently written scanner is not itself
    # mistaken for vendor content. Includes by reference are intentionally safe.
    signatures = [
        rb"copyright[^\n]{0,160}" + rb"pleora\s+technologies",
        rb"#\s*(?:ifndef|define)\s+__" + rb"PV[A-Z0-9_]+_H__",
        rb"class\s+(?:PV_[A-Z_]+_API\s+)?" + rb"Pv(?:Device|Buffer|Pipeline|System|GenParameter)\s*[:{]",
        rb"pleora\s+technologies\s+inc\.\s*-\s*" + rb"end\s+user\s+license",
        rb"-----BEGIN " + rb"(?:RSA |OPENSSH |EC )?PRIVATE KEY-----",
        rb"\bgh[pousr]_" + rb"[A-Za-z0-9]{30,}",
    ]
    if any(re.search(pattern, data, re.IGNORECASE) for pattern in signatures):
        return "recognizable SDK declaration, vendor copyright/EULA, or credential"
    return None


def check(entries, seen):
    failures = []
    for path, mode, oid in entries:
        key = (path, mode, oid)
        if key in seen:
            continue
        seen.add(key)
        data = b"" if mode == "160000" else git("cat-file", "blob", oid)
        reason = violation(path, mode, data)
        if reason:
            failures.append(f"{path}: {reason} (blob {oid})")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--history", metavar="BASE")
    args = parser.parse_args()
    failures, seen = [], set()
    entries = []
    for entry in git("ls-files", "--stage", "-z").split(b"\0"):
        if not entry:
            continue
        meta, path = entry.split(b"\t", 1)
        mode, oid, stage = meta.decode().split()
        if stage != "0":
            raise RuntimeError("unmerged index")
        entries.append((path.decode(), mode, oid))
    failures.extend(check(entries, seen))
    if args.history:
        for commit in git("rev-list", args.history + "..HEAD").decode().splitlines():
            entries = []
            for entry in git("ls-tree", "-rz", commit).split(b"\0"):
                if not entry:
                    continue
                meta, path = entry.split(b"\t", 1)
                mode, _, oid = meta.decode().split()
                entries.append((path.decode(), mode, oid))
            failures.extend(check(entries, seen))
    if failures:
        print("PROPRIETARY CONTENT GUARD FAILED:\n" + "\n".join(failures), file=sys.stderr)
        return 1
    print(f"Proprietary content guard: {len(seen)} Git entries checked; no recognized prohibited material")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Proprietary content guard could not complete: {error}", file=sys.stderr)
        sys.exit(1)
