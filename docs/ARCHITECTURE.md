# Architecture and development direction

This describes the native implementation reviewed at `ba1c468` on 2026-10-03.
[Current status](CURRENT_STATUS.md) owns capability claims and acceptance totals;
the [usage guide](USAGE.md) describes the exposed workflows. Full preservation of
the recovered reference behavior is a development objective, not a statement
that every original GPA feature has been implemented.

## Components

- **Core** owns checked capture parsing, typed identifiers, descriptors,
  command/state decoding and shared semantic validation.
- **Replay** owns D3D11 devices/COM resources, captured execution and supported
  replay experiments. Capture metadata and native execution stay distinct.
- **Analysis/application services** provide inspection, geometry, shader tools,
  measurements, experiment history, jobs, provenance and result processing.
- **Qt UI** owns selection, navigation, presentation and interaction. Native
  replay does not run on the GUI thread; jobs use isolated workers.
- **FloraGPA.Worker** isolates native replay/analysis jobs; **FloraGPA.Cli** exposes
  reproducible commands. **FloraGPA.Rdc** isolates optional RenderDoc analysis;
  **FloraGPA.Metrics.dll** bridges optional Intel Metrics Discovery access.

Ordinary application replay uses neither Python nor GPA DLLs. Optional native
backends have feature-specific dependencies. Python reference comparisons and
pinned original GPA kernels are development tools, not application backends.

## Correctness boundaries

Raw resource/event IDs remain uint64, never JSON doubles. Missing information
is distinct from zero. Unknown layouts and unsupported execution paths must
produce diagnostics; decoding a record or passing preflight does not prove GPU
execution. Captured snapshot reconstruction, prepared inspection and actual
submission boundaries are distinct, with provenance retained in reports.

Acceptance uses suitable original captures, exact resource/image checks,
state/execution observations, negative controls and independent producer evidence.
Timing distributions and documented workload variability are not deterministic
hash oracles. Original-player completion and original-player image agreement
are separate results. Unavailable capture data is not synthesized to obtain
agreement. See the [compatibility infrastructure](COMPATIBILITY_BASELINE.md).

The GPA-inspired workspace uses a chart/overview, API log, central output and
resource/pipeline tools, right-side metrics/properties, and compact task logs.
Available tools keep English UI labels. Empty/disabled sections do not claim
unimplemented features are working.

## Current roadmap

The forward sequence is M1 → M2 → M3 → M4 → M5, followed by remaining M6 work:

1. **M1:** Maintain corpus, coverage, offline diagnostics and serial comparisons.
2. **M2:** Complete evidenced ordinary replay paths and explicit rejection of gaps.
3. **M3:** Prove captured deferred/list identities, order, versions and state semantics.
4. **M4:** Complete saved resource restoration and remaining boundary behavior.
5. **M5:** Converge compatibility, stability, recovery and clean-environment release.
6. **M6:** Complete remaining analyzer integration and version-specific adapters.

M1's minimum acceptance loop is implemented; M2 is in progress and M3–M6 are
incomplete. Existing analyzer features remain usable. Internally generated native
command lists used by metrics or diagnostics do not establish captured-list
replay support. Scope is reading/replaying existing captures; self-owned capture
probes serve validation, not development of a complete capture product.

## Historical migration milestones

The earlier five-step migration sequence was: toolchain/reader/CLI/UI;
GF2/BF1 replay; assets/experiments/geometry/coverage; debugging/optional metrics;
and ledger/UI/deployment closure. That sequence explains earlier batch records;
it is superseded as the forward plan by M1–M6 above, not deleted from history.

The 204-module [migration ledger](migration.json) records Python-to-native module
status, not complete original-GPA coverage. The [development log](MIGRATION_STATUS.md)
and original JSON baselines retain historical counts and hashes. Old captures,
reference sources and raw validation outputs remain outside the public source
tree. Their local paths are evidence locations, not build prerequisites.
