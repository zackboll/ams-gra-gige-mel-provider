#!/usr/bin/env python3
"""Default-build guard for GIGE-MEL-GENTL-001 (no SDK, header or producer needed).

The GenTL feasibility code must stay outside the installed MEL provider, the
selection catalog and every factory path, behind a default-OFF option, and the
repository must not contain the third-party GenTL header itself.
"""
import pathlib
import re
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def tracked():
    out = subprocess.check_output(["git", "-C", str(ROOT), "ls-files", "-z"]).decode()
    return [p for p in out.split("\0") if p]


class GenTLIsolation(unittest.TestCase):
    def test_option_is_default_off_and_only_gates_the_module(self):
        text = (ROOT / "CMakeLists.txt").read_text()
        self.assertRegex(text, r'option\(GIGE_MEL_BUILD_GENTL_PROBE "[^"]*" OFF\)')
        self.assertEqual(len(re.findall(r"GigeMelGenTL\.cmake", text)), 1)
        self.assertRegex(text, r"if\(GIGE_MEL_BUILD_GENTL_PROBE\)\s*\n\s*include\(cmake/GigeMelGenTL\.cmake\)")

    def test_module_never_installs_or_links_the_provider(self):
        lines = [line.split("#", 1)[0] for line in (ROOT / "cmake/GigeMelGenTL.cmake").read_text().splitlines()]
        code = "\n".join(lines)
        for forbidden in ("install(", "gige_mel_provider", "gige_mel_selection", "FetchContent",
                          "file(DOWNLOAD", "ExternalProject"):
            self.assertNotIn(forbidden, code)

    def test_no_provider_selection_or_factory_reference(self):
        for path in ["src/provider.cpp", "src/factories.cpp", "src/config_parser.cpp",
                     *[str(p.relative_to(ROOT)) for p in sorted((ROOT / "src/provider").rglob("*"))
                       if p.suffix in (".cpp", ".hpp")],
                     *[str(p.relative_to(ROOT)) for p in (ROOT / "src/selection").glob("*")],
                     *[str(p.relative_to(ROOT)) for p in (ROOT / "include").rglob("*.hpp")],
                     "examples/select_session.cpp"]:
            text = (ROOT / path).read_text()
            self.assertNotRegex(text, r"(?i)gentl", path)

    # GIGE-MEL-GENTL-002: the backend is internal, default OFF, never installed,
    # never in the default catalog and never reachable from public factories.
    def test_backend_option_is_default_off_and_isolated(self):
        text = (ROOT / "CMakeLists.txt").read_text()
        self.assertRegex(text, r'option\(GIGE_MEL_BUILD_GENTL_BACKEND "[^"]*" OFF\)')
        self.assertEqual(len(re.findall(r"GigeMelGenTLBackend\.cmake", text)), 1)
        self.assertRegex(text, r"if\(GIGE_MEL_BUILD_GENTL_BACKEND\)\s*\n\s*include\(cmake/GigeMelGenTLBackend\.cmake\)")
        lines = [line.split("#", 1)[0] for line in (ROOT / "cmake/GigeMelGenTLBackend.cmake").read_text().splitlines()]
        code = "\n".join(lines)
        for forbidden in ("install(", "gige_mel_provider", "gige_mel_selection_builtins", "FetchContent",
                          "file(DOWNLOAD", "ExternalProject", "aravis-0.9", "aravis-0.10"):
            self.assertNotIn(forbidden, code)
        self.assertIn("aravis-0.8=${GIGE_MEL_GENTL_ARAVIS_VERSION}", code)

    def test_backend_types_stay_internal(self):
        header = (ROOT / "src/backends/gentl/gentl_backend.hpp").read_text()
        genapi = (ROOT / "src/backends/gentl/gentl_genapi.hpp").read_text()
        for text in (header, genapi):
            text = "\n".join(line.split("//", 1)[0] for line in text.splitlines())  # code only
            self.assertNotRegex(text, r"#include\s*[<\"](arv|glib|GenTL|libxml)")
            self.assertNotRegex(text, r"\b(ArvDevice|ArvGc|GError|GObject|TL_HANDLE|PORT_HANDLE)\b")
        for path in (ROOT / "include").rglob("*.hpp"):
            self.assertNotIn("gentl_backend", path.read_text(), str(path))
        # No GenTL use of Aravis's experimental consumer or a native GigE connection.
        source = (ROOT / "src/backends/gentl/gentl_genapi.cpp").read_text()
        for forbidden in ("arv_open_device", "arv_gv_device", "arv_interface", "ArvGenTL", "arv_gentl"):
            self.assertNotIn(forbidden, source)

    def test_header_is_never_committed(self):
        for path in tracked():
            name = pathlib.PurePosixPath(path).name.lower()
            self.assertFalse(name.startswith("gentl") and name.endswith(".h"), path)
            self.assertFalse(name.endswith(".cti"), path)
            if path.endswith((".h", ".hpp", ".cpp", ".c")):
                data = (ROOT / path).read_bytes()
                # The EMVA header's copyright banner; quoting it is not allowed.
                self.assertNotIn(b"by GenICam GenTL Subcommittee", data, path)


if __name__ == "__main__":
    unittest.main()
