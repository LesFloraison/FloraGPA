# Original initialization dependencies and list registration

Reviewed **2026-10-04**. This is an M3 research milestone. Production replay,
the 470-case compatibility inventory and traditional-list rejection behavior
are unchanged. It does not complete traditional Command List execution.

The old workspace already established the initialization call chain and Clear
list construction in controlled file copies (`analysis/COMMAND_LIST_ORDER.md`).
This batch extends that evidence with a general forwarding observer, native
dependency snapshots from unchanged captures, and controlled calls to the
original scheduler connected to the original list-registration function.

## New results

| Measurement | Result |
|---|---|
| Unmodified captures observed | Six merged-list cases (0/16/31/32/48/63), GF2, BF1 |
| Original playback with / without observers | Eight paired runs, 16 total; raw images equal in each pair |
| Actual ERG initialization calls | 7,042, with matching creation/return sequences |
| Controlled dependency graphs | 28: hand-derived witnesses plus 24 fixed-seed acyclic graphs |
| Original version schedules | 56: version 0 and independent version 1 for every graph |
| Original leaf initialization calls | 662 |
| Schedules containing repeated IDs | 28/56 |
| Scheduler-to-list-registration checks | All 56 preserve the observed ordering and duplicates |
| CPU checker tests | Seven pass, including false-success and ordering counterexamples |

Counts are evidence scope, not feature-completion percentages. The dependency
graphs are research inputs, not original captures or synthetic GPU acceptance.
The eight original files were already in the corpus; no new compatibility case
is added. No production CTest, Qt or full GPU regression was repeated because
no production code changed.

## Observed file initialization

`trace_original_erg_initialization.py` installs forwarding observers only in its
own isolated original-player process. It observes D3D11 manager factory slot 10
and each created ERG's initialization slot 5, forwards every call, and restores
the modified vtable slots. It does not patch DLL instructions, installed files,
captures or another process. `--no-observers` is a paired playback control with
zero modified slots. Callback errors or incomplete restoration fail verification.

At the first ERG initialization, it reads version-0 nodes in all four original
containers: data, resources, states and ERGs. The snapshots retain dependencies,
dependents, readiness states and allocated version counts. Snapshotting an
allocated version count does not mean later versions have been initialized.

In these eight files, all ERG prerequisites are non-ERG nodes already ready;
ERG nodes initially have state 1. The observed creation and initialization
sequences each contain every ERG once in ascending ID order. All sampled return
stacks start `9a703 → 9a815 → 9ad86 → 92a7c`. That is a verified **specific
dependency scope**, not a general rule that lists can be reconstructed by sorting
file IDs. Original-player device/configuration equivalence with FloraGPA remains
unproven; these pairs compare the original player with and without observers.

| Capture | Initialized ERGs |
|---|---:|
| Merged 0 / 16 | 95 / 95 |
| Merged 31 / 32 | 89 / 86 |
| Merged 48 / 63 | 86 / 80 |
| GF2 / BF1 | 263 / 6,248 |

## Why sorting and deduplication are insufficient

`probe_original_init_scheduler.py` calls the unchanged native scheduler on
bounded, acyclic research graphs. Original map helpers allocate native map
nodes. Leaf version records and empty-set sentinels use the recovered layouts;
custom leaf callbacks only log initialization, mark readiness and call the
original registration helper for nodes designated as ERGs. They never impersonate
a DX11 device or resource. Bounded research allocations are reclaimed at process
exit. No GPU work runs in this experiment.

Two hand-derived examples demonstrate materially different orders:

| Input | Original initialization | Original list 600 vector |
|---|---|---|
| ERG 10 depends on ERG 20; both need initialization | `20, 10` | `20, 10` |
| Same graph; ERG 20 initially ready | `10, 20, 10` | `10, 20, 10` |

The second example is an explicit controlled readiness condition. It does not
assert that an unchanged `.gpa_frame` encountered this exact ERG dependency.
It proves how the actual scheduler behaves when that condition exists. Sorting
or deduplication would lose behavior established by the original functions.

A cross-category case initializes state 20, resource 10, then ERG 30. A diamond
initializes `40,20,30,1`. The fixed-seed graphs vary IDs, prerequisites and node
categories. All runs match a separate model of the recovered state transitions;
both versions are checked for callback version identity and final readiness.
Original list grouping retains only designated ERG callbacks, including their
repetitions, and is compared independently against expected group vectors.

## Native chain and qualification

Pinned player SHA-256:
`39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b`.

| RVA | Evidence |
|---|---|
| `3c550` | ERG factory; creates ID/type objects before initialization |
| `929a0` | Builds version-0 playback references and requests initialization |
| `9a8f0` | Visits data, resource, state, then ERG containers; selects `version * 0x40` |
| `9a730` | Evaluates prerequisite readiness, registers dependent nodes, initializes and propagates |
| `9a6f0` | Calls ERG vtable slot 5, then sets version-node readiness to 2 |
| `9b730` | Resolves ID category and selected version-node address |
| `980c0` | Adds a dependent ID to a node's native set |
| `3a980` | Appends the ERG ID to a list vector, retaining order and duplicates |

The initial exploratory decompilation used return addresses rather than function
starts. It is **not** authoritative. PE exception-directory ranges resolved the
actual starts (`9a6f0`, `9a730`, `9a8f0`, `929a0`, `8e0d0`); the subsequent
`m3-list-init-chain-functions.c` uses those starts. Headless sessions were read-only
and discarded analysis changes.

The first scheduler prototype attempted to use uninitialized native version-set
headers and faulted in its isolated process. That output is preserved as failed
research. The corrected probe explicitly initializes the recovered version-node
storage and set sentinels; only `m3-list-init-scheduler-final` is accepted. This
correction affects the research harness, not GPA or FloraGPA replay semantics.

## Evidence and next implementation boundary

The [baseline](initialization-scheduler-baseline.json) pins the reports, tools,
decompilation evidence and source revision. Local raw data remains outside Git:

- `artifacts/m3-list-init-trace-validation`: paired original runs and full graphs.
- `artifacts/m3-list-init-scheduler-final`: all original schedules and registration vectors.
- `artifacts/m3-list-init-final-cpu-audit.json`: re-audit using the strengthened checker.
- `artifacts/m3-list-init-checker-tests.log`: seven checker tests.

```powershell
python tools/validate_original_init_traces.py --merges artifacts/m3-deferred-merge-originals --legacy D:/CDXrepo/FloraGPA --reference-tools D:/CDXrepo/FloraGPA/tools --out <fresh-traces>
python tools/probe_original_init_scheduler.py --out <fresh-scheduler>
python -m unittest discover -s tests -p test_original_init_evidence.py -v
```

Original playback must remain serial. These tools require the development
installation and private-ABI hash match; neither becomes a production dependency.

Remaining work is to recover the complete mapping from saved references and
experimental edits to version-node dependencies and readiness transitions,
including rebuilding/reinitialization. The new snapshots provide direct
comparison data for that implementation. Traditional Execute operand identity,
resource-version ownership and an unmodified original containing real legacy
list records remain unresolved. `registration_sequence_available` and
`execution_supported` therefore remain false in production inspection.
