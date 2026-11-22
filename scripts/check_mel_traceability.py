#!/usr/bin/env python3
"""Verify the IR MEL declaration reconciliation and generate a review report offline."""

import argparse
from collections import Counter
from copy import deepcopy
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]
INVENTORY = ROOT / "docs/traceability"
STATUSES = ("supported", "partial", "unsupported stub", "unavailable", "blocked", "unresolved")
GAPS = {
    "required-incomplete": "Required functionality incomplete",
    "conditional-decision": "Conditional deployment or profile decisions",
    "optional-unimplemented": "Optional functionality not implemented",
    "interface-decision": "Upstream annotation, enum or interface decisions",
    "missing-positive-test": "Implemented behavior lacking direct positive tests",
    "sensor-source": "Real sensor or data sources needed",
    "hardware-qualification": "Hardware qualification still needed",
}
OP_KEYS = set("id family kind upstream related_upstream obligation applicability status availability "
              "metadata implementation behavior evidence gap responsibility gap_groups".split())
HEX = re.compile(r"[0-9a-f]{40}")


def require(ok, description):
    if not ok:
        raise ValueError(description)


def read_json(path):
    def no_duplicates(pairs):
        result = dict(pairs)
        require(len(result) == len(pairs), f"{path}: duplicate JSON property")
        return result
    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=no_duplicates)


def keys(obj, names, label):
    require(isinstance(obj, dict) and obj.keys() == set(names.split()), f"{label}: wrong fields")


def text(value):
    return isinstance(value, str) and bool(value.strip())


def relpath(value):
    return (text(value) and not PurePosixPath(value).is_absolute() and ".." not in value.split("/")
            and PurePosixPath(value).as_posix() == value)


def local_file(value):
    require(relpath(value), f"invalid repository path: {value}")
    path = ROOT / value
    require(path.is_file() and path.resolve() == path, f"missing or indirect repository file: {value}")
    return path


def git(repo, *args):
    return subprocess.check_output(["git", "-C", str(repo), *args])


def blob_id(data):
    return hashlib.sha1(f"blob {len(data)}\0".encode() + data).hexdigest()


def load():
    rows = []
    for path in sorted(INVENTORY.glob("*.json")):
        if path.name == "upstream-lock.json":
            continue
        doc = read_json(path)
        keys(doc, "schema family operations", path)
        require(doc["schema"] == 1 and isinstance(doc["operations"], list) and doc["operations"], path)
        require(all(o.get("family") == doc["family"] for o in doc["operations"]), f"{path}: family mismatch")
        rows.extend(doc["operations"])
    return read_json(INVENTORY / "upstream-lock.json"), rows


