# ADR 0001: Native C++ MEL provider with internal C++ backends

**Status:** Accepted for the bootstrap.

## Context

The project must provide AMS-GRA IR MEL objects to Skills, and it must be adaptable to more than one GigE Vision implementation. The user chose C++ for the first provider. The upstream interface is already a C++ object API with C-linkage factory symbols. The existing MEL bridge is a consumer-side interoperability layer and does not define this provider's required implementation language or backend contract.

## Decision

Implement one native C++20 MEL provider. Use the pinned upstream declarations without replacing them with locally invented equivalents. Start with the `getAPI_Manager`, `getControl`, and `getBuffer` loading path and normal channel attachment.

Keep SDK access behind project-owned C++ backend interfaces. Build adapter implementations with the provider using ordinary CMake targets and C++ factories. Keep camera feature/model translation separate from SDK mechanics as real adapters are introduced. The synthetic backend is the first implementation and must work without hardware or commercial SDK dependencies.

Do not introduce a provider-facing C ABI, multilingual provider SDK, network service, or stable binary backend-plugin protocol in this bootstrap. Those are separate products with separate versioning and lifetime requirements. Continue to allow the existing consumer bridge to consume this provider through MEL.

## Consequences

- MEL methods, callbacks, futures, and buffer semantics can be implemented directly in the language of the upstream API.
- Each adapter shares lifecycle, command validation, MEL metadata, and destination-buffer code.
- The provider and its clients need compatible C++ ABI settings. `extern "C"` controls symbol names; it does not remove `std::shared_ptr`, `std::string`, or standard-library ABI requirements from the signatures.
- Internal backend headers can evolve with the project. Rebuild all adapters on interface changes; do not load arbitrary independently compiled backend binaries.
- Adding a second real SDK is an architectural acceptance test. It must not require copying the MEL facade or exposing vendor types in common headers.
- A later requirement for separately shipped plugins, process isolation, or a provider in another language needs a new ADR and a versioned boundary.

## Evidence

The pinned upstream [Control](https://github.com/open-arsenal/ams-gra-hello-world-sk-interfaces-ir-mel/blob/8d9224519f12b44e0b28815755c56a32a28d24a0/include/irmel/library/irmel-types/Control.h) and [Buffer](https://github.com/open-arsenal/ams-gra-hello-world-sk-interfaces-ir-mel/blob/8d9224519f12b44e0b28815755c56a32a28d24a0/include/irmel/library/irmel-types/Buffer.h) headers declare the C++ factory results. The alternative registry-loading interface is documented in upstream [IRFactory](https://github.com/open-arsenal/ams-gra-hello-world-sk-interfaces-ir-mel/blob/8d9224519f12b44e0b28815755c56a32a28d24a0/include/irmel/library/factory/IRFactory.h).
