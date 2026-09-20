# Persistent IA setter editing

The API Log **Edit Setter** action supports IASetInputLayout,
IASetVertexBuffers and IASetIndexBuffer. The Qt dialog uses captured-resource
selectors, a start-slot control, a buffer/stride/offset table and an index-format
selector. Resource IDs retain uint64 precision. Numeric offsets accept decimal
and hexadecimal uint32 values. The native application does not use Python.

## State behavior

Vertex edits persist per slot. Moving or shortening a command's range preserves
the displaced slots from preceding buffer, stride and offset observations;
unknown preceding values are rejected. A later original setter resets only the
slots it writes. Empty vertex arrays do not clear unrelated bindings. Layout and
index edits persist until a later original setter of the same kind; ClearState
clears every override. Missing original input layouts retain the existing gap
reporting and snapshot recovery; replacing the missing reference resolves it.

IA tuples track input/output conflicts with CS/OM UAVs, RTVs and stream-output
buffers. Binding an input that conflicts with an already-bound output preserves
the submitted stride/format/offset while the resource becomes null. Binding an
output after an input evicts the resource and resets its metadata. Removing the
output does not resurrect an evicted input. Native getters update active tuples;
original-capture history remains authoritative across incomplete output calls.

When output/SO setter experiments are also active, IA replacements participate
in the existing original/edited output-binding model. This preserves collateral
input differences across captured draw snapshots. Layout persistence remains
separate, matching the reference implementation.

Replay snapshots, Pipeline navigation, buffer-edit validation and geometry
inspection use the effective IA bindings. Buffer patches can target a newly
bound VB in either project operation order, and scoped input patches restore the
original buffer storage after the command. Geometry exports read the edited
layout's slots and effective index format/offset.

## Validation

`tests/IaSetterTests.cpp` checks native getters, displaced slot retention,
persistence, original reset, ClearState, repeat traversal, project save/load,
undo/redo and invalid-operation atomicity. `tests/UiTests.cpp` checks editor
validation and uint64 resource selection; an actual worker replay changes an
indexed draw after editing its IB and restores it with undo/redo.

`tools/validate_ia_setters_port.py` compares the preserved Python implementation's
argument and wire validation, complete native pipeline fields, rendered pixels,
output metadata, geometry tables/CSV/OBJ and scoped buffer patches. It covers
hardware and WARP, partial reset, missing layout/output recovery, displaced slots
without prior observations, output-before/input-before hazards and combined
output experiments. `--real-only` edits every vertex, index or layout setter in
GF2/BF1 independently and compares edited and undone frames.

The synthetic comparison completed all **133 checks**, including **411 CPU cases**
(66 accepted, 345 rejected), in `artifacts/ia-setters-package/validation.json`.
The real-frame comparison completed **18 checks** in
`artifacts/ia-setters-real-final/validation.json`: GF2 edits 72 VB, 72 IB or 16
layout setters; BF1 edits 1,194 VB, 641 IB or 645 layout setters. Each family is
edited independently, produces a visible change matching Python and restores the
original golden frame through undo. Hardware/WARP coverage in the synthetic
corpus does not imply the two real captures were tested on both adapters.

Release regression evidence covers all 30 CTest suites: the first full run passed
29 non-UI suites, including the new IA suite, but an existing blend UI test failed.
That test checked process idleness before the 180 ms event-preview debounce had
fired, allowing a cancelled preview notification to race the edit's completion.
It now awaits the selected event's preview before opening the editor. The complete
UI rerun passed **46 cases with no skips**, recorded in
`artifacts/ctest-ia-setters-ui-final.log` and
`artifacts/ia-setters-ui-final/qt-results.txt`. The production scheduler is unchanged.
Final editor and workspace screenshots are in `artifacts/ia-setters-ui-final/`.

The final package is `out/FloraGPA-ia-setters-final/`. All three executables match
the Release build. The synthetic and real-frame evidence uses the same CLI SHA-256
`d6d54ad7abf82496c02a73192a6cc018f85829e91e12331f91c7546ddf01518e`.
The audit of 139 native replay/pipeline reports found no Python, GPA or RenderDoc
modules and no Qt DLL loaded outside the corresponding application package;
see `artifacts/ia-setters-package-audit.json`. These checks ran on this host,
not on a separate clean Windows machine. The final package also passed both
original-frame goldens and both suppressed-draw negative controls in
`artifacts/validation-ia-setters-final/validation.json`.

The initial comparison stopped at a test-script event-selection error after 75
successful checks. The input-first fixture's output command is event 96, not 85;
that selector was corrected. Initial results remain in
`artifacts/ia-setters-comparison/` and its sibling log.

Coverage, quad diagnostics and post-transform/debugger consumers are still
pending their own migration. CB/CB1 setter editing is also separate remaining
work. This change does not establish complete Python feature parity.

```powershell
python tools/validate_ia_setters_port.py `
  --reference D:/CDXrepo/FloraGPA `
  --exe out/FloraGPA-ia-setters-final/FloraGPA.Cli.exe `
  --oracle build/vs2022/Release/FloraIaSetterTests.exe `
  --qt-bin D:/Qt/6.11.2/msvc2022_64/bin `
  --out artifacts/ia-setters-new
```

For packaged real-frame checks, add `--real-only` and omit `--qt-bin`; the oracle
is unused in that mode. Native children use a Windows-only PATH in that mode.
