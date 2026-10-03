# Native RenderDoc Pixel History backend

> Historical batch record. Results, package paths, module counts and remaining-work
> statements below describe this batch. See [current status](CURRENT_STATUS.md)
> for present support and [the documentation index](README.md) for navigation.
> `artifacts/`, `out/`, `build/` and external-reference paths identify local
> evidence not distributed with a clone; original JSON baselines are preserved.

`FloraGPA.Rdc.exe` is an isolated C++20 process that opens the independent RDC
produced by FloraGPA replay. It uses the optional installed RenderDoc 1.45 release
DLL directly; no Python, qrenderdoc or GPA backend is part of its runtime.
The normal replay and Qt executable do not import this DLL.

## Job and result

Create a UTF-8 JSON job outside its new/empty output directory:

```json
{
  "action": "history",
  "renderdoc": "C:/Program Files/RenderDoc/renderdoc.dll",
  "capture": "D:/results/recapture/independent_capture.rdc",
  "out": "D:/results/history",
  "gpa_event": 33,
  "resource": null,
  "x": 32,
  "y": 32,
  "mip": 0,
  "layer": 0,
  "sample": 0
}
```

```powershell
# Run from the repository root; set external paths for your environment.
$ResultsRoot = Join-Path (Get-Location) 'artifacts/results'
.\out\FloraGPA-history-backend\FloraGPA.Rdc.exe --job "$ResultsRoot/history-job.json"
```

`gpa_event` selects a uniquely mapped original command. Alternatively `eid`
selects an explicit RenderDoc event; the two selectors cannot be combined.
Omitting both follows the Python worker's last executed draw/dispatch selection,
or the greatest mapped event when no draw exists. `resource` is an original GPA
resource ID; null selects color target zero at the chosen event. Mip, layer,
sample and pixel coordinates are validated against the actual replay texture.

`result.json` is written atomically. A failure returns a nonzero exit code and
an error where an output directory has been accepted. Existing output contents
are never overwritten. The result includes loaded modules, exact event maps and
the backend version/commit. Job input is limited to 1 MiB; numeric selectors are
checked. Internal `events` jobs expose the native action index and parsed CPU
writes for integration verification.

## Recovered behavior

- Opens and shuts down the real replay controller and capture file with scoped
  ownership. Uses the public replay marker, DLL identity/version checks and
  RenderDoc allocation functions for API strings and arrays.
- Reads native actions and structured API chunks, then connects the previously
  recovered exact GPA provenance mapping to this production consumer.
- Calls RenderDoc `PixelHistory` and preserves fragment/primitive IDs, all test
  failure flags, direct writes, unbound shaders, before/after/shader-output
  values, depth/stencil, integer bit interpretations and nonfinite values.
- Parses exact UpdateSubresource and matching Map/Unmap boundaries. CPUWrite
  usages absent from backend fragment history receive before/after PickPixel
  snapshots, with original command provenance. Mip/array/volume/box coverage,
  integer formats and unchanged mapped candidates follow the Python behavior.
- CPU records have no invented fragment, primitive, shader-output or
  depth/stencil values. Unknown structured layouts and failed reads remain
  explicit gaps. The selected frame event is restored after supplementation.

This is a DX11 adapter tied to RenderDoc's 1.45 C++ ABI. Other versions are
rejected before opening a capture. The checked version and tested binary commit
do not prove compatibility with arbitrary modified builds sharing a version.
See [the header notes](../third_party/renderdoc/REPLAY_HEADERS.md) for provenance
and the two compatibility changes; no RenderDoc binary is redistributed.

## Remaining integration

The subsequent [Qt Pixel History migration](PIXEL_HISTORY_UI_MIGRATION.md) connects
main-window job scheduling, recapture/cache invalidation with experiments,
cancellation and history-table navigation/export. This backend does not complete the
shared `rdc_worker.py` module: its RenderDoc-backed texture/postmesh/counter and
pixel/vertex/compute debugger consumers remain pending. The full migration goal
is unchanged; passing this backend's checks is not full UI parity.

## Validation

The full Release build and the `rdc_worker` / `rdc_events` CTest suites pass. The worker
tests cover invalid selectors, wrong libraries, malformed/oversized jobs, Unicode
output and preservation of existing results without requiring RenderDoc.

Packaged GPU validation uses the original Python worker only as an external
development oracle. It compares complete history records, maps, subresource
selection and scope reports, with independent known-texel checks for CPU writes.
The final package passed 152 checks, including 22 complete history comparisons
(35 records, of which 10 are CPU snapshots), nine actual replay-controller
indices compared against the original reference, and five invalid selection
cases. Coverage includes MSAA samples, Resolve, native/API event selection,
copy/clear/depth/mip operations, Update boxes, 1D/array/volume Map writes,
uint/sint values and unchanged partial-Map candidates. Evidence:
`artifacts/rdc-history-package-final/validation.json`.

The optional UAV-clear fixture uses the SDK's five-word UAV descriptor. The old
Python test supplied an extra trailing zero ignored by ctypes; the native checked
reader intentionally rejects that malformed wire size. Both history consumers
compare the same valid recapture.

The native task tests report 10 passed, zero failed/skipped (including setup and
cleanup) in `artifacts/rdc-worker-tests.txt`; the two CTest suites are recorded in
`artifacts/ctest-rdc-history.log`. Packaged GF2/BF1 golden images and suppressed-draw
negative controls pass all four checks in `artifacts/rdc-history-golden/validation.json`.
All 44 native success/failure and recapture reports pass module checks, all four
executables match the Release build, and no RenderDoc/Python runtime is bundled
(`artifacts/rdc-history-runtime-audit.json`).

The malformed structured-chunk and failed-PickPixel mock cases from the Python
unit suite have not yet been independently injected into the native controller;
the production parser preserves explicit gaps, but these branches remain unverified.
Qt workflow coverage is recorded separately in the UI migration document. Shared
native module migration entries stay `partial`.

```powershell
# Run from the repository root; set external paths for your environment.
$ReferenceRoot = 'C:/reference/FloraGPA'
python tools/validate_rdc_history.py --reference "$ReferenceRoot" --exe out/FloraGPA-history-backend/FloraGPA.Rdc.exe --cli out/FloraGPA-history-backend/FloraGPA.Cli.exe --qt-bin out/FloraGPA-history-backend --captures artifacts/rdc-capture-package --out artifacts/rdc-history-check
```

Use a fresh output directory. `--captures` points to the existing recapture
validation corpus, including its original reference event dumps. GPU jobs and
their external oracles run sequentially. Validation on this machine does not
replace a separate clean Windows deployment check.
