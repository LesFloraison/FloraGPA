# Initial dependency graph recovery

Reviewed 2026-10-04. This M3 batch adds a CPU-only C++ export of initial resource,
state, data and ERG dependencies for the pinned GPA 2025 R1 DX11 format. It builds
on [initial cache membership](INITIAL_CACHE_AUDIT.md). All 19,387 registered nodes
in three unchanged captures match the original collectors and dependency sets.
Traditional Command List GPU execution remains unaccepted.

## Available command and exact meaning

```powershell
FloraGPA.Cli.exe initialization-graph <capture.gpa_frame> --out <new-or-empty-directory>
```

The command accepts only `--out` and writes `initialization-graph.json`. It uses
the independent checked reader and requires neither GPA nor Python. Nodes retain
their capture ID, category, wire type, original collector RVA, full saved reference
IDs, original low-word IDs, ordered references including duplicates, and unique
dependency sets. Missing descriptor references identify both the referring node
and the missing ID. Malformed known layouts return `invalid_record`; unverified
types return `unrecovered`. Failed decoding does not expose partial references.

`dependency_graph_complete` means every registered initial node has a recovered
collector and every collected dependency has a registered descriptor. It is not
a claim of an acyclic or executable graph, valid GPU objects, later resource
versions, or a recovered initialization schedule. Both
`initialization_schedule_available` and `execution_supported` remain false. The
latter describes this metadata reconstruction, not whether the existing ordinary
replayer can play the capture. CLI `completed`/exit zero means the export finished;
consumers must inspect the graph/node statuses rather than treating it as playback.

The preceding initial-index restrictions still apply: categories 3/5/7/9, zero
entry flags, uint32 file IDs and no view edits. Other profiles produce an explicit
unrecovered cache and no claimed complete graph. Unsupported non-ERG types are not
silently treated as dependency-free. Category-only API records remain separate
from actual ERG descriptors.

## Recovered collector rules

This adds 33 category/type pairs: 22 resource layouts, ten data layouts and one
state layout, alongside the preceding 26 ERG types. These are dependency metadata
rules, not additional GPU execution support. Native collectors also deserialize
objects and sometimes access caches or devices; those side effects are not cloned
by this read-only feature.

| Family | Initial dependency behavior |
|---|---|
| Device `0x81`, SwapChain `0x38` | No collected references, including their saved owner fields |
| Input Layout `0x82` | Layout data only; original rejects a zero low-word data ID |
| Buffer/textures `0x83..0x87` | Owner and initial data, with the observed per-type zero filtering |
| Texture2D `0x85` | Additional original-object low word when saved CPU-access high bit is set and Usage is zero |
| State resources and context | Owner, with mandatory versus optional zero retained per collector |
| Views `0x8c..0x8f` | Owner and viewed resource; viewed-resource zero is retained |
| Shaders `0x90,0x92..0x95` | Owner, shader data, linkage, in that order; the saved SO-data field is omitted |
| Input-layout data `0x84` | Every semantic ID, including zero and duplicates |
| Counter data `0x104` | Owner, including zero |
| Other eight observed data types | Length-checked payloads, no collected references |

All resource/data IDs above are truncated to uint32 before optional zero filtering.
The pre-existing Map ERG collector's full-QWORD nonzero test remains a separate
rule. Fixed record sizes and variable arrays/blobs are checked before use; array
length multiplication is widened before bounds checking.

State `3/3`, collector `0x63d80`, has a particularly non-obvious order:

1. Input layout and all vertex buffers.
2. Constant buffers, samplers and SRVs, each interleaved by slot across
   VS, GS, PS, HS, DS, CS.
3. Shaders in that same stage order; graphics-stage classes up to their saved
   counts. CS classes and the index buffer are omitted completely.
4. CS UAVs and extended UAVs, SO buffers, scissors/rasterizer/viewports,
   blend/depth state and DSV, RTVs, extended OM UAVs, predicate.

Masks and saved binding counts do not filter the fixed arrays. Low-word zero is
omitted; duplicates remain. Graphics class counts exceeding the saved 256 slots
are rejected by C++ rather than following an out-of-bounds native access.

## Acceptance evidence

The forwarding observer now tracks state/resource/data objects through their
original factory outputs, then observes their actual collector callbacks. It
does not infer IDs from a common object offset. Factory coverage, wire types,
collector RVAs, exact ordered references, original dependency sets, cache maps
and original file indexes are checked together.

| Unchanged capture | Data | Resources | States | ERGs | Total exact nodes |
|---|---:|---:|---:|---:|---:|
| BF1 | 8,217 | 2,598 | 1,275 | 6,248 | 18,338 |
| Merged-list mode 32 | 43 | 21 | 10 | 86 | 160 |
| GF2 | 404 | 150 | 72 | 263 | 889 |
| Total | 8,664 | 2,769 | 1,357 | 6,597 | 19,387 |

All 59 observed category/type pairs match. Three new original observer runs have
byte-identical output to three existing uninstrumented controls from the scheduler
batch. This checks observer effects; it does not establish cross-implementation
adapter equivalence. No additional capture registrations are claimed.

Independent boundary evidence contains 126 complete native collector records:

- Six full StateBlock probes cover all 2,663 saved ID fields, all-zero masks,
  high words, low-word zero, duplicates and zero class counts. The marker case
  collects 2,406 references and omits exactly 257 fields: IB plus 256 CS classes.
- 80 resource probes cover 19 types, zero/high-word/duplicate/marker IDs and the
  Texture2D alias branch. Device, SwapChain and cached InputLayout private-ABI
  calls are excluded; original full playback supplies their evidence. Buffer and
  texture data IDs stay zero in these isolated probes to avoid native cache reads.
- 40 data probes cover ten types with bounded, complete records. Counter-data
  factory `0x3bfc0` needs its manager-to-holder pointer even though collector
  `0x45590` never queries the cache. A bounded inert pointer holder supplies that
  constructor field; it contains no COM object or virtual callbacks.

Every native probe's raw record is passed through the C++ CLI in a synthetic CPU
fixture; all ordered references and dependency sets match. The generated file is
explicitly not an original capture or a GPU acceptance case. An initial probe
stopped after 86 calls at the missing counter-data manager pointer; that incomplete
artifact is retained and excluded from the 126 accepted final records.

Seven C++ test slots cover resource zero policies, shader linkage/SO distinctions,
alias branches, state ordering/omissions, all byte-prefix truncations of the small
resource/data layouts, trailing bytes, huge lengths, class-count overflow, missing
IDs, unknown layouts, invalid cache flags and edited versions. Six related CTest
suites and 23 Python evidence-checker tests pass. Four serial GF2/BF1 golden and
negative replay checks also pass. The full 470-registration GPU matrix was not
rerun, and there is no new GUI/package acceptance claim.

## Remaining M3 work

This closes initial resource/state/data dependency metadata for the accepted
types, not their later-version lifecycle. Recover the original file-to-version
rebuild semantics, changed-node readiness and dependent propagation before
claiming a production initialization schedule. Preserve explicit rejection for
unexplained traditional Execute identity and resource-version ownership. A real,
unmodified traditional-list capture is still required for production acceptance;
the merged capture above is an expanded immediate stream.

The module ledger remains 72 ported / 117 partial / 15 pending. Inventory remains
470 registrations / 460 hashes. UI and GPU execution paths are unchanged; the new
CLI metadata is in the development build. The latest packaged replay correction
remains the CB-lifetime release described in [current status](CURRENT_STATUS.md).

## Reproduction and provenance

See [the baseline](initialization-graph-baseline.json) for source/evidence hashes.
Local captures, traces, raw images and Ghidra output remain outside Git. Original
DLLs and Python are required only for development comparisons.

```powershell
python tools/trace_original_erg_initialization.py <capture> --reference-tools D:/CDXrepo/FloraGPA/tools --references --cache --all-references --out <trace>
FloraGPA.Cli.exe initialization-graph <capture> --out <graph>
python tools/validate_initial_graph_evidence.py --case <trace> <uninstrumented-control> <commands>/commands.json <capture> <graph>/initialization-graph.json --reference-python D:/CDXrepo/FloraGPA/standalone --out <audit>
python tools/probe_original_state_references.py --out <state-probe>
python tools/probe_original_node_references.py --out <node-probe>
python tools/validate_node_reference_probes.py --state <state-probe>/state.json --nodes <node-probe>/nodes.json --exe <FloraGPA.Cli.exe> --qt-bin <Qt-bin> --out <probe-comparison>
ctest --test-dir build/vs2022 -C Release -R "^initialization_graph$" --output-on-failure
```

Repeat `--case` for all three captures and run GPU processes serially. The existing
`commands` export supplies the independently checked initial cache and ERG data.
Read-only Ghidra evidence: `m3-allrefs-collectors.c`,
`m3-allrefs-layout-readers.c`, `m3-node-factories.c`. The installed player and
original capture/research files were not modified.