def validate(lock, rows, checkouts=None):
    keys(lock, "schema components files interfaces", "upstream-lock")
    require(lock["schema"] == 1, "unknown lock schema")
    manifest = read_json(ROOT / "third_party/manifest.json")
    pins = {c["id"]: c for c in manifest["components"]}
    vendored = {(f["component"], f["source_path"]): f for f in manifest["files"]}
    require(lock["components"].keys() == {"ir-mel", "common-mel"}, "wrong upstream components")
    for component, pin in lock["components"].items():
        keys(pin, "id repo revision tree tree_paths", component)
        require(all(pin[k] == v for k, v in pins[component].items()), f"{component}: pinned revision/repo mismatch")
        require(HEX.fullmatch(pin["tree"]) and type(pin["tree_paths"]) is int and pin["tree_paths"] > 0,
                f"{component}: invalid tree identity")
        if checkouts:
            repo = checkouts[component]
            require(git(repo, "rev-parse", "HEAD").decode().strip() == pin["revision"], f"{component}: checkout HEAD mismatch")
            require(git(repo, "rev-parse", "HEAD^{tree}").decode().strip() == pin["tree"], f"{component}: tree mismatch")
            require(len(git(repo, "ls-tree", "-rz", "--name-only", "HEAD").split(b"\0")) - 1 == pin["tree_paths"],
                    f"{component}: tree path count mismatch")

    files, sources = {}, {}
    for f in lock["files"]:
        keys(f, "component path blob vendored", "locked upstream file")
        identity = f["component"], f["path"]
        require(identity not in files and identity[0] in lock["components"] and relpath(identity[1])
                and identity[1].startswith("include/") and HEX.fullmatch(f["blob"]), f"bad upstream file: {identity}")
        files[identity] = f
        entry = vendored.get(identity)
        require(f["vendored"] == ("third_party/" + entry["path"] if entry else None), f"{identity}: vendor path mismatch")
        if entry:
            data = local_file(f["vendored"]).read_bytes()
            require(blob_id(data) == f["blob"] == entry["git_blob_sha1"], f"{identity}: vendored blob mismatch")
            sources[identity] = data.decode().splitlines()
        if checkouts:
            data = git(checkouts[identity[0]], "show", "HEAD:" + identity[1])
            require(blob_id(data) == f["blob"], f"{identity}: pinned blob mismatch")
            sources[identity] = data.decode().splitlines()

    expected, overloads = {}, set()
    for h in lock["interfaces"]:
        keys(h, "component path interface class_line class_annotation inherits declarations", "interface")
        identity = h["component"], h["path"]
        require(identity in files and h["declarations"], f"{identity}: unlocked interface")
        lines = sources.get(identity)
        if lines and h["class_line"]:
            lineno = h["class_line"]
            require(re.match(rf"\s*class {re.escape(h['interface'])}\b", lines[lineno - 1]), f"{identity}: class line drift")
            require(not h["class_annotation"] or h["class_annotation"] in annotations(lines, lineno),
                    f"{identity}: class annotation drift")
        for d in h["declarations"]:
            keys(d, "id component path declaring signature line annotations", "locked declaration")
            identifier = d["id"]
            overload = (d["declaring"], d["signature"])
            require(text(identifier) and identifier not in expected and overload not in overloads,
                    f"duplicate locked identity: {identifier}")
            require((d["component"], d["path"]) == identity and text(d["declaring"]) and text(d["signature"])
                    and type(d["line"]) is int and d["line"] > 0 and isinstance(d["annotations"], list),
                    f"{identifier}: invalid declaration")
            if lines:
                start = d["line"]
                snippet = " ".join(" ".join(lines[start - 1:start + 10]).split())
                require(snippet.startswith(d["signature"]) and snippet[len(d["signature"])] in "; {",
                        f"{identifier}: exact signature/line drift")
                require(d["annotations"] == annotations(lines, start), f"{identifier}: method annotation drift")
            expected[identifier] = {k: v for k, v in d.items() if k != "id"}
            overloads.add(overload)

    seen = set()
    for o in rows:
        require(isinstance(o, dict) and o.keys() == OP_KEYS, f"wrong operation fields: {o.get('id', '?')}")
        i = o["id"]
        require(i not in seen and i in expected, f"duplicate or unrecognized operation: {i}")
        seen.add(i)
        require(o["upstream"] == expected[i], f"{i}: signature/annotation not in reconciliation lock")
        require(o["kind"] in ("operation", "type seam") and text(o["family"]) and text(o["behavior"])
                and text(o["availability"]), f"{i}: invalid kind, family, behavior or availability")
        require(o["obligation"] in ("required", "conditional", "optional", "unresolved") and o["status"] in STATUSES,
                f"{i}: invalid obligation/status")
        tags = o["upstream"]["annotations"]
        if o["obligation"] != "unresolved" and tags:
            inferred = "conditional" if any(t.startswith("@RequiredIf") for t in tags) else (
                "required" if "@Required" in tags else "optional")
            require(o["obligation"] == inferred, f"{i}: obligation contradicts annotation")
        keys(o["applicability"], "condition current", i)
        require(text(o["applicability"]["condition"]) and o["applicability"]["current"] in (
            "applies", "not-current", "fixture-only", "unresolved"), f"{i}: invalid applicability")
        require(o["responsibility"] in ("provider", "backend", "camera profile", "detector/sensor", "external system"),
                f"{i}: unknown responsibility")
        for field in ("related_upstream", "implementation", "evidence", "gap_groups"):
            require(isinstance(o[field], list), f"{i}: {field} must be a list")
        require(all(g in GAPS for g in o["gap_groups"]) and (o["gap"] is None or text(o["gap"]))
                and (text(o["gap"]) or (not o["gap_groups"] and o["status"] == "supported")),
                f"{i}: missing or invalid gap")
        for r in o["related_upstream"]:
            keys(r, "component path", i)
            require((r["component"], r["path"]) in files, f"{i}: unlocked upstream source")
        for r in o["implementation"]:
            keys(r, "path symbol", i)
            local_file(r["path"])
            require(text(r["symbol"]), f"{i}: missing implementation symbol")
        positives = []
        for e in o["evidence"]:
            keys(e, "path scenario result level demonstrates", i)
            path = local_file(e["path"])
            require(e["path"].startswith("tests/") and text(e["demonstrates"]) and text(e["scenario"])
                    and e["result"] in ("positive", "negative", "indirect") and e["level"] in (
                        "SDK-free", "native SDK fixture", "simulated transport", "physical-camera observation"),
                    f"{i}: invalid evidence")
            require(re.search(rf"\b{re.escape(e['scenario'])}\s*\(", path.read_text(encoding="utf-8")),
                    f"{i}: missing scenario {e['scenario']} in {e['path']}")
            if e["result"] == "positive":
                positives.append(e)
        meta = o["metadata"]
        if meta is not None:
            keys(meta, "enumerator advertisement", i)
            require(meta["advertisement"] in ("advertised", "source-gated", "not advertised", "missing enumerator")
                    and (text(meta["enumerator"]) if meta["advertisement"] != "missing enumerator" else meta["enumerator"] is None),
                    f"{i}: invalid metadata advertisement")
        if o["status"] in ("supported", "partial", "unsupported stub", "blocked"):
            require(o["implementation"], f"{i}: missing implementation reference")
        if o["status"] == "supported":
            require(positives, f"{i}: supported without positive test evidence")
            require(meta is None or meta["advertisement"] in ("advertised", "source-gated"), f"{i}: supported callback not advertised")
        if o["status"] in ("unsupported stub", "unavailable", "blocked", "unresolved"):
            require(not positives and (meta is None or meta["advertisement"] in ("not advertised", "missing enumerator")),
                    f"{i}: unavailable behavior marked positive/advertised")
    require(seen == expected.keys(), f"missing dispositions: {', '.join(sorted(expected.keys() - seen))}")


