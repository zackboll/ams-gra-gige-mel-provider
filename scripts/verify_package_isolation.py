#!/usr/bin/env python3
"""Checked package-isolation verifier for the optional Aravis build (CI step).

Every check records an explicit failure, and a failure to RUN a check (ldd/nm
error, unreadable file, traversal error, missing input) is itself a failure;
nothing is treated as "no match". All checks run, then the process exits 1 if
any failed, so a later successful command (e.g. the consumer build) cannot
hide an earlier violation. Only descendants of the install prefix are
examined, so a prefix whose own name contains "aravis" is not a violation.

Checks:
  adapter-sdk        the adapter test executable resolves libaravis-0.8.so.0 to
                     exactly the provisioned library
  provider-deps      the common provider loads no Aravis/GLib/GObject/GIO/
                     libxml2 library (ldd, including transitive dependencies)
  installed-text     installed non-documentation text (public headers, CMake
                     package metadata) does not reference Aravis or its stack
  installed-symbols  installed libraries define/require no Aravis symbols
  installed-names    no installed file or directory name below the prefix
                     mentions Aravis, except the intended documentation file
  installed-links    every non-documentation symlink resolves, within a bounded
                     number of hops and inside the prefix, to an existing
                     regular file; its RESOLVED content is then checked under
                     the LINK's own (public) path policy, so a public alias of
                     an excluded documentation file is still scanned. Only a
                     same-directory libX.so[.N] -> libX.so.N.M version link to an
                     ELF file is skipped, because that target is itself a
                     scanned public path. Directory links, cycles, broken links
                     and targets outside the prefix are explicit failures.
  consumer           a separate consumer configures (without PKG_CONFIG_PATH),
                     builds and runs against the install prefix
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

DOC_ROOT = Path("share/doc/ams-gra-gige-mel-provider")
ALLOWED_NAMES = {DOC_ROOT / "docs" / "aravis-integration.md"}
FORBIDDEN_LIBRARY = re.compile(r"^lib(aravis|glib-2\.0|gobject-2\.0|gio-2\.0|gmodule-2\.0|xml2)\b", re.IGNORECASE)
# Aravis's own name, its umbrella header and its GLib/pkg-config stack.
FORBIDDEN_TEXT = re.compile(rb"aravis|\barv\.h\b|glib-2\.0|gobject-2\.0|PkgConfig::", re.IGNORECASE)
# Consumer cache ENTRY NAMES that would mean the package triggered SDK discovery.
FORBIDDEN_CACHE_ENTRY = re.compile(r"aravis|pkg_config|pkgconfig|glib|gobject", re.IGNORECASE)
FORBIDDEN_SYMBOL = re.compile(r"aravis|^arv_|^g_object_|^g_type_", re.IGNORECASE)
NAME_PATTERN = re.compile(r"aravis", re.IGNORECASE)


class Verifier:
    def __init__(self) -> None:
        self.failures: list[str] = []

    def fail(self, check: str, reason: str) -> None:
        self.failures.append(f"{check}: {reason}")
        print(f"FAIL [{check}] {reason}", flush=True)

    def ok(self, check: str, detail: str) -> None:
        print(f"ok   [{check}] {detail}", flush=True)

    def run(self, check: str, argv: list[str], **kwargs) -> subprocess.CompletedProcess | None:
        """Runs a tool; an inability to run it or a nonzero status is a failure."""
        try:
            result = subprocess.run(argv, capture_output=True, text=True, **kwargs)
        except OSError as error:
            self.fail(check, f"could not run {argv[0]}: {error}")
            return None
        if result.returncode != 0:
            detail = result.stderr.strip() or result.stdout.strip()
            self.fail(check, f"{' '.join(argv)} exited {result.returncode}: {detail}")
            return None
        return result


def ldd_entries(verifier: Verifier, check: str, binary: Path) -> dict[str, str] | None:
    """Maps each dependency name to its resolved path ("" when not found)."""
    if not binary.is_file():
        verifier.fail(check, f"{binary} does not exist")
        return None
    result = verifier.run(check, ["ldd", str(binary)])
    if result is None:
        return None
    entries: dict[str, str] = {}
    for line in result.stdout.splitlines():
        line = line.strip()
        if "=>" in line:
            name, _, target = line.partition("=>")
            target = target.strip()
            entries[name.strip()] = "" if target.startswith("not found") else target.split(" (")[0].strip()
        elif line:
            entries[line.split(" (")[0].strip()] = line.split(" (")[0].strip()
    # A successful ldd of a dynamically linked C++ binary always lists libc.
    if not any(name.startswith("libc.so") for name in entries):
        verifier.fail(check, f"ldd output for {binary} is not a dependency list: {result.stdout.strip()!r}")
        return None
    return entries


def check_adapter_sdk(verifier: Verifier, adapter_test: Path, expected: Path) -> None:
    check = "adapter-sdk"
    entries = ldd_entries(verifier, check, adapter_test)
    if entries is None:
        return
    resolved = entries.get("libaravis-0.8.so.0")
    if resolved is None:
        verifier.fail(check, f"{adapter_test} does not depend on libaravis-0.8.so.0")
    elif not resolved:
        verifier.fail(check, f"{adapter_test}: libaravis-0.8.so.0 is not found by the loader")
    elif os.path.realpath(resolved) != os.path.realpath(expected):
        verifier.fail(check, f"{adapter_test} loads {resolved}, not the provisioned {expected}")
    else:
        verifier.ok(check, f"{adapter_test.name} loads {resolved}")


def check_provider(verifier: Verifier, provider: Path) -> None:
    check = "provider-deps"
    entries = ldd_entries(verifier, check, provider)
    if entries is None:
        return
    forbidden = sorted(name for name in entries if FORBIDDEN_LIBRARY.match(name))
    if forbidden:
        verifier.fail(check, f"{provider} depends on forbidden SDK libraries: {', '.join(forbidden)}")
    else:
        verifier.ok(check, f"{provider.name}: {len(entries)} dependencies, none from the Aravis/GLib stack")


def walk(verifier: Verifier, check: str, prefix: Path) -> list[Path] | None:
    """Relative paths of every DESCENDANT of prefix; traversal errors fail."""
    if not prefix.is_dir():
        verifier.fail(check, f"install prefix {prefix} is not a directory")
        return None
    errors: list[OSError] = []
    paths: list[Path] = []
    for root, dirs, files in os.walk(prefix, onerror=errors.append):
        for name in dirs + files:
            paths.append(Path(root, name).relative_to(prefix))
    for error in errors:
        verifier.fail(check, f"cannot traverse {error.filename}: {error.strerror or error}")
    if errors:
        return None
    if not paths:
        verifier.fail(check, f"install prefix {prefix} is empty")
        return None
    return paths


def is_documentation(relative: Path) -> bool:
    return relative == DOC_ROOT or DOC_ROOT in relative.parents


def check_installed_names(verifier: Verifier, prefix: Path, paths: list[Path]) -> None:
    check = "installed-names"
    # Only components BELOW the prefix are matched: the prefix name is irrelevant.
    violations = [p for p in paths if any(NAME_PATTERN.search(part) for part in p.parts) and p not in ALLOWED_NAMES]
    for relative in violations:
        verifier.fail(check, f"installed path {relative} mentions Aravis")
    if not violations:
        verifier.ok(check, f"{len(paths)} installed paths under {prefix}; only the documented exception mentions Aravis")


def binary_kind(head: bytes) -> str | None:
    if head.startswith(b"\x7fELF"):
        return "elf"
    if head.startswith(b"!<arch>\n"):
        return "archive"
    return None


def scan_symbols(verifier: Verifier, path: Path, relative: Path, kind: str) -> bool:
    """True when the binary was listed successfully and is clean."""
    check = "installed-symbols"
    argv = ["nm", "-A"] + (["-D"] if kind == "elf" else []) + [str(path)]
    result = verifier.run(check, argv)
    if result is None:
        return False
    names = [line.split()[-1] for line in result.stdout.splitlines() if line.split()]
    if not names:
        verifier.fail(check, f"nm listed no symbols for {relative}")
        return False
    bad = sorted({name for name in names if FORBIDDEN_SYMBOL.search(name)})
    if bad:
        verifier.fail(check, f"{relative} contains Aravis/GObject symbols: {', '.join(bad[:5])}")
        return False
    return True


MAX_LINK_HOPS = 16


def resolve_link(prefix: Path, path: Path) -> tuple[Path | None, str]:
    """Follows one installed symlink chain hop by hop, bounded.

    Returns (final regular-file target, "") or (None, reason). Unsupported:
    a missing target, a link cycle or overlong chain, any hop outside the
    installation prefix, and a final target that is not a regular file
    (directory links would hide content os.walk does not traverse).
    """
    root = prefix.resolve()
    current = path
    for _ in range(MAX_LINK_HOPS):
        if not current.is_symlink():
            break
        try:
            target = os.readlink(current)
        except OSError as error:
            return None, f"cannot read link: {error.strerror or error}"
        current = Path(os.path.normpath(current.parent / target))
        try:
            current.parent.resolve(strict=True).relative_to(root)
        except (OSError, ValueError):
            return None, f"link hop {current} is outside the installation prefix or unresolvable"
    else:
        return None, f"link chain exceeds {MAX_LINK_HOPS} hops (cycle?)"
    if not os.path.lexists(current):
        return None, f"broken link: target {current} does not exist"
    if current.is_dir():
        return None, f"directory link to {current} is unsupported (its content would not be traversed)"
    if not current.is_file():
        return None, f"link target {current} is not a regular file"
    return current, ""


def is_library_version_link(link: Path, target: Path) -> bool:
    """Narrow exception: libX.so[.N] -> regular ELF libX.so.N.M in the SAME directory.

    The caller has already excluded documentation paths, so the target is a
    non-documentation installed regular file that the scan visits in its own
    right, under the public policy, with symbol checks.
    """
    return (target.parent == link.parent and link.name.startswith("lib") and ".so" in link.name
            and target.name.startswith(link.name) and target.name != link.name)


def check_installed_content(verifier: Verifier, prefix: Path, paths: list[Path]) -> None:
    text_files = binaries = 0
    text_clean = symbols_clean = True
    for relative in paths:
        path = prefix / relative
        if is_documentation(relative):
            continue
        if path.is_symlink():
            target, reason = resolve_link(prefix, path)
            if target is None:
                verifier.fail("installed-links", f"{relative}: {reason}")
                text_clean = False
                continue
            try:
                is_elf = target.read_bytes()[:4] == b"\x7fELF"
            except OSError as error:
                verifier.fail("installed-links", f"cannot read link target of {relative}: {error.strerror or error}")
                text_clean = False
                continue
            if is_elf and is_library_version_link(path, target):
                continue  # the regular ELF target is a public path and receives symbol checks itself
            # Every other link: its RESOLVED content is checked under the
            # link's own (public) path policy below, never the target's.
            # Documentation exclusion of the target does not exempt the alias.
        elif path.is_dir():
            continue
        try:
            data = path.read_bytes()
        except OSError as error:
            verifier.fail("installed-text", f"cannot read {relative}: {error.strerror or error}")
            text_clean = False
            continue
        kind = binary_kind(data[:8])
        if kind is not None:
            binaries += 1
            symbols_clean = scan_symbols(verifier, path, relative, kind) and symbols_clean
            continue
        if b"\0" in data:
            verifier.fail("installed-text", f"{relative} is an unexpected non-ELF binary file")
            text_clean = False
            continue
        text_files += 1
        match = FORBIDDEN_TEXT.search(data)
        if match:
            line = data.count(b"\n", 0, match.start()) + 1
            verifier.fail("installed-text", f"{relative}:{line} references {match.group().decode(errors='replace')!r}")
            text_clean = False
    if text_clean:
        verifier.ok("installed-text", f"{text_files} non-documentation text files are Aravis-free")
    if symbols_clean:
        verifier.ok("installed-symbols", f"{binaries} installed binaries have no Aravis symbols")


def check_consumer(verifier: Verifier, prefix: Path, source: Path, build: Path, generator: str) -> None:
    check = "consumer"
    env = {k: v for k, v in os.environ.items() if k not in {"PKG_CONFIG_PATH", "PKG_CONFIG_LIBDIR"}}
    steps = [
        ["cmake", "-S", str(source), "-B", str(build), "-G", generator, f"-DCMAKE_PREFIX_PATH={prefix}"],
        ["cmake", "--build", str(build)],
        [str(build / "installed_consumer")],
    ]
    for argv in steps:
        if verifier.run(check, argv, env=env) is None:
            return
    cache = build / "CMakeCache.txt"
    try:
        cached = cache.read_text(errors="replace")
    except OSError as error:
        verifier.fail(check, f"cannot read {cache}: {error.strerror or error}")
        return
    # Entry names only (NAME:TYPE=VALUE): paths such as the prefix or build
    # directory may legitimately contain "aravis"; a discovery variable may not.
    entries = [line.split(":", 1)[0] for line in cached.splitlines()
               if line and not line.startswith(("#", "//")) and ":" in line]
    if not entries:
        verifier.fail(check, f"{cache} contains no cache entries")
        return
    lookups = sorted(name for name in entries if FORBIDDEN_CACHE_ENTRY.search(name))
    if lookups:
        verifier.fail(check, f"{cache} records SDK discovery entries: {', '.join(lookups)}")
        return
    verifier.ok(check, "configured without PKG_CONFIG_PATH, built and ran against the installed package")



def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--adapter-test", type=Path, required=True)
    parser.add_argument("--expected-sdk-library", type=Path, required=True)
    parser.add_argument("--provider", type=Path, required=True)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--consumer-source", type=Path, required=True)
    parser.add_argument("--consumer-build", type=Path, required=True)
    parser.add_argument("--generator", default="Ninja")
    args = parser.parse_args()

    verifier = Verifier()
    check_adapter_sdk(verifier, args.adapter_test, args.expected_sdk_library)
    check_provider(verifier, args.provider)
    paths = walk(verifier, "installed-tree", args.prefix)
    if paths is not None:
        check_installed_names(verifier, args.prefix, paths)
        check_installed_content(verifier, args.prefix, paths)
    # Runs even after a failure, for diagnostics; its success cannot clear one.
    check_consumer(verifier, args.prefix, args.consumer_source, args.consumer_build, args.generator)
    if verifier.failures:
        print(f"package isolation FAILED ({len(verifier.failures)} failure(s))", file=sys.stderr)
        return 1
    print("package isolation verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
