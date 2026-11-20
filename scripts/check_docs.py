#!/usr/bin/env python3
"""Validate a Doxygen site or verify the critical entry points on GitHub Pages."""

import argparse
from html.parser import HTMLParser
from pathlib import Path
import posixpath
import re
import subprocess
import time
from urllib.parse import unquote, urlsplit
from urllib.request import urlopen

from check_proprietary import violation


REPOSITORY = "https://github.com/zackboll/ams-gra-gige-mel-provider"
PAGES = "https://zackboll.github.io/ams-gra-gige-mel-provider/"
CONSUMER_GUIDE = "md_docs_2consumer-integration.html"
PROFILE_TUTORIAL = "md_docs_2camera-profile-tutorial.html"
PROFILE_EXAMPLES = "md_docs_2camera-profile-examples.html"
MEL_PROFILE = "md_docs_2implementation-profile.html"
TRACEABILITY = "md_docs_2mel-traceability.html"
TRACE_EVIDENCE = "md_docs_2mel-traceability-evidence.html"
QUALIFICATION = "md_docs_2qualification-evidence.html"
STATUS = "md_docs_2status.html"
BACKEND_AUTHORING = "md_docs_2backend-authoring.html"
QUALIFICATION_GROUPS = (
    "sdk-free-provider-and-profiles", "aravis-fake-and-simulated-gige",
    "cognex-cic-5000r-physical-observations", "pleora-ebus",
    "gentl-producers-and-consumer", "register-transport-and-navigation",
    "sanitizers-lifetime-and-package-isolation", "open-gates-and-next-actions",
)
QUALIFICATION_RECORDS = (
    "docs/validation.md#unpacked-mono12-gige-mel-022",
    "docs/cognex-cic-5000r.md#physical-capture-and-stream-evidence",
    "docs/gvsp-packet-spacing.md#10-hz-production-adapter",
    "docs/pleora-integration.md#initial-images-and-extended-acquisition",
    "docs/tasks/GIGE-MEL-GENTL-002-validation.md#b1-gige-mel-gentl-002-r1-final-source-qualification",
    "docs/tasks/GIGE-MEL-039-validation.md#server-observed-contracts",
    "docs/provider-thread-exit-tsan.md#qualification",
)
TRACE_LINKS = (
    "src/provider.cpp", "tests/provider_standby_tests.inc", "tests/install_consumer/main.cpp",
)
EXAMPLE_SOURCES = {
    "Cognex profile implementation": "camera_profiles/cognex_cic_5000r/profile.cpp",
    "Cognex profile declaration": "camera_profiles/cognex_cic_5000r/profile.hpp",
    "Cognex module entry point": "camera_profiles/cognex_cic_5000r/module.cpp",
    "Rate-limited profile implementation": "examples/profile_module/rate_limited_policy.hpp",
    "Rate-limited module descriptor": "examples/profile_module/module.cpp",
}
EXAMPLE_BUILDS = {
    "Cognex CMakeLists.txt": "camera_profiles/cognex_cic_5000r/CMakeLists.txt",
    "Rate-limited CMakeLists.txt": "examples/profile_module/CMakeLists.txt",
}
SECTIONS = {
    "api_installed.html": ("Installed C++ interfaces", (
        "make_api_manager", "ProviderSettings", "ProviderStatistics", "ICameraBackend",
        "ICameraDevice", "IFrameStream", "FrameLease", "FrameDescriptor", "CameraConfiguration",
        "CameraCapabilities", "NavigationReport", "INavigationSink", "ILineOfSightSource",
        "ILineOfSightStream", "IStackedImageSource", "IStackedImageStream", "StackLease",
    )),
    "api_extensions.html": ("Internal, uninstalled extension interfaces", (
        "ICameraProfile", "ICameraProfileState", "IDeviceFeatures", "BackendCatalog",
        "Descriptor", "load_profile_module", "select_session", "validate_frame", "apply_configuration",
    )),
    "api_core.html": ("Core implementation reference", (
        "provider.cpp", "factories.cpp", "synthetic.cpp", "aravis_backend.cpp",
        "pleora_backend.cpp", "gentl_backend.cpp", "config_parser.cpp", "gvcp_control_owner.cpp",
    )),
    PROFILE_EXAMPLES: ("internal development references", tuple(EXAMPLE_SOURCES)),
}
SEARCH = ("search/search.js", "search/searchdata.js")
ENTRY_PAGES = ("index.html", CONSUMER_GUIDE, PROFILE_TUTORIAL, MEL_PROFILE, TRACEABILITY,
               TRACE_EVIDENCE, QUALIFICATION, STATUS, BACKEND_AUTHORING, *SECTIONS)
