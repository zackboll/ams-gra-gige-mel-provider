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
                     *[str(p.relative_to(ROOT)) for p in (ROOT / "src/selection").glob("*")],
                     *[str(p.relative_to(ROOT)) for p in (ROOT / "include").rglob("*.hpp")],
                     "examples/select_session.cpp"]:
            text = (ROOT / path).read_text()
            self.assertNotRegex(text, r"(?i)gentl", path)

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
