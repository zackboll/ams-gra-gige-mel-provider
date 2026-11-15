#!/usr/bin/env python3
"""Summarize a pthread-trace.jsonl from pthread_lifecycle_trace_gdb.py (SDK-free).

Answers, per pthread_t, only questions the observed glibc entry events support:
created by an observed pthread_create? detached/joined how many times? did a
detach follow a join or a detach? Vendor internals are never inferred.
"""
import argparse
from collections import defaultdict
import json
from pathlib import Path
import sys


def caller(record):
    """First non-glibc, non-sanitizer frame object/function as reported by gdb's unwinder."""
    for frame in record.get("stack", [])[1:]:
        obj = frame.get("object") or ""
        if "libc.so" in obj or "libtsan" in obj or "libasan" in obj:
            continue
        return f"{frame.get('function')}@{Path(obj).name}"
    return None


def analyze(records):
    """Pair entries/returns LIFO per thread. Under a sanitizer the interceptor (outer)
    calls the real glibc function (inner); only the outermost call is counted, and
    an entry that never returns (e.g. a fatal sanitizer CHECK) is reported."""
    pending = defaultdict(list)
    threads = defaultdict(lambda: {"created": 0, "create_detached_attr": None, "detach": [], "join": [],
                                   "creator": None, "start_routine": None, "events": []})
    order = []
    for record in records:
        event = record["event"]
        lwp = record.get("lwp")
        if event in ("pthread_create", "pthread_detach", "pthread_join"):
            pending[(lwp, event)].append(record)
            continue
        if event.endswith("_return") and event[:-7] in ("pthread_create", "pthread_detach", "pthread_join"):
            base = event[:-7]
            stack = pending.get((lwp, base))
            if not stack:
                continue
            entry = stack.pop()
            if stack:
                continue  # nested real call inside an interceptor: counted at the outer level
            identity = record.get("pthread_t") if base == "pthread_create" else entry.get("pthread_t")
            item = threads[identity]
            step = {"event": base, "result": record.get("result"), "lwp": lwp, "caller": caller(entry),
                    "t_ns": record["t_ns"]}
            if base == "pthread_create":
                item["created"] += 1
                item["creator"] = caller(entry)
                item["start_routine"] = entry.get("start_routine")
                item["create_detached_attr"] = entry.get("attr_detached", False)
            elif base == "pthread_detach":
                step["prior"] = [e["event"] for e in item["events"] if e["event"] != "pthread_create"]
                item["detach"].append(step)
            else:
                item["join"].append(step)
            item["events"].append(step)
            order.append({"pthread_t": identity, **step})
        elif event == "sanitizer_check_failed":
            order.append({"event": event, "check": record.get("check"), "lwp": lwp, "caller": caller(record),
                          "t_ns": record["t_ns"]})
    findings = []
    for (lwp, base), stack in pending.items():
        if stack:
            entry = stack[0]  # outermost call that never returned
            identity = entry.get("pthread_t")
            step = {"event": base, "result": None, "unreturned": True, "lwp": lwp, "caller": caller(entry),
                    "implementation": entry.get("implementation"), "t_ns": entry["t_ns"]}
            if identity is not None:
                item = threads[identity]
                step["prior"] = [e["event"] for e in item["events"] if e["event"] != "pthread_create"]
                item[base[8:]].append(step) if base != "pthread_create" else None
                item["events"].append(step)
            order.append({"pthread_t": identity, **step})
            findings.append({"pthread_t": identity, "finding": f"{base} entered but never returned", "prior": step.get("prior")})
    order.sort(key=lambda item: item["t_ns"])
    for identity, item in threads.items():
        consumes = [e for e in item["events"] if e["event"] in ("pthread_detach", "pthread_join")]
        if not item["created"]:
            findings.append({"pthread_t": identity, "finding": "detached/joined but no observed pthread_create"})
        if item["create_detached_attr"] and consumes:
            findings.append({"pthread_t": identity, "finding": "created detached by attribute and later consumed"})
        if len(consumes) > 1:
            findings.append({"pthread_t": identity, "finding": "consumed more than once",
                             "sequence": [(e["event"], e["result"]) for e in consumes]})
        if item["created"] > 1:
            findings.append({"pthread_t": identity, "finding": "pthread_t value reused by distinct creates"})
    return {"threads": threads, "order": order, "findings": findings,
            "trace": completeness(records), "join_then_detach": join_then_detach(threads)}


def completeness(records):
    """A trace is evidence only if gdb started, observed exactly one inferior exit
    with a status, and finished. Anything else (e.g. a killed debugger) is incomplete."""
    events = [record.get("event") for record in records]
    reasons = []
    for name in ("trace_ready", "inferior_exit", "trace_finished"):
        if events.count(name) != 1:
            reasons.append(f"{name} observed {events.count(name)} times")
    if events and events[0] != "trace_ready":
        reasons.append("trace does not begin with trace_ready")
    if events and events[-1] != "trace_finished":
        reasons.append("trace does not end with trace_finished")
    exits = [record for record in records if record.get("event") == "inferior_exit"]
    if len(exits) == 1 and exits[0].get("exit_code") is None:
        reasons.append("inferior exit status missing")
    return {"complete": not reasons, "reasons": reasons,
            "inferior_exit": exits[0].get("exit_code") if len(exits) == 1 else None}


def join_then_detach(threads):
    """Every pthread_t created successfully (observed create, result 0), joined
    successfully (result 0) and afterwards passed to pthread_detach. The detach
    result is the observed return value, or None when it never returned."""
    found = []
    for identity, item in threads.items():
        creates = [e for e in item["events"] if e["event"] == "pthread_create" and e["result"] == 0]
        joins = [e for e in item["events"] if e["event"] == "pthread_join" and e["result"] == 0]
        if not creates or not joins:
            continue
        for detach in item["detach"]:
            prior = [join for join in joins if join["t_ns"] < detach["t_ns"]]
            if prior:
                found.append({"pthread_t": identity, "create": creates[0], "join": prior[-1], "detach": detach})
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    args = parser.parse_args()
    records = [json.loads(line) for line in args.trace.read_text().splitlines() if line.strip()]
    result = analyze(records)
    json.dump({"trace": result["trace"], "order": result["order"], "findings": result["findings"],
               "join_then_detach": result["join_then_detach"],
               "threads": {k: {key: value for key, value in v.items() if key != "events"}
                           for k, v in result["threads"].items()}}, sys.stdout, indent=1, default=str)
    print()


if __name__ == "__main__":
    main()
