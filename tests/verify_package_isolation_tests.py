#!/usr/bin/env python3
"""Permanent regression suite for scripts/verify_package_isolation.py.

Runs the REAL verifier CLI (never a copy of its algorithm) against
FIXTURES: a small synthetic installed tree, fixture adapter-test/provider
files, and PATH stubs for ldd/nm/cmake. No Aravis SDK, compiler or network
is needed, so it runs in every CI job. Every negative case starts from the
passing baseline and asserts its specific failure reason, so an unrelated
failure cannot masquerade as detection.

These fixtures test the verifier; they are not findings about the real
project package (CI runs the verifier on the real install separately).
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
VERIFIER = REPO / "scripts" / "verify_package_isolation.py"
DOC = Path("share/doc/ams-gra-gige-mel-provider")
ELF_HEADER = b"\x7fELF\x02\x01\x01" + b"\0" * 57

# Fixture tools. ldd prints a canned dependency list (FIXTURE_LDD_EXTRA adds a
# provider dependency, FIXTURE_LDD_FAIL fails); nm prints a canned symbol list
# (FIXTURE_NM_SYMBOL adds one to EVERY file, FIXTURE_NM_FAIL fails). nm is also
# content-aware: a "FIXTURE-SYMBOL=<name>" marker in the scanned file adds
# <name> for THAT file only, and every scanned path is appended to
# FIXTURE_NM_LOG. cmake "configures" by writing a CMakeCache.txt and "builds"
# a consumer exiting FIXTURE_CONSUMER_EXIT.
LDD_STUB = """\
#!/bin/sh
[ -n "$FIXTURE_LDD_FAIL" ] && { echo "ldd: fixture failure" >&2; exit 1; }
echo "	linux-vdso.so.1 (0x00007ffd00000000)"
case "$1" in
  *adapter*) echo "	libaravis-0.8.so.0 => $FIXTURE_SDK (0x00007f0000001000)";;
  *provider*) [ -n "$FIXTURE_LDD_EXTRA" ] && echo "	$FIXTURE_LDD_EXTRA => /lib/$FIXTURE_LDD_EXTRA (0x1)";;
esac
echo "	libc.so.6 => /lib/x86_64-linux-gnu/libc.so.6 (0x00007f0000002000)"
"""
NM_STUB = """\
#!/bin/sh
[ -n "$FIXTURE_NM_FAIL" ] && { echo "nm: fixture failure" >&2; exit 1; }
for last; do :; done
echo "$last:0000000000001000 T gige_mel_create_backend"
[ -n "$FIXTURE_NM_SYMBOL" ] && echo "$last:                 U $FIXTURE_NM_SYMBOL"
[ -n "$FIXTURE_NM_LOG" ] && echo "$last" >> "$FIXTURE_NM_LOG"
grep -ao 'FIXTURE-SYMBOL=[A-Za-z0-9_]*' "$last" | while IFS= read -r marker; do
  echo "$last:0000000000002000 T ${marker#FIXTURE-SYMBOL=}"
