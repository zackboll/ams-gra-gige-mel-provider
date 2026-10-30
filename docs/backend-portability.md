# Backend portability qualification

## Purpose

`provider_backend_contract_tests` (CTest `provider_backend_portability`) shows
that the **existing** common MEL provider works, through its existing C++
backend-injection seam, with two independently implemented camera backends.
The same MEL-facing scenarios run against both. A real GigE Vision SDK has not
been selected or qualified. The test backend is not a camera simulator, a
production factory selection or a qualified SDK.

The path under test is the existing embedding seam:

```cpp
auto manager = gige_mel::make_api_manager(settings, backend);
auto control = ams::iface::irmel::getControl(settings.instance, manager);
auto buffer  = ams::iface::irmel::getBuffer(settings.instance, manager);
```

The exported `getAPI_Manager()` still selects only the synthetic backend. This
suite does not add a runtime adapter registry, plugin loading or a new factory.

## The two cases and their deliberate differences

| Property | `synthetic` case | `contract-fixture` case |
| --- | --- | --- |
| Implementation | `make_synthetic_backend()` (production reference backend) | `tests/support/contract_fixture_camera.cpp`, test-only and independent |
| Backend ID / device ID / instance | `synthetic` / `synthetic-0` / `synthetic` | `contract-fixture` / `fixture-camera-17` / `portable-fixture-session` |
| Sensor | 640 × 480 | 384 × 256 (virtual) |
| ROI increments (W/H/X/Y) | 1/1/1/1 | 4/2/2/2 |
| Rate range | 1–120 Hz | 10–90 Hz; exposure ≥ 100 µs and ≤ one period |
| Initial configuration | 64 × 48 at (0,0), 30 Hz, 1 ms | 80 × 60 at (4,6), 40 Hz, 1,250,000 ns |
| Alternate configuration | 48 × 32 at (6,3), 60 Hz, 2 ms | 96 × 64 at (8,10), 50 Hz, 2,000,000 ns |
| Apply semantics | Atomic: validates everything before writing | ROI → rate → exposure; each group validated just before its write, so a later failure leaves earlier groups applied (truthful partial application) |
| Source rows | Tight (`stride == width`) | Padded: `stride = round_up(width,16) + 16`, padding bytes `0xA5` |
| Pixel model | `(x + y + id) & 255`, ROI-local x/y | `(3·sx + 5·sy + 11·(id & 255)) & 255`, **sensor** coordinates `sx = offset_x + x` |
| Native frame IDs | 1, 2, 3, … | `0x2'FFFF'FFF0 + 7k` (above 32 bits; low 32 bits wrap after three frames) |
| Stream generation | Starts at 1 | Starts at 900 |
| Configuration generation | Exactly the value given to `start()` | Exactly the value given to `start()` |
| Clock | `host_monotonic`, integration start = receive time − exposure | `device`, integration start; virtual clock starting at 7,200 s, advancing exactly one frame period per generated frame; clock generation 3 |
| Source pool | 4 slots | 3 slots in the shared cases (1 or 2 in direct checks) |
| Provider settings | Explicit: 0.001/0.001 rad/px synthetic virtual optics, 16 buffers | Explicit: 0.0005/0.00075 rad/px **virtual test** optics, 12 buffers |

The fixture's clock is a virtual model. It does not claim camera UTC, PTP
synchronization or physical timing. Its optics values are labeled test values,
not calibration.

## Shared scenarios (common provider guarantees)

Both cases run these six scenarios. Every backend-dependent expectation comes
from the `BackendCase` data. The shared code never branches on a backend name.
The same forwarding test probe wraps both backends. It records only opened
device IDs, discovery calls, `apply` calls and the readback the provider
observed. It is test instrumentation, not a production interface.

1. **Identity, instance lookup and discovery.** Construction and Control
   creation perform no device I/O. Wrong-instance Control/Buffer lookup is
   rejected, including the other case's instance and the standard factory's
   `synthetic`. `init()` opens exactly `settings.device_id`, once. Discovery
   and attached Image/C2 geometry match the case's initial configuration.
   Health has zero image geometry. Detailed Health reports the case's
   `backend_id` and `device_id`.
2. **Configuration commands and selected capabilities.** These are exercised
   through CameraCommand via C2 and via Image, and through version-1 ConfigSet
   via C2. The checks are ready futures, command-ID correlation, Image reply
   versus C2 status routing, and updated geometry after actual readback. Five
   shared capability probes are each judged by the selected case's own
   capabilities. Each case states the outcome: accepted, rejected by provider
   prevalidation (no backend `apply`), or rejected by backend application. For
   example, `64x48@(0,0) 120 Hz` and `81x60@(4,6)` are valid for synthetic and
   invalid for the fixture, and `400x300` fails provider prevalidation only for
   the fixture's sensor. After a backend rejection, the next state is the actual
   readback, not the request. Active acquisition rejects every configuration
   path without calling `apply`.
