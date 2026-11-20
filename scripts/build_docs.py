#!/usr/bin/env python3
"""Stage documentation inputs, generate Doxygen HTML, and check the result."""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
from urllib.parse import quote, unquote, urlsplit

from check_docs import REPOSITORY, check_status_source, project_sources, validate_site
import check_mel_traceability as traceability
import check_qualification_evidence as evidence


ROOT = Path(__file__).resolve().parents[1]

# Maintain an explicit publication boundary. Historical reports and test files
# are linked at the source revision, not imported into the generated site.
GUIDES = {
    "README.md", "CONTRIBUTING.md",
    *(f"docs/{name}.md" for name in (
        "index", "consumer-integration", "status", "qualification-evidence", "architecture", "implementation-profile",
        "configuration", "camera-profile-authoring", "camera-profile-tutorial",
        "camera-profile-examples", "backend-authoring",
        "camera-profile-portability", "backend-portability", "runtime-selection",
        "mel-surface-roadmap", "mel-traceability", "mel-traceability-evidence",
        "upstream-provenance", "stacked-image-provider",
        "image-los-metadata", "mono12-image-profile", "mode-command", "health-status",
        "software-bit", "subsystem-status", "navigation-reception",
        "navigation-backend-forwarding", "device-register-transport", "documentation-warnings",
    )),
}

# Repository prose uses inline Markdown links for local paths. Reference-style
# link definitions in the published guides use absolute upstream URLs.
LINK = re.compile(r"(!?)\[([^\[\]]+)\]\(([^\s()]+)\)")
FENCE = re.compile(r"^\s*(`{3,}|~{3,})")


def git(*args):
    return subprocess.check_output(["git", "-C", str(ROOT), *args]).decode()


def stage_guide(path, stage, tracked, sources, sha):
    def rewrite(match):
        image, label, target = match.groups()
        # Doxygen's Markdown-to-reference conversion can consume subsequent
        # paragraphs when it encounters a wrapped link label.
        label = re.sub(r"\s*\n\s*", " ", label)
        prefix = f"{image}[{label}]("
        url = urlsplit(target)
        if url.scheme or url.netloc or not url.path:
            return f"{prefix}{target})"
        resolved = (ROOT / path).parent / unquote(url.path)
        relative = resolved.resolve().relative_to(ROOT).as_posix()
        if relative == "docs/index.md":
            api_page = {"c-api-reference": "api_installed",
                        "camera-profile-c-interface": "api_extensions"}.get(url.fragment)
            if api_page:
                # GitHub lands on the index; the generated guide can go directly
                # to the reference for its audience.
                return f"{prefix}@ref {api_page})"
        if relative in GUIDES:
            # Doxygen resolves staged Markdown links and their heading IDs.
            return f"{prefix}{target})"
        if relative in sources and not url.fragment:
            # Doxygen file reference (documentation page, then line-numbered source).
            return f"{prefix}@ref {relative})"
        directory = relative not in tracked and any(p.startswith(relative + "/") for p in tracked)
        if not directory and relative not in tracked:
            raise ValueError(f"{path}: repository link target not tracked: {target}")
        if image:
            raise ValueError(f"{path}: local images are not publication inputs: {target}")
        route = "tree" if directory else "blob"
        fragment = "#" + url.fragment if url.fragment else ""
        return f"{prefix}{REPOSITORY}/{route}/{sha}/{quote(relative)}{fragment})"

    chunks, prose, fence = [], [], None
    for line in (ROOT / path).read_text(encoding="utf-8").splitlines(keepends=True):
        marker = FENCE.match(line)
        if marker:
            token = marker[1]
            if fence is None:
                chunks.append(LINK.sub(rewrite, "".join(prose)))
                prose.clear()
                fence = token
            elif token[0] == fence[0] and len(token) >= len(fence):
                fence = None
            chunks.append(line)
        else:
            (chunks if fence else prose).append(line)
    output = stage / path
    output.parent.mkdir(parents=True, exist_ok=True)
    text = "".join(chunks) + LINK.sub(rewrite, "".join(prose))
    if path == "docs/index.md":
        text = text.replace("<!-- generated-api-navigation -->", """- [Installed C++ interfaces](@ref api_installed) — for MEL consumers and native C++ integrators.
- [Camera extension interfaces (internal, uninstalled)](@ref api_extensions) — for profile and backend authors.
- [Core implementation reference (internal)](@ref api_core) — for runtime maintainers, with a line-numbered source browser.
""")
        text = text.replace("<!-- generated-profile-navigation -->",
                            "- [Camera profile C++ interface](@ref api_extensions)\n")
    if path == "docs/mel-traceability.md":
        text = text.replace("[C++ Implementation](@ref src/provider.cpp)",
                            "[C++ Implementation](@ref api_core)")
    output.write_text(text, encoding="utf-8")