ENTRY_LINKS = {
    "index.html": (CONSUMER_GUIDE, PROFILE_TUTORIAL, "api_installed.html", "api_extensions.html",
                   "api_core.html", MEL_PROFILE, TRACEABILITY, QUALIFICATION),
    MEL_PROFILE: (TRACEABILITY,),
    TRACEABILITY: (MEL_PROFILE, "api_core.html", TRACE_EVIDENCE),
    "api_core.html": (TRACEABILITY, TRACE_EVIDENCE),
    TRACE_EVIDENCE: (TRACEABILITY, "api_core.html", STATUS),
    QUALIFICATION: (STATUS, TRACEABILITY),
    STATUS: (QUALIFICATION,),
    CONSUMER_GUIDE: ("api_installed.html", "provider_8hpp.html", "backend_8hpp.html",
                     "navigation_8hpp.html", "line__of__sight_8hpp.html"),
    "api_installed.html": (CONSUMER_GUIDE,),
    PROFILE_TUTORIAL: ("api_extensions.html", PROFILE_EXAMPLES, "md_docs_2camera-profile-authoring.html"),
    "api_extensions.html": (PROFILE_TUTORIAL, PROFILE_EXAMPLES),
    PROFILE_EXAMPLES: ("api_extensions.html", PROFILE_TUTORIAL, "camera__profile_8hpp.html",
                       "device__features_8hpp.html", "frame__policy_8hpp.html", "profile__module_8hpp.html",
                       "profile__loader_8hpp.html"),
}


def project_sources(paths):
    tracked = set(paths)
    missing = set(EXAMPLE_SOURCES.values()) - tracked
    if missing:
        raise ValueError(f"Missing tracked profile example sources: {', '.join(sorted(missing))}")
    return {p for p in tracked if p.startswith(("include/gige_mel/", "src/"))
            and p.endswith((".hpp", ".cpp"))} | set(EXAMPLE_SOURCES.values())


class Page(HTMLParser):
    def __init__(self, text):
        super().__init__()
        self.anchors = set()
        self.links = []
        self.assets = []
        self.breadcrumb = []
        self.in_breadcrumb_item = False
        self.current_link = None
        self.link_text = []
        self.feed(text)

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        for key in ("id", "name"):
            if key in attrs:
                self.anchors.add(attrs[key])
        if tag == "a" and "href" in attrs:
            self.current_link = attrs["href"]
            self.link_text = []
        elif tag == "li" and "navelem" in attrs.get("class", "").split():
            self.in_breadcrumb_item = True
        elif tag in {"img", "script", "iframe", "link", "object"}:
            key = "href" if tag == "link" else "data" if tag == "object" else "src"
            if key in attrs:
                self.assets.append(attrs[key])

    def handle_data(self, data):
        if self.current_link is not None:
            self.link_text.append(data)

    def handle_endtag(self, tag):
        if tag == "li":
            self.in_breadcrumb_item = False
        if tag == "a" and self.current_link is not None:
            link = (self.current_link, " ".join("".join(self.link_text).split()))
            self.links.append(link)
            if self.in_breadcrumb_item:
                self.breadcrumb.append(link)
            self.current_link = None


def local_target(page, href):
    url = urlsplit(href)
    if url.scheme or url.netloc:
        return None
    target = (posixpath.normpath(posixpath.join(posixpath.dirname(page), unquote(url.path)))
              if url.path else page)
    return target, unquote(url.fragment)


def section_targets(page, parsed):
    targets = {}
    for href, label in parsed.links:
        if not label:
            continue
        symbol = label.rsplit("::", 1)[-1].removesuffix("()")
        resolved = local_target(page, href)
        if resolved and resolved[0] != page and resolved[0].endswith(".html"):
            targets.setdefault(symbol, resolved)
    return targets


def verify_entry(page, parsed, text, sha):
    if (f"{REPOSITORY}/commit/{sha}", f"Source commit: {sha}") not in parsed.links:
        raise ValueError(f"{page}: missing source provenance for {sha}")
    linked = {local_target(page, href) for href, _ in parsed.links}
    for target in ENTRY_LINKS.get(page, ()):
        if (target, "") not in linked:
            raise ValueError(f"{page}: no navigation to {target}")
    for fragment in {PROFILE_TUTORIAL: (QUALIFICATION_GROUPS[0], QUALIFICATION_GROUPS[2]),
                     BACKEND_AUTHORING: (QUALIFICATION_GROUPS[1], QUALIFICATION_GROUPS[3],
                                         QUALIFICATION_GROUPS[4])}.get(page, ()):
        if (QUALIFICATION, fragment) not in linked:
            raise ValueError(f"{page}: no navigation to qualification evidence #{fragment}")
    if page == QUALIFICATION:
        for fragment in QUALIFICATION_GROUPS:
            if fragment not in parsed.anchors or (page, fragment) not in linked:
                raise ValueError(f"{page}: no evidence navigation to #{fragment}")
        urls = {href for href, _ in parsed.links}
        for source in QUALIFICATION_RECORDS:
            if f"{REPOSITORY}/blob/{sha}/{source}" not in urls:
                raise ValueError(f"{page}: missing revision-pinned original record: {source}")
    if page == PROFILE_EXAMPLES:
        for label, source in EXAMPLE_BUILDS.items():
            if (f"{REPOSITORY}/blob/{sha}/{source}", label) not in parsed.links:
                raise ValueError(f"{page}: missing revision-pinned link for {source}")
    if page == TRACE_EVIDENCE:
        urls = {href for href, _ in parsed.links}
        for source in TRACE_LINKS:
            if f"{REPOSITORY}/blob/{sha}/{source}" not in urls:
                raise ValueError(f"{page}: missing revision-pinned evidence/implementation link: {source}")
    if page in SECTIONS:
        title, symbols = SECTIONS[page]
        if title.casefold() not in text.casefold():
            raise ValueError(f"{page}: missing audience label: {title}")
        targets = section_targets(page, parsed)
        for symbol in symbols:
            if symbol not in targets:
                raise ValueError(f"{page}: no API link for {symbol}")
            if symbol == "make_api_manager" and not targets[symbol][1]:
                raise ValueError("make_api_manager must link to its generated definition anchor")
        return {symbol: targets[symbol] for symbol in symbols}
    return {}