def annotations(lines, start):
    tags = []
    for line in reversed(lines[:start - 1]):
        if line.lstrip().startswith("#"):
            continue  # Clang linkage pragmas between the global annotation and declaration.
        if not line.lstrip().startswith("///"):
            break
        match = re.match(r"\s*///\s*(@(?:Required\w*|Optional))\b", line)
        if match:
            tags.append(match[1])
    return tags[::-1]


def negative_checks(lock, rows):
    def row(items, identifier):
        return next(o for o in items if o["id"] == identifier)

    def supported(items):
        return row(items, "Control.init")

    def blocked(items):
        return row(items, "HealthStatusChannel.cb-DiscreteStatus")

    cases = (
        ("duplicate", lambda l, r: r.append(r[0])),
        ("duplicate locked identity", lambda l, r: l["interfaces"][0]["declarations"].append(
            dict(l["interfaces"][0]["declarations"][0], id="different-id-same-overload"))),
        ("missing dispositions", lambda l, r: r.pop()),
        ("signature/annotation", lambda l, r: supported(r)["upstream"].update(signature="wrong overload")),
        ("pinned revision", lambda l, r: l["components"]["ir-mel"].update(revision="0" * 40)),
        ("unlocked upstream", lambda l, r: supported(r)["related_upstream"].append({"component": "ir-mel", "path": "include/missing.h"})),
        ("missing or indirect", lambda l, r: supported(r)["implementation"][0].update(path="src/absent.cpp")),
        ("missing or indirect", lambda l, r: supported(r)["evidence"][0].update(path="tests/absent.cpp")),
        ("missing scenario", lambda l, r: supported(r)["evidence"][0].update(scenario="nonexistent_scenario")),
        ("without positive", lambda l, r: supported(r).update(evidence=blocked(r)["evidence"])),
        ("marked positive/advertised", lambda l, r: blocked(r)["evidence"][0].update(result="positive")),
        ("supported callback not advertised", lambda l, r: blocked(r).update(status="supported", evidence=supported(r)["evidence"])),
        ("contradicts annotation", lambda l, r: supported(r).update(obligation="optional")),
    )
    for message, mutate in cases:
        copied_lock, copied_rows = deepcopy(lock), deepcopy(rows)
        mutate(copied_lock, copied_rows)
        try:
            validate(copied_lock, copied_rows)
        except ValueError as err:
            require(message in str(err), f"negative case {message!r} failed for wrong reason: {err}")
        else:
            raise ValueError(f"validator accepted negative case: {message}")
    print(f"Traceability validator rejected {len(cases)} negative controls")


def counts(rows):
    lines = ["| Family | Operations | Type seams | " + " | ".join(STATUSES) + " |",
             "| --- | ---: | ---: | " + " | ".join(["---:"] * len(STATUSES)) + " |"]
    for family in sorted({o["family"] for o in rows}) + ["Total"]:
        group = [o for o in rows if family == "Total" or o["family"] == family]
        status = Counter(o["status"] for o in group)
        operations = sum(o["kind"] == "operation" for o in group)
        lines.append(f"| {family} | {operations} | {len(group) - operations} | " + " | ".join(
            str(status[s]) for s in STATUSES) + " |")
    return "\n".join(lines)


