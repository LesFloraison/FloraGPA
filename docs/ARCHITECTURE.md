# Native migration contract

The final application preserves all existing production functionality, including
optional backends. The source ledger contains 204 module baselines. Partial ports
must retain their pending cases; passing GF2/BF1 does not prove general parity.

Core owns checked capture parsing and typed identifiers. Replay owns D3D11 COM
objects. Analysis owns inspection and diagnostics. Application owns selection,
experiments, jobs and caches. Qt views never call GPU replay on the GUI thread.
FloraGPA.Worker isolates native replay; FloraGPA.Cli supports reproducible checks.
Raw IDs are uint64, never JSON doubles. Missing values are distinct from zero.

The UI follows Graphics Frame Analyzer: chart and overview above a dockable API
log, central output/pipeline/assets, right metrics/properties, and collapsible
debug/experiments/log panes. English, compact dark blue-gray styling and cyan
selection. Unmigrated panels remain empty or disabled, with brief tooltips.

Milestones: (1) toolchain, checked reader, CLI/worker/UI; (2) native GF2/BF1 replay;
(3) pipeline, assets, editing, compatible experiment projects, geometry/coverage;
(4) shader debugging, optional RenderDoc C++ API, Intel metrics and GTPin;
(5) ledger closure, UI interaction/DPI checks, standalone deployment.

Acceptance requires original RGBA hashes, command counts, resource/state and
geometry comparisons, experiment round trips and exact undo, plus existing
debugging/metric semantics. Timing values are not deterministic golden files.
Unknown layouts must fail closed. No new capture/reverse engineering capability
is added. Old evidence, captures and outputs remain untouched.
