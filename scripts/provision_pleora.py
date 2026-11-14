#!/usr/bin/env python3
"""Explicit user-space provisioning for the optional Pleora development build.

Usage: python3 scripts/provision_pleora.py PREFIX
       [--sdk-source EXISTING_SDK] [--genicam-archive VERIFIED_ZIP]

This helper is never run by CMake or CI. It uses the repository owner's chosen
SDK mirror at an immutable commit, and official checksum-pinned EMVA headers.
All SDK/license files stay under an external prefix. It installs no driver,
service, system library, network setting, or shell-profile modification.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile

SDK_VERSION = "6.5.3.7155"
SDK_URL = "https://github.com/RIVeR-Lab/ibex.git"
SDK_COMMIT = "fa8139f75362eb377e97354f1eb079f042076b1a"
SDK_SUBTREE = "docs/place_in_opt/pleora/ebus_sdk/Ubuntu-22.04-x86_64"
GENAPI_URL = "https://www.emva.org/wp-content/uploads/GenICam_V3_4_1_1_public_data.zip"
GENAPI_SHA256 = "04cae9e8004edef0653c1af35522d04e01b01ac85962456b554c5894d904490a"
GENAPI_MEMBER = "GenICam_V3_4_1_1-Linux64_x64_gcc48-SDK.tgz"
GENAPI_MEMBER_SHA256 = "bc6287f516b46fe882cd2425fed334166360b275584ee932a25d271a4d9807eb"


def sdk_version(path: Path) -> str:
    content = (path / "include/PvVersion.h").read_text()
    values = []
    for part in ("MAJOR", "MINOR", "SUB", "BUILD"):
        match = re.search(r"^\s*#define\s+VERSION_" + part + r"\s+(\d+)\s*$", content, re.M)
        if match is None:
            raise ValueError(f"cannot read VERSION_{part} in {path}/include/PvVersion.h")
        values.append(match.group(1))
    return ".".join(values)


def verify_sdk(path: Path) -> None:
    found = sdk_version(path)
    if found != SDK_VERSION:
        raise ValueError(f"expected eBUS {SDK_VERSION}; found {found} in {path}")
    required = ["include/PvDeviceGEV.h", "include/PvSoftDeviceGEV.h", "licenses/EULA/EULA.txt"]
    required += [f"lib/lib{name}.so" for name in (
        "PvBase", "PvBuffer", "PvDevice", "PvGenICam", "PvStream", "PvSystem", "PvVirtualDevice")]
    required += [f"lib/genicam/bin/Linux64_x64/lib{name}_gcc48_v3_4.so" for name in ("GenApi", "GCBase")]
    for name in required:
        if not (path / name).is_file():
            raise ValueError(f"SDK is incomplete: missing {path / name}")


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def provision_headers(archive: Path, destination: Path) -> None:
    if digest(archive) != GENAPI_SHA256:
        raise ValueError("GenApi archive SHA-256 differs from the published EMVA checksum")
    with zipfile.ZipFile(archive) as package:
        member = package.read(GENAPI_MEMBER)
    if hashlib.sha256(member).hexdigest() != GENAPI_MEMBER_SHA256:
        raise ValueError("GenApi Linux SDK member SHA-256 mismatch")
    header_prefix = PurePosixPath("library/CPP/include")
    count = 0
    with tarfile.open(fileobj=io.BytesIO(member), mode="r:gz") as sdk:
        for entry in sdk.getmembers():
            relative = PurePosixPath(entry.name)
            if not relative.is_relative_to(header_prefix):
                continue  # No second GenApi runtime is installed.
            if relative.is_absolute() or ".." in relative.parts or not (entry.isdir() or entry.isfile()):
                raise ValueError(f"unsafe GenApi header archive member: {entry.name}")
            target = destination.joinpath(*relative.parts)
            if entry.isdir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                source = sdk.extractfile(entry)
                if source is None:
                    raise ValueError(f"unreadable GenApi header member: {entry.name}")
                with source, target.open("xb") as output:
                    shutil.copyfileobj(source, output)
                count += 1
    if count == 0 or not (destination / "library/CPP/include/GenApi/IFloat.h").is_file():
        raise ValueError("archive did not supply the required GenApi headers")
    print(f"Provisioned {count} checksum-verified GenApi header files.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("prefix", type=Path)
    parser.add_argument("--sdk-source", type=Path, help="use an existing complete SDK instead of downloading the pinned mirror")
    parser.add_argument("--genicam-archive", type=Path, help="use an existing official ZIP; its SHA-256 is still verified")
    args = parser.parse_args()
    prefix = args.prefix.expanduser().resolve()
    repository = Path(__file__).resolve().parent.parent
    if prefix.is_relative_to(repository):
        raise ValueError("the SDK prefix must be outside this Git repository (including its build directory)")
    if prefix.exists():
        # Existing installations are reusable, but never replaced or repaired
        # silently. An incomplete prefix must be inspected by its owner.
        verify_sdk(prefix)
        header = prefix / "genicam-headers/library/CPP/include/GenApi/IFloat.h"
        version_header = prefix / "genicam-headers/library/CPP/include/_GenICamVersion.h"
        provenance = prefix / "GIGE_MEL_SDK_PROVENANCE.json"
        if not header.is_file() or not version_header.is_file() or not provenance.is_file():
            raise ValueError(f"existing prefix lacks verified GenApi headers/provenance: {prefix}; use a new external prefix")
        recorded = json.loads(provenance.read_text())
        if recorded.get("genapi_sha256") != GENAPI_SHA256 or recorded.get("sdk_version") != SDK_VERSION:
            raise ValueError("existing prefix has different provisioning provenance; leaving it unchanged")
        print(f"Reusing {SDK_VERSION} at {prefix}; no download or mutation.")
        return 0
    prefix.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="pleora-provision-", dir=prefix.parent) as temporary:
        work = Path(temporary)
        if args.sdk_source:
            sdk = args.sdk_source.expanduser().resolve()
            source_description = str(sdk)
        else:
            checkout = work / "mirror"
            checkout.mkdir()
            commands = [
                ["git", "init", "--quiet", str(checkout)],
                ["git", "-C", str(checkout), "remote", "add", "origin", SDK_URL],
                ["git", "-C", str(checkout), "sparse-checkout", "init", "--cone"],
                ["git", "-C", str(checkout), "sparse-checkout", "set", SDK_SUBTREE],
                ["git", "-C", str(checkout), "fetch", "--depth=1", "--filter=blob:none", "origin", SDK_COMMIT],
                ["git", "-C", str(checkout), "checkout", "--detach", "FETCH_HEAD"],
            ]
            for command in commands:
                subprocess.run(command, check=True, timeout=300)
            actual = subprocess.check_output(["git", "-C", str(checkout), "rev-parse", "HEAD"], text=True).strip()
            if actual != SDK_COMMIT:
                raise ValueError("mirror checkout does not match the pinned commit")
            sdk = checkout / SDK_SUBTREE
            source_description = f"{SDK_URL}@{SDK_COMMIT}:{SDK_SUBTREE}"
        verify_sdk(sdk)
        staged = work / "installation"
        staged.mkdir()
        for directory in ("include", "lib", "licenses", "share/doc/third_party_licenses"):
            source = sdk / directory
            if not source.is_dir():
                raise ValueError(f"SDK lacks required headers, runtime or notices: {source}")
            shutil.copytree(source, staged / directory, symlinks=True)
        if args.genicam_archive:
            archive = args.genicam_archive.expanduser().resolve()
        else:
            archive = work / "GenICam_V3_4_1_1_public_data.zip"
            print(f"Downloading official GenApi headers: {GENAPI_URL}", flush=True)
            with urllib.request.urlopen(GENAPI_URL, timeout=60) as source, archive.open("xb") as output:
                shutil.copyfileobj(source, output)
        provision_headers(archive, staged / "genicam-headers")
        verify_sdk(staged)
        provenance = {
            "sdk_version": SDK_VERSION, "sdk_source": source_description,
            "genapi_url": GENAPI_URL, "genapi_sha256": GENAPI_SHA256,
            "genapi_sdk_member": GENAPI_MEMBER, "genapi_sdk_sha256": GENAPI_MEMBER_SHA256,
            "runtime": "Original eBUS libraries only; separate GenApi download supplies headers only",
            "changes": "User-space files only; no drivers, services, system paths or network settings",
        }
        (staged / "GIGE_MEL_SDK_PROVENANCE.json").write_text(json.dumps(provenance, indent=2) + "\n")
        if prefix.exists():
            raise ValueError(f"destination appeared during provisioning; leaving it unchanged: {prefix}")
        os.rename(staged, prefix)
    print(f"Provisioned external eBUS development SDK: {prefix}")
    print(f"CMake: -DGIGE_MEL_BUILD_PLEORA=ON -DGIGE_MEL_PLEORA_SDK_ROOT={prefix}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.SubprocessError, zipfile.BadZipFile, tarfile.TarError) as error:
        print(f"provision_pleora: {error}", file=sys.stderr)
        raise SystemExit(1)
