# Native Quad diagnostic execution

`application/Quad` integrates the previously migrated target, depth, serial,
final-geometry and private-UAV helpers into the original diagnostic workflow.
`FloraGPA.Cli` and `FloraGPA.Worker` accept the same native `quad` command.
The Qt Quad panel is still pending; the existing delivered Coverage GUI is
unchanged by this backend integration.

## Behavior and artifacts

The selected draw has its own inclusive replay boundary. Diagnosis executes
before the normal command, which processes the original event exactly once,
including disabled-event accounting and current experiment edits. Diagnostics
operate on private resources and retain original resource/counter/SO storage.

`--quad-depth prepared|before|none` preserves all three original modes. Prepared
depth clears and submits the original draw to a private depth target, retaining
the original PS discard/depth behavior. Counting uses a read-only LESS_EQUAL
depth state without stencil; `none` disables diagnostic depth. The original
event still uses its original pipeline. A null original PS explicitly rejects
active prepared mode, as in Python.

IA/VS draws use primitive/instance serialization. Tessellation, GS, adjacency,
SO and pre-raster UAV writers use isolated final-geometry capture followed by
primitive serialization. Reference counting submits the original geometry with
a replacement atomic-counting PS. Pre-raster UAV writers retain private copies
and relocate away from the four diagnostic UAVs. High slots and class linkage
are preserved by the existing native helpers.

Targets retain native extent, sample layout and selected layer/slice. Bound
outputs are checked for compatible dimensions, array lengths and samples before
execution. Prepared buffer/3D cases use the original depth-compatible 2D adapter.
The embedded HLSL is the exact extracted reference file (SHA-256
`6107c02de2a66244cea28890046b245c26c42e3da3dd7f90f12831266e3433c7`);
its raw bytes are retained by `.gitattributes`. Compilation uses normalized
source newlines and shifts the four counter registers to u1–u4, as in Python.

```powershell
.\FloraGPA.Cli.exe quad C:\captures\sample.gpa_frame --id 113 `
  --quad-depth prepared --quad-target auto --out C:\results\quad-113
```

Optional `--quad-layer`, `--warp`, `--experiment` and `--disable` use the normal
native replay infrastructure. `--before`, timing and suppress-draw switches
cannot change the command's owned boundary. Quad flags are rejected on other
commands.

Exports retain `quad.json`, `data/result.json`, `data/quad_counts.png` and five
little-endian uint32 arrays: locks, counts, live, histogram and reference.
Reports retain source/compiled-shader hashes, target provenance, original
storage fingerprints, prepared depth hashes, serialization and reference
accounting. The preview uses the original nine log2 color bands. CLI/Worker
also produce their normal report and runtime module audit.

## Validation approach

`tools/validate_quad_executor.py` compares complete reports, decoded preview
pixels, all five binary exports, original output fingerprints and normal replay
counts with the unmodified Python implementation. Reference and native phases
are separate; rerunning native validation requires unchanged reference source
hashes and writes a fresh evidence directory. Hardware and WARP run serially.

Errors retain exact original text. The only accepted envelope is the verified
native replay prefix `Event <id> (<actual Draw name>): `; raw expected and actual
errors remain in the saved evidence. No report fields or GPU values are ignored.

The viewport structured-counter fixture does not define its initial hidden
counter. The validator seeds it to 5 using the existing initial-counter
experiment setting, so ordinary and diagnostic replays take the real write
branch from identical initial state. Without this seed, the original Python
engine retains the previous hidden count across replay restarts; this is a
fixture baseline issue, not a diagnostic storage write.

`tests/QuadTests.cpp` additionally checks the lower-level nonserial interface,
disabled internal storage verification with independent external fingerprints,
unchanged normal replay counts and rejection of contradictory depth settings.
`tools/validate_quad_cli.py` checks both real executable entry points, exports,
experiment propagation, validation failures and isolated runtime modules.

The full synthetic matrix passes **582 cases**, including **14 expected
rejections**, across both drivers and all three depth modes. It covers original
depth/stencil/discard/depth-output cases, final geometry, pre-raster writers at
slots 0/1/4/8/63, MSAA 1/2/4/8, array layers, 1D/2D/3D/buffer/viewport targets,
point/line/triangle/strip and indirect submissions, SO raster streams 0–3 and
NO_RASTER rejection, passthrough providers, dynamic classes in every graphics
stage, disabled events and shader edits. Evidence:
`artifacts/quad-executor-full-v3/native-1/validation.json`.

The real-frame matrix passes another **12 cases**: GF2 event 113 and BF1 MRT
event 3313, both drivers and all three depth modes. Complete reports, raw arrays,
preview pixels and original outputs match Python, and native original-event
results match separate ordinary native replays. Evidence:
`artifacts/quad-executor-real/native-1/validation.json`.
The probe SHA-256 for both matrices is
`45fea0b6069f8d04c9632101e3415583e38d6ea416732486df0ea7eb11f4cad6`.

Both executable entry points pass **24 checks**, including all exported bytes,
prepared/before/none modes, WARP, selected layers, initial counter edits, disabled
events, original refusals and malformed/boundary/foreign-option errors. Runtime
audits find no Python/Tk/GPA modules (`artifacts/quad-executor-cli/validation.json`).
GF2/BF1 golden pixels, original command counts and suppress-draw negative controls
pass in an isolated runtime environment (`artifacts/quad-executor-golden/validation.json`).

The complete Release build passes (`artifacts/build-quad-executor-final.log`).
All 15 selected CTest suites pass, including Quad, its five helpers, Coverage,
the Coverage Qt view, depth/stencil, frame output, view edits, predication, SO,
dynamic classes and core (`artifacts/ctest-quad-executor-final.log`). The new
Quad suite passes four cases without skips; Coverage UI passes five cases and
skips its optional real-capture panel check because its fixture environment
variable is unset. This run does not constitute Quad UI validation.

The module ledger now records 28 ported, 115 partial and 61 pending modules.
These counts are not a workload completion percentage. `quad.py` and
`quad_counter.py` are ported; `quad_ui.py` remains pending.

## Scope

This is a migration of the recovered diagnostic. It does not claim physical
quad invocations, exact hardware scheduling or full Intel GPA feature parity.
The original bounded-lock, primitive identity, replacement-PS, uint32 overflow,
MSAA initialization and buffer-coordinate limitations remain in exported data.
GUI controls, cell inspection, settings persistence and ZIP export remain the
next consumer work; Python is not used by either native executable.
