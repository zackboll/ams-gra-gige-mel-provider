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
  installed-links    every non-documentation symlink is resolved in FILESYSTEM
                     order (a symlink in a directory component is expanded
                     before a following ".."), counting every expansion
                     (16 allowed), and no expansion may leave the canonical
                     prefix, even transiently. The final target must be an
                     existing regular file; exactly that file's content is
                     then checked under the LINK's own (public) path policy,
                     so a public alias of an excluded documentation file is
                     still scanned. Only a libX.so[.N] -> libX.so.N.M version
                     link whose ACTUAL target is a regular ELF, enumerated,
                     non-documentation file in the link's own directory is
                     skipped, because that target is symbol-checked itself.
                     Directory links, cycles, overlong chains, broken links,
                     unresolvable components and excursions outside the
                     prefix are explicit failures. The tree is assumed
                     quiescent during verification.
  consumer           a separate consumer configures (without PKG_CONFIG_PATH),
                     builds and runs against the install prefix
"""

from __future__ import annotations

import argparse
import os
import re
import stat
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


OUTSIDE = "outside the installation prefix"
# libX.so[.N...] -> libX.so.N[.M...]: both names are matched exactly.
LIBRARY_LINK_NAME = re.compile(r"lib.+\.so(\.[0-9]+)*")
VERSION_SUFFIX = re.compile(r"[0-9]+(\.[0-9]+)*")


def strip_root(root: Path, pieces: list[str]) -> list[str] | None:
    """For an ABSOLUTE link target split on "/", returns its components after
    the canonical root, or None when it does not spell the root. Only the
    no-op components "" and "." are skipped; ".." is never simplified, so
    "/…/prefix/../prefix/x" counts as leaving the prefix. The root's
    components are canonical real directories, so spelling them reaches
    exactly the root without expanding any symlink."""
    rest, index = pieces[1:], 0
    for wanted in root.parts[1:]:
        while index < len(rest) and rest[index] in ("", "."):
            index += 1
        if index >= len(rest) or rest[index] != wanted:
            return None
        index += 1
    return rest[index:]


def resolve_installed(root: Path, relative: Path) -> tuple[Path, os.stat_result] | str:
    """Resolves an installed path in FILESYSTEM order, inside the canonical root.

    Returns (canonical target relative to root, its lstat) or a failure
    reason. Components are consumed from a queue. A symlink in ANY position
    (directory or final component) is expanded by pushing its target's
    components to the front of the queue, so a following ".." acts on the
    directory the link actually reaches; relative targets are interpreted in
    the link's own directory. `resolved` only ever holds lstat-verified real
    directories below the root, so ".." just drops the last one; nothing is
    normalized lexically. Every expansion counts towards MAX_LINK_HOPS
    (16 are allowed, the 17th fails). Any expansion or ".." that leaves the
    root fails at once: a later link pointing back inside does not excuse
    the excursion. The install tree is assumed quiescent while verified.
    """
    resolved: list[str] = []
    pending: list[str] = list(relative.parts)
    expansions = 0
    while pending:
        part = pending.pop(0)
        if part in ("", "."):
            continue  # resolved is a directory here: a non-directory with pending parts fails below
        if part == "..":
            if not resolved:
                return f"'..' above the prefix resolves {OUTSIDE}"
            resolved.pop()
            continue
        component = Path(*resolved, part)
        try:
            info = os.lstat(root / component)
        except FileNotFoundError:
            return f"broken link: target {component} does not exist"
        except OSError as error:
            return f"cannot stat {component}: {error.strerror or error}"
        if stat.S_ISLNK(info.st_mode):
            expansions += 1
            if expansions > MAX_LINK_HOPS:
                return f"link chain exceeds {MAX_LINK_HOPS} symlink expansions at {component} (cycle?)"
            try:
                target = os.readlink(root / component)
            except OSError as error:
                return f"cannot read link {component}: {error.strerror or error}"
            if not target:
                return f"link {component} has an empty target and is unresolvable"
            pieces = target.split("/")
            if target.startswith("/"):
                rest = strip_root(root, pieces)
                if rest is None:
                    return f"link {component} -> {target} resolves {OUTSIDE}"
                resolved, pieces = [], rest
            pending[:0] = pieces
            continue
        if pending and not stat.S_ISDIR(info.st_mode):
            return f"unresolvable component: {component} is not a directory"
        resolved.append(part)
    final = Path(*resolved)
    try:
        info = os.lstat(root / final)
    except OSError as error:
        return f"cannot stat link target {final}: {error.strerror or error}"
    if stat.S_ISDIR(info.st_mode):
        return f"directory link to {final} is unsupported (its content would not be traversed)"
    if not stat.S_ISREG(info.st_mode):
        return f"link target {final} is not a regular file"
    return final, info


def read_validated(path: Path, expected: os.stat_result) -> bytes:
    """Reads exactly the validated regular file: the final component is not
    followed, a special file cannot block the open, and the opened inode
    must be the one that was validated. Raises OSError otherwise."""
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_NONBLOCK", 0) | getattr(os, "O_CLOEXEC", 0)
    fd = os.open(path, flags)
    with os.fdopen(fd, "rb") as handle:
        opened = os.fstat(handle.fileno())
        if not stat.S_ISREG(opened.st_mode) or (opened.st_dev, opened.st_ino) != (expected.st_dev, expected.st_ino):
            raise OSError(f"{path} is not the validated regular file")
        return handle.read()


def is_library_version_link(link: Path, target: Path, enumerated: set[Path]) -> bool:
    """Narrow exception: libX.so[.N] -> libX.so.N.M in the SAME actual directory.

    `link` is the public link's path and `target` its ACTUAL resolved
    target, both relative to the canonical root; the caller has established
    that the target is a regular ELF file. The target must also be a
    non-documentation file that the walk enumerated (and, being a regular
    file, will itself receive the symbol checks under the public policy).
    A lexical lookalike cannot qualify, because only the actual target is
    compared.
    """
    suffix = target.name[len(link.name) + 1:]
    return (target.parent == link.parent and not is_documentation(target) and target in enumerated
            and LIBRARY_LINK_NAME.fullmatch(link.name) is not None
            and target.name.startswith(link.name + ".") and VERSION_SUFFIX.fullmatch(suffix) is not None)


def check_installed_content(verifier: Verifier, prefix: Path, paths: list[Path]) -> None:
    text_files = binaries = 0
    text_clean = symbols_clean = True
    try:
        root = prefix.resolve(strict=True)
    except (OSError, RuntimeError) as error:
        verifier.fail("installed-links", f"cannot canonicalize the installation prefix {prefix}: {error}")
        verifier.fail("installed-text", "installed content was not checked")
        return
    enumerated = set(paths)
    for relative in paths:
        if is_documentation(relative):
            continue
        # walk() does not follow directory links, so every ancestor of
        # `relative` is a real directory below the canonical root.
        path = root / relative
        try:
            info = os.lstat(path)
        except OSError as error:
            verifier.fail("installed-text", f"cannot stat {relative}: {error.strerror or error}")
            text_clean = False
            continue
        is_link = stat.S_ISLNK(info.st_mode)
        if is_link:
            resolution = resolve_installed(root, relative)
            if isinstance(resolution, str):
                verifier.fail("installed-links", f"{relative}: {resolution}")
                text_clean = False
                continue
            # From here on ONLY the actual resolved file is read, classified
            # and symbol-checked, under the LINK's own (public) path policy,
            # never the target's: documentation exclusion of a target does
            # not exempt its public alias.
            target, info = resolution
            path = root / target
        elif stat.S_ISDIR(info.st_mode):
            continue
        elif not stat.S_ISREG(info.st_mode):
            verifier.fail("installed-text", f"{relative} is not a regular file")
            text_clean = False
            continue
        try:
            data = read_validated(path, info)
        except OSError as error:
            if is_link:
                verifier.fail("installed-links", f"cannot read link target of {relative}: {error.strerror or error}")
            else:
                verifier.fail("installed-text", f"cannot read {relative}: {error.strerror or error}")
            text_clean = False
            continue
        kind = binary_kind(data[:8])
        if is_link and kind == "elf" and is_library_version_link(relative, target, enumerated):
            continue  # the actual regular ELF target is an enumerated public file, symbol-checked itself
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
