# GIGE-MEL-045 design and pinned declaration audit

Base main `1ba4f6dc00e391d2749224d8333269f6c52e1a26`, tree
`51fc8a74790cfed0245e1b1d296b2dcb604440e4`; branch
`feature/gige-mel-045-task-scheduling-foundation`.

## Actual upstream API

Reviewed IR MEL `8d9224519f12b44e0b28815755c56a32a28d24a0` and Common MEL
`f6908437d8fd2f7fb69896f9eb9cfd272d10c439` from detached upstream checkouts.
Existing 100 manifest entries/bytes and all revisions are preserved. Recursive
SchedulingChannel closure adds **20 unchanged IR headers**, 120 total files.
Added blob SHA-1 identities were independently checked against the pinned Git
tree before copying/SHA-256 locking. No upstream declaration is edited.

| Declaration | Annotation / signature consequence |
| --- | --- |
| C2 ModeCmd / MFA_Mode | Required; send returns RequestFor<MFA_Mode>, not Return |
| TaskInfo, StareTaskParam, AbortTask | Required data definitions; independent request ID, time, action, priority, precedence |
| IRSTStareTask / exec | RequiredIfIRSTStareTasking; RequestFor<Return> exec(IRSTStareTask) |
| SchedulingChannel | Class RequiredIfIRSTTrackTasking, but member conditions differ; destructor/prioritized-list send Required |
| Abort exec | RequiredIfIRSTScanTasking, RequiredIfIRSTStareTasking and RequiredIfIRSTTrackTasking; RequestFor<Return> |
| Scan / track exec | RequiredIfIRSTScanTasking / RequiredIfIRSTTrackTasking |
| ExecuteTaskAck / TaskExecutingRep | RequiredIfIRSTTrackTasking data/callbacks; truthful stare ack does not claim tracking |
| SchedCreatedRep | Data RequiredIfScanVolumeSchedulingOnMFA; callback RequiredIfIRSTTrackTasking: distinct conditions |
| UpdateTrackListResponse callback | RequiredIfTrack (exact pinned spelling) |
| TaskEvents / ScanPerformanceReport callbacks | Optional; TaskEvents is scan-oriented, not generic completion |
| ChannelCapability | Required; zero depth explicitly means immediate replacement |
| CommandStatus / Return | Exact pinned Accepted/Rejected, Success/NotSupported; no new upstream result types |
| RequestFor<T> | Common MEL future of ErrorOr of shared_ptr<T>; existing error mapping |

AbortTask only exposes constructors/assignment, with private TaskInfo and no
getter. No layout tricks or targeted ID abort. TaskExecutingRep requires Task
values; pinned Task lacks a documented stare mapping. It remains unsupported,
rather than inventing one. Neither gap blocks truthful admission/current abort.

## Ownership and linearization

The optional interface is separate from backend.hpp, preserving existing
SDK/MEL-independent production adapters. Runtime owns one unique session and a
fixed 256-entry duplicate-ID history, further bounded by source declarations.
Operations serializes mode/admit/abort/retire against acquisition/configuration.
Health reads session ownership under state_mutex without calling the source or
claiming completion. Source admission is acceptance; replacement is atomic and
rejection nonmutating. Owned-value requests preserve both correlations.

Ack function/result preparation precedes source admission; reservation precedes
operation unlock. Detach closes admission and drains reservations. ModeCmd also
reserves terminal status. No new worker or queue. Fixture-owned virtual time and
state explicitly progress execution/completion. Standby failures retain ownership;
mandatory teardown retirement remains a bounded recovery route, not fabricated
cancellation success. Image/Stack buffer ownership is untouched.

See [profile](../task-scheduling-provider.md) for exact inputs, finite bounds,
capacity, metadata, shutdown and conformance gaps.