def build(args):
    lock, operations = traceability.load()
    traceability.validate(lock, operations)
    traceability.negative_checks(lock, operations)
    sha = git("rev-parse", "HEAD").strip()
    dirty = bool(git("status", "--porcelain", "--untracked-files=no"))
    tracked = set(git("ls-files", "-z").split("\0"))
    evidence_ids = evidence.check(tracked, self_test=True)
    sources = project_sources(tracked)
    annotations = {p for p in tracked if p.startswith("cmake/doxygen/") and p.endswith(".dox")}
    for path in GUIDES | sources | annotations:
        source = ROOT / path
        if not source.is_file() or source.is_symlink() or source.resolve() != source:
            raise ValueError(f"Not a regular in-tree documentation input: {path}")
    check_status_source((ROOT / "docs/status.md").read_text(encoding="utf-8"))

    out = args.output.resolve()
    stage, html = out / "inputs", out / "html"
    for directory in (stage, html):
        if directory.exists():
            shutil.rmtree(directory)
    stage.mkdir(parents=True)
    for path in sorted(GUIDES):
        stage_guide(path, stage, tracked, sources, sha)
    (stage / "docs/mel-traceability-evidence.md").write_text(
        traceability.report(lock, operations, sha), encoding="utf-8")

    # Keep provenance visible on every page with Doxygen's standard footer.
    footer = stage / "footer.html"
    subprocess.run([args.doxygen, "-w", "html", "header.html", "footer.html", "doxygen.css"],
                   cwd=stage, check=True, stdout=subprocess.DEVNULL)
    footer_text = footer.read_text(encoding="utf-8")
    provenance = (f'<a id="source-commit" href="{REPOSITORY}/commit/{sha}">Source commit: {sha}</a>'
                  f'{" (local changes)" if dirty else ""} · '
                  f'<a href="{REPOSITORY}">GitHub repository</a> · '
                  f'<a href="{REPOSITORY}/blob/{sha}/LICENSE">Apache-2.0</a> · '
                  f'<a href="{REPOSITORY}/blob/{sha}/NOTICE">Attribution and notices</a>')
    if "</body>" not in footer_text:
        raise ValueError("Cannot add provenance to the Doxygen footer")
    footer.write_text(footer_text.replace("</body>",
                      f'<div class="footer">{provenance}</div>\n</body>'), encoding="utf-8")

    inputs = [ROOT / p for p in sorted(sources | annotations)] + sorted(GUIDES)
    version_definitions = " ".join(f"GIGE_MEL_VERSION_{key}={value}" for key, value in
                                   zip(("MAJOR", "MINOR", "PATCH"), args.version.split(".")))
    config = (ROOT / "cmake/Doxyfile").read_text(encoding="utf-8") + f"""
PROJECT_NUMBER = {args.version}
OUTPUT_DIRECTORY = "{out}"
STRIP_FROM_PATH = "{ROOT}" "{stage}"
INPUT = {" ".join(f'"{p}"' for p in inputs)}
USE_MDFILE_AS_MAINPAGE = docs/index.md
HTML_FOOTER = "{footer}"
WARN_LOGFILE = "{out / 'doxygen-warnings.log'}"
DOT_PATH = "{Path(args.dot).parent}"
PREDEFINED += {version_definitions} GenTLMajorVersion=1 GenTLMinorVersion=6 GC_GENTL_HEADER_VERSION=106
"""
    (out / "doxygen-warnings.log").write_text("", encoding="utf-8")
    (out / "Doxyfile").write_text(config, encoding="utf-8")
    subprocess.run([args.doxygen, out / "Doxyfile"], cwd=stage, check=True,
                   env={**os.environ, "LC_ALL": "C"})
    # Doxygen's crawler helper bypasses the configured HTML footer.
    crawler = html / "doxygen_crawl.html"
    if crawler.is_file():
        text = crawler.read_text(encoding="utf-8")
        crawler.write_text(text.replace("</body>", f'<div class="footer">{provenance}</div>\n</body>'),
                           encoding="utf-8")
    # Doxygen retains graph caches even with DOT_CLEANUP enabled. The HTML
    # already embeds the graph maps and needs only the rendered SVG assets.
    for pattern in ("*.map", "*.md5"):
        for cache in html.glob(pattern):
            cache.unlink()
    warnings = out / "doxygen-warnings.log"
    print(f"Doxygen warnings: {warnings.read_text(encoding='utf-8').count('warning:')} ({warnings})", flush=True)
    validate_site(html, sha, sources, evidence_ids)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--doxygen", required=True)
    parser.add_argument("--dot", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--version", required=True)
    build(parser.parse_args())