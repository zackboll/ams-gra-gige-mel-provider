# IR MEL traceability inventory schema (version 1)

Start at [Requirements Traceability](../mel-traceability.md). Each family JSON
file has `schema: 1`, a `family`, and `operations`. Edit the relevant family file;
the docs target renders the review report automatically. Do not edit the generated
HTML or change the lock to make a missing disposition pass.

| Field | Contract |
| --- | --- |
| `id`, `family`, `kind` | Stable local key `DeclaringInterface.Operation` (callbacks use `cb-Payload`; sends/execs use input type). Kind is `operation` or `type seam`. No formal upstream ID is implied. |
| `upstream` | `component`, `path`, `declaring`, whitespace-normalized exact `signature`, one-based `line`, literal `annotations` (method only). Combined class/namespace + signature distinguishes overloads. |
| `related_upstream` | Additional `{component, path}` for payload, capability or Common MEL context. |
| `obligation` | `required`, `conditional`, `optional`, `unresolved`; never derived from implementation status. |
| `applicability` | `condition` records class/method predicate and interpretation; `current` is `applies`, `not-current`, `fixture-only` or `unresolved`. Required source gaps are not recategorized as optional. |
| `status`, `behavior`, `availability` | Status vocabulary in the overview; behavior describes observed implementation; availability names attachable families or gates, separately from metadata flags. |
| `metadata` | `null` for non-registrations; otherwise `enumerator` and `advertisement`: `advertised`, `source-gated`, `not advertised`, or `missing enumerator` (null enumerator). |
| `implementation` | `{path, symbol}` references to concrete implementation/shared seam. Empty if none. Macros identify the specific expansion. |
| `evidence` | `{path, scenario, result, level, demonstrates}`. Results are `positive`, `negative`, or `indirect`; levels are `SDK-free`, `native SDK fixture`, `simulated transport`, or `physical-camera observation`. An empty list is no cited operation-level test. Non-SDK-free entries reference historical scenarios, not new results. |
| `gap`, `responsibility`, `gap_groups` | Next action or null, primary integration layer, and zero or more report group keys (defined in validator). For a partial row the next action names the missing behavior, source or evidence. |

## Reconciliation procedure and exceptions

`upstream-lock.json` is the separately reviewed declaration census. It records
Git tree and blob identities (not replacement headers) and links each declaration
to its disposition. To refresh a pin, inspect `git ls-tree -r` on **both** exact
commits, scan the complete include trees for abstract/virtual providers, exports,
factory types, and delivery listeners, then compare the declaring signatures,
tags, and class inheritance with the lock and each row. Closures and dependencies
must be checked independently with the repository vendor verifier.

The census counts declared operations including teardown and loader helpers;
seven `IRCreateFunctions` entries are counted separately as type seams, not
provider export functions. `using Channel::send` and `using Channel::registerMetadataCallback`
re-expose the same declarations, not new requirements. Exclude value/data accessors,
ordinary and deleted constructors/assignments, internal storage and the unrelated
Common MEL security event value hierarchy. `StackedImageListener::onImage` is
included because it is a distinct inline override with different semantics.

Manual entries cover the global C-linkage pragma interposition, implicit class
lifetime on `API_Manager`, unannotated Control/Buffer exports, the protected
`IRFactory::baseCreateChannel` template, typed `IRInterfaceBase::QueryInterface`,
`IRRegistry` and `LibraryLoader` members. Loader function pointer fields repeat
the three factory declarations and are not separate operations. No out-of-line
loader bodies or `create*Channel` declarations are present at this IR pin.
Where comments disagree (including the IR registry's RF reference), use
`unresolved` and preserve the source rather than inferring a contract.

```sh
python3 -B scripts/check_mel_traceability.py --self-test
python3 -B scripts/check_mel_traceability.py --self-test \
  --upstream-checkouts /absolute/path/to/pinned-ir /absolute/path/to/pinned-common
```

The ordinary docs path is offline: it compares pins to the project manifest,
checks vendored blobs and their exact signatures/tags, checks nonvendored
references against the census, and validates disposition, references and evidence
shape. With pre-existing pinned checkouts it also verifies tree/blob identities and
the exact declarations and annotations for nonvendored files. The census is a
manual reconciliation, not a complete C++ parser. Static matching cannot prove
the correctness of a status, applicability decision, test assertion or runtime
capability; reviewers must inspect the linked sources and actual test results.