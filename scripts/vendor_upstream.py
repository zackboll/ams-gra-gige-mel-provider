#!/usr/bin/env python3
"""Verify or explicitly restore the immutable MEL declaration dependencies.

Normal builds use --verify and do not access the network. --restore fetches only
the exact files/commits already recorded in third_party/manifest.json; it does
not select newer releases or regenerate the lock file.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import sys
import tempfile
import urllib.request


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
VENDOR_ROOT = REPOSITORY_ROOT / "third_party"
MANIFEST = VENDOR_ROOT / "manifest.json"


def checked_path(value: str) -> PurePosixPath:
    path = PurePosixPath(value)
    if path.is_absolute() or ".." in path.parts or "\\" in value or not path.parts:
        raise ValueError(f"unsafe manifest path: {value!r}")
    return path


def read_manifest() -> tuple[dict, dict[str, dict]]:
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    if manifest["schema_version"] != 1:
        raise ValueError("unsupported manifest schema")
    components = {component["id"]: component for component in manifest["components"]}
    if len(components) != len(manifest["components"]):
        raise ValueError("duplicate component in manifest")
    for component in components.values():
        if not re.fullmatch(r"[a-z0-9-]+", component["id"]):
            raise ValueError("invalid component identifier")
        if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", component["repo"]):
            raise ValueError("invalid upstream repository")
        if not re.fullmatch(r"[0-9a-f]{40}", component["revision"]):
            raise ValueError("upstream revision must be an immutable full commit SHA")
    seen = set()
    for entry in manifest["files"]:
        path = checked_path(entry["path"])
        source = checked_path(entry["source_path"])
        component = components[entry["component"]]
        if str(path) != str(PurePosixPath(component["id"]) / source):
            raise ValueError(f"source/destination mismatch: {path}")
        if str(path) in seen:
            raise ValueError(f"duplicate manifest path: {path}")
        seen.add(str(path))
        if not re.fullmatch(r"[0-9a-f]{64}", entry["sha256"]):
            raise ValueError(f"invalid SHA-256: {path}")
        if not re.fullmatch(r"[0-9a-f]{40}", entry["git_blob_sha1"]):
            raise ValueError(f"invalid Git blob SHA-1: {path}")
        if not isinstance(entry["size"], int) or entry["size"] < 0:
            raise ValueError(f"invalid size: {path}")
    return manifest, components


def verify_bytes(entry: dict, data: bytes) -> None:
    digest = hashlib.sha256(data).hexdigest()
    blob = hashlib.sha1(b"blob " + str(len(data)).encode("ascii") + b"\0" + data).hexdigest()
    if len(data) != entry["size"] or digest != entry["sha256"] or blob != entry["git_blob_sha1"]:
        raise ValueError(f"size or checksum mismatch: {entry['path']}")


def verify(manifest: dict, components: dict[str, dict]) -> None:
    expected = {entry["path"] for entry in manifest["files"]}
    for entry in manifest["files"]:
        path = VENDOR_ROOT / entry["path"]
        if path.is_symlink():
            raise ValueError(f"symlinks are not accepted in the pinned closure: {entry['path']}")
        verify_bytes(entry, path.read_bytes())
    actual = {
        str(path.relative_to(VENDOR_ROOT))
        for component in components
        for path in (VENDOR_ROOT / component).rglob("*")
        if path.is_file() or path.is_symlink()
    }
    unexpected = actual - expected
    if unexpected:
        raise ValueError("untracked vendored files: " + ", ".join(sorted(unexpected)))
    print(f"Verified {len(expected)} pinned upstream files; no network access.")


def restore(manifest: dict, components: dict[str, dict], source_dir: Path | None) -> None:
    # Validate every fetched byte before replacing any existing dependency.
    # The temporary directory is on the destination filesystem for os.replace.
    with tempfile.TemporaryDirectory(prefix=".vendor-restore-", dir=VENDOR_ROOT) as temporary:
        staged = Path(temporary)
        for entry in manifest["files"]:
            component = components[entry["component"]]
            if source_dir is not None:
                data = (source_dir / component["id"] / entry["source_path"]).read_bytes()
            else:
                url = (
                    "https://raw.githubusercontent.com/"
                    + component["repo"] + "/" + component["revision"]
                    + "/" + entry["source_path"]
                )
                request = urllib.request.Request(url, headers={"User-Agent": "ams-gra-gige-mel-vendor/0.1"})
                with urllib.request.urlopen(request, timeout=30) as response:
                    data = response.read(entry["size"] + 1)
            verify_bytes(entry, data)
            target = staged / entry["path"]
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        for entry in manifest["files"]:
            target = VENDOR_ROOT / entry["path"]
            target.parent.mkdir(parents=True, exist_ok=True)
            os.replace(staged / entry["path"], target)
    verify(manifest, components)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    operation = parser.add_mutually_exclusive_group()
    operation.add_argument("--verify", action="store_true", help="verify every locked byte offline (default)")
    operation.add_argument("--restore", action="store_true", help="restore exact locked files; accesses public GitHub unless --source-dir is supplied")
    parser.add_argument("--source-dir", type=Path, help="offline restore source containing ir-mel/, common-mel/, and ams-math/ checkouts")
    args = parser.parse_args()
    if args.source_dir is not None and not args.restore:
        parser.error("--source-dir requires --restore")
    try:
        manifest, components = read_manifest()
        if args.restore:
            restore(manifest, components, args.source_dir)
        else:
            verify(manifest, components)
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"Vendor verification failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
