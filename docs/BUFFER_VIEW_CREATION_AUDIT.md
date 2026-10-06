# M2: creation-time buffer SRV, RTV and UAV replay

> **2026-10-06:** [Initial counter provenance](INITIAL_COUNTER_BOUNDARY_AUDIT.md)
> now diagnoses missing frame-before counts as well. The following creation-time
> evidence remains unchanged.

> Historical batch record. Results, package paths, module counts and remaining-work
> statements below describe this batch. See [current status](CURRENT_STATUS.md)
> for present support and [the documentation index](README.md) for navigation.
> `artifacts/`, `out/`, `build/` and external-reference paths identify local
> evidence not distributed with a clone; original JSON baselines are preserved.

> **Subsequent work:** Subsequent [pipeline creation](PIPELINE_CREATION_AUDIT.md), [geometry/SO creation](GEOMETRY_CREATION_AUDIT.md) and [class creation](CLASS_CREATION_AUDIT.md) extend frame-time object support. General versions and frame-before counters remain separate work.

The recovered Device5 creation path now accepts buffer sources for SRV, RTV and
UAV records (`357c`, `357e`, `357d`). Checked ranges use 64-bit arithmetic and
respect typed element size, raw DWORDs or structured stride. Bind flags, view
dimension, raw/structured requirements, counter flags and saved resource/device
identity are checked before creation. Buffer DSVs remain invalid. Native view
creation and GetDesc still validate device support and saved descriptor agreement.
The inactive SRV BUFFER/BUFFEREX union tail is not treated as semantics.

## Original fixtures

[buffer-view-corpus.json](buffer-view-corpus.json) registers nineteen unmodified
GPA 2025 R1 legacy DX11 captures from `tools/native/buffer_view_creation_probe.cpp`.

| Modes | Scope |
|---|---|
| 0–3 | Typed, structured, raw BUFFEREX and default structured SRVs |
| 10–12, 15 | Typed, raw, structured and default structured UAVs |
| 13–14 | Counter and Append UAV, explicit count 2, one Dispatch produces count 3 |
| 16–17 | Same counter families, second Dispatch with KEEP produces count 4 |
| 20 | Typed R32_FLOAT buffer RTV |
| 30–35 | Failed and validation-only SRV/UAV/RTV calls, each followed by valid creation |

Explicit views select a nonzero first element and a proper subset of storage.
Default views cover the full structured buffer. Initial contents are nonuniform
and uploaded inside the frame. The producer verifies all 64 buffer bytes and a
dependent image for twelve frames, both natively and under the original shim:
456 frame checks. Counter fixtures additionally verify CopyStructureCount bytes.
Source/executable, capture, oracle and device IDs are retained with hashes.

C++ tests check resource bytes immediately after the relevant creation/write and
at frame end, on hardware and WARP. Counter checks also stop directly after
creation and reset, verify final 3/4 values, reject execution after disabling the
only initializing setter, and verify an explicitly supplied experiment initial
count restores the expected result. The original files remain unmodified.

## Counter provenance

The accepted creation record and saved UAV descriptor contain no counter value.
FloraGPA now marks newly created Counter/Append views as requiring initialization.
A captured explicit reset establishes that value; KEEP preserves it. A draw or
Dispatch binding such a still-uninitialized view, CopyStructureCount, or counter
inspection fails with the view identity and a specific reason. Initial counter
experiments are applied at the creation event and recorded as user-provided
values. Clearing buffer data does not initialize the hidden counter.

The KEEP interpretation follows Microsoft's
[CSSetUnorderedAccessViews contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-cssetunorderedaccessviews),
and is independently exercised by modes 16–17. This is a conservative acceptance
boundary for new views; it does not infer a creation-time count from observed
driver memory, nor finish the separate M4 task of frame-before counter recovery.

## Additional wire records

Buffer GetDevice (`3015`, eight-byte returned identity) and GetDesc (`301c`,
checked flag plus 24-byte descriptor) were real blockers exposed by these files.
SRV GetResource (`302d`) is also present. SRV QI/AddRef/Release/GetDevice
(`3026`–`3029`) now have strict decoders using original writer evidence and
synthetic tests; these four family-specific records are absent from this corpus.
SRV GetDesc (`302e`) retains its existing strict passive decoder.

Read-only Ghidra evidence is retained in
`artifacts/m2-buffer-view-srv-wrappers.c` and
`artifacts/m2-buffer-view-buffer-wrappers.c`, with matching RVA lists and logs.
All new metadata layouts have truncated-prefix/trailing-data and API inspection
tests. Buffer GetDesc also checks absent and illegal presence flags. Failed
HRESULTs and validation-only S_FALSE allocate no view; successful views remain
unavailable before creation. Metadata never drives replay COM lifetime.

## Acceptance and limits

The verified package is `out/FloraGPA-buffer-views-20261003/`. Across 179
registered files, 358 independent replays complete: 178 repeat exactly and the
known Helldivers variability remains. All 159 previously stable image hashes are
unchanged. Thirty-eight new original-player comparisons, 123 corpus controls,
88 retained texture-boundary exports and four golden/negative checks pass.
New buffer boundaries and counter values are checked by the C++ hardware/WARP
tests, rather than reported as texture exports. All 358 ordinary runtime module
audits pass and the five packaged application binaries match Release.

Nineteen related CTest suites pass, including UAV counters, buffer edits and Quad
UAVs. Buffer view creation has 42 passing Qt cases, earlier view creation 32,
texture creation 55, main UI 55 and texture edit replay 34, all without skips.
All nineteen new files were rejected by the previous package; its preflight
reports are retained in `artifacts/m2-buffer-view-before/validation.json`.

Final results are pinned in [buffer-view-baseline.json](buffer-view-baseline.json).
The producer and original player run only in the development environment;
the application remains C++/Qt without GPA or Python runtime dependencies.
The original player's adapter identity is still unknown, so matching images
are observed agreement, not a matched-device equivalence claim.

These fixtures cover four-byte typed/raw/structured elements, nonzero element
ranges, default structured views and the stated counter sequences. They do not
certify every format/stride, aliasing view pattern or cross-device combination.
Missing frame-before counters, other interface/version layouts, repeated object
identities and Deferred/Command List remain open. M2 and M3–M6 are incomplete.

Build `FloraBufferViewCreationProbe` explicitly; its arguments are
`<new-output-dir> <mode> [<capture.gpa_frame> <shimloader64.dll>]`. Use
`GPA_LOCAL_INJECT=true` for capture runs and execute all GPU checks serially.
Use `FLORA_BUFFER_VIEW_CAPTURES` for the original-fixture CTest directory.
Captures and generated artifacts remain outside Git.

Subsequent acceptance: [PIPELINE_CREATION_AUDIT.md](PIPELINE_CREATION_AUDIT.md)
retains these nineteen fixtures in its 211-file matrix and records unchanged
prior stable image hashes. That increment handles cached pipeline state identity;
general resource identity/version reconstruction remains open.
