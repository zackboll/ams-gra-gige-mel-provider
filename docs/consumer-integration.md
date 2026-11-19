# Integrating a MEL Consumer

This guide takes a native C++20 application from installation to one synthetic
Mono8 frame and clean shutdown. It describes this provider's implemented subset,
not the requirements of MEL in general. For declarations, use the pinned
[IR MEL](../third_party/ir-mel/include/) and
[Common MEL](../third_party/common-mel/include/) sources and the
[dependency provenance](upstream-provenance.md). The provider does not depend
on the separate MEL bridge.

## Install and choose a loading approach

On Linux, use a compatible C++20 compiler/runtime, CMake 3.24+, Ninja, Python
3.9+ (for the provider build) and POSIX threads. From the checkout, build and
install without optional SDKs (the default in a fresh build):

```sh
cmake --preset dev
cmake --build --preset dev --parallel 2
cmake --install build/dev --prefix "$PWD/build/install"
```

The install contains the shared provider and static backend under the CMake
install library directory, `cmake/GigeMelProvider` under that directory, and
the project headers plus pinned IR MEL, Common MEL and AMS Math headers under
the install include directory. It does not install `src/` interfaces, SDK
adapters, camera profile modules, the private JSON parser, or the example
sources/binaries. Take example sources from the matching repository revision.

### Link the installed CMake package

The [standalone example CMakeLists.txt](../examples/installed_consumer/CMakeLists.txt)
is the entire consumer build:

```cmake
cmake_minimum_required(VERSION 3.24)
project(gige_mel_consumer LANGUAGES CXX)

find_package(GigeMelProvider 0.1 CONFIG REQUIRED)
add_executable(capture_one main.cpp)
target_compile_features(capture_one PRIVATE cxx_std_20)
target_link_libraries(capture_one PRIVATE gige_mel::provider Threads::Threads)
```

| Imported target | Use |
| --- | --- |
| `gige_mel::provider` | Shared provider for direct calls to the MEL factories. Propagates the MEL and project include paths; ordinary consumers do not need to link the backend. |
| `gige_mel::mel_headers` | Header-only target for the pinned upstream declarations and their include path. Use this with `${CMAKE_DL_LIBS}` for an application that loads the provider at runtime instead of linking it. |
| `gige_mel::backend` | Static project-native backend library, for applications using or implementing the installed backend contract. Not the MEL channel API or an SDK selector. |

The package finds `Threads`; the example uses it for its own synchronization.
Build and run from the checkout (or copy the two example files to a separate
source directory and use its path with `-S`):

```sh
cmake -S examples/installed_consumer -B build/installed-example -G Ninja \
  -DCMAKE_PREFIX_PATH="$PWD/build/install"
cmake --build build/installed-example
./build/installed-example/capture_one
```

Expect one 64×48 Mono8 frame with 3072 copied bytes and a successful explicit
release. `CMAKE_PREFIX_PATH` is a configure-time search path, not a runtime
library path. For deployment, arrange for the loader to find the matching
`libgige_mel_provider.so.0` (for example through a suitable runtime search path).

Native embedding is separate: [provider.hpp](../include/gige_mel/provider.hpp)
declares `gige_mel::make_api_manager`, `ProviderSettings` and optional
`provider_statistics` diagnostics. An embedder explicitly supplies an
`ICameraBackend` from [backend.hpp](../include/gige_mel/backend.hpp); it does
not request one by passing a different name to the standard factory. These are
source-level C++ contracts, not a stable cross-toolchain SDK/plugin ABI. The
[runtime selection guide](runtime-selection.md) describes **internal**, uninstalled
selection and profile interfaces.

### Load the shared provider at runtime

The complete [dynamic capture example](../examples/capture.cpp) uses only the
pinned declarations and writes a PGM. The repository build produces
`build/dev/mel_capture`; to compile it separately using the installed headers,
run the following from the checkout (adjust `include` and `lib` for custom
install directories):

```sh
c++ -std=c++20 -isystem "$PWD/build/install/include" examples/capture.cpp \
  -pthread -ldl -o build/mel_capture_dynamic
./build/mel_capture_dynamic "$PWD/build/install/lib/libgige_mel_provider.so" capture.pgm
```

