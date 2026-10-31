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
# (FIXTURE_NM_SYMBOL adds one, FIXTURE_NM_FAIL fails); cmake "configures" by
# writing a CMakeCache.txt and "builds" a consumer exiting FIXTURE_CONSUMER_EXIT.
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
exit 0
"""
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
        self.env.update(PATH=f"{tools}{os.pathsep}{os.environ.get('PATH', '')}", FIXTURE_SDK=str(self.sdk))

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


if __name__ == "__main__":
    unittest.main(verbosity=2)
