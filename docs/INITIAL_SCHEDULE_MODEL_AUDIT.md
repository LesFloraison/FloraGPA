# Independent initial scheduling model

Reviewed 2026-10-05. This M3 batch implements the recovered native initialization
scheduler in C++ and compares its output to complete original initialization
traces. It is a prerequisite for recovering list construction order; traditional
list GPU execution remains unaccepted. See the [pinned baseline](initial-schedule-model-baseline.json).

## What changed

`modelInitializationSchedule` is an independent C++20 component in `FloraCore`.
It models the pinned player's initial version scheduling, with empty dependent
sets and successful initialization callbacks. The `initialization-graph` export
now includes `modeled_initialization` when every initial collector is recovered
and its references resolve:

- `status: modeled`, ordered node IDs and their prior readiness states.
- Explicit `scope: initial_version_fresh_dependents` and
  `assumes_all_initializers_succeed: true`.
- `gpu_execution_verified: false`. The existing top-level
  `initialization_schedule_available` and `execution_supported` remain false;
  a predicted successful schedule is not an observed execution or GPU object proof.

If a structurally complete graph has a cycle or exceeds the model work budget,
the model reports `unavailable` with a node-specific reason and no partial order.
Incomplete graphs retain their existing dependency diagnostics and omit the model.

The implementation follows the original container order (data, resource, state,
ERG), then ascending IDs inside each container. Each node registers itself in
its prerequisites' dependent sets. It initializes only when all prerequisites
are ready. Pending dependents trigger recursive-equivalent traversal; ready
dependents are reinitialized directly without further propagation. Repeated
callbacks are retained. A topological sort or a sorted unique list is insufficient.

The traversal uses an explicit stack. Duplicate identities, missing dependencies,
invalid categories and cyclic/cycle-dependent graphs are rejected. A two-million
work-unit limit bounds collection, dependency validation and traversal; exceeding
it is a model limitation, not evidence that the original capture is corrupt.

## Direct original evidence

The research observer's new `--schedule` mode forwards the four actual native
version-node initializer vtable slots unchanged. It records initial graph state,
node ID through the original virtual getter, category, version, prerequisites,
prior readiness and post-return readiness. It restores all modified slots before
exit. Existing ERG observations remain a second consistency check.

Three unchanged captures start with every node at state 1 and empty dependent
sets. C++ graph references match those original initial snapshots. The complete
call sequence and each prior readiness state match the C++ model:

| Capture | Data | Resource | State | ERG | Exact calls |
|---|---:|---:|---:|---:|---:|
| Merged-list mode 32 | 43 | 21 | 10 | 86 | 160 |
| GF2 | 404 | 150 | 72 | 263 | 889 |
| BF1 | 8,217 | 2,598 | 1,275 | 6,248 | 18,338 |
| Total | 8,664 | 2,769 | 1,357 | 6,597 | 19,387 |

Each observed original run has byte-identical pixels to its previously accepted
uninstrumented original control. These are observer-effect checks; they do not
establish strict adapter equivalence between GPA and FloraGPA. No new capture
registration is claimed.

The observed schedules differ from simple category/ID sorting at 4, 197 and 4,253
positions respectively. For example, BF1 data node 244 depends on 245, 246 and
247. The original initializes 245, 246, 247, then 244; C++ reproduces that order.
This closes a concrete ordering gap beyond merely collecting dependency sets.

## Boundary and regression checks

The original bounded CPU scheduler probe was rerun: 28 graphs, versions 0 and 1,
56 schedules and 662 callbacks. Committed test fixtures retain actual native
orders and the source report hash; they contain research graph data, not captures
or proprietary binaries. C++ matches every original schedule, including the
`20,10` versus `10,20,10` readiness witness and repeated callbacks.

Four new C++ test slots cover native fixtures, direct reinitialization semantics,
malformed graph rejection and iterative traversal of a 1,000-node dependency
chain. Graph-export tests additionally cover a complete cyclic graph, missing
references and explicit prediction assumptions. The first exploratory test used
a 10,000-node reverse chain and exceeded the intentional work budget; its failure
is retained outside accepted evidence. The final tests separately verify normal
bounded traversal and work-limit rejection. The initial test-target build also
needed the repository's JSON include directory; it is included in CMake.

Seven related CTest suites pass. Eight new false-success checker tests bring the
related evidence suite to 43 passing tests. GF2/BF1 strict image hashes and both
draw-suppressed negative controls pass in four serial isolated-runtime checks.
No full corpus, GUI or package acceptance is claimed by this batch.

## Remaining M3 work

This model does not resolve Execute operand identity, initialize GPU objects,
reconstruct list temporal resource contents, perform edited-version rebuilds,
or recover failure rollback. It must not translate GPA experiment-cache versions
into captured temporal versions. The real captures above use the already supported
immediate or expanded-immediate path. Unmodified traditional-list capture
acceptance and production Command List execution remain open.

The model supplies the independently verified ordering component for that work.
Any future list registration must use actual supported initializer semantics and
retain duplicates; it cannot infer list membership from this order alone.

```powershell
python tools/trace_original_erg_initialization.py <capture> --reference-tools D:/CDXrepo/FloraGPA/tools --schedule --references --cache --all-references --out <fresh-trace>
python tools/validate_initial_schedule_evidence.py --case <trace> <uninstrumented-control> <capture> --exe build/vs2022/Release/FloraGPA.Cli.exe --qt-bin D:/Qt/6.11.2/msvc2022_64/bin --out <fresh-validation>
ctest --test-dir build/vs2022 -C Release -R '^(initialization_schedule|initialization_graph|api_commands|commands|contexts|command_state|frame_validation)$' --output-on-failure -j1
```

Repeat `--case` for the three captures. Original GPU observations are serial.
GPA and Python remain development tools; the C++ model and export use neither.
