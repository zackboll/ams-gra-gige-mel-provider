# GIGE-MEL-GENTL-003B — Euresys Gigelink validation

Companion [design](GIGE-MEL-GENTL-003B-design.md).

## Disposition and source

**`BLOCKED_BY_PRODUCER_TOPOLOGY` — native initialization passes; no producer
interface is exposed in the required loopback-only topology. Read-only wire
safety is NOT qualified.** No physical camera was contacted.

Executed 2026-11-19 UTC on Debian 13 x86-64, Linux
`6.12.116+deb13-amd64`, Python 3.13.5. Fetched main and continued the existing
`feature/gige-mel-gentl-003b-euresys-qualification` branch without a duplicate
branch/worktree. PR #45 is merged. Executed source:

* Commit: `15542919b45febb809bfd24d5469dcddb3b456b6`.
* Tree: `f3de23559a761067d67a17685a9e1e6bcb54dbd1`.
* Probe: `/home/zboll/git/ams-gra-gige-mel2/build/qual003a-gcc/gige_mel_gentl_probe`.
* Probe SHA-256: `000f354a488f6c19628c2e951b9cf1dc7858b3a179109e8fe5ac287568289bc5`.

The probe target build returned exit 0, `ninja: no work to do`. Qualification
implementation bytes match accepted R1 commit `19e2ba3`; this task changes only
documentation. Final delivery commit/tree and hosted status are reported on the
PR rather than self-referenced here.

## Legitimate package and static inspection

The repository owner supplied the official package. Initially only a growing
browser `.crdownload` existed; inspection waited for the completed filename.
The original archive was not renamed or modified.

| Item | Actual identity |
| --- | --- |
| Archive | `/home/zboll/Downloads/egrabber-linux-x86_64-26.10.0.11.tar.gz` |
| Size | **402,627,537 bytes** |
| SHA-256 | `e7ee8e2cf90d274090170dd4b65e9941e12ad5cac6896277f415bad963a975c9` |
| Package root | `egrabber-linux-x86_64-26.10.0.11/` |
| Structure | 1,550 entries: 1,515 regular files, 35 symlinks; 995,902,523 file bytes |
| Private extraction | `/home/zboll/sdk/euresys-26.10/` (mode 0700) |
| Producer | `/home/zboll/sdk/euresys-26.10/lib/x86_64/gigelink.cti` |
| Producer size | 2,329,176 bytes |
| Producer SHA-256 | `3a1df9d05f87c46b239f3d7b0ccd3936ef0e94ff53d135ac58ab37371e56e89d` |
| ELF | 64-bit LSB x86-64 shared object, stripped, build ID `c1653391064ad89f27c5fc32c830debb18c5c1ed` |

`tar -tzf` completed with exit 0. All members were validated before extraction:
no traversal/absolute paths, duplicate normalized names, special nodes, sparse or
set-ID entries, non-directory ancestors, escaping/dangling/cyclic links, or
expansion-limit breaches. An initial stricter validator stopped on an internal
multi-hop wxWidgets symlink; nothing was extracted. The completed validation
explicitly resolved all link chains. Only 13 named regular runtime/documentation
files were subsequently copied, with exclusive file creation and mode 0600.
No SDK symlinks, drivers, bundled libstdc++, GUI, or vendor executable program was
installed. Installer/check-install scripts were read as text, never run.

Selected runtime files: `gigelink.cti`, `gigelink.default.ini`, `egrabber.genapi`,
`egrabber.0` through `.3`, and `libegrabber.so`, all under the extracted
`lib/x86_64/`. The last five were precautionary same-package siblings, not proven
necessary: loader diagnostics for reached stages show only `gigelink.cti` and
`egrabber.genapi` loaded from this directory. This does not establish a complete
minimum runtime for unexecuted device/stream operations.

Static tools: `file`, `readelf -h/-d/-Ws/-V`, `objdump -T/-d`, `strings` and text
inspection. `ldd` was not used. All **25 mandatory and 17 optional** symbols
requested by this consumer are exported. An initial inspection script mistakenly
included the macro parameter `name` as a symbol; its extraction was corrected to
start at the first real invocation. This was an inspection-script error, not a
missing producer symbol or a native failure.

### Dependencies, configuration and privileges

`gigelink.cti` DT_NEEDED:
`librt.so.1`, `libstdc++.so.6`, `libm.so.6`, `libgcc_s.so.1`,
`libpthread.so.0`, `libc.so.6`, `ld-linux-x86-64.so.2`.
RUNPATH: `$ORIGIN:/opt/euresys/egrabber/lib/x86_64`.
Required C++ symbol versions include `GLIBCXX_3.4.21` and `CXXABI_1.3.9`.

Measured runtime additionally loaded system `libffi.so.8` and external
`egrabber.genapi`; the latter brings `libz.so.1`, `libutil.so.1`, `libdl.so.2`
and the common runtime libraries. Loader logs resolve system dependencies under
`/lib/x86_64-linux-gnu/` and `/lib64/`, not bundled older libraries. Static strings
also name RDMA libraries and `/dev/gigelink*`; these are not evidence of use or
requirements for the reached no-device stages.