3. **Image publication and row repacking.** Buffers are sized from the current
   advertised geometry, with no hardcoded byte counts. This runs in the initial
   and alternate geometries and checks, for every frame:
   - dimensions, offsets, exposure, Mono/1 band/8 bits and subframe fields;
   - frame-ID conversion (low 32 bits of a documented native ID; gaps allowed);
   - the case's timestamp model and per-frame spacing;
   - field of view from the case's settings;
   - every pixel against the documented model, computed from inputs.

   Padded source rows must arrive tightly packed.
4. **Destination retention, release and restart.** A delivered registered MEL
   buffer is held while acquisition continues, across disable and across a
   refused restart. Its bytes stay unchanged and its slot is never reused.
   After explicit release, a fresh acquisition delivers valid frames.
5. **Health, BIT and Subsystem.** Idle is Standby/idle/Idle. Owned acquisition
   is Operate/running/Imaging. The provider-software BIT executes and its Pass
   completion is observed. Ordinary stop returns state to idle. Subsystem
   identifies the provider software, not the backend. Detailed reports always
   name the selected backend/device. Freshness uses payload predicates after
   captured baselines. Sequence numbers are checked only for monotonic increase
   within one Health interval, with no exact `+1`.
6. **Disable drains an admitted Image callback.** The callback is held at a
   watchdog gate. Disable's actual acquisition-worker `pthread_join` is
   observed (with the existing `test_join` interposer) before the suite checks
   that disable is still waiting. Gates open on every unwind path before
   futures are waited on. Collectors outlive drain. A watchdog expiry fails
   the test.

Item 4 concerns **MEL destination-buffer** ownership. Because the provider
copies pixels out of the backend lease, it does not by itself prove native
source-lease retention.

## Fixture-specific checks (not shared guarantees)

Two direct-backend checks exercise the fixture's own documented semantics.
They are not requirements for future SDK adapters: pool size, ID sequence,
generation numbering and allocation strategy are fixture choices.

- **Source lease outlives stream, device and backend.** The check holds two
  leases. A full pool produces a bounded timeout and one starvation event.
  Released storage is recycled. A receive that is actually blocked (observed
  by a fixture wait counter) is promptly cancelled by `request_stop()`.
  Restart is refused while leases remain. The check then stops the stream,
  closes the device logically and destroys the stream, device and backend in
  that order, after joining the receiving thread. The held descriptor and
  bytes stay valid and unchanged, and the pool and shared state survive. They
  retire exactly once, on the final lease release. The observation counters
  hold nothing of the fixture's, so observing never keeps the resource graph
  alive.
- **Close cancellation and truthful partial apply.** A rejected rate leaves
  the already-written ROI in actual readback. Logical close wakes a blocked
  receive with `disconnected`, and later I/O is refused. The held lease
  survives teardown.

Native `FrameLease` retention (above) and copied MEL destination-buffer
retention (shared scenario 4) are different guarantees and are tested
separately.

## Adding a future adapter case

A real adapter can run the shared scenarios against an attached camera by
adding a `BackendCase` with:

- a factory returning its `ICameraBackend`;
- explicit `ProviderSettings` (instance, stable device ID, characterized optics
  and its timestamp domain);
- expected backend/device identities;
- initial and alternate configurations valid for that camera;
- the outcome its capabilities imply for each shared capability probe;
- its documented pixel model (or a test pattern/known scene), native-ID model
  and time model.

Do not add backend-name branches to shared scenarios. If an adapter cannot
supply a fact, such as a deterministic pixel pattern from a physical sensor,
record that limitation explicitly. Do not weaken a shared assertion for every
case. The existing `backend_contract` suite remains synthetic-specific; a real
adapter needs its own direct contract tests in the style of the fixture checks.

## What still requires real SDK/camera qualification

This suite does not establish any of the following:

- SDK compatibility or vendor buffer semantics (announce/queue/revoke);
- callback threads, disconnection or reconnection;
- packet loss/resend, bandwidth, latency or trigger timing;
- device clock mapping, PTP/UTC or calibration;
- multi-camera behavior or physical health.

The roadmap's "first real camera" (P2) and "second implementation" (P3)
milestones remain open: both cases here are software backends.
