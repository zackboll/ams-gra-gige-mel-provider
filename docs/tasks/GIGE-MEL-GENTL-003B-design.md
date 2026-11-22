# GIGE-MEL-GENTL-003B — Euresys Gigelink qualification design

Companion [executed validation](GIGE-MEL-GENTL-003B-validation.md).
Baseline: `15542919b45febb809bfd24d5469dcddb3b456b6`, current fetched main,
including merged PR #45 and its GENTL-003A-R1 corrections.

## Scope and trust boundaries

Evaluate the owner-downloaded Euresys eGrabber 26.10 Linux x86-64 runtime through
the existing project-owned consumer and qualification supervisor. This is not a
production integration, physical-camera authorization, streaming implementation,
or MEL capability change. No physical Cognex contact is permitted.

Reviewed repository instructions, prior GENTL-003 results, GENTL-003A design and
validation, consumer/backend ownership boundaries, the probe, C++ isolation
preflight, supervisor and independent wire classifier. These implementation files
remain byte-identical to accepted R1 commit `19e2ba3`. In particular:

* Wire admission still requires the C++ kernel/proc/netlink checks before loading
  a producer, not just a Python/environment assertion.
* ACK correlation includes both complete UDP endpoints, transaction, command,
  status, and payload constraints.
* A successful designated TestRegister read requires its matching wire exchange
  inside the mandatory post-open read interval.
* Unknown/repeated commands, writes, control acquisition, capture loss and
  post-close commands cannot be waived to accommodate a producer.
* Uncertain native ownership retains the existing process-lifetime quarantine;
  no close retry, access escalation, producer fallback or forced unload is added.

## Archive and extraction admission

Wait for the browser's completed archive; never rename or execute a growing
`.crdownload`. Identify the archive with its gzip/tar contents and SHA-256.
Validate every member before extraction, including paths, types, mode bits,
duplicate names, parent types, link chains, cycles, terminal targets and expansion
bounds. Permit only contained regular files/directories and validated links;
reject traversal, absolute links, special nodes, sparse entries and set-ID modes.
Applied limits: 20,000 members, 2 GiB per member, 4 GiB total file bytes.

Select regular runtime/documentation members explicitly, copy their bytes into a
new private external directory with exclusive file creation, and retain a
member/path/size/hash extraction record. Do not preserve archive ownership or
execute permission. No archive link is needed in the selected subset. Do not run
installer/check-install scripts, install drivers, modify `/etc` or host libraries,
or use the package's bundled older libstdc++ in place of system libraries.

Inspect ELF headers, dynamic sections, exports, symbol-version requirements,
configuration and installer text without execution first. Check the actual
consumer's 25 mandatory and 17 optional symbols against the export table. A
runtime query, not the archive filename or header, establishes the GenTL version.

## Native execution stages

1. Reuse the unchanged GENTL-003 supervisor: fresh user/network namespaces,
   namespace-local loopback DOWN, no addresses/routes or physical interfaces,
   bounded independent load/init/query/close children, then system/interface
   enumeration and checked native close. Stop on the first failure, signal or
   timeout. Disable core dumps; retain stdout/stderr and supervisor log hashes.
2. Scope `EURESYS_EGRABBER_LIB64` to the extracted directory for these children.
   Scope `LD_DEBUG=libs` to retain loader/dependency diagnostics. Do not mutate
   persistent environment or producer configuration. Loader finalizer messages
   are not independent proof of when mappings disappeared.
3. If native stages succeed but DOWN-loopback enumeration is empty, perform one
   separately identified topology diagnostic: fresh user/network/PID namespaces,
   namespace init validates the fresh network, brings only its own loopback UP,
   revalidates addresses/routes, and runs the same `interfaces` command through
   existing `run_stage`. Record remaining namespace PIDs before init exits. This
   is not a retry of a failed operation or an automatic networking fallback.
4. Only an actual enumerated interface ID can authorize a subsequent existing
   `--wire` invocation. Never guess `lo` as a producer ID. No interface means stop
   before simulated-device discovery/open; inspect static filtering and local
   kernel facts rather than creating an Ethernet link.

The external topology diagnostic is task evidence, not a new repository tool or
wire-admission bypass. It does not discover or open devices. The existing offline
supervisor uses user/network namespaces; the separate diagnostic additionally
uses a fresh PID namespace and mounted proc with init-owned child reaping.

## Wire acceptance and stop conditions

If reachable, use only the unchanged `gentl_qualification.py --wire` mode with
pinned Aravis 0.8.36, independent calibrated AF_PACKET capture, exact producer path
and exact enumerated interface. Require actual READONLY open, module/port/XML
inspection, TestRegister `0x1f0` read, checked native closes, quiet observation,
complete endpoint/ACK correlation and zero drops. READONLY alone proves nothing.

The executed result instead stops at interface admission:
**`BLOCKED_BY_PRODUCER_TOPOLOGY`**. No wire invocation is fabricated, and no
zero-write, required-read, quiet-interval or full cleanup claim is inferred from
empty interface enumeration. Further virtual-Ethernet work needs a separate
audited task covering link construction, preflight admission, simulator binding,
calibration, broadcasts, endpoint correlation and fail-closed tests. No veth or
physical interface is authorized here.

## Delivery and limits

Deliver documentation-only evidence, not SDK files, XML, raw captures or large
logs. No executable/tooling changes require repeating the unrelated full compiler
and sanitizer matrix; execute focused baseline contracts and integrity checks,
and independently verify recorded evidence hashes. Keep earlier synthetic passes
distinct from actual vendor results. This producer is not ready for physical
qualification: native initialization is measured, but READONLY wire safety,
device cleanup and post-close traffic remain unqualified.