def report(lock, rows, sha):
    repository = f"https://github.com/zackboll/ams-gra-gige-mel-provider/blob/{sha}/"
    lines = ["# MEL Operations and Evidence", "", "[Requirements Traceability](mel-traceability.md) · "
             "[C++ Implementation](@ref api_core) · [Current qualification scope](status.md)", "",
             "Generated from the checked-in inventory. Status counts include the seven unavailable alternate factory type seams;",
             "they are not a compliance measure.",
             "Positive evidence is limited to the stated assertion and environment. Native, simulated and hardware rows are",
             "references to existing evidence, not executions during the documentation build.", "", counts(rows), "", "## Gap report", ""]
    for group, title in GAPS.items():
        lines += [f"### {title}", ""]
        for o in rows:
            if group in o["gap_groups"]:
                lines.append(f"- [{o['id']}](#{o['id'].lower().replace('.', '-')}) — **{o['responsibility']}**: {o['gap']}")
        lines.append("")
    lines += ["## Declaring interfaces", "",
              "Class annotations and inheritance are separate from member obligations; inherited declarations are counted once.", "",
              "| Interface or header | Class annotation | Inherits |", "| --- | --- | --- |"]
    for h in lock["interfaces"]:
        pin = lock["components"][h["component"]]
        anchor = f"#L{h['class_line']}" if h["class_line"] else ""
        url = f"https://github.com/{pin['repo']}/blob/{pin['revision']}/{h['path']}{anchor}"
        tag = f"`{h['class_annotation']}`" if h["class_annotation"] else "—"
        lines.append(f"| [{h['interface']}]({url}) | {tag} | {h['inherits'] or '—'} |")
    lines += ["", "## Operation inventory", ""]
    for family in sorted({o["family"] for o in rows}):
        lines += [f"### {family}", ""]
        for o in rows:
            if o["family"] != family:
                continue
            i = o["id"]
            u = o["upstream"]
            pin = lock["components"][u["component"]]
            upstream = f"https://github.com/{pin['repo']}/blob/{pin['revision']}/{u['path']}#L{u['line']}"
            tags = ", ".join(f"`{tag}`" for tag in u["annotations"]) or "none on declaration"
            anchor = i.lower().replace('.', '-')
            condition = re.sub(r"(@(?:Required\w*|Optional))\b", r"`\1`", o["applicability"]["condition"])
            lines += [f"#### {i} {{#{anchor}}}", "", f"[{u['declaring']} declaration]({upstream}) — {tags}.", "",
                      f"```cpp\n{u['signature']}\n```", "",
                      f"- **Obligation:** {o['obligation']}; **applicability:** {o['applicability']['current']}. {condition}.",
                      f"- **Status:** {o['status']}. {o['behavior']}", f"- **Availability:** {o['availability']}."]
            if o["metadata"]:
                m = o["metadata"]
                lines.append(f"- **Metadata:** `{m['enumerator'] or 'no enumerator'}` — {m['advertisement']}.")
            for r in o["implementation"]:
                symbol = r["symbol"]
                label = (f"[{symbol}](@ref {symbol})" if symbol in (
                    "gige_mel::make_api_manager", "gige_mel::backend::FrameDescriptor", "gige_mel::backend::IStackedImageSource")
                    else f"`{symbol}`")
                lines.append(f"- **Implementation:** {label} in [{r['path']}]({repository}{r['path']}).")
            for r in o["related_upstream"]:
                source = lock["components"][r["component"]]
                name = PurePosixPath(r["path"]).name
                lines.append(f"- **Related upstream:** [{name}](https://github.com/{source['repo']}/blob/{source['revision']}/{r['path']}).")
            for e in o["evidence"]:
                lines.append(f"- **{e['result'].capitalize()} ({e['level']}):** [{e['scenario']}]({repository}{e['path']}) — {e['demonstrates']}")
            if not o["evidence"]:
                lines.append("- **Evidence:** no operation-level test cited.")
            if o["gap"]:
                lines.append(f"- **Next ({o['responsibility']}):** {o['gap']}")
            lines.append("")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--upstream-checkouts", nargs=2, metavar=("IR", "COMMON"), type=Path)
    args = parser.parse_args()
    lock, rows = load()
    checkouts = dict(zip(("ir-mel", "common-mel"), args.upstream_checkouts)) if args.upstream_checkouts else None
    validate(lock, rows, checkouts)
    if args.self_test:
        negative_checks(lock, rows)
    print(counts(rows))
    print(f"Verified dispositions for {len(rows)} upstream declarations; runtime assertions require separate evidence.")


if __name__ == "__main__":
    main()