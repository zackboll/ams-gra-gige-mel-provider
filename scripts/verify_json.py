#!/usr/bin/env python3
"""Verify the private JSON dependency offline, independently of the MEL lock."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1] / "third_party"
manifest = json.loads((root / "json-manifest.json").read_text())
component = root / "nlohmann-json"
expected = set()
for entry in manifest["files"]:
    path = component / entry["path"]
    expected.add(path)
    if path.is_symlink():
        raise ValueError(f"vendored symlink: {path}")
    data = path.read_bytes()
    if len(data) != entry["size"] or hashlib.sha256(data).hexdigest() != entry["sha256"]:
        raise ValueError(f"vendored size/SHA-256 mismatch: {path}")
    blob = b"blob " + str(len(data)).encode("ascii") + b"\0" + data
    if hashlib.sha1(blob).hexdigest() != entry["git_blob_sha1"]:
        raise ValueError(f"vendored Git blob mismatch: {path}")
if {p for p in component.rglob("*") if p.is_file() or p.is_symlink()} != expected:
    raise ValueError("unexpected files in private parser dependency")
print(f"Verified nlohmann/json {manifest['version']}: {len(expected)} pinned files; no network access.")