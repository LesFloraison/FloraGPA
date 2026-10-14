# GF2 inspection through the original GPA Plugin API

The original GPA 2025 R1 Frame Analyzer successfully exported all six retained
GF2 candidates through its own Plugin API. This supplies detailed inspection
evidence without depending on the unresponsive automation input path in the GUI.
The previously requested manual click check is superseded for this inspection;
the test window was closed before starting the exports. Manual GUI interaction
itself remains unverified.

Six analyzer processes completed serially with exit code zero. Their event
counts, in launch/frame order, are **273, 637, 632, 272, 631 and 638**. Every
event's binding query succeeded, and all events appear in each of the original
Render Target, Command List, Shader Set, Pipeline State and Debug Region trees.
The exporter uses the installed `plugin_api` accessors; it does not parse the
capture through Flora's format reader or use a Flora replay backend.

## Cross-tool checks

The CPU-only development validator compares the official exports with the
retained Flora inventories and native getter reports:

* All **2,881 Draw/Dispatch actions** agree in order, API name and arguments.
  The full ordered sequence maps original event IDs to Flora record IDs; numeric
  IDs are not assumed to be interchangeable.
* **16 selected events** cover each file's large indexed draw and final indexed
  draw, plus the first compute dispatch where present.
* **28 selected shader disassemblies** agree exactly with disassembly of Flora's
  exported DXBC using system `D3DDisassemble` with flags zero. Only CRLF/LF and
  trailing NUL differences are normalized. Native shader bindings must match the
  selected capture IDs. Flora's earlier hexadecimal-immediate text export differs
  from GPA's default decimal text, so that display mode is not used for comparison.
* **144 selected resource bindings** have matching descriptions and event usages
  in the original resource table. These include the compute UAV and graphics
  input/output views. This checks original metadata consistency, not new resource
  byte readbacks; the existing buffer and BC1 comparisons remain separate evidence.
* **180 graphics state groups** match the native getter projection exactly:
  blending including eight targets, depth, stencil, front/back operations,
  rasterizer, viewports and scissors. Compute exports expose no such graphics
  groups. The comparison does not infer missing sampler, hidden counter or SO
  state from this interface.

Default shader disassembly equality does not establish bitwise DXBC identity:
the original Plugin API provides text and shader IDs/hashes, not raw bytecode in
this export. This limitation is separate from verifying that shader details are
available and consistent in the original analyzer.

Eight CPU tests pass against a retained equipment export. Besides the unchanged
positive case, they reject a changed draw argument, dropped draw, changed shader
instruction, missing resource usage, changed viewport, missing grouping and
failed event binding query. They do not rerun GPA or execute GPU work.

## Reproduction and provenance

The installed GPA MCP command failed before exporting because its packaged
plugin-source path was unavailable. Direct use of the local export script then
failed GPA's module-name check for `flora_gf2_evidence_export`. That rejected
plugin was moved out of the plugin scan directory into the evidence archive.
The same exporter succeeds under the accepted name `flora_capture_audit`.
The first successful log also records the rejected module during GPA's scan;
subsequent logs use the corrected installation. All attempts are retained.

The existing exporter source, its hash, analyzer identity, exact commands,
per-frame hashes, six exports and logs are retained under
`C:/Users/Myrrr/FloraGPA-evidence/capture-gf2-plugin-20261014`. The
[baseline](capture-gf2-plugin-inspection-baseline.json) hashes these artifacts,
the selected Flora evidence and the new validator sources. No exporter or GPA
Python code is linked into or packaged with FloraGPA.

```powershell
$env:PYTHONDONTWRITEBYTECODE='1'
python tools/validate_capture_gpa_inspection.py `
  --evidence artifacts/capture-gf2-current-20261014 `
  --manifest C:/Users/Myrrr/FloraGPA-evidence/capture-gf2-plugin-20261014/export-manifest.json `
  --out C:/Users/Myrrr/FloraGPA-evidence/new-inspection-comparison.json
python tools/test_capture_gpa_inspection.py `
  --evidence artifacts/capture-gf2-current-20261014 `
  --export C:/Users/Myrrr/FloraGPA-evidence/capture-gf2-plugin-20261014/equipment-export-v2.json
```

This stage completes these selected original-API inspections of the retained
files. It does not repair the two historical pixel mismatches or transfer their
older runtime identity to the current build. The formal six-frame gate and
overall delivery remain incomplete. No production Capture/Replay/Format change,
new captured frame, new final-pixel comparison, CTest or golden-frame run is
claimed here. Original analyzer exports were serialized and all analyzer
processes had exited before the final CPU checks.