done
exit 0
"""
# A fixture "ELF" whose content-aware nm listing defines arv_camera_new.
FORBIDDEN_ELF = ELF_HEADER + b"FIXTURE-SYMBOL=arv_camera_new\n"
CMAKE_STUB = """\
#!/bin/sh
if [ "$1" = "-S" ]; then
  build=""; while [ $# -gt 0 ]; do [ "$1" = "-B" ] && build="$2"; shift; done
  mkdir -p "$build"
  printf 'CMAKE_PREFIX_PATH:UNINITIALIZED=fixture\\nCMAKE_BUILD_TYPE:STRING=Debug\\n' > "$build/CMakeCache.txt"
  exit 0
fi
if [ "$1" = "--build" ]; then
  printf '#!/bin/sh\\nexit %s\\n' "${FIXTURE_CONSUMER_EXIT:-0}" > "$2/installed_consumer"
  chmod +x "$2/installed_consumer"; exit 0
fi
exit 2
"""


class VerifierFixture(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="gige-mel-verifier-test-"))
        self.addCleanup(self._cleanup)
        tools = self.tmp / "tools"
        tools.mkdir()
        for name, body in (("ldd", LDD_STUB), ("nm", NM_STUB), ("cmake", CMAKE_STUB)):
            (tools / name).write_text(body)
            (tools / name).chmod(0o755)
        self.sdk = self.tmp / "sdk" / "lib" / "libaravis-0.8.so.0"
        self.sdk.parent.mkdir(parents=True)
        self.sdk.write_bytes(ELF_HEADER)
        self.build = self.tmp / "build"
        self.build.mkdir()
        (self.build / "aravis_adapter_tests").write_bytes(ELF_HEADER)
        (self.build / "libgige_mel_provider.so").write_bytes(ELF_HEADER)
        # The prefix's own path contains "aravis": harmless by itself.
        self.prefix = self.tmp / "aravis-root" / "gige-mel-aravis-install"
        self._install_clean_tree()
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("FIXTURE_")}
        self.nm_log = self.tmp / "nm-calls.log"
        self.env.update(PATH=f"{tools}{os.pathsep}{os.environ.get('PATH', '')}", FIXTURE_SDK=str(self.sdk),
                        FIXTURE_NM_LOG=str(self.nm_log))

    def _cleanup(self) -> None:
        for root, dirs, files in os.walk(self.tmp):
            for name in dirs + files:
                path = os.path.join(root, name)
                if not os.path.islink(path):
                    os.chmod(path, 0o755)
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _install_clean_tree(self) -> None:
        p = self.prefix
        (p / "include" / "gige_mel").mkdir(parents=True)
        (p / "include" / "gige_mel" / "backend.hpp").write_text("#pragma once\nnamespace gige_mel {}\n")
        (p / "lib" / "cmake" / "GigeMelProvider").mkdir(parents=True)
        (p / "lib" / "cmake" / "GigeMelProvider" / "GigeMelProviderConfig.cmake").write_text("set(GIGE_MEL_FOUND 1)\n")
        (p / "lib" / "libgige_mel_provider.so.0.1.0").write_bytes(ELF_HEADER)
        # Genuine library-version links, as the real package installs them.
        (p / "lib" / "libgige_mel_provider.so.0").symlink_to("libgige_mel_provider.so.0.1.0")
        (p / "lib" / "libgige_mel_provider.so").symlink_to("libgige_mel_provider.so.0")
        (p / DOC / "docs").mkdir(parents=True)
        (p / DOC / "docs" / "aravis-integration.md").write_text("Aravis integration (documentation exception)\n")
        (p / DOC / "README.md").write_text("mentions aravis in documentation only\n")

    def run_verifier(self, **fixture_env: str) -> subprocess.CompletedProcess:
        env = dict(self.env, **fixture_env)
        self.nm_log.unlink(missing_ok=True)
        argv = [sys.executable, str(VERIFIER), "--adapter-test", str(self.build / "aravis_adapter_tests"),
                "--expected-sdk-library", str(self.sdk), "--provider", str(self.build / "libgige_mel_provider.so"),
                "--prefix", str(self.prefix), "--consumer-source", str(self.tmp / "consumer-src"),
                "--consumer-build", str(self.tmp / "consumer-build")]
        return subprocess.run(argv, capture_output=True, text=True, env=env, timeout=60)

    def assert_passes(self, result: subprocess.CompletedProcess) -> None:
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("FAIL", result.stdout)
        self.assertIn("package isolation verified", result.stdout)

    def assert_fails_for(self, result: subprocess.CompletedProcess, check: str, detail: str) -> None:
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        failures = [line for line in result.stdout.splitlines() if line.startswith("FAIL")]
        self.assertTrue(any(f"[{check}]" in line and detail in line for line in failures),
                        f"expected FAIL [{check}] ... {detail!r}; got:\n" + "\n".join(failures))

    def failures(self, result: subprocess.CompletedProcess) -> list[str]:
        return [line for line in result.stdout.splitlines() if line.startswith("FAIL")]

    def nm_calls(self) -> list[str]:
        return self.nm_log.read_text().splitlines() if self.nm_log.exists() else []


class ContentAndLinkCases(VerifierFixture):
    def test_01_clean_prefix_named_aravis_passes(self) -> None:
        result = self.run_verifier()
        self.assert_passes(result)
        self.assertIn("aravis-root/gige-mel-aravis-install", result.stdout)

    def test_02_regular_forbidden_public_header_fails(self) -> None:
        (self.prefix / "include/gige_mel/optional.hpp").write_text("#include <arv.h>\n")
        self.assert_fails_for(self.run_verifier(), "installed-text", "include/gige_mel/optional.hpp:1 references 'arv.h'")

    def test_03_public_symlink_into_excluded_documentation_fails(self) -> None:
        (self.prefix / DOC / "optional.hpp").write_text("#include <arv.h>\n")
        (self.prefix / "include/gige_mel/optional.hpp").symlink_to(Path("../..") / DOC / "optional.hpp")
        self.assert_fails_for(self.run_verifier(), "installed-text", "include/gige_mel/optional.hpp:1 references 'arv.h'")

    def test_04_broken_in_prefix_public_symlink_fails(self) -> None:
        (self.prefix / "include/gige_mel/missing.hpp").symlink_to("not-installed.hpp")
        self.assert_fails_for(self.run_verifier(), "installed-links", "include/gige_mel/missing.hpp: broken link")

    def test_05_library_version_links_to_scanned_regular_binary_pass(self) -> None:
        result = self.run_verifier()
        self.assert_passes(result)
        # Two version links were skipped; exactly the regular target was symbol-checked.
        self.assertIn("1 installed binaries have no Aravis symbols", result.stdout)

    def test_05b_library_version_link_target_still_receives_symbol_checks(self) -> None:
        self.assert_fails_for(self.run_verifier(FIXTURE_NM_SYMBOL="arv_device_get_feature"), "installed-symbols",
                              "lib/libgige_mel_provider.so.0.1.0 contains Aravis")

    def test_05c_same_directory_broken_library_link_fails(self) -> None:
        (self.prefix / "lib/libgige_mel_extra.so").symlink_to("libgige_mel_extra.so.1")
        self.assert_fails_for(self.run_verifier(), "installed-links", "lib/libgige_mel_extra.so: broken link")

    def test_05d_library_named_link_to_documentation_is_not_exempt(self) -> None:
        (self.prefix / DOC / "libgige_mel_provider.so.9").write_bytes(ELF_HEADER)
        (self.prefix / "lib/libgige_mel_doc.so").symlink_to(Path("..") / DOC / "libgige_mel_provider.so.9")
        self.assert_fails_for(self.run_verifier(FIXTURE_NM_SYMBOL="arv_camera_new"), "installed-symbols",
                              "lib/libgige_mel_doc.so contains Aravis")

    def test_06_directory_symlink_fails_explicitly(self) -> None:
        (self.prefix / "include/gige_mel/docs").symlink_to(Path("../..") / DOC, target_is_directory=True)
        self.assert_fails_for(self.run_verifier(), "installed-links", "include/gige_mel/docs: directory link")

    def test_06b_link_cycle_fails_bounded(self) -> None:
        (self.prefix / "include/gige_mel/a.hpp").symlink_to("b.hpp")
        (self.prefix / "include/gige_mel/b.hpp").symlink_to("a.hpp")
        result = self.run_verifier()
        self.assert_fails_for(result, "installed-links", "include/gige_mel/a.hpp: link chain exceeds")
        self.assert_fails_for(result, "installed-links", "include/gige_mel/b.hpp: link chain exceeds")

    def test_06c_link_outside_prefix_fails(self) -> None:
        outside = self.tmp / "outside.hpp"
        outside.write_text("#pragma once\n")
        (self.prefix / "include/gige_mel/outside.hpp").symlink_to(outside)
        self.assert_fails_for(self.run_verifier(), "installed-links", "outside the installation prefix")


class ToolAndOrderingCases(VerifierFixture):
    def test_07_verification_command_failure_fails(self) -> None:
        result = self.run_verifier(FIXTURE_LDD_FAIL="1")
        self.assert_fails_for(result, "adapter-sdk", "exited 1: ldd: fixture failure")
        self.assert_fails_for(result, "provider-deps", "exited 1: ldd: fixture failure")

    def test_07b_symbol_tool_failure_fails(self) -> None:
        self.assert_fails_for(self.run_verifier(FIXTURE_NM_FAIL="1"), "installed-symbols", "nm: fixture failure")

    def test_07c_file_read_failure_fails(self) -> None:
        header = self.prefix / "include/gige_mel/backend.hpp"
        header.chmod(0)
        if os.access(header, os.R_OK):
            self.skipTest("privileges bypass file permissions; read failure cannot be arranged")
        self.assert_fails_for(self.run_verifier(), "installed-text", "cannot read include/gige_mel/backend.hpp")

    def test_08_later_successful_consumer_cannot_erase_earlier_failure(self) -> None:
        result = self.run_verifier(FIXTURE_LDD_EXTRA="libglib-2.0.so.0")
        self.assert_fails_for(result, "provider-deps", "libglib-2.0.so.0")
        self.assertIn("ok   [consumer]", result.stdout)  # ran and succeeded AFTER the failure
        self.assertIn("package isolation FAILED (1 failure(s))", result.stderr)

    def test_08b_consumer_failure_fails(self) -> None:
        self.assert_fails_for(self.run_verifier(FIXTURE_CONSUMER_EXIT="3"), "consumer", "exited 3")

    def test_09_forbidden_installed_name_fails(self) -> None:
        (self.prefix / "include/gige_mel/aravis_backend.hpp").write_text("#pragma once\n")
        self.assert_fails_for(self.run_verifier(), "installed-names", "include/gige_mel/aravis_backend.hpp mentions Aravis")


class FilesystemOrderResolutionCases(VerifierFixture):
    """GIGE-MEL-008-R3: links resolve in FILESYSTEM order (a directory symlink
    is expanded before a following ".."), containment holds for every
    expansion, and the one actually resolved file is the one checked.

    Each case first establishes its fixture facts independently of the
    verifier (actual kernel resolution, the distinct lexical target, the
    fixture nm listing), then requires the specific failure and no other."""

    def setUp(self) -> None:
        super().setUp()
        self.root = self.prefix.resolve(strict=True)
        self.outside = (self.tmp / "outside").resolve()  # a sibling of the prefix, outside it
        self.outside.mkdir()

    # ---- fixture-fact helpers (independent of the verifier) ----
    def link(self, relative: str, target: str | Path, directory: bool = False) -> Path:
        path = self.prefix / relative
        path.symlink_to(target, target_is_directory=directory)
        return path

    def assert_actual_and_lexical(self, link: Path, actual: Path, lexical: Path | None) -> None:
        """The kernel resolves `link` to `actual`; lexical normalization (as
        in R2) selects the distinct, existing regular file `lexical`."""
        self.assertEqual(link.resolve(strict=True), actual.resolve(strict=True))
        if lexical is not None:
            normalized = Path(os.path.normpath(link.parent / os.readlink(link)))
            self.assertEqual(normalized, lexical)
            self.assertTrue(lexical.is_file() and not lexical.is_symlink())
            self.assertNotEqual(lexical.resolve(), actual.resolve())

    def fixture_symbols(self, path: Path) -> str:
        nm = self.tmp / "tools" / "nm"
        return subprocess.run([str(nm), "-A", "-D", str(path)], capture_output=True, text=True,
                              env=dict(self.env, FIXTURE_NM_LOG=""), timeout=30, check=True).stdout

    def assert_only_failure(self, result: subprocess.CompletedProcess, check: str, detail: str) -> None:
        self.assert_fails_for(result, check, detail)
        self.assertEqual(len(self.failures(result)), 1, "\n".join(self.failures(result)))
        self.assertIn("package isolation FAILED (1 failure(s))", result.stderr)

    # ---- A: clean outside header through a directory symlink plus ".." ----
    def test_10_clean_outside_header_through_directory_link_and_dotdot_fails(self) -> None:
        (self.outside / "a/b/c/d").mkdir(parents=True)
        (self.outside / "a/b/c/clean.hpp").write_text("#pragma once\n// harmless\n")
        (self.prefix / DOC / "clean.hpp").write_text("#pragma once\n// harmless\n")
        self.link(f"{DOC}/alias", self.outside / "a/b/c/d", directory=True)
        header = self.link("include/gige_mel/optional.hpp", f"../../{DOC}/alias/../clean.hpp")
        self.assert_actual_and_lexical(header, self.outside / "a/b/c/clean.hpp", self.prefix / DOC / "clean.hpp")
        result = self.run_verifier()
        self.assert_only_failure(result, "installed-links", "include/gige_mel/optional.hpp: ")
        self.assert_fails_for(result, "installed-links", "outside the installation prefix")

    # ---- B: outside library hidden by a clean lexical version target ----
    def test_11_outside_library_hidden_by_clean_lexical_version_target_fails(self) -> None:
        (self.outside / "a/b/c/d").mkdir(parents=True)
        (self.outside / "lib").mkdir()
        actual = self.outside / "lib/libextra.so.1"
        actual.write_bytes(FORBIDDEN_ELF)
        clean = self.prefix / "lib/libextra.so.1"
        clean.write_bytes(ELF_HEADER)
        self.link(f"{DOC}/alias", self.outside / "a/b/c/d", directory=True)
        alias = self.link("lib/libextra.so", f"../{DOC}/alias/../../../../lib/libextra.so.1")
        self.assert_actual_and_lexical(alias, actual, clean)
        self.assertIn("arv_camera_new", self.fixture_symbols(actual))
        self.assertNotIn("arv_", self.fixture_symbols(clean))
        result = self.run_verifier()
        self.assert_only_failure(result, "installed-links", "lib/libextra.so: ")
        self.assert_fails_for(result, "installed-links", "outside the installation prefix")
        # The clean regular sibling is still checked in its own right, and passes.
        self.assertIn(str(self.root / "lib/libextra.so.1"), self.nm_calls())
        self.assertIn("2 installed binaries have no Aravis symbols", result.stdout)

    # ---- C: in-prefix compositions whose actual and lexical targets differ ----
    def _inner_composition(self, header_text: str, library_bytes: bytes) -> None:
        real = self.prefix / DOC / "real"
        (real / "a/b/c/d").mkdir(parents=True)
        (real / "lib").mkdir()
        (real / "x/y").mkdir(parents=True)
        (real / "x/optional.hpp").write_text(header_text)
        (real / "lib/libextra.so.1").write_bytes(library_bytes)
        (self.prefix / DOC / "optional.hpp").write_text("#pragma once\n// harmless lexical lookalike\n")
        (self.prefix / "lib/libextra.so.1").write_bytes(ELF_HEADER)
        self.link(f"{DOC}/inner", "real/x/y", directory=True)
        self.link(f"{DOC}/deep", "real/a/b/c/d", directory=True)
        header = self.link("include/gige_mel/optional.hpp", f"../../{DOC}/inner/../optional.hpp")
        library = self.link("lib/libextra.so", f"../{DOC}/deep/../../../../lib/libextra.so.1")
        self.assert_actual_and_lexical(header, real / "x/optional.hpp", self.prefix / DOC / "optional.hpp")
        self.assert_actual_and_lexical(library, real / "lib/libextra.so.1", self.prefix / "lib/libextra.so.1")

    def test_12_in_prefix_composition_checks_actual_header_target(self) -> None:
        self._inner_composition("#include <arv.h>\n", ELF_HEADER)
        self.assert_only_failure(self.run_verifier(), "installed-text",
                                 "include/gige_mel/optional.hpp:1 references 'arv.h'")

    def test_12b_in_prefix_composition_checks_actual_library_target(self) -> None:
        self._inner_composition("#pragma once\n", FORBIDDEN_ELF)
        self.assertIn("arv_camera_new", self.fixture_symbols(self.prefix / DOC / "real/lib/libextra.so.1"))
        self.assertNotIn("arv_", self.fixture_symbols(self.prefix / "lib/libextra.so.1"))
        result = self.run_verifier()
        self.assert_only_failure(result, "installed-symbols",
                                 "lib/libextra.so contains Aravis/GObject symbols: arv_camera_new")
        # Not exempted as a version link: the ACTUAL (documentation) target was
        # scanned under the public alias's name and policy.
        self.assertIn(str(self.root / DOC / "real/lib/libextra.so.1"), self.nm_calls())

    def test_12c_clean_supported_compositions_pass(self) -> None:
        self._inner_composition("#pragma once\n", ELF_HEADER)
        self.link("include/gige_mel/sibling.hpp", "../gige_mel/./backend.hpp")
        self.link("include/gige_mel/absolute.hpp", self.root / "include/gige_mel/backend.hpp")
        result = self.run_verifier()
        self.assert_passes(result)
        # provider + the clean regular libextra.so.1 + the alias, scanned through its actual target
        self.assertIn("3 installed binaries have no Aravis symbols", result.stdout)
        self.assertIn(str(self.root / DOC / "real/lib/libextra.so.1"), self.nm_calls())

    # ---- D: an outside excursion that returns inside is still an escape ----
    def test_13_outside_excursion_returning_inside_fails(self) -> None:
        (self.outside / "door").mkdir()
        (self.outside / "door/back").symlink_to(self.root / "include/gige_mel", target_is_directory=True)
        self.link(f"{DOC}/away", self.outside / "door", directory=True)
        returning = self.link("include/gige_mel/compat.hpp", f"../../{DOC}/away/back/backend.hpp")
        detour = self.link("include/gige_mel/detour.hpp",
                           f"../../../{self.prefix.name}/include/gige_mel/backend.hpp")
        # The FINAL file of both is the clean installed header inside the prefix.
        for link in (returning, detour):
            self.assert_actual_and_lexical(link, self.prefix / "include/gige_mel/backend.hpp", None)
        result = self.run_verifier()
        self.assert_fails_for(result, "installed-links",
                              f"include/gige_mel/compat.hpp: link {DOC}/away -> {self.outside / 'door'} resolves outside")
        self.assert_fails_for(result, "installed-links",
                              "include/gige_mel/detour.hpp: '..' above the prefix resolves outside")
        self.assertEqual(len(self.failures(result)), 2, "\n".join(self.failures(result)))

    # ---- E: directory-component cycles and hop accounting ----
    def test_14_directory_component_cycle_fails_bounded(self) -> None:
        self.link(f"{DOC}/cyc1", "cyc2", directory=True)
        self.link(f"{DOC}/cyc2", "cyc1", directory=True)
        cycle = self.link("include/gige_mel/cycle.hpp", f"../../{DOC}/cyc1/file.hpp")
        with self.assertRaises(OSError):
            os.stat(cycle)  # the kernel reports a loop in a DIRECTORY component
        self.assert_only_failure(self.run_verifier(), "installed-links",
                                 "include/gige_mel/cycle.hpp: link chain exceeds 16 symlink expansions")

    def _directory_chain(self, directory_links: int) -> None:
        """One public link whose path crosses `directory_links` chained directory links."""
        (self.prefix / DOC / "chain-real").mkdir()
        (self.prefix / DOC / "chain-real/target.hpp").write_text("#pragma once\n")
        names = [f"hop{index:02d}" for index in range(directory_links)] + ["chain-real"]
        for name, following in zip(names, names[1:]):
            self.link(f"{DOC}/{name}", following, directory=True)
        link = self.link("include/gige_mel/chain.hpp", f"../../{DOC}/{names[0]}/target.hpp")
        self.assert_actual_and_lexical(link, self.prefix / DOC / "chain-real/target.hpp", None)

    def _final_chain(self, links: int) -> None:
        """An ordinary final-component chain final00 -> ... -> backend.hpp of `links` links."""
        names = [f"final{index:02d}.hpp" for index in range(links)] + ["backend.hpp"]
        for name, following in zip(names, names[1:]):
            self.link(f"include/gige_mel/{name}", following)
        self.assert_actual_and_lexical(self.prefix / "include/gige_mel/final00.hpp",
                                       self.prefix / "include/gige_mel/backend.hpp", None)

    def test_14b_exactly_sixteen_expansions_pass(self) -> None:
        self._directory_chain(15)  # 1 public link + 15 directory-component links = 16 expansions
        self._final_chain(16)      # 16 final-component expansions
        self.assert_passes(self.run_verifier())

    def test_14c_seventeenth_expansion_fails(self) -> None:
        self._directory_chain(16)  # 17 expansions
        self._final_chain(17)      # final00 needs 17; final01..final16 need at most 16
        result = self.run_verifier()
        self.assert_fails_for(result, "installed-links", "include/gige_mel/chain.hpp: link chain exceeds 16")
        self.assert_fails_for(result, "installed-links", "include/gige_mel/final00.hpp: link chain exceeds 16")
        self.assertEqual(len(self.failures(result)), 2, "\n".join(self.failures(result)))

    # ---- bounded, explicit failures of the other resolution steps ----
    def test_15_non_regular_and_unresolvable_targets_fail(self) -> None:
        os.mkfifo(self.prefix / DOC / "pipe")
        self.link("include/gige_mel/pipe.hpp", f"../../{DOC}/pipe")
        os.mkfifo(self.prefix / "include/gige_mel/raw-pipe")
        self.link("include/gige_mel/through-file.hpp", "backend.hpp/inner.hpp")
        locked = self.prefix / DOC / "locked"
        locked.mkdir()
        (locked / "f.hpp").write_text("#pragma once\n")
        self.link("include/gige_mel/locked.hpp", f"../../{DOC}/locked/f.hpp")
        locked.chmod(0o600)  # listable, but its entries cannot be looked up
        result = self.run_verifier()  # the subprocess timeout bounds it: no FIFO may be opened blocking
        self.assert_fails_for(result, "installed-links",
                              f"include/gige_mel/pipe.hpp: link target {DOC}/pipe is not a regular file")
        self.assert_fails_for(result, "installed-text", "include/gige_mel/raw-pipe is not a regular file")
        self.assert_fails_for(result, "installed-links",
                              "include/gige_mel/through-file.hpp: unresolvable component: include/gige_mel/backend.hpp")
        if os.access(locked / "f.hpp", os.F_OK):
            # Privileges bypass directory permissions: the stat failure cannot be arranged.
            self.assertEqual(len(self.failures(result)), 3, "\n".join(self.failures(result)))
            return
        self.assert_fails_for(result, "installed-links",
                              f"include/gige_mel/locked.hpp: cannot stat {DOC}/locked/f.hpp")
        self.assertEqual(len(self.failures(result)), 4, "\n".join(self.failures(result)))

    # ---- F: ordinary behavior is preserved ----
    def test_16_version_links_skip_only_to_their_symbol_checked_regular_target(self) -> None:
        result = self.run_verifier()
        self.assert_passes(result)
        self.assertEqual(self.nm_calls(), [str(self.root / "lib/libgige_mel_provider.so.0.1.0")])

    def test_16b_forbidden_elf_as_ordinary_file_and_as_version_target_fails(self) -> None:
        (self.prefix / "lib/libextra.so.1").write_bytes(FORBIDDEN_ELF)
        self.assert_only_failure(self.run_verifier(), "installed-symbols",
                                 "lib/libextra.so.1 contains Aravis/GObject symbols: arv_camera_new")
        (self.prefix / "lib/libextra.so").symlink_to("libextra.so.1")
        result = self.run_verifier()  # the genuine version link is skipped; its regular target still fails
        self.assert_only_failure(result, "installed-symbols",
                                 "lib/libextra.so.1 contains Aravis/GObject symbols: arv_camera_new")
        self.assertEqual(sorted(self.nm_calls()), sorted([str(self.root / "lib/libextra.so.1"),
                                                         str(self.root / "lib/libgige_mel_provider.so.0.1.0")]))


if __name__ == "__main__":
    unittest.main(verbosity=2)
