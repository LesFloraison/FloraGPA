# Traditional list dependency metadata

Reviewed 2026-10-05. This M3 batch closes two C++ migration gaps in the
[initial dependency graph](INITIALIZATION_GRAPH_AUDIT.md). It does not enable
traditional Command List replay. The [baseline](list-dependencies-baseline.json)
pins the implementation and local evidence hashes.

## Implemented behavior

| Saved record | Length | Native collector | C++ initial dependencies |
|---|---:|---|---|
| Resource `5/0x9a` | 16 bytes | `0x5bad0` | Parent context at offset 8, truncated to uint32, including zero |
| ERG `7/0x41` | 28 bytes | `0x23be0` | Owner at offset 8, truncated to uint32, including zero |

Neither collector adds the captured list pointer / Execute operand. The record
link and restore-state flag also do not add initialization dependencies. The
export retains the full saved owner alongside its original low-word identity.
This is native initialization metadata, not a replacement for API references.
Raw API `0x30d1` remains outside the original ERG descriptor type range; its
existing API decoding does not make it an ERG graph node.

Malformed known records return `invalid_record` with no partial dependency
sequence. Missing dependencies identify the referring entry and missing ID.
Unknown types remain `unrecovered`. Even when all initial dependencies exist,
`execution_supported` and `initialization_schedule_available` stay false.
Production replay still rejects unverified traditional Execute paths.

There are now 61 implemented category/type collector rules (23 resource, ten
data, one state, 27 ERG). The earlier unchanged-capture observations still cover
59 rules. The two new rules have native CPU evidence, not acceptance on an
unmodified traditional-list capture. Inventory and module migration counts do
not change.

## Verification

The hash-pinned original DLL allocated real native objects through its factories.
Complete, bounded records were passed to the actual collector vtable slots:

- 16 Command List resource cases: four parent values crossed with four pointer
  values, including zero, high-word-only, ordinary IDs and high/low-word markers.
- 64 Execute cases: the same combinations crossed with restore values 0, 1, 2
  and `UINT32_MAX`; a separate nonzero high-word record link remains inert.
- All 80 ordered native outputs and dependency sets match independent C++ export.
  Generated captures are explicitly CPU metadata fixtures. No COM invocation,
  GPU execution or operand dereference is claimed by these native probes.
- The existing default probe route was rerun: 120 native resource/data calls plus
  six reused state observations match all 126 C++ results.
- Six relevant CTest suites pass. New cases include every short record length,
  one trailing byte, missing references, high-word IDs, unknown-type rejection,
  API-versus-ERG classification, and a complete graph that cannot claim execution.
- 35 existing evidence-checker tests pass. GF2 and BF1 each pass their strict
  golden pixel hash and draw-suppressed negative control, serially, in the
  isolated runtime environment.

The complete corpus, GUI acceptance and release packaging were not rerun; no
runtime GPU semantics or UI changed. This batch adds no original capture sample.

## Investigation boundary

The contemporaneous read-only Ghidra inspection found the shim string
`ExecuteCommandLists` referenced by a startup text dictionary (`0x3a6ec0`). That
reference does not establish a setting to disable deferred-command expansion.
The `StreamCommandListDX11` vtable continues to point to the already investigated
Prepare method at `0x317500`; the pinned player's Execute ERG virtual dispatch
slot points to `0x166a0`. These observations support the existing investigation,
not an inference that every GPA version lacks another route.

The earlier reference investigations remain in the unchanged local workspace:
`D:/CDXrepo/FloraGPA/analysis/COMMAND_LIST_EXECUTE.md`,
`COMMAND_LIST_BUILD.md`, `COMMAND_LIST_DISPATCH.md` and
`LEGACY_CAPTURE_VALIDATION.md`. Their deliberate research rewrites are not
original-capture acceptance. In particular, owner collection does not settle
whether the saved Execute operand is a pointer or resource ID.

Remaining M3 work is still the file-to-list identity/lifetime mapping, original
list construction and resource-version ordering, followed by original unmodified
traditional-list capture acceptance. The separate GPA playback/experiment cache
version study must not be treated as temporal resource-history recovery. Expanded
immediate-stream captures retain their current replay classification.

## Reproduction

```powershell
python tools/probe_original_node_references.py --lists-only --out <fresh-native>
python tools/validate_node_reference_probes.py --nodes <fresh-native>/nodes.json --exe build/vs2022/Release/FloraGPA.Cli.exe --qt-bin D:/Qt/6.11.2/msvc2022_64/bin --out <fresh-comparison>
ctest --test-dir build/vs2022 -C Release -R '^(initialization_graph|api_commands|commands|contexts|command_state|frame_validation)$' --output-on-failure -j1
```

The native probe is development-only and requires the pinned installed player.
The C++ `initialization-graph` command remains independent of GPA and Python.
Captures, original binaries and generated evidence stay outside Git.