def validate_site(root, sha, sources, evidence_ids=None):
    allowed = {".html", ".css", ".js", ".svg", ".png"}
    files, pages, errors = set(), {}, []
    for item in sorted(root.rglob("*")):
        path = item.relative_to(root).as_posix()
        if item.is_symlink():
            errors.append(f"{path}: symlinks must not be published")
        elif item.is_dir():
            if path != "search":
                errors.append(f"{path}: unexpected publication directory")
        elif not item.is_file() or item.suffix not in allowed:
            errors.append(f"{path}: unexpected publication file")
        else:
            files.add(path)
            data = item.read_bytes()
            reason = violation(path, "100644", data)
            if reason:
                errors.append(f"{path}: {reason}")
            if item.suffix == ".html":
                pages[path] = Page(data.decode("utf-8"))

    for path in (*ENTRY_PAGES, "files.html", *SEARCH):
        if path not in files:
            errors.append(f"Missing required generated file: {path}")
    if evidence_ids is not None and QUALIFICATION in pages:
        text = (root / QUALIFICATION).read_text(encoding="utf-8")
        rows = re.findall(r"<td[^>]*>\s*(QE-[A-Z0-9-]+) — <a\s", text)
        if len(rows) != len(evidence_ids) or set(rows) != evidence_ids:
            errors.append(f"{QUALIFICATION}: not all evidence rows rendered with original record links")
    search_index = "\n".join((root / path).read_text(encoding="utf-8") for path in sorted(files)
                             if re.fullmatch(r"search/all_[0-9a-f]+\.js", path))

    for path, parsed in pages.items():
        if (f"{REPOSITORY}/commit/{sha}", f"Source commit: {sha}") not in parsed.links:
            errors.append(f"{path}: missing source provenance for {sha}")
        for href in {*parsed.assets, *(url for url, _ in parsed.links)}:
            target = local_target(path, href)
            if target and (target[0] not in files or
                           (target[1] and target[0] in pages and target[1] not in pages[target[0]].anchors)):
                errors.append(f"{path}: broken local URL: {href}")

    for target in (TRACEABILITY, TRACE_EVIDENCE, TRACE_EVIDENCE + "#c2channel-modecmd"):
        if f"'../{target}'" not in search_index:
            errors.append(f"Missing traceability search result: {target}")

    for path in ENTRY_PAGES:
        if path not in pages:
            continue
        targets = verify_entry(path, pages[path], (root / path).read_text(encoding="utf-8"), sha)
        for symbol, (target, fragment) in targets.items():
            if target not in pages:
                errors.append(f"{symbol}: missing API page: {target}")
            elif target.endswith("_source.html") or (symbol.endswith(".cpp") and not any(
                    href.endswith("_source.html") for href, _ in pages[target].links)):
                errors.append(f"{symbol}: missing API-to-source navigation: {target}")
            search_url = "../" + target + ("#" + fragment if fragment else "")
            if f"'{search_url}'" not in search_index:
                errors.append(f"{symbol}: API target is not in Doxygen search: {search_url}")

    # Resolve the full repository path from Doxygen's source breadcrumbs, not
    # HTML basename/suffix matches (both example modules are named module.cpp).
    source_by_path = {}
    for page, parsed in pages.items():
        if not page.endswith("_source.html"):
            continue
        source = "/".join(label for _, label in parsed.breadcrumb)
        if source not in sources or source in source_by_path:
            errors.append(f"Unexpected or duplicate source browser: {page} ({source})")
            continue
        source_by_path[source] = page
        if "l00001" not in parsed.anchors:
            errors.append(f"Missing line-numbered browser for {source}")
    for source in sorted(sources - source_by_path.keys()):
        errors.append(f"Missing source browser for {source}")

    if PROFILE_EXAMPLES in pages:
        targets = section_targets(PROFILE_EXAMPLES, pages[PROFILE_EXAMPLES])
        for label, source in EXAMPLE_SOURCES.items():
            target, _ = targets[label]
            browser = source_by_path.get(source)
            if not browser or target not in pages:
                continue  # The missing page/browser was already reported above.
            forward_links = {local_target(target, href) for href, _ in pages[target].links}
            back_link = local_target(browser, pages[browser].breadcrumb[-1][0])
            if (browser, "") not in forward_links or back_link != (target, ""):
                errors.append(f"{label}: file and source links must resolve to {source}")

    if errors:
        raise ValueError("Documentation validation failed:\n" + "\n".join(sorted(set(errors))))
    print(f"Verified {len(pages)} HTML pages, {len(sources)} source browsers, search, "
          f"navigation and publication boundary at {sha}")


def verify_live(sha):
    def fetch(path):
        url = PAGES + path + f"?commit={sha}"
        with urlopen(url, timeout=30) as response:
            return response.read().decode("utf-8")

    for attempt in range(6):
        try:
            targets = set()
            for path in ENTRY_PAGES:
                text = fetch(path)
                targets.update(verify_entry(path, Page(text), text, sha).values())
            for target, fragment in sorted(targets):
                parsed = Page(fetch(target))
                verify_entry(target, parsed, "", sha)
                if fragment and fragment not in parsed.anchors:
                    raise ValueError(f"{target}: missing published API anchor {fragment}")
                if target.endswith(("_8cpp.html", "_8hpp.html")):
                    source = target.removesuffix(".html") + "_source.html"
                    if (source, "") not in {local_target(target, url) for url, _ in parsed.links}:
                        raise ValueError(f"{target}: missing published source link")
                    if "l00001" not in Page(fetch(source)).anchors:
                        raise ValueError(f"{source}: missing published source lines")
            for path in SEARCH:
                if not fetch(path).strip():
                    raise ValueError(f"Empty live search asset: {path}")
            print(f"Published site and critical entries verified: {PAGES} ({sha})")
            return
        except (OSError, ValueError) as error:
            if attempt == 5:
                raise
            print(f"Waiting for Pages to serve {sha}: {error}", flush=True)
            time.sleep(15)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--commit", default=None, help="Expected full Git commit (default: HEAD)")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--site", type=Path, help="Check a locally generated HTML directory")
    mode.add_argument("--live", action="store_true", help="Check the production Pages site")
    args = parser.parse_args()
    sha = args.commit or subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    if not re.fullmatch(r"[0-9a-f]{40}", sha):
        parser.error("expected a full lowercase Git commit SHA")
    if args.live:
        verify_live(sha)
    else:
        sources = subprocess.check_output(["git", "ls-files", "-z"], text=True)
        validate_site(args.site, sha, project_sources(sources.split("\0")))