The POSIX lookup pattern, with an RAII owner for the `dlopen` handle (see the
dynamic example), is:

```cpp
namespace ir = ams::iface::irmel;
// SharedLibrary calls dlopen(path, RTLD_NOW | RTLD_LOCAL).
// symbol clears dlerror(), calls dlsym(), checks dlerror() and the address,
// then converts the address to the exact upstream function pointer type.
SharedLibrary library(path);
const auto get_manager = library.symbol<decltype(&getAPI_Manager)>("getAPI_Manager");
const auto get_control = library.symbol<decltype(&ir::getControl)>("getControl");
const auto get_buffer = library.symbol<decltype(&ir::getBuffer)>("getBuffer");
```

Fail before constructing provider objects if loading or any of the three
lookups fails; report the loader error. The symbol spelling is exact, including
case and underscores. C linkage supplies unmangled names, **not a C-callable
API**: the signatures use C++ strings/string views and shared pointers, and
channels use C++ virtual interfaces, callbacks and futures. Build both sides
against matching pinned headers and a compatible compiler, STL, runtime and C++
ABI configuration (including libstdc++ dual ABI/debug settings where applicable).
Prefer the same toolchain and standard library. Keep the handle alive until
all provider objects, callbacks, futures/results, buffers and deleters are gone.

## Initialize, discover and attach

1. Call `getAPI_Manager("synthetic")`. Check for a null result. The exported
   manager factory accepts exactly `synthetic`; this opens no device yet.
2. Call `ir::getControl("synthetic", manager)` (or the resolved equivalent).
   Check for null. Keep the same manager and instance for `getBuffer` as well;
   mismatched instances and foreign/null managers are rejected.
3. Call `control->init("")` on the owner thread, before sharing Control or its
   capability reference. The empty string (also `"mono8-bootstrap"`) is an
   aperture profile, not a device ID. It opens `synthetic-0` for this manager.
   Unknown profiles return `NotSupported`; open or initial configuration
   errors can return `Fail`. Stop setup on anything other than `Success`.
4. Search `control->getCapabilities()` for the desired `ChannelType` in each
   entry's `getChannelTypes()`. Default discovery includes `IRSTImage`,
   `CommandAndControl` and `HealthAndStatus`. Check
   `getChannelMetadataCapabilities()` before using an optional registration.
   An upstream declaration alone does not mean the provider implements it.
5. Build `ir::Config` with the advertised `getChanID()` and matching channel
   type, and copy platform and sensor location from the capability. For Image,
   pass your `std::shared_ptr<ir::ImageListener>` with `setImgLstnr`. Call
   `attachChannel(config)` and check for null. There can be only one attached
   channel per type per manager. Do not guess IDs or reuse them as global
   hardware identities. For typed operations, use a checked
   `std::dynamic_pointer_cast` to the requested interface, such as `ir::C2Channel`.

Keep strong references to the manager, Control, returned channel and listener
through use and teardown. Control retains attached channels and the Image
configuration retains the listener. Avoid reference cycles (for example, a
listener owning its channel). Keep callback state alive until callbacks have
drained. Discovery is a Control initialization snapshot; use the **attached
channel's** `getCapabilities()` for current layout, especially after commands.

## Capture and release an image

The complete [capture_one source](../examples/installed_consumer/main.cpp)
uses only installed headers. It attaches the advertised Image, registers one
host slot (the minimum), waits up to three seconds for the first callback, and
cleans up on success or failure. For a workload that keeps frames longer, size
the pool for your bounded in-flight work (default limit: 16 registered slots).

1. While disabled, verify current width, height, format, bits per pixel, bands,
   image size, row pitch and buffer size. The example requires single-band
   tightly packed Mono8, pitch equal to width, and a checked image size of
   `width * height` bytes. Allocate `getBufferSize()` bytes for each slot; do not
   just allocate `width * height` for other profiles. See the separate
   [Mono12 layout contract](mono12-image-profile.md) for the injected Mono12 case.
2. Call `getBuffer("synthetic", manager)` for each slot; check for null. Pass
   its stable allocation, byte count and optional application context integer
   to `buffer->init(address, size, context)`. Keep default `BufferFlag::Host`;
   use distinct nonoverlapping allocations and check each `init` and
   `channel->registerBuffer(buffer)` result. The caller owns the allocation;
   do not move, resize, reuse or free its bytes while registered or held.
3. Enable and check `channel->enable()`. The acquisition worker calls
   `ImageListener::onImage(channel, header, buffer)`. `FrameHeader` is a borrowed
   value for that callback; copy fields or the header if needed later. Check the
   delivered width/height, `getBitsPerPixel()`, `getNumBands()` and `getFormat()`
   against the expected layout. Inspect `getFrameID()` (low 32 bits of native
   ID), `getIntegrationTime()` and `getSystemTime()` as needed. The Image header
   does not supply row pitch: use the validated channel layout. For this
   profile `getImageAddress()` must be nonnull and equal to `getBufferAddress()`;
   calculate the byte count in a wide type and check it against `getSize()`
   before accessing the bytes.
4. Copy or process the pixels **before** calling `Buffer::release()`. Check
   the result. Use a cleanup path that releases every delivery, including an
   ignored extra frame or an exception in validation or copy. Publish your
   completion/error to the owner thread; the example signals only after its
   release attempt. After release, the slot may immediately be overwritten;
   only your copy is safe to use. Returning from `onImage` or dropping a
   `shared_ptr` **does not release the slot**.
5. Disable on the owner thread, release any slots still held by the application,
   unregister the buffers and detach the channel. Keep the allocations alive
   through all these operations.

The backend's [native `FrameLease` contract](../include/gige_mel/backend.hpp)
is not the MEL buffer contract. The native lease owns access to source memory
and retires according to the backend's lease lifetime. The provider validates
the source descriptor and copies pixels into an available registered host slot.
The consumer never receives a native lease or an SDK pointer. Bad input can be
rejected before any delivery, and a full destination pool drops incoming frames
instead of overwriting held buffers or allocating an unbounded queue.

You can hand off a delivered `shared_ptr<Buffer>` to another thread, but must
also preserve the backing allocation and agree on exactly one releaser. Neither
disable nor detach revokes already delivered bytes. A callback exception escaping
`onImage` stops acquisition; the provider cannot know what the consumer retained
and does **not** release the slot. Arrange release before unwinding or keep a
recoverable owner that releases it later. This also applies to callbacks already
entered when shutdown begins.

## Commands, metadata and reported state

Attach the advertised channel and register the supported overloads before
sending requests. Check registration results; a registered metadata pointer
is borrowed only for the callback. Copy what you need. Normal command metadata
runs on the submitting thread **inside `send()`**, before the caller receives
the future; install any correlation state first, and do not hold a mutex that
your callback needs across that call. Calls from different submitting threads
can overlap at callback dispatch; protect shared callback state.

### Configure the camera

`ImageChannel::send(CameraCommand)` and `C2Channel::send(CameraCommand)` enter
the same transaction. Send a **complete** staring configuration: dimensions,
row/column offsets, frame rate, integration time and zero TDI stages. `systemTime`
is caller context, not scheduled execution. Stop Image or Stack acquisition
first. Check the future for a typed error, then check `CameraCommandResp`'s
`getCommandSuccess()` and echoed command ID. An unsatisfiable setting can give
a valid response object with `commandSuccess == false`. An Image response
callback can receive the response from either submission channel, but an
error-only future does not fabricate an Image response.

For `C2Channel::send(ConfigSetCommand)`, set a command ID with `setCommandID`
and pass the provider-defined snapshot to `setConfig`, for example:

```json
{"schema":"gige-mel.camera-config","version":1,"width":64,"height":48,"offset_x":0,"offset_y":0,"frame_rate_hz":30,"exposure_ns":1000000}
```

This is **not standard MEL JSON**. All eight fields are required; settings are
integer tokens, not strings or partial updates. The object is limited to 4096
bytes and extra/duplicate fields are rejected. The
[configuration reference](configuration.md) defines units, bounds, exact
validation and failure outcomes. ConfigSet emits no Image CameraCommandResp.

`RequestFor<T>` is a `std::future<ErrorOr<std::shared_ptr<T>>>`. These command
paths return ready futures, but `send()` itself executes the transaction and
dispatches callbacks synchronously; `wait_for` on the returned future does
not bound `send()`. A future being valid/ready is not execution success. After
`get()`, check the `ErrorOr` (false means inspect `result.getError()`), and then
check the nonnull value and its outcome (e.g. `*result.get() == ir::Return::Success`
for ConfigSet). Check current attached Image capabilities after configuration,
even after a failed apply: actual readback can differ from the requested or old
settings. Invalid readback
prevents restart. Revalidate or resize and re-register buffers while disabled.

### Correlate C2 completion

Register a `CommandStatus` callback on `C2Channel` and match the exact echoed
command ID (use distinct IDs for outstanding requests). C2 does not need to be
enabled for these requests. In ordinary attached use, ConfigSet, C2
CameraCommand, the [software BIT command](software-bit.md) and the implemented
[Standby ModeCmd](mode-command.md) report terminal `Accepted` or `Rejected`
with a reason when applicable. There is no separate `Received` event, command
queue or history. Image-origin CameraCommand does not send status to C2.
Pre-admission errors (e.g. callback reentry or detached channel) can return an
error without a status; do not wait indefinitely for a callback instead of
consuming the future. Concurrent detach can also suppress a status whose
dispatch was not yet admitted without changing the future. The five documented
unsupported C2 commands return typed `Unsupported` errors and can emit
correlated `Rejected` statuses; that does not mean they executed or are
advertised. A metadata callback throw is contained and does not undo a
committed operation.

For example, accepted Standby stops the owned Image or Stack stream but does
not detach or release any held buffer. Command success describes the operation;
query current capabilities and Health for actual reported state, not the
last status event alone.

### Receive navigation acknowledgements and software Health

- **Navigation:** Submit a `mel::NavigationReport` on `ImageChannel`, with or
  without acquisition. Reports must pass structural checks and have positive,
  strictly increasing `systemTime` within the attachment. An accepted report
  yields `NavigationReportResp` in the ready future and in the optional
  registered synchronous response callback. It echoes `systemTime`; command
  and request IDs are zero because the input has none. Malformed/stale reports
  return errors, not success callbacks. On synthetic this acknowledges receipt
  only; it does not assert time mapping, navigation quality, or association with
  images. See [validation and receipt](navigation-reception.md) and the
  [optional native sink contract](navigation-backend-forwarding.md).
- **Health:** Attach `HealthAndStatus` without an image listener or buffers.
  Supported registrations publish provider-software `MFA_Status`,
  `MFA_StatusDetailed`, `BIT_Status` and `SubsystemStatusResp`. Registration does
  not invoke these callbacks inline. Enable starts the Health worker, which
  samples software state initially, periodically and when prompted by new
  registration; disable joins it. This is distinct from acquisition
  enable/disable. Subsystem reports use the limited software subsystem profile;
  see [Health](health-status.md) and [subsystem status](subsystem-status.md) for
  publication triggers and fields. No Image-style release applies to metadata.
  Valid idle configuration reports Standby, healthy owned acquisition reports
  Operate, and invalid configuration or owned terminal failure reports Degraded.
  This is sampled provider state, not a camera-health verdict.
- **Software BIT:** Use C2's `BIT_Configuration` registration for the catalogue
  and `BIT_Command` for the one supported immediate provider self-check. The
  command future/CommandStatus describes operation acceptance, **not** a
  passing BIT observation. Inspect `BIT_Status` for the latest completion and
  current software faults; these are different. No hardware BIT, diagnostic
  work queue or fault-clear service is provided. See the
  [software BIT contract](software-bit.md) for exact criteria and publication.

The [installed MEL contract test](../tests/install_consumer/main.cpp) exercises
these paths with only published MEL operations (its repository-local helpers
are not needed by `capture_one`). The broader
[provider contracts](../tests/provider_tests.cpp) exercise errors, callback
admission and shutdown as well as success.

### Conditional stacked-image delivery

`StackedImageListener::onImageStack` runs on a worker like Image delivery, but
returns `ir::Return`. `Success` continues acquisition; any other return (including
`NotSupported`) or a throw terminally fails it. **No return value releases a
buffer.** Use the declared stack layout and `StackFrameHeader` (including depth
and packed size) to validate the entire volume before reading. Image and Stack
acquisition are mutually exclusive. The
[dedicated installed stack example](../tests/install_stack_consumer/) uses an
explicit backend recording fixture; the default synthetic camera does not
advertise the stack channel. See the [stack delivery contract](stacked-image-provider.md).

## Stop, drain and destroy

Give a non-callback owner thread responsibility for teardown. The following
order keeps borrowed state, destination memory and provider code valid:

1. Stop admitting new application submissions, reconfiguration and buffer
   handoffs. Disable the Image or Stack channel and check the result. Disable
   periodic Health reporting if enabled. Disable joins the acquisition/Health
   worker on the owner thread, but does not drain unrelated synchronous
   submissions or release delivered slots.
2. Synchronize with request-producing threads, let admitted callbacks finish,
   consume or retire their futures and destroy any provider-owned results.
   Wait for application workers that still use delivered buffers. Release
   every remaining delivery exactly once. Release may safely overlap disable;
   never race it with another reader or another release of the same delivery.
3. Unregister each released acquisition buffer while disabled. Not all stop
   paths are alike: unregister refuses a still-outstanding slot; detach unbinds
   registered slots but does not release delivered ones. A retained slot can
   still be released after detach if its buffer object, host allocation and
   library are alive; prefer release before unregister/detach.
4. Detach the channels. External detach closes callback admission and drains
   admitted metadata callbacks, including callbacks reserved but not yet
   entered. Release channel references, callbacks/listeners and buffer objects;
   free host storage only once it is neither registered nor held. Destroy
   Control, then the manager. Finally close the dynamic library handle, if any.

Do not mutate lifecycle, register callbacks/buffers, send configuration or
destroy the provider graph from a callback. The implementation rejects
reentrant mutations; enqueue work for the owner thread instead. Do not wait in
a callback for work that needs that callback to return, or hold a lock across
disable, detach or thread join that the callback/consumer worker needs. Normal
delivery and synchronous metadata can overlap; this does not make concurrent
ownership changes or unsynchronized application data safe. Bounded frame waiting
is an application policy, not a hard shutdown deadline for blocking user code.

An ordinary stop requests cancellation and joins. A terminal stream error (or
escaping delivery callback error) stops publication but **retains acquisition
ownership** and can report degraded software Health. A frame wait timing out
alone does not tell you which happened: ordinary drops/invalid frames can also
lead to no deliveries. Explicitly disable even after failure, drain and release
as above, and fix the cause before re-enabling. Retrying `enable()` on a still
enabled, failed stream does not restart it. An embedding application can use
`provider_statistics` for diagnostics, not for a device-health certification.

## Limits and next steps

- The exported `synthetic` path delivers Mono8 without a camera. Its Image
  timestamps are host steady-clock nanoseconds for simulated integration start,
  not UTC or PTP, and virtual optics are not physical calibration. Health/BIT
  software wall-clock fields do not change the Image time domain.
- Aravis and Pleora have source acquisition evidence, but the current production
  adapter timestamps are invalid for MEL admission. A successful SDK grab does
  not demonstrate validated integration-start time, truthful frame metadata or
  production-qualified MEL delivery. See [status and evidence](status.md).
- Scheduling, Track, Instrumentation and the alternate direct channel
  factory/registry are not implemented. Most MEL commands (including
  calibration, protection, erase and tracking) and several required metadata
  surfaces remain unsupported; consult the [requirements traceability](mel-traceability.md).
- Image Euler/Report [LOS](image-los-metadata.md) and
  [StackedImage](stacked-image-provider.md) require eligible, explicitly supplied
  sources. Recording fixtures exercise these paths; default synthetic and
  production adapters do not advertise them. This is not full MEL 4.0 conformance.

Continue to the [C++ API Reference](index.md#c-api-reference) for the installed project
interfaces and the separate internal references. Start with the installed
[provider](../include/gige_mel/provider.hpp), [backend](../include/gige_mel/backend.hpp),
[navigation](../include/gige_mel/navigation.hpp) and
[line-of-sight](../include/gige_mel/line_of_sight.hpp) declarations for native
embedding; ordinary MEL applications should use the pinned upstream declarations.