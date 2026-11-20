#!/usr/bin/env python3
"""Validate a Doxygen site or verify the critical entry points on GitHub Pages."""

import argparse
from html.parser import HTMLParser
import json
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
STATUS_MATRIX = "Backend and camera-profile matrix"
STATUS_HEADERS = ("Backend / profile", "Implemented path and availability", "Evidence and important limits")
GENTL_LABEL = "GenTL / injected existing profiles"
GENTL_EVIDENCE = "gentl-producers-and-consumer"
STATUS_BACKENDS = {
    "Synthetic / default", "Aravis / mono_free_running_v1", "Pleora / mono_free_running_v1",
    "Aravis or Pleora / cognex_cic_5000r_v1", GENTL_LABEL,
}
BACKEND_AUTHORING = "md_docs_2backend-authoring.html"
QUALIFICATION_GROUPS = (
    "sdk-free-provider-and-profiles", "aravis-fake-and-simulated-gige",
    "cognex-cic-5000r-physical-observations", "pleora-ebus",
    GENTL_EVIDENCE, "register-transport-and-navigation",
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
        "ProviderSettings", "ProviderStatistics", "ICameraBackend",
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
# Critical installed entry points must link to member documentation, not just
# the declaration or an automatically generated source/implementation link.
INSTALLED_METHODS = (
    "make_api_manager", "provider_statistics", "open", "apply", "receive", "request_stop",
    "submit_navigation", "open_line_of_sight", "open_stacked_images",
)
SEARCH = ("search/search.js", "search/searchdata.js")


def guide(name):
    return f"md_docs_2{name}.html"


HOME = "index.html"
GUIDES_INDEX = "pages.html"
GETTING_STARTED = guide("getting-started")
CAMERA = guide("camera-integration")
MEL = guide("mel-capabilities")
API = guide("cpp-api-reference")
ENGINEERING = guide("engineering-qualification")
INDEXES = guide("source-type-indexes")
NAVIGATION = guide("navigation-los")
QUICK_START = ("md_README.html", "sdk-free-quick-start")
HISTORICAL = guide("historical-validation")
EXPERIMENTS = guide("backend-transport-experiments")
DESIGN = guide("design-decisions")
REFERENCE_INDEXES = ("namespaces.html", "namespacemembers.html", "annotated.html", "classes.html",
                     "hierarchy.html", "functions.html", "files.html", "globals.html")
API_PAGES = ("api_installed.html", "api_extensions.html", "api_core.html")
EXTENSIONS = API_PAGES[1]
EXTENSION_SECTIONS = ("extension_profile", "extension_modules", "extension_registers", "extension_navigation")
EXTENSION_TYPES = tuple(f"gige_mel::{scope}::{name}" for scope, names in {
    "backend::profiles": ("ICameraProfile", "ICameraProfileState", "NativeFrameFacts"),
    "backend::device_features": ("IDeviceFeatures",),
    "backend::profiles::module": ("Descriptor",),
    "selection": ("BackendCatalog", "LoadedProfile"),
    "backend::device_registers": ("IRegisterDevice", "IDeviceRegisters"),
    "controlled_gvcp": ("IControlledRegisters", "Client", "ControlOwner"),
}.items() for name in names)
EXTENSION_METHODS = tuple(f"gige_mel::{scope}::{name}" for scope, names in {
    "backend::profiles::ICameraProfile": ("open",),
    "backend::profiles::ICameraProfileState": ("configuration", "apply", "preflight", "validate_frame"),
    "backend::device_features::IDeviceFeatures": ("read_integer_feature", "write_integer"),
    "selection::BackendCatalog": ("register_backend", "create"),
    "selection": ("select_session", "load_profile_module"),
    "backend::device_registers::IRegisterDevice": ("with_registers",),
    "backend::device_registers::IDeviceRegisters": ("write_memory",),
    "controlled_gvcp::Client": ("create",),
    "controlled_gvcp::ControlOwner": ("acquire", "shutdown"),
}.items() for name in names)
HIERARCHY = {
    HOME: (GETTING_STARTED, CAMERA, MEL, API, ENGINEERING),
    GETTING_STARTED: (QUICK_START[0], CONSUMER_GUIDE, STATUS),
    CAMERA: (guide("backend-selection"), PROFILE_TUTORIAL, guide("genicam-sdk-guide"),
             guide("pleora-guide"), guide("gentl-status")),
    guide("backend-selection"): (guide("runtime-selection"), BACKEND_AUTHORING),
    BACKEND_AUTHORING: (guide("backend-portability"),),
    PROFILE_TUTORIAL: (guide("camera-profile-authoring"), PROFILE_EXAMPLES, guide("camera-profile-portability")),
    MEL: (MEL_PROFILE, *map(guide, ("image-frame-metadata", "configuration", "health-status",
                                   "navigation-los", "stacked-image-provider"))),
    guide("image-frame-metadata"): (guide("mono12-image-profile"),),
    guide("configuration"): (guide("mode-command"),),
    guide("health-status"): (guide("software-bit"), guide("subsystem-status")),
    NAVIGATION: tuple(map(guide, ("navigation-reception", "navigation-backend-forwarding", "image-los-metadata"))),
    API: (*API_PAGES, INDEXES),
    ENGINEERING: (TRACEABILITY, QUALIFICATION, EXPERIMENTS, HISTORICAL, DESIGN),
    TRACEABILITY: (TRACE_EVIDENCE,),
    EXPERIMENTS: (guide("device-register-transport"),),
    DESIGN: (*map(guide, ("architecture", "mel-surface-roadmap", "upstream-provenance", "documentation-warnings")),
             "md_CONTRIBUTING.html"),
}
ORIGINAL_RECORDS = {
    EXPERIMENTS: ("docs/tasks/GIGE-MEL-GENTL-003-validation.md", "docs/gvcp-single-owner.md"),
    HISTORICAL: ("docs/validation.md", "docs/cognex-cic-5000r.md", "docs/provider-thread-exit-tsan.md"),
    DESIGN: ("docs/adr/0001-native-cpp-provider.md", "docs/adr/0002-copy-first-memory.md"),
    guide("genicam-sdk-guide"): ("docs/aravis-integration.md",),
    guide("pleora-guide"): ("docs/pleora-integration.md",),
}
ENTRY_PAGES = tuple(dict.fromkeys((GUIDES_INDEX, *HIERARCHY, *ORIGINAL_RECORDS, INDEXES, CONSUMER_GUIDE,
                                 MEL_PROFILE, TRACE_EVIDENCE, QUALIFICATION, STATUS, PROFILE_EXAMPLES, *SECTIONS)))
ENTRY_LINKS = {
    **HIERARCHY,
    HOME: (*HIERARCHY[HOME], CONSUMER_GUIDE, STATUS),
    NAVIGATION: (*HIERARCHY[NAVIGATION], "navigation_8hpp.html", "line__of__sight_8hpp.html"),
    INDEXES: REFERENCE_INDEXES,
    MEL_PROFILE: (TRACEABILITY,),
    TRACEABILITY: (*HIERARCHY[TRACEABILITY], MEL_PROFILE, "api_core.html"),
    "api_core.html": (TRACEABILITY, TRACE_EVIDENCE),
    TRACE_EVIDENCE: (TRACEABILITY, "api_core.html", STATUS),
    QUALIFICATION: (STATUS, TRACEABILITY),
    STATUS: (QUALIFICATION,),
    CONSUMER_GUIDE: ("api_installed.html", "provider_8hpp.html", "backend_8hpp.html",
                     "navigation_8hpp.html", "line__of__sight_8hpp.html"),
    "api_installed.html": (CONSUMER_GUIDE, BACKEND_AUTHORING),
    PROFILE_TUTORIAL: (*HIERARCHY[PROFILE_TUTORIAL], "api_extensions.html"),
    EXTENSIONS: (PROFILE_TUTORIAL, PROFILE_EXAMPLES, guide("camera-profile-authoring"), BACKEND_AUTHORING,
                 guide("runtime-selection"), guide("device-register-transport"), EXPERIMENTS,
                 guide("navigation-backend-forwarding"), QUALIFICATION),
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
        self.guide_rows = []
        self.body_links = []
        self.in_content = False
        self.guide_row = None
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
        elif tag == "tr" and re.fullmatch(r"row_(\d+_)+", attrs.get("id", "")):
            self.guide_row = attrs["id"]
        elif tag == "li" and "navelem" in attrs.get("class", "").split():
            self.in_breadcrumb_item = True
        elif tag == "div" and attrs.get("id") == "doc-content":
            self.in_content = True
        elif tag == "div" and attrs.get("id") in {"page-nav", "nav-path"}:
            self.in_content = False
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
        if tag == "tr":
            self.guide_row = None
        if tag == "a" and self.current_link is not None:
            link = (self.current_link, " ".join("".join(self.link_text).split()))
            self.links.append(link)
            if self.in_content:
                self.body_links.append(link)
            if self.guide_row:
                self.guide_rows.append((self.guide_row, self.current_link))
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


def guide_parents(rows):
    """Read Doxygen's Related Pages directory row IDs (e.g. row_1_0_)."""
    paths = dict(rows)
    if len(paths) != len(rows):
        raise ValueError("Guides index contains duplicate row IDs")
    for row, url in rows:
        parent = row.rstrip("_").rsplit("_", 1)[0] + "_"
        if parent != "row_" and parent not in paths:
            raise ValueError(f"Guides index missing parent for {url}")
        yield paths.get(parent, HOME), url


def sidebar_parents(root, pages, authored):
    """Read the pinned generator's JSON-shaped NAVTREE data without executing JS."""
    def read_tree(file, variable):
        text = (root / file).read_text(encoding="utf-8")
        data = re.search(rf"\bvar {re.escape(variable)} =\s*(\[.*?\n\s*\]);", text, re.S)
        if not data:
            raise ValueError(f"{file}: missing sidebar tree data")
        return json.loads(data[1])

    def visit(nodes, parent, seen):
        for label, url, children in nodes:
            if parent == HOME and label == "Reference Indexes":
                continue  # A shortcut, not a second structural parent for INDEXES.
            if url not in authored:
                continue  # Ignore sections and the separately checked reference group.
            if url in seen:
                raise ValueError(f"Sidebar cycle at {url}")
            yield parent, url
            if isinstance(children, str):
                children = read_tree(children + ".js", children.replace("-", "_"))
            if children:
                yield from visit(children, url, seen | {url})

    [(title, home, nodes)] = read_tree("navtreedata.js", "NAVTREE")
    if not title or home != HOME:
        raise ValueError("Sidebar has no project root")
    groups = [(url, children) for label, url, children in nodes if label == "Reference Indexes"]
    if len(nodes) != len(HIERARCHY[HOME]) + 1 or len(groups) != 1:
        raise ValueError("Sidebar must expose five guide roots and one reference-index group")
    url, indexes = groups[0]
    target, anchor = local_target(HOME, url)
    if target != INDEXES or (anchor and anchor not in pages[INDEXES].anchors):
        raise ValueError("Sidebar reference group must link to Source/Type Indexes")
    if {url for _, url, _ in indexes} != {"namespaces.html", "annotated.html", "files.html"}:
        raise ValueError("Sidebar reference group must retain namespaces, classes and files")
    yield from visit(nodes, HOME, {HOME})


def check_parents(edges, authored, name):
    parents = {}
    for parent, child in edges:
        if child in parents:
            raise ValueError(f"{name}: duplicate parent for {child}")
        parents[child] = parent
    for child in parents:
        visited = {child}
        parent = parents[child]
        while parent != HOME:
            if parent in visited:
                raise ValueError(f"{name}: cyclic parent at {parent}")
            if parent not in parents:
                raise ValueError(f"{name}: missing parent page {parent}")
            visited.add(parent)
            parent = parents[parent]
    expected = {child: parent for parent, children in HIERARCHY.items() for child in children}
    for child in sorted(authored | parents.keys() | expected.keys()):
        if child not in authored or child not in expected or parents.get(child) != expected[child]:
            raise ValueError(f"{name}: wrong or missing parent for {child}: "
                             f"expected {expected.get(child)}, found {parents.get(child)}")


def check_navigation(root, pages, search_index):
    authored = {path for path in pages if path.startswith(("md_", "api_"))}
    if any(guide(Path(record).stem) in authored for records in ORIGINAL_RECORDS.values() for record in records):
        raise ValueError("Original historical records must be linked, not published as guides")
    related = list(guide_parents(pages[GUIDES_INDEX].guide_rows))
    check_parents(related, authored, "Guides index")
    check_parents(sidebar_parents(root, pages, authored), authored, "Sidebar")
    for guide_url in (*HIERARCHY[HOME], CONSUMER_GUIDE, PROFILE_TUTORIAL, NAVIGATION,
                      QUALIFICATION, HISTORICAL, EXPERIMENTS, STATUS, *API_PAGES):
        if f"'../{guide_url}'" not in search_index:
            raise ValueError(f"Missing guide search result: {guide_url}")

    # Focused negative controls against generated data, not mock page IDs.
    try:
        check_parents([edge for edge in related if edge != (GETTING_STARTED, CONSUMER_GUIDE)],
                      authored, "Guides index")
    except ValueError as error:
        if f"missing parent for {CONSUMER_GUIDE}" not in str(error):
            raise
    else:
        raise ValueError("Guide parent negative control accepted a missing child")
    try:
        check_entry_links(API, [(href, label) for href, label in pages[API].body_links
                                if href != "api_installed.html"])
    except ValueError as error:
        if "no navigation to api_installed.html" not in str(error):
            raise
    else:
        raise ValueError("Reference path negative control accepted a missing Installed Interfaces link")
    print(f"Verified {len(authored)} authored pages in Guides and sidebar, and 2 navigation negative controls")


def check_status_source(text):
    """Check this one simple pipe table, not arbitrary Markdown."""
    _, heading, section = text.partition(f"\n## {STATUS_MATRIX}\n\n")
    if not heading:
        raise ValueError("docs/status.md: missing backend matrix heading")
    lines = section.split("\n\n", 1)[0].splitlines()

    def validate(lines):
        def cells(line):
            match = re.fullmatch(r"\|([^|]*)\|([^|]*)\|([^|]*)\|", line.strip())
            if not match:
                raise ValueError("docs/status.md: expected three backend matrix columns")
            return tuple(cell.strip() for cell in match.groups())

        if not lines or cells(lines[0]) != STATUS_HEADERS:
            raise ValueError("docs/status.md: invalid backend matrix header")
        if len(lines) < 2 or not all(re.fullmatch(r":?-{3,}:?", cell) for cell in cells(lines[1])):
            raise ValueError("docs/status.md: missing backend matrix separator after header")
        rows = [cells(line) for line in lines[2:]]
        labels = [row[0].replace("`", "") for row in rows]
        if len(labels) != len(STATUS_BACKENDS) or set(labels) != STATUS_BACKENDS:
            raise ValueError("docs/status.md: expected exactly one row per backend/profile")
        evidence = rows[labels.index(GENTL_LABEL)][2]
        if not re.search(rf"\[[^\]]+\]\(qualification-evidence\.md#{GENTL_EVIDENCE}\)", evidence):
            raise ValueError("docs/status.md: missing GenTL qualification-evidence link")

    validate(lines)
    gentl_row = next(line for line in lines[2:] if line.split("|")[1].strip() == GENTL_LABEL)
    for invalid, expected in ((lines[:1] + lines[2:], "missing backend matrix separator"),
                              (lines + [gentl_row], "exactly one row per backend/profile")):
        try:
            validate(invalid)
        except ValueError as error:
            if expected not in str(error):
                raise
        else:
            raise ValueError(f"Backend matrix negative control did not fail: {expected}")
    print("Verified status Markdown matrix and rejected both negative controls (separator, duplicate GenTL)")


def verify_rendered_status_matrix(text):
    # Match the table immediately after the specific Doxygen heading, not the
    # separate MEL behavior table later on the same page.
    matrix = re.search(r'id="backend-and-camera-profile-matrix"></a>[^<]*</h[1-6]>\s*'
                       r'<table\b[^>]*>(.*?)</table>', text, re.S)
    if not matrix:
        raise ValueError(f"{STATUS}: backend matrix did not render as a table")
    head, *rows = re.findall(r"<tr\b[^>]*>(.*?)</tr>", matrix[1], re.S) or ("",)
    headers = tuple(cell.strip() for cell in re.findall(r"<th\b[^>]*>(.*?)</th>", head, re.S))
    cells = [re.findall(r"<td\b[^>]*>(.*?)</td>", row, re.S) for row in rows]
    if headers != STATUS_HEADERS or any(len(row) != len(STATUS_HEADERS) for row in cells):
        raise ValueError(f"{STATUS}: expected three backend matrix columns")
    labels = [re.sub(r"<[^>]+>", "", row[0]).strip() for row in cells]
    if len(labels) != len(STATUS_BACKENDS) or set(labels) != STATUS_BACKENDS:
        raise ValueError(f"{STATUS}: expected exactly one rendered row per backend/profile")
    evidence = Page(cells[labels.index(GENTL_LABEL)][2])
    if (QUALIFICATION, GENTL_EVIDENCE) not in {local_target(STATUS, href) for href, _ in evidence.links}:
        raise ValueError(f"{STATUS}: missing rendered GenTL qualification-evidence link")


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


def member_detail(text, anchor):
    # Doxygen emits an anchor and heading before each detailed memdoc.
    start = re.search(rf'<a id="{re.escape(anchor)}"[^>]*></a>\s*<h2 class="memtitle">', text)
    return text[start.end():].split('<h2 class="memtitle">', 1)[0] if start else ""


MEMBER_PROSE = re.compile(r'<div class="memdoc">\s*<p>(.*?)</p>', re.S)


def has_member_description(detail):
    member = MEMBER_PROSE.search(detail)
    prose = re.sub(r"<[^>]+>", "", member[1]).strip() if member else ""
    return bool(prose) and not prose.startswith(("Implemented in ", "Reimplemented "))


def check_installed_methods(root, targets):
    for symbol in INSTALLED_METHODS:
        target, anchor = targets.get(symbol, ("", ""))
        if not anchor or target.endswith("_source.html"):
            raise ValueError(f"Installed API: no member documentation link for {symbol}")
        text = (root / target).read_text(encoding="utf-8")
        if not has_member_description(member_detail(text, anchor)):
            raise ValueError(f"Installed API: no generated description for {symbol} ({target}#{anchor})")
    print(f"Verified {len(INSTALLED_METHODS)} installed API method descriptions")


def check_extension_member(symbol, detail):
    signature, _, _ = detail.partition('<div class="memdoc">')
    name = re.search(r'<td class="memname">(.*?)</td>', signature, re.S)
    if (not name or re.sub(r"<[^>]+>", "", name[1]).split()[-1:] != [symbol]
            or (symbol.endswith("::register_backend") and "ProfileBackendConstructor" not in signature)):
        raise ValueError(f"{EXTENSIONS}: wrong member declaration for {symbol}")
    if not has_member_description(detail):
        raise ValueError(f"{EXTENSIONS}: no generated description for {symbol}")


def check_internal_extensions(root, pages, search_index):
    extension = pages[EXTENSIONS]
    targets = {label: local_target(EXTENSIONS, href) for href, label in extension.body_links}
    indexed = {local_target("annotated.html", href) for href, _ in pages["annotated.html"].body_links}
    installed = {local_target("api_installed.html", href) for href, _ in pages["api_installed.html"].body_links}
    for symbol in (*EXTENSION_TYPES, *EXTENSION_METHODS):
        target, anchor = targets.get(symbol) or ("", "")
        member = symbol in EXTENSION_METHODS
        owner = symbol.rsplit("::", 1)[0] if member else symbol
        if (target not in pages or bool(anchor) != member
                or "::".join(label for _, label in pages[target].breadcrumb) != owner):
            raise ValueError(f"{EXTENSIONS}: no link to the declaration of {symbol}")
        if not member and (target, "") not in indexed:
            raise ValueError(f"{EXTENSIONS}: type missing from generated index: {symbol}")
        if (target, anchor) in installed:
            raise ValueError(f"Installed Interfaces must not list internal declaration {symbol}")
        search_url = f"../{target}" + (f"#{anchor}" if anchor else "")
        if f"'{search_url}'" not in search_index:
            raise ValueError(f"{EXTENSIONS}: declaration missing from search: {symbol}")
        if member:
            check_extension_member(symbol, member_detail((root / target).read_text(encoding="utf-8"), anchor))

    for section in EXTENSION_SECTIONS:
        if section not in extension.anchors or f"'../{EXTENSIONS}#{section}'" not in search_index:
            raise ValueError(f"{EXTENSIONS}: missing section or search entry: {section}")

    # Test against the generated detail for a real link, then substitute a
    # different valid anchor on the same class page (existence alone is insufficient).
    symbol = "gige_mel::backend::profiles::ICameraProfileState::configuration"
    target, anchor = targets[symbol]
    text = (root / target).read_text(encoding="utf-8")
    detail = member_detail(text, anchor)
    no_description, removed = MEMBER_PROSE.subn('<div class="memdoc">', detail, count=1)
    if removed != 1:
        raise ValueError(f"{EXTENSIONS}: could not remove description for negative control")
    wrong_anchor = targets[symbol.rsplit("::", 1)[0] + "::apply"][1]
    for invalid_detail, reason in ((no_description, "no generated description"),
                                   (member_detail(text, wrong_anchor), "wrong member declaration")):
        try:
            check_extension_member(symbol, invalid_detail)
        except ValueError as error:
            if reason not in str(error):
                raise
        else:
            raise ValueError(f"{EXTENSIONS}: negative control accepted {reason}")
    print(f"Verified {len(EXTENSION_TYPES)} internal types, {len(EXTENSION_METHODS)} member descriptions, "
          "section search and 2 negative controls (missing description, wrong member anchor)")


def check_entry_links(page, body_links):
    linked = {local_target(page, href) for href, _ in body_links}
    for target in ENTRY_LINKS.get(page, ()):
        if (target, "") not in linked:
            raise ValueError(f"{page}: no navigation to {target}")
    if page in {HOME, GETTING_STARTED} and QUICK_START not in linked:
        raise ValueError(f"{page}: no direct SDK-free quick start link")
    if page == HOME and [local_target(page, href) for href, label in body_links if label][:3] != [
            QUICK_START, (CONSUMER_GUIDE, ""), (STATUS, "")]:
        raise ValueError("Homepage must lead with the quick start, consumer guide and current status")


def verify_entry(page, parsed, text, sha):
    if (f"{REPOSITORY}/commit/{sha}", f"Source commit: {sha}") not in parsed.links:
        raise ValueError(f"{page}: missing source provenance for {sha}")
    check_entry_links(page, parsed.body_links)
    linked = {local_target(page, href) for href, _ in parsed.body_links}
    urls = {href for href, _ in parsed.body_links}
    for record in ORIGINAL_RECORDS.get(page, ()):
        if f"{REPOSITORY}/blob/{sha}/{record}" not in urls:
            raise ValueError(f"{page}: missing revision-pinned original record: {record}")
    if page == STATUS:
        verify_rendered_status_matrix(text)
    for fragment in {PROFILE_TUTORIAL: (QUALIFICATION_GROUPS[0], QUALIFICATION_GROUPS[2]),
                     BACKEND_AUTHORING: (QUALIFICATION_GROUPS[1], QUALIFICATION_GROUPS[3],
                                         QUALIFICATION_GROUPS[4])}.get(page, ()):
        if (QUALIFICATION, fragment) not in linked:
            raise ValueError(f"{page}: no navigation to qualification evidence #{fragment}")
    if page == QUALIFICATION:
        for fragment in QUALIFICATION_GROUPS:
            if fragment not in parsed.anchors or (page, fragment) not in linked:
                raise ValueError(f"{page}: no evidence navigation to #{fragment}")
        for source in QUALIFICATION_RECORDS:
            if f"{REPOSITORY}/blob/{sha}/{source}" not in urls:
                raise ValueError(f"{page}: missing revision-pinned original record: {source}")
    if page == PROFILE_EXAMPLES:
        for label, source in EXAMPLE_BUILDS.items():
            if (f"{REPOSITORY}/blob/{sha}/{source}", label) not in parsed.links:
                raise ValueError(f"{page}: missing revision-pinned link for {source}")
    if page == TRACE_EVIDENCE:
        for source in TRACE_LINKS:
            if f"{REPOSITORY}/blob/{sha}/{source}" not in urls:
                raise ValueError(f"{page}: missing revision-pinned evidence/implementation link: {source}")
    if page in SECTIONS:
        title, symbols = SECTIONS[page]
        if title.casefold() not in text.casefold():
            raise ValueError(f"{page}: missing audience label: {title}")
        targets = section_targets(page, parsed)
        if page == "api_installed.html":
            symbols = (*symbols, *INSTALLED_METHODS)
        for symbol in symbols:
            if symbol not in targets:
                raise ValueError(f"{page}: no API link for {symbol}")
            if symbol in INSTALLED_METHODS and not targets[symbol][1]:
                raise ValueError(f"{symbol} must link to its generated definition anchor")
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

    for path in (*ENTRY_PAGES, *REFERENCE_INDEXES, "navtreedata.js", "menudata.js", *SEARCH):
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
        if path == "api_installed.html":
            check_installed_methods(root, targets)
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
    check_internal_extensions(root, pages, search_index)
    check_navigation(root, pages, search_index)
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
                parsed = Page(text)
                targets.update(verify_entry(path, parsed, text, sha).values())
                if path == GUIDES_INDEX:
                    authored = {child for children in HIERARCHY.values() for child in children}
                    check_parents(guide_parents(parsed.guide_rows), authored, "Live Guides index")
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