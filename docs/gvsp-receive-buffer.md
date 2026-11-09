# GVSP socket receive buffer (GIGE-MEL-019)

The optional internal Aravis adapter now requests a **4 MiB** kernel receive
buffer (`SO_RCVBUF`) for every GigE Vision stream. It measures what the
kernel actually granted, and reports a shortfall once per stream. Acquisition
is never refused because of the result.

The change helps only when the host allows it. `net.core.rmem_max` must be at
least 4 MiB (4194304 bytes). At the Linux default of 212992 the kernel clamps
the request to 212992 bytes, which gives no protection. The adapter then prints
the one-line warning described below. See
[Host prerequisite](#host-prerequisite-not-applied-by-this-project).

The measurements below are from one host, one camera and one cable
arrangement. **They are not hardware qualification.** At 10 Hz under heavy host
load the 4 MiB buffer reduced UDP drops by about 180× but did **not** give
600/600 complete frames. 14 Hz is still unsupported by the production adapter
(see [the camera record](cognex-cic-5000r.md)). Production timestamps remain
invalid.

## Pinned Aravis 0.8.36 behaviour (verified in source)

The release archive was checked against SHA-256 `246deaa0…51bf` and the
behaviour below was read from its source.

| Fact | Where |
| --- | --- |
| `ArvGvStream:socket-buffer` is `FIXED` (default) or `AUTO`; `socket-buffer-size` is an int, default 0 | `src/arvgvstream.c:1817-1842`, `arvgvstream.h:49-56` |
| `FIXED` with size ≤ 0 returns early, so `setsockopt` is **never called**. The socket keeps `net.core.rmem_default` | `_update_socket()`, `arvgvstream.c:230-232` |
| `FIXED` with size N requests N; `AUTO` requests buffer `allocated_size` + 1024 (or the smaller of that and the size if positive); the floor is 20 KiB | `arvgvstream.c:236-250` (`ARV_GV_STREAM_BUFFER_SIZE_PROTOCOL_OVERHEAD` 1024, `ARV_GV_STREAM_MIN_BUFFER_SIZE` 20480) |
| The size is applied in `_update_socket()`, called when the **first packet of each new frame** claims an input buffer. It is not applied when the property is set | `_find_frame_data()`, `arvgvstream.c:402` |
| `arv_socket_set_recv_buffer_size()` calls `setsockopt(SO_RCVBUF)` and then `getsockopt`. If the reported value is below the request it logs a warning and returns FALSE | `src/arvnetwork.c:450-504` |
| On FALSE, `current_socket_buffer_size` is not updated, so the SDK **retries on every frame** | `arvgvstream.c:252-262` |
| These warnings go only to Aravis's own debug channel (`arv_debug_with_level`, enabled by `ARV_DEBUG`), **not** to GLib `g_log`. A default process shows nothing | `src/arvdebug.c` |
| The stream socket is a GIO UDP socket bound to the device's interface address and port 0; its port is public through `arv_gv_stream_get_port()` | `arvgvstream.c:1657-1666` |

Linux (`socket(7)`) doubles the value passed to `setsockopt(SO_RCVBUF)` for
bookkeeping, and `getsockopt` returns the doubled value. An unprivileged
request is capped at `net.core.rmem_max`. The SDK's own check therefore
succeeds whenever the doubled grant is at least the request, which happens at
up to twice `rmem_max`. **A successful property setter, and even the SDK's own
check, are not evidence that the requested bytes were granted.**

## Adapter behaviour (`src/backends/aravis/aravis_backend.cpp`)

1. **Where it is applied.** In `start()`, after `arv_device_create_stream()`
   succeeds and before any pool buffer is queued, with the device lock held.
   It applies only if the stream is an `ArvGvStream` **and** the device is an
   `ArvGvDevice`. Fake and any other stream are not touched: no property is
   set and nothing is measured, and the observation is `not_applicable`.
2. **What is set.** `socket-buffer = FIXED`, `socket-buffer-size = 4194304`.
   The property readback is recorded only as the "SDK property" value, never as
   an allocation.
3. **What is measured, and how.** On the receiving thread, after the
   **first** popped buffer of each native stream (the SDK has applied the size
   by then):
   - The adapter finds the socket from public facts only: the stream port
     (`arv_gv_stream_get_port`) and the device's interface IPv4 address
     (`arv_gv_device_get_interface_address`).
   - It scans this process's `/proc/self/fd` for **exactly one** UDP IPv4
     socket bound to that address and port, and reads its `SO_RCVBUF`.
   - No private SDK structure and no descriptor layout is assumed.
   - While it does this, the receive slice's waiter count keeps the stream,
     and so the socket, alive. No adapter lock is held across the lookup.
4. **Assessment.** The granted bytes are the reported value divided by 2. The
   result is `sufficient` if the grant is at least the request, otherwise
   `insufficient`. A missing, ambiguous or failed lookup gives `unknown`,
   never success. `/proc/sys/net/core/rmem_max` is recorded for diagnosis only.
5. **Reporting.**
   - `insufficient` and `unknown` write **one** line to stderr per native
     stream. A restart creates a new native stream, so it can warn again. No
     warning is ever written per frame.
   - The adapter adds no monitoring thread.
   - The private, SDK-free `aravis::receive_buffer_observation(stream)` returns
     the latest run's observation.
   - The diagnostic `capture` command prints it. There is no MEL or public API
     change.

Example warning, from the loopback test on a host with the default
`rmem_max`:

```text
gige_mel aravis WARNING: GVSP receive buffer insufficient for stream generation 2: requested 4194304 bytes,
granted 212992 bytes (kernel SO_RCVBUF 425984), net.core.rmem_max 212992; ... Acquisition continues;
packet loss under host load is more likely. See docs/gvsp-receive-buffer.md for the host prerequisite
```

## Measurements (2026-11-08)

**Hardware.**

- Camera: Cognex CIC-5000R `COGNEX-CIC-5000R-21874533`, firmware V3.7-1,
  2592×1944 Mono8, 5,038,848-byte payload, 35 ms exposure.
- Link: camera 1 Gb/s (`GevLinkSpeed` 1000); `GevSCPSPacketSize` 1500 and
  `GevSCPD` 0, never changed.
- Host: `enp5s0` at 10 Gb/s, MTU 1500. AMD Ryzen 9 7945HX (32 logical CPUs),
  Debian 6.12 kernel. Aravis 0.8.36.

**Measurement method.**

- Every leg lasted 60 s.
- Counters were recorded before and after each leg: UDP `RcvbufErrors`, NIC,
  softnet and CPU from `/proc/stat`.
- Load average and CPU pressure were sampled every 2 s, and `ss -uamn` was
  taken at +8 s and +40 s.
- "Resend requests" are packet requests (`n_resend_requests`). "Recovered" is
  `n_resent_packets`. "Lost" is packets the SDK could not recover
  (`n_missing_packets`).
- At 10 Hz the harness drove the **production adapter**, with the policy
  applied in its stream-creation test hook. At 14 Hz it drove a **raw-SDK**
  harness: the camera reads back 14.000112 Hz, which the integer-Hz production
  profile correctly rejects.
- Raw data and harness sources are kept outside Git.

### Kernel-reported buffer per policy (at `rmem_max` 33554432)

| Policy | Requested bytes | Kernel `SO_RCVBUF` reported | Granted bytes |
| --- | --- | --- | --- |
| A SDK default (`FIXED`, 0) | none (`setsockopt` never called) | 212992 (= `rmem_default`) | 212992 |
| B `FIXED` 4 MiB | 4194304 | 8388608 | 4194304 |
| C `FIXED` 8 MiB | 8388608 | 16777216 | 8388608 |
| D `AUTO` | 5039872 (payload + 1024) | 10079744 | 5039872 |
| B at the default `rmem_max` 212992 | 4194304 | **425984** | **212992** (5 %) |

Before the first frame every policy reports 212992.

### Light host load, `rmem_max` 33554432 (load1 0.6–4.5, host CPU 1–13 %)

Order: A B C D D C B A. Each cell is the leg's `UdpRcvbufErrors` delta.

| Rate | A | B 4 MiB | C 8 MiB | D AUTO | D AUTO | C 8 MiB | B 4 MiB | A |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 10 Hz production | 2939 | 0 | 0 | 0 | 0 | 0 | 0 | 741 |
| 14 Hz raw SDK | 1157 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |

Every leg was 600/600 or 840/840 complete, with no frame-ID gaps and nothing
lost. At this load, losses with the default policy were already recovered by
resend, and the second 14 Hz control was loss-free. Light load therefore does
not show a 14 Hz benefit.

### Baseline, SDK default, `rmem_max` 212992 (load1 7–21, host CPU 19–34 %)

| Run | 10 Hz complete | 10 Hz resend requests (all recovered) | 14 Hz complete | 14 Hz resend requests | 14 Hz lost |
| --- | --- | --- | --- | --- | --- |
| 1 | 600/600 | 24246 | 839/840 | 36408 | 1180 |
| 2 | 600/600 | 4000 | 840/840 | 6032 | 0 |
| 3 | 600/600 | 3356 | 840/840 | 6529 | 0 |

### Heavy host load, `rmem_max` 33554432 (load1 17–24, host CPU 41–69 %)

**Load.** The owner-authorized synthetic load was 8 workers × 64 MiB in a
user-level systemd scope limited to 8 CPUs, 2 GiB, 64 tasks and 1500 s. The
owner's own workload also ran and was neither started nor stopped. Default and
4 MiB legs saw comparable load. Order: A B B A A B B A.

| Rate | Policy | Complete frames (4 legs) | `UdpRcvbufErrors` (4 legs) | Lost (sum) |
| --- | --- | --- | --- | --- |
| 10 Hz production | A default | 241, 61, 35, 73 of 600 | 185122, 350906, 449276, 377128 | 5,454,529 |
| 10 Hz production | **B 4 MiB** | **596, 598, 591, 585** of 600 | **1083, 514, 2370, 3552** | **12,389** |
| 14 Hz raw SDK | A default | 183, 54, 137, 346 of 840 | 466807, 592853, 539146, 145880 | 6,940,490 |
| 14 Hz raw SDK | **B 4 MiB** | **825, 828, 823, 820** of 840 | **11064, 5345, 10681, 18037** | **138,477** |

- **Default policy:** the SDK sent 2.0–3.5 M resend requests per leg. Only the
  first default leg received any resent packets (45,494).
- **4 MiB:** every leg beat every default leg. UDP drops fell about 180× at
  10 Hz and 39× at 14 Hz. No 4 MiB leg received a resent packet: under this
  load, packets lost were not recovered.
- **Both policies:** NIC errors and softnet drops were 0 in every leg.
- **Receiver thread:** in the legs where the 2 s sampler caught the SDK
  receiver thread at least twice, it ran 38–77 % of one core and waited
  runnable 2–6 % of the time, in both policies. This does **not** identify the
  cause of the remaining loss.
- **Cost:** the 4 MiB policy did not change process RSS (35–42 MiB) or stop
  latency (65–115 ms under load). Process CPU was 45–72 % of one core under
  load in both policies.
- **Not tested:** 8 MiB and `AUTO` under heavy load.

### Conclusion and selected policy

- **Selected:** `FIXED` 4 MiB is the smallest tested policy with a significant,
  repeatable improvement under comparable heavy load. It is about 83 % of one
  frame (`AUTO` would request 5.04 MB).
- **Not qualified:** it does **not** meet 600/600 or 840/840 under that load.
  Packet spacing (`GevSCPD`), jumbo frames and CPU affinity were not tested and
  each needs a separate owner decision.

## Host prerequisite (not applied by this project)

The adapter never changes host settings. To give a GigE stream its 4 MiB, the
host owner must allow it:

```sh
# runtime only (lost at reboot):
sudo sysctl -w net.core.rmem_max=4194304
# persistent - an owner decision, not part of this project:
#   /etc/sysctl.d/90-gige-mel-gvsp.conf containing "net.core.rmem_max = 4194304", then sudo sysctl --system
```

- The value must be at least 4194304. Only 33554432 was used in testing.
- `rmem_default` does not need to change: the adapter sets the stream's own
  buffer.
- `rmem_max` is host-wide. It raises the limit any unprivileged process can
  request.
- To verify, the diagnostic `capture` output must report
  `receive buffer: sufficient`. You can also check `ss -uamn`, which shows
  `rb8388608` on the stream socket while acquiring.

### Restoration

First record the original with `sysctl net.core.rmem_max`. Restore it exactly
afterwards (212992 on the test host). Remove the persistent file if you added
one:

```sh
sudo sysctl -w net.core.rmem_max=212992
```

For the GIGE-MEL-019 experiments the value was changed twice:

- Each time, an operator-started root guard recorded the original value, set
  33554432 and verified it.
- It then waited for a completion marker (with a 90-minute limit) and restored
  the original under an `EXIT`/`INT`/`TERM`/`HUP` trap.
- Both times it reported `RESTORED_OK 212992`, and an independent re-read
  confirmed it.
- No `/etc` file was written.

If such a guard is killed with SIGKILL, run the `sysctl -w` restore above. A
reboot also restores a runtime-only change.

## Tests

| Test | What it proves |
| --- | --- |
| `aravis_adapter` "GVSP receive-buffer assessment…" (SDK-free) | Doubling arithmetic and boundaries (2000 is sufficient for 1000, 1999 is not; a value only equal to the request is a half grant); `unknown` for an absent, zero or negative value; exact one-line warning text |
| `aravis_stream_contract` O (Fake) | `ArvFakeStream` has no `socket-buffer` property and is never touched; `not_applicable`; no lookup or warning over two runs; GLib warnings and criticals are fatal; a foreign stream is refused |
| `aravis_gige_loopback_contract` R (real `ArvGvStream` over loopback UDP; the host's `rmem_max` is only read) | R1 sufficient grant; R2 the production 4 MiB request clamped by the real kernel limit (`insufficient`), with exactly one warning per native stream over 12 frames and an explicit restart, while acquisition continues; R3 a genuinely unobservable socket gives `unknown`, never success; R4 the adapter's value equals an independent `getsockopt` on the live socket |
| `aravis_physical_camera` (opt-in, label `hardware`) | The unchanged MEL path on the physical camera, with the policy active |

R2 assumes the host `rmem_max` is below 4 MiB, as on Linux defaults and in
hosted CI. On a host that already allows 4 MiB, R2 requests twice `rmem_max`
instead, so that it still observes a real kernel clamp.

## Limitations

- **Not hardware qualification.** One camera, one host and one switch port
  were tested, with synthetic and incidental host load.
- **Heavy load:** 4 MiB still loses 1–3 % of frames at 10 Hz and 1.4–2.4 % at
  14 Hz (raw SDK). The cause of the remaining loss was not identified.
- **Host dependency:** the benefit requires `rmem_max` ≥ 4 MiB. On an
  unprepared host the adapter warns once per stream and otherwise behaves as
  before.
- **SDK retries:** on an unprepared host the pinned SDK repeats its own
  `setsockopt` on every frame (upstream behaviour; Aravis is not modified).
- **Fixed size:** the 4 MiB request is a constant and is not configurable.
- **Not covered:** an IPv6 stream, or a socket bound to an address other than
  the one the device reports, gives `unknown`.
- **Unchanged:** 14 Hz remains unsupported by the production adapter;
  production timestamps remain invalid.
