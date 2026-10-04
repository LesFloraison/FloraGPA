# Original initialization references

Reviewed 2026-10-04. This M3 batch adds read-only `original_initialization`
metadata to the native `commands` export. It recovers the pinned GPA 2025 R1
player's fixed reference collectors for 25 wire types. It does not change GPU
execution or establish traditional Command List playback.

## Two different relationships

An API record's saved resource references are not its ERG initialization
prerequisites. Keep both representations:

| Record family | Original collector registers | Examples deliberately omitted by that collector |
|---|---|---|
| Observed Draw / Dispatch variants | Context | State snapshot, indirect argument buffer |
| Observed CB setters | Owner | Bound constant buffers |
| MapCapturedWrites | Owner; data when its full saved QWORD is nonzero | Target resource |
| Clear / GenerateMips | Owner, view | Resources reachable through the view |
| CopyResource / CopySubresourceRegion | Owner, destination, source | Other API fields |
| CopyStructureCount | Owner, destination, source UAV | Byte offset |
| UpdateSubresource | Owner, destination, data | Box and pitches |
| ClearState | Owner | No other fixed reference |

These omissions are **not** permission to skip GPU bindings or missing resources.
Existing API references, replay validation and execution remain unchanged.

`collector_sequence` preserves ordering, duplicates and zero IDs. `dependency_set`
is the sorted unique projection, not an initialization schedule. Each reference
retains its full captured ID and the original player's low 32-bit ID; truncation
is explicit. Map tests the full 64-bit data ID before truncation, so data zero is
omitted while `0x100000000` contributes zero. The production reader must fully
decode the record before recovery; truncated, invalid and trailing-byte records
do not acquire a recovered sequence.

The whitelist is restricted to types supported by evidence, rather than API-name
aliases. Exact types and collector RVAs reside in
`src/application/InitializationReferences.cpp`. The profile names the pinned
player SHA-256 prefix `39061ff3`; this is not a multi-version claim.

## Acceptance evidence

Two unchanged captures were replayed through the original player with forwarding
slot-4 reference observers. The previous uninstrumented control runs from the
[scheduler batch](INITIALIZATION_SCHEDULER_AUDIT.md) were reused, with identical
capture/player hashes. Both raw output images are byte-identical to their controls.
All observers were restored, and original lifecycle/graph checks passed.

| Capture | Observed ERGs | Exact C++ collector/graph matches | Explicit CSUAV gap |
|---|---:|---:|---:|
| BF1 | 6,248 | 6,033 | 215 |
| Immutable merged-list mode 32 | 86 | 77 | 9 |
| Total | 6,334 | 6,110 | 224 |

Across these captures, 25 of 26 observed collector wire types are recovered.
This is coverage of this observed set, not overall DX11 or M3 completion.

An isolated native probe additionally constructs original ERGs and invokes their
actual collectors on 25 complete original record layouts with six ID variants
each: captured values, zeros, high-word-only IDs, all ones, duplicates and seeded
random values. All 150 comparisons pass. It changes only QWORD fields, preserving
record sizes/counts/flags. No malformed record is sent to the original unsafe
reader. No GPU, fake COM object or DLL patch is involved; original allocations
remain owned by that isolated process until exit. These are boundary experiments,
not additional accepted captures.

Native C++ tests cover omitted API resources remaining visible, truncation,
duplicate/zero IDs, Map's zero distinction, unsupported aliases and hundreds of
invalid/truncated/trailing records. Five new evidence-checker counterexamples and
seven existing scheduler-checker tests pass. Related CTest and the four serial
GF2/BF1 positive/negative golden checks pass. The full 470-case inventory was not
rerun; its registrations and prior outcomes are unchanged.

## Remaining boundaries

`CSSetUnorderedAccessViews` (`0x25e`, collector `0x23c30`) adds nonzero UAV IDs only
when native cache membership (`0x9b8e0`) succeeds. That cache has category-specific
maps and cannot yet be replaced by file-entry existence. Its 224 observations
match their original graph snapshots, but production exposes `unrecovered` and a
reason rather than guessing the sequence.

This batch does not reconstruct complete cache population, resource/state/data
collectors, edited version transitions, readiness or reinitialization. Traditional
Execute operand identity, list ownership and a real unmodified traditional-list
capture are still needed. `registration_sequence_available` and
`execution_dependencies_complete` remain false, including for recovered collectors.
The native list execution rejection remains in place.

## Reproduction and provenance

The [baseline](initialization-references-baseline.json) pins implementation and
local evidence hashes. Captures, native traces and generated exports stay outside
Git. Development tools require the original pinned DLL; production requires none.

```powershell
python tools/trace_original_erg_initialization.py <capture> --reference-tools D:/CDXrepo/FloraGPA/tools --references --out <fresh-observed-dir>
FloraGPA.Cli.exe commands <capture> --out <fresh-commands-dir>
python tools/validate_original_reference_evidence.py --case <observed-dir> <original-control-dir> <commands-dir>/commands.json --out <fresh-audit-dir>
python tools/probe_original_reference_collectors.py --case <capture> <commands-dir>/commands.json --reference-python D:/CDXrepo/FloraGPA/standalone --out <fresh-probe-dir>
python -m unittest discover -s tests -p test_original_reference_evidence.py -v
```

Pass both BF1 and merged mode 32 as repeated `--case` arguments to reproduce all
25 fixed types. Run original playback serially. Static corroboration is preserved
in `artifacts/m3-reference-collectors-functions.c` and
`artifacts/m3-reference-presence.c`; the read-only Ghidra runs modified no binaries.
