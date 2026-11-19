#!/usr/bin/env python3
"""Validate a Doxygen site or verify the critical entry points on GitHub Pages."""

import argparse
from html.parser import HTMLParser
from pathlib import Path, PurePosixPath
import posixpath
import re
import subprocess
import time
from urllib.parse import unquote, urlsplit
from urllib.request import urlopen

from check_proprietary import violation


REPOSITORY = "https://github.com/zackboll/ams-gra-gige-mel-provider"
PAGES = "https://zackboll.github.io/ams-gra-gige-mel-provider/"
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
}
SEARCH = ("search/search.js", "search/searchdata.js")


def project_sources(paths):
    return {p for p in paths if p.startswith(("include/gige_mel/", "src/"))
            and p.endswith((".hpp", ".cpp"))}


class Page(HTMLParser):
    def __init__(self, text):
        super().__init__()
        self.anchors = set()
        self.links = []
        self.assets = []
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
        elif tag in {"img", "script", "iframe", "link", "object"}:
            key = "href" if tag == "link" else "data" if tag == "object" else "src"
            if key in attrs:
                self.assets.append(attrs[key])

    def handle_data(self, data):
        if self.current_link is not None:
            self.link_text.append(data)

    def handle_endtag(self, tag):
        if tag == "a" and self.current_link is not None:
            self.links.append((self.current_link, " ".join("".join(self.link_text).split())))
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
    if page == "index.html":
        linked = {local_target(page, href) for href, _ in parsed.links}
        for section in SECTIONS:
            if (section, "") not in linked:
                raise ValueError(f"{page}: no navigation to {section}")
    elif page in SECTIONS:
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


def validate_site(root, sha, sources):
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

    for path in ("index.html", "files.html", *SECTIONS, *SEARCH):
        if path not in files:
            errors.append(f"Missing required generated file: {path}")
    search_index = "\n".join((root / path).read_text(encoding="utf-8") for path in sorted(files)
                             if re.fullmatch(r"search/all_[0-9a-f]+\.js", path))

    for path, parsed in pages.items():
        for href in {*parsed.assets, *(url for url, _ in parsed.links)}:
            target = local_target(path, href)
            if target and (target[0] not in files or
                           (target[1] and target[0] in pages and target[1] not in pages[target[0]].anchors)):
                errors.append(f"{path}: broken local URL: {href}")

    for path in ("index.html", *SECTIONS):
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

    # File names come from project inputs only. Doxygen escapes each underscore
    # and encodes the extension; directory prefixes may disambiguate basenames.
    source_pages = {p for p in pages if p.endswith("_source.html")}
    for source in sorted(sources):
        name = PurePosixPath(source).name.replace("_", "__").replace(".", "_8") + "_source.html"
        matches = {name} & source_pages or {p for p in source_pages if p.endswith("_2" + name)}
        if len(matches) != 1 or "l00001" not in pages[next(iter(matches))].anchors:
            errors.append(f"Missing unique line-numbered browser for {source}")
        source_pages -= matches
    if source_pages:
        errors.append(f"Unexpected source browsers: {', '.join(sorted(source_pages))}")

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
            for path in ("index.html", *SECTIONS):
                text = fetch(path)
                targets.update(verify_entry(path, Page(text), text, sha).values())
            for target, fragment in sorted(targets):
                parsed = Page(fetch(target))
                verify_entry(target, parsed, "", sha)
                if fragment and fragment not in parsed.anchors:
                    raise ValueError(f"{target}: missing published API anchor {fragment}")
                if target.endswith("_8cpp.html"):
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
        sources = subprocess.check_output(["git", "ls-files", "-z", "include/gige_mel", "src"], text=True)
        validate_site(args.site, sha, project_sources(sources.split("\0")))