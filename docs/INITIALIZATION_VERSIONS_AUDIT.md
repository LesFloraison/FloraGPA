# Original cache version lifecycle

Reviewed 2026-10-05. This M3 research batch establishes native cache cloning,
dependency recollection and selected reinitialization behavior after the
[initial graph recovery](INITIALIZATION_GRAPH_AUDIT.md). It changes development
tools and evidence only. Production replay, metadata exports, UI, module ledger
and compatibility registrations are unchanged.

These are GPA playback/experiment cache version slots. They are not automatically
the temporal resource-content versions of a deferred Command List. Recovering
this lifecycle is useful for reproducing the original engine, but does not resolve
traditional Execute identity, capture-side lifetime ownership or list execution.

## Verified behavior

| Operation | Original behavior established here |
|---|---|
| Allocate a sparse version slot | New slots have readiness 0, empty dependency/dependent sets and no object. Allocation is distinct from registration in the version set. |
| Copy a version node | Dependency and dependent sets are independently copied; readiness is copied; the object and shared ownership control are shared. |
| Recollect a node's dependencies | New references are inserted into the existing set, and readiness becomes 1. Old references are not cleared, including when the new collector emits no references. |
| Replace a nonzero version's target | The real reload path creates a new target object through its category factory. The tested unchanged payloads retain logical dependencies while changing target wrapper/control identity. |
| Reinitialize with all prerequisites ready | The target changes readiness 1→2, followed by its registered direct dependents in sorted ID order. Already-ready dependents are initialized directly; this branch does not recursively advance their dependents. |
| Copy back over an existing node | The source sets replace destination sets. This differs from recollection's union behavior. |

The prior scheduler probe covers other ready/collected prerequisite patterns.
This batch's real-capture reloads do not claim general changed-payload, failing
initializer, cyclic dependency or arbitrary version-submission acceptance.

## Original captures and interference controls

Three unchanged captures were opened and replayed in separate original-player
processes. Each process:

1. Replays default version 0 and records raw framebuffer pixels and ERG dispatch
   counts, including the available DrawIndexed audit.
2. Copies `0 → 7 → 9` through original `0x98450`, comparing every node's kind,
   readiness, dependency/dependent sets, object identity and ownership control.
3. Confirms allocated-but-unregistered slot 1 is blank.
4. Reloads one state, resource, data and ERG node in version 7 through original
   `0x9ca90`, using each record's exact saved bytes. Process-local forwarding
   observers record actual native initializers and are restored afterward.
5. Copies modified version `7 → 11`, verifying its updated target identities.
6. Replays default version 0 again, then closes the player. Pixels and execution
   counts must match the before run. Pixels also match the previous uninstrumented
   original controls from the scheduler batch.

| Capture | Initial nodes | Copied nodes over three clones | Reload IDs: state/resource/data/ERG | Initializer calls | Default ERGs per playback |
|---|---:|---:|---|---:|---:|
| Merged-list mode 32 | 160 | 480 | 59 / 5 / 6 / 8 | 7 | 86 |
| GF2 | 889 | 2,667 | 174 / 6 / 7 / 5 | 5 | 263 |
| BF1 | 18,338 | 55,014 | 263 / 30 / 31 / 18 | 6 | 6,248 |
| Total | 19,387 | 58,161 | 12 reloads | 18 | 6,597 |

The acceptance set contains **three new original sessions, six explicit default
playbacks and nine version clones**, plus three reused uninstrumented image
controls. It excludes preliminary runs without complete cloning or execution
count instrumentation. No new captured workload is added to the corpus.

GF2 retains 72 DrawIndexed calls / 5,526 submitted indices; BF1 retains 41 /
747 before and after. The compute-oriented mode 32 has no DrawIndexed calls;
its 86 actual ERG dispatches are still checked. ERG totals must equal the initial
registered ERG inventory, so two equally incomplete count reports cannot pass.

Reload observers show, for example, mode 32 data 6 reinitializing `6,5`, not
`6,5,4,8`; resource 5 itself has ready ERG dependents 4 and 8. Reloading resource 5
directly initializes `5,4,8`. A generic transitive invalidation algorithm would
therefore differ from the observed original behavior.

