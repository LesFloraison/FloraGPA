# Native hull shader outputs

Geometry **HS output** exposes the original Python hull-output workflow through
C++ and Qt. It retains paired HS/DS execution and observes control-point and
patch-constant writes without recompiling the original shader math. The two
tables show original instance, local patch and control-point identities, typed
values and a written flag per component. Unwritten values remain blank.
These stage attributes do not establish an assembled surface or position space,
so HS has no inferred mesh or OBJ.

## Implementation

`DxbcHull.cpp` ports the HS schema and output-write transform, including implicit
control-point phase materialization, per-phase temporary allocation, fork/join
phases, relative output operands and checked 256 MiB value/validity storage.
`carryHullInstance` transports the original VS InstanceID through a free VS
output / HS input component. Signatures and executable declaration ranges both
reserve components. A new system-generated VS input requires a whole register;
adding InstanceID to a VS with UAV access is rejected when the original shader
did not consume it, preserving the reference implementation's reuse guard.

The original full draw executes once for the normal counts-only workflow.
Selected instances are sliced from its patch slots afterward. GPU indirect
arguments, empty/disabled draws, source overrides and input experiments use the
existing replay paths. The private UAV slot reserves actual bindings and original
VS/HS/DS declarations. Original class linkage and class instances are retained.
Private output copies protect RTV/DSV/UAV resources and hidden counters, and
the existing SO path redirects diagnostic output while retaining original buffers.

The native API also supports `PostTransformOptions::hullDownstream`, matching
Python's optional full DS witness: the result retains DS bytes separately from
HS storage. This mode uses the existing bounded SO retries. The ordinary Qt/CLI
workflow records DS statistics without materializing its output table.

Export includes `geometry.json`, control points in `vertices.csv`, `vertices.bin`
and `vertices.validity.bin`, plus the corresponding three `patch_constants.*`
files. Validity binaries contain uint32 flags; numeric zero in the value binary
alone does not establish that the component was written. Export removes obsolete
OBJ output. Qt offers **Control points** and **Patch constants** in the existing
compact table selector and restores the selection from experiment projects.

```powershell
.\out\FloraGPA-hull-outputs\FloraGPA.Cli.exe post-geometry D:\captures\sample.gpa_frame --event 11276 --geometry-stage hs --out D:\results\hs
```

Use `--instance N` for a zero-based instance within the original draw. HS accepts
stream zero only and requires feature level 11.1 and a free graphics UAV slot.

## Validation

Development bytecode comparisons pass **226 cases** (216 successful transforms,
ten expected rejections) in `artifacts/hull-transform-final-v2/validation.json`.
They compare complete patched containers and metadata, both shaders for instance
carrying, low/high UAV slots, typed/wide signatures, implicit/explicit phases,
zero patch allocations, colliding slots and two distinct original BF1 HS shaders.

Development GPU comparisons pass **112 cases** (100 successful inspections,
twelve expected rejections) in `artifacts/hull-gpu-extended/validation.json`.
Every success compares complete report metadata, exact control-point/constant
value and validity bytes, and typed CSV values and identities. Hardware/WARP
coverage includes tri/quad/isoline domains, zero/NaN tessellation factors, wide
packed carriers, direct/indexed/indirect calls, IA instance step rates, selected
instances, HS/DS UAVs at u0/u7/u63, disabled/empty draws and BF1 event 11276.

Release **40/40 CTest suites** pass (`artifacts/ctest-hull-final.log`), with the
original Qt suite retaining 53 passed and no failures/skips. Native HS tests
compare original CPU values, full downstream DS bytes, selected-instance slices,
original UAV storage and hidden counters, and a subsequent direct native draw
against clean replay on Hardware/WARP. A deliberately tiny inspection limit is
followed by a successful capture. Qt tests select an actual Draw, invoke the
Worker, switch both tables, export all seven files and restore the HS project
selection. The inspected screenshot is `artifacts/hull-ui/hs-patch-constants.png`.

The final portable package is `out/FloraGPA-hull-outputs/FloraGPA.exe`. Its HS
matrix passes all **112 cases**, and the previous post-transform and output-log
matrices retain **140/140** and **100/100** passes. GF2/BF1 golden frames and
suppressed-draw controls pass **4/4**. The 324 successful reports load no
Python/Tk/GPA/RenderDoc modules; Qt loads from the package, and the GUI, CLI and
Worker EXEs match the final Release build. These checks are on this host and do
not replace a separate clean-machine run.

Evidence: `artifacts/hull-portable-final/validation.json`,
`artifacts/hull-post-regression/validation.json`,
`artifacts/hull-log-regression/validation.json`,
`artifacts/hull-golden-final/validation.json`, and
`artifacts/hull-runtime-audit.json`.

## Remaining work

This completes the native HS output route, not the full migration. HS/DS/GS
checkpoints, traces and shader debugging still need their own implementations and
UI. Broad dynamic class-linkage combinations, active-predicate and timeout fault
injection specific to HS, further real captures, clean-machine deployment and
large-output memory stress remain verification gaps. The GPU log limit does not
cap CPU memory consumed by typed tables and JSON. Private re-execution does not
guarantee identical atomic scheduling on arbitrary programs and devices.
