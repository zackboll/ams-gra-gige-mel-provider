# GIGE-MEL-034: pre-implementation ownership and dispatch design

Baseline: main `7a2539a123dcb1a0cb1d5a7d7884d1b38c615c66`, tree
`062bf6b9e239f86a29af0026671c71184ca97297`; PR #27 verified merged.

## Existing dependency audit

* Aravis `open()` reads `mono_free_running::NativeSnapshot`, builds capabilities,
  and retains the snapshot as `DeviceCore::envelope`.
* Aravis `read_configuration_locked()` and `AravisDevice::apply()` call shared
  monochrome read/apply orchestration. `start_locked()` reads a new snapshot,
  invokes monochrome preflight, checks Continuous acquisition and plans a pool.
  `AravisStream::receive()` decodes native buffers then invokes frame validation.
  `inspect()` also uses monochrome snapshot/capability/configuration translation.
* Pleora `Backend::open()` performs the same snapshot/capability construction,
  retaining the concrete envelope in `State`. `Device::configuration/apply()`
  call monochrome orchestration. `Source::start()` reads a fresh snapshot,
  preflights, checks Continuous and plans a native pool. `Stream` plans its copy
  pool and calls monochrome frame admission after decoding a PvBuffer.
* Feature aliases, units, numeric conversion, free-running/manual checks,
  capability interpretation, immutable-envelope comparison, ROI/timing write
  ordering, partial-write readback and frame admission are camera policy.
* Native discovery, exact identity/reservations, feature node access/type/range
  checks, connection-loss priority, SDK command recovery, stream setup, native
  buffer decoding, resource budgets, cancellation, leases and copy mechanics
  belong to adapters. They are not profile registration or selection work.

## Intended boundary

`ICameraProfile` identifies a statically linked implementation and explicitly
opens a borrowed `IDeviceFeatures` service into an owned `ICameraProfileState`.
Open constructs and validates the profile-specific native snapshot. Each concrete
state stores its own snapshot type; no `void*`, unchecked cast, shared universal
snapshot, native SDK object or retained feature service is necessary.

The state supplies capabilities, fresh configuration readback, validated ordered
apply/readback, acquisition preflight (including camera acquisition-mode policy),
and frame validation over project-owned facts. The existing monochrome functions
remain the implementation of the default concrete state. Frame facts and checked
pool sizing move to the neutral profile boundary without changing their rules.

Backends explicitly receive `shared_ptr<const ICameraProfile>`; compatibility
factories supply a new default profile. Backend and device/stream cores retain
the selected implementation and state through teardown. Immutable state methods
are const; native operations retain existing adapter serialization and connection
checks. Frame validation can run concurrently with device operations and must be
thread-safe. No profile adds locks or calls the provider. Leases retain their
existing storage owners, not native feature services.

The explicitly owned catalog separately registers profile factories and backend
constructors accepting the selected profile. An explicit list of compatible
profile IDs belongs to each backend registration. Exact matching precedes any
constructor call. Profile factories create independent session instances; no
registration/selection does camera I/O. Synthetic retains only its historical
fixed-profile compatibility entry, not an assertion of arbitrary dispatch.

A test-only independent profile/state will deliberately restrict configuration
and frame admission while reusing tested low-level monochrome algorithms by
composition where appropriate. The same implementation must execute through both
native fixture paths; it is not a physical-camera support declaration. Tests
will cover rejection before writes, changed native state, teardown/reopen, catalog
destruction, independent instances and constructor failures without fallback.