Default/other-clone checks cover metadata and wrapper/control identities, not
deep copies of GPU storage. Shared wrappers and ready-dependent initialization
can have native side effects. Equal framebuffer pixels and dispatch counts do
not prove that every offscreen resource or native handle is isolated. The new
versions are initialized as described, but no version-7/9/11 workload is submitted
for rendering. Cross-implementation device equivalence is not claimed.

## Bounded native primitive controls

Four CPU-only category cases use actual original descriptors, version-vector
allocation, objects, collectors, set insertion and copy helpers. They cover 32
transitions and 68 node snapshots. State, a sampler resource, CopyResource ERG
and input-layout data use complete bounded records; no GPU initializer or fake
COM callback is called.

Tests distinguish independent copied sets from shared object identity, sparse
allocation from populated copies, replacing versus accumulating dependencies,
and a genuinely empty collector versus an ERG that retains mandatory zero IDs.
Every expectation is rechecked by a separate evidence validator. Twelve new
counterexample tests reject reordered/deepened propagation, missing or duplicate
nodes, wrong version/RVA/status, incomplete execution counts, reused target
wrappers, invalid scope claims and altered primitive expectations. Together with
the existing initial-graph/cache/reference/scheduler checks, **35 tests pass**.

No C++ or GPU execution implementation changed, so this batch does not claim a
new CTest, independent golden-frame, GUI or package validation run. The prior
initial graph batch's results remain the most recent production-code checks.

## Machine-code qualification

Pinned player SHA-256 remains
`39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b`.

| RVA | Role |
|---|---|
| `98450` | Register a nonzero version and clone each category's source slot |
| `9d070`, `9d150`, `9d230`, `9d310` | Allocate/select data, ERG, resource and state version slots |
| `977d0` | Copy sets/readiness and shared object ownership |
| `9d020` | Assign a newly created shared object to the target node |
| `9c1a0`, `9c250`, `9c300` | Invoke the relevant collector, insert dependencies, set readiness 1 |
| `9ca90` | Rebuild a requested cache version node, then request propagation |
| `9a730` | Prerequisite readiness and dependent propagation |
| `9a6d0`, `9a6f0`, `9a710` | Actual resource/data, ERG and state node initialization |

Ghidra's uncorrected decompilation displays a zero second argument at the three
collector calls. The machine code leaves RDX intact up to the virtual call;
the complete saved-record argument is forwarded. Direct native probes confirm
the two-argument wrapper ABI. `m3-version-collector-abi.json` preserves the bytes
and disassembly; the unqualified zero-argument decompilation must not guide C++.

## Next boundary and reproduction

The next implementation must preserve shared-object identity separately from
copied dependency sets and must distinguish original recollection from ordinary
graph replacement. These constraints can now guide a version model. They do not
authorize enabling legacy Execute or treating GPA experiment versions as captured
resource history. Those paths still need a verified file-to-list identity/lifetime
mapping and acceptance on a real unmodified traditional-list capture. All three
captures here retain their existing replay classification; mode 32 is expanded
immediate replay.

See the [pinned baseline](initialization-versions-baseline.json). Original captures,
raw images, native snapshots and Ghidra outputs are local evidence outside Git.
The installed DLL and existing reference workspace were not modified. Runtime
remains independent of GPA/Python.

```powershell
python tools/probe_original_version_nodes.py --out <fresh-primitives>
python tools/probe_original_version_clones.py <capture> --reference-tools D:/CDXrepo/FloraGPA/tools --reload-id <state> --reload-id <resource> --reload-id <data> --reload-id <erg> --out <fresh-native-run>
python tools/validate_original_version_evidence.py --primitives <fresh-primitives>/nodes.json --case <native-run> <uninstrumented-control> <initial-graph>/initialization-graph.json <capture> --reference-python D:/CDXrepo/FloraGPA/standalone --out <fresh-validation>
python -m unittest discover -s tests -p test_original_version_evidence.py -v
```

Use the exact four IDs in the table for these three captures; repeat `--case` for
the acceptance matrix. GPU processes are serial. Read-only Ghidra evidence is
`m3-version-node-primitives.c`, `m3-version-copy-callers.c`, and the preceding
`m3-cache-category-callers.c` for the rebuild entry point.