`gigelink.default.ini` has no CameraSubnet restriction, AllowBroadcastAck=y and
UseFilterDriver=y (comment: if installed/enabled). The binary names
`EGRABBER_CUSTOM_CONFIG_DIR`, `/etc/opt/euresys/egrabber/`, and `gigelink.ini`.
No custom ini was created and no GIGELINK_* settings were overridden.
Only `EURESYS_EGRABBER_LIB64` selected the extracted runtime directory.

The package installer explicitly describes socket-only Gigelink fallback without
the optional filter driver and permits installation without Memento support.
Release notes recommend installing Memento first; that recommendation was not
treated as proof of a hard dependency. Neither `/opt/euresys`, `/dev/memento` nor
`/dev/gigelink*` existed in the checked host locations. Actual initialization
succeeded without installing a service/driver or modifying the host.

All vendor execution used an ordinary host user's unprivileged namespaces,
mapped UID 0 inside the new user namespace. No host root privilege, sudo,
system-wide environment, kernel module or library change was used. This is not
a separate demonstration of producer operation as a nonzero UID *inside* the
namespace, nor proof of prerequisites for later device operations.

## Native offline execution

External evidence root: `/home/zboll/gentl-003b-evidence/`.
Original supervisor bundle: `/home/zboll/gentl-003b-evidence/offline-1/`.
Core dumps disabled. Exact launch (from the source baseline above):

```sh
ulimit -c 0
env TMPDIR=/home/zboll/gentl-003b-evidence/tmp \
  PYTHONDONTWRITEBYTECODE=1 \
  EURESYS_EGRABBER_LIB64=/home/zboll/sdk/euresys-26.10/lib/x86_64 \
  LD_DEBUG=libs \
  python3 /home/zboll/git/ams-gra-gige-mel2/scripts/gentl_qualification.py \
  --probe /home/zboll/git/ams-gra-gige-mel2/build/qual003a-gcc/gige_mel_gentl_probe \
  --producer /home/zboll/sdk/euresys-26.10/lib/x86_64/gigelink.cti \
  --repeat 3 --timeout 20 \
  --output /home/zboll/gentl-003b-evidence/offline-1
```

The existing supervisor creates independent `unshare --user --map-root-user
--net` children, leaves loopback DOWN and never requests device discovery/open.
A separate pre-execution namespace snapshot confirmed only DOWN lo, no addresses,
and empty IPv4/IPv6 route tables. The supervisor itself checks namespace creation,
not a full per-child network snapshot; do not reinterpret that limitation as the
wire mode's stronger C++ preflight.

| Stage | Native/process result | Elapsed seconds |
| --- | --- | --- |
| Namespace availability | Exit 0 | 0.003584 |
| Load child 1 | Load/symbol validation, GCInitLib, GCGetInfo, GCCloseLib; exit 0 | 0.015583 |
| Load child 2 | Same, independent process; exit 0 | 0.015797 |
| Load child 3 | Same, independent process; exit 0 | 0.015682 |
| Interfaces child | Above plus TLOpen, TLGetInfo, TLUpdateInterfaceList, enumeration, TLClose; exit 0 | 0.015578 |

All five stages: no signal, no timeout. Checked native operations returned the
consumer's success path (`GC_ERR_SUCCESS`, 0); no error was skipped. Producer
facts: ID `EuresysGigelink`, vendor Euresys, model Gigelink, version **26.10.0.11**,
type GEV, native **GenTL 1.6**. All ten producer properties were printed normally.
Write/acquisition exports are present but were not exercised.

The interface loop emitted **no interface IDs** and enumeration succeeded: zero
interfaces, not a failing TLUpdateInterfaceList. TLClose and GCCloseLib completed
successfully. Loader logs record CTI and GenApi finalizers. The unchanged supervisor
correctly labels cleanup `PROBE_REPORTED_SUCCESS_NOT_INDEPENDENTLY_TRACED`:
finalizer output and process exit do not independently establish exact dlclose
unmapping time or device cleanup. No uncertain close was retried.

## Topology investigation — loopback UP, still zero interfaces

After successful offline native cleanup, one separately identified interfaces-only
diagnostic ran inside fresh user/network/PID namespaces with mounted proc and
namespace PID 1. External script:
`/home/zboll/gentl-003b-evidence/enumerate-loopback-up.py`.
It imports unchanged `run_stage`, `network_snapshot`, and `require_isolation`.
It validates the fresh DOWN-loopback network and different user namespace, brings
only that namespace's lo UP, validates the result, then runs exactly:

```text
/home/zboll/git/ams-gra-gige-mel2/build/qual003a-gcc/gige_mel_gentl_probe
/home/zboll/sdk/euresys-26.10/lib/x86_64/gigelink.cti
--offline-qualification interfaces
```

Outer launch: `timeout --kill-after=3 30 unshare --user --map-root-user --net
--pid --fork --mount-proc --kill-child -- python3` followed by the external script
and the actual parent network/user namespace identities. Same scoped environment
as offline execution. Native deadline: 20 seconds. No simulator/device operations
were requested. This was not an automatic retry following a native failure.

Bundle: `/home/zboll/gentl-003b-evidence/loopback-up-1/`.
Recorded network `net:[4026533113]`, user `user:[4026533096]`; only lo, addresses
127.0.0.1/8 and ::1/128, local loopback routes, no gateway/default route, physical
interface or veth. Native stage: exit 0, no signal/timeout, **0.007405 seconds**,
zero enumerated interfaces, successful checked TLClose/GCCloseLib. Before init
exit, `remaining_namespace_pids` was empty. No lingering probe was found afterward.

### Concrete filtering evidence versus speculation

Disassembly of this exact CTI shows an `if_nameindex` call at `0x11c18e`, interface
address ioctl `0x8915` (`SIOCGIFADDR`), then flags ioctl `0x8913`
(`SIOCGIFFLAGS`) at `0x11c212`. Interface admission tests:

* `0x11c224`: flags & `0x01` (IFF_UP), otherwise skip.
* `0x11c22c`: flags & `0x40` (IFF_RUNNING), otherwise skip.
* `0x11c234`: flags & `0x02` (**IFF_BROADCAST**), otherwise skip.
* A subsequent test also requires a nonzero IPv4 address.

An independent, non-vendor ioctl check in a fresh loopback-UP namespace measured
lo flags **0x49**: UP, RUNNING, LOOPBACK; **not BROADCAST**. Linux header definitions
match these bits/ioctl values. Together with actual empty enumeration, this is
concrete evidence that the inspected enumeration path excludes ordinary loopback
because it lacks broadcast capability. It is not a named `IFF_LOOPBACK` rejection,
and does not prove every other admission condition or a future veth will work.
No producer patch, NIC flag spoofing, subnet override or filter workaround was
attempted. No absent runtime dependency was reported by reached native calls.

## Wire result and established-baseline comparison

**Wire mode NOT RUN: no actual producer interface ID exists in the admitted
topology.** Guessing `lo` would violate exact-ID qualification. There was no
READONLY open, XML/port inspection, mandatory TestRegister read, AF_PACKET capture,
calibration/drop result, ACK pair, device close or post-close quiet interval.
Zero writes cannot be claimed from an unexecuted wire stage.

| Area | Real Euresys result versus fixture/003A baseline |
| --- | --- |
| GenTL version | Native 1.6; version 26.10.0.11, not a fixture identity |
| Interface identity | Zero with lo DOWN and UP; no exact ID available |
| Device access | Not reached; no READONLY wire-safety evidence |
| Discovery | Not requested; no real command/ACK sequence |
| Port reads | Not reached; TestRegister and complete exchange unqualified |
| XML location/format | Not queried |
| Byte order | ELF is little-endian; device port declaration/bytes not queried |
| Cleanup | Native system/library closes pass; no device/interface close exercised |
| Quiet interval | Not executed; no independent post-close wire evidence |

The earlier project fixture and 12-scenario synthetic matrix validate harness
guards, not this producer. No READ_ONLY_WIRE_PASS or HARNESS_SELF_TEST_PASS is
assigned to Euresys. Streaming, Cognex firmware compatibility, physical-network
reliability, timestamps and full MEL conformance remain outside this evidence.

## Executed checks and evidence integrity

* Pinned-upstream verification: **100 files**, exit 0.
* Complete post-root reachable-history proprietary guard: **1,034 Git entries**,
  passed; root-tree check is repeated for final delivery.
* Focused optional GCC 14 baseline CTest: **6/6 passed**, exit 0:
  `gentl_default_isolation`, `gentl_qualification_supervisor`, `gentl_wire_safety`,
  `gentl_consumer_contract`, `gentl_probe_fixture`, `gentl_backend_contract`.
* Build target check: exit 0; unchanged qualified probe needed no rebuild.
* All original offline supervisor log sizes and SHA-256 hashes independently
  reverified; original archive checksum reverified after execution.
* SDK/configuration extraction hashes retained externally. Final external evidence
  manifest and SHA-256 are recorded with PR delivery, not raw files in Git.

Host `/tmp` has no free inodes. Compiler/test temporaries used external
`/home/zboll/gentl-003b-evidence/tmp`; focused CTest bound it over `/tmp` in a private
user/mount namespace, as in prior qualification. No host mount or network changed.

No executable/source/tooling changes: the unrelated complete default, GCC/Clang,
sanitizer, build-off and synthetic 12-scenario matrix was not repeated. Earlier
matrix results are historical, not newly executed here. Hosted CI is separate
and checked once after final push; status appears on the PR. No proprietary
producer is added to hosted CI.

## Remaining gate and recommendation

Request independent technical review of the measured native results and topology
analysis. **Do not proceed to physical Cognex qualification or GENTL-004.** A
separate audited virtual-Ethernet simulator qualification may be proposed; it must
preserve conservative capture/classification/read/ACK/cleanup requirements and
explicitly review any necessary preflight changes. No such extension or physical
network workaround is implemented by this task.
