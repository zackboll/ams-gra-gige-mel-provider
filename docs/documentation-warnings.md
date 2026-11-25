# Doxygen warning baseline

The documentation target records Doxygen diagnostics in `doxygen-warnings.log`
beside the generated site. CI retains that file separately from the Pages artifact.
The reference extracts undocumented project symbols; absence of prose on private
internals is not treated as a warning or a build failure.

Baseline with Doxygen **1.14.0**: one warning, in
[`gige_mel::gentl::Device::with_registers`](../src/transport/gentl/gentl_consumer.cpp).
Doxygen extracts the declaration in the class reference and displays the full
implementation in the source browser, but cannot pair its definition with the
declaration because it resolves the `reg::Status` alias incorrectly inside the
callback type. This affects automated declaration-to-definition linking for that
member, not the C++ build or GenTL behavior. No configuration or unresolved
reference warnings are expected. Leave alias cleanup to a targeted follow-up;
do not change the implementation just to silence the parser.

The site validates internal links, anchors, and source browsers. Links out to
upstream sites are not crawled in CI. Historical evidence and SDK setup records
are linked at the source commit rather than rendered or packaged in the site.