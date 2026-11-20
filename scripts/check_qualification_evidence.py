#!/usr/bin/env python3
"""Check the SDK-free evidence index, not the truth of the historical results."""

import argparse
from functools import lru_cache
from pathlib import Path
import re
import subprocess
from urllib.parse import unquote, urlsplit


ROOT = Path(__file__).resolve().parents[1]
INDEX = "docs/qualification-evidence.md"
ROW_PREFIX = "| QE-"
ID = re.compile(r"QE-[A-Z0-9]+(?:-[A-Z0-9]+)*")
LINK = re.compile(r"\[([^\[\]]+)\]\(([^\s()]+)\)")
CLASSES = {
    "SDK-free contract", "Native SDK fixture", "Simulated network transport",
    "Physical-camera observation", "Blocked / not executed",
}
EXECUTED = {"Positive", "Negative", "Inconclusive"}
UNEXECUTED = {"Blocked", "Not executed"}


def prose(text):
    """Skip fenced examples when looking for headings and links."""
    fence = None
    for line in text.splitlines():
        marker = re.match(r"^\s*(`{3,}|~{3,})", line)
        if marker:
            token = marker[1]
            if fence is None:
                fence = token
            elif token[0] == fence[0] and len(token) >= len(fence):
                fence = None
        elif fence is None:
            yield line


def anchors(text):
    """GitHub-style anchors for the ATX headings and explicit anchors we link."""
    used = set()
    for line in prose(text):
        heading = re.match(r"^ {0,3}#{1,6}\s+(.+?)(?:\s+#+)?\s*$", line)
        if heading:
            label = LINK.sub(r"\1", heading[1]).lower()
            base = re.sub(r"[^\w -]", "", label).replace(" ", "-")
            slug, suffix = base, 0
            while slug in used:
                suffix += 1
                slug = f"{base}-{suffix}"
            used.add(slug)
        used.update(re.findall(r'<a\s+(?:name|id)="([a-z0-9_-]+)"', line))
    return used


def validate(text, tracked):
    @lru_cache(maxsize=None)
    def path_anchors(path):
        return anchors(text if path == INDEX else (ROOT / path).read_text(encoding="utf-8"))

    def target(href):
        url = urlsplit(href)
        if url.scheme or url.netloc:
            # External PRs and sources are supplemental, not original paths.
            if url.scheme != "https":
                raise ValueError(f"Unsupported external URL: {href}")
            return None
        path = ((ROOT / INDEX).parent / unquote(url.path)).resolve() if url.path else ROOT / INDEX
        if not path.is_relative_to(ROOT):
            raise ValueError(f"Link leaves the repository: {href}")
        path = path.relative_to(ROOT).as_posix()
        if path not in tracked or not (ROOT / path).is_file():
            raise ValueError(f"Link is not a tracked repository file: {href}")
        if url.fragment and (not path.endswith(".md") or unquote(url.fragment) not in path_anchors(path)):
            raise ValueError(f"Missing Markdown anchor: {href}")
        return path

    ids = set()
    for line in prose(text):
        refs = [target(href) for _, href in LINK.findall(line)]
        if not line.startswith(ROW_PREFIX):
            continue
        columns = [cell.strip() for cell in line.rstrip().split("|")[1:-1]]
        if len(columns) != 5 or not all(columns):
            raise ValueError(f"Incomplete evidence row: {line[:80]}")
        record, state, _, _, limits = columns
        identifier = record.split()[0]
        if not ID.fullmatch(identifier) or identifier in ids:
            raise ValueError(f"Invalid or duplicate evidence ID: {identifier}")
        ids.add(identifier)
        classification, separator, outcome = state.rpartition(" / ")
        if not separator or classification not in CLASSES:
            raise ValueError(f"Unapproved evidence class: {identifier}: {state}")
        allowed = UNEXECUTED if classification == "Blocked / not executed" else EXECUTED
        if outcome not in allowed:
            raise ValueError(f"Incompatible evidence outcome: {identifier}: {state}")
        if (outcome in UNEXECUTED | {"Inconclusive"}
                and not re.search(r"authoriz|await|requir|need|remain|gat[ei]", limits, re.I)):
            raise ValueError(f"Missing unresolved gate: {identifier}")
        originals = [path for path in refs[:len(LINK.findall(record))] if path]
        if not any(p.startswith("docs/") and p.endswith(".md") and p != INDEX for p in originals):
            raise ValueError(f"Missing original repository record: {identifier}")
    if not ids:
        raise ValueError("Missing evidence rows")
    return ids


def check(tracked, self_test=False):
    text = (ROOT / INDEX).read_text(encoding="utf-8")
    ids = validate(text, tracked)
    if self_test:
        row = next(line for line in text.splitlines() if line.startswith(ROW_PREFIX))
        controls = (
            (text.replace("validation.md#unpacked-mono12-gige-mel-022", "never-committed.md", 1), "not a tracked"),
            (text.replace("#unpacked-mono12-gige-mel-022", "#absent-anchor", 1), "Missing Markdown anchor"),
            (text + "\n" + row, "duplicate evidence ID"),
            (text.replace("SDK-free contract / Positive", "Production-qualified / Positive", 1), "Unapproved evidence class"),
            (text.replace("Blocked / not executed / Blocked", "Blocked / not executed / Positive", 1), "Incompatible evidence outcome"),
        )
        for invalid, reason in controls:
            try:
                validate(invalid, tracked)
            except ValueError as error:
                if reason in str(error):
                    continue
                raise
            raise ValueError(f"Negative control did not fail: {reason}")
        print(f"Verified {len(controls)} evidence-index negative controls")
    print(f"Verified {len(ids)} evidence entries, outcomes, tracked paths and Markdown anchors")
    return ids


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    tracked = set(subprocess.check_output(["git", "-C", str(ROOT), "ls-files", "-z"]).decode().split("\0"))
    check(tracked, args.self_test)