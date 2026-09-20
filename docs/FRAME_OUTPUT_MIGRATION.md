# Frame output migration

The Output tab now uses native C++ target selection, selected storage readback
and CPU RGBA8 display conversion. It retains the GPA-style central viewport and
compact controls. Python is used only by development comparison scripts.

## Implemented

- `auto`, `present`, RT0–RT7, depth, stencil and explicit swap-chain selection.
  Native OM getters identify the currently bound views. Ambiguous identities and
  missing outputs produce an explicit no-image result, not reference pixels.
- Capture-end presentation follows the live texture sharing the recorded
  swap-chain parent. The `0x87` image supplies identity metadata only. Explicit
  swap-chain selection supports captures with multiple chains.
- RTV/DSV format, mip, absolute array layer, 3D W slice and buffer element ranges
  control readback. Global view experiments are read through the effective frame.
- Selected original storage is exported separately from the display as
  `output_storage.bin` and `output_storage.json`. Channels and numeric ranges
  affect the display only. sRGB encoded values remain encoded; no HDR tone
  mapping is implied. Float, integer, normalized, packed color, depth and stencil
  conversion follows the reference decoder, including nonfinite values and
  ties-to-even rounding.
- MSAA hardware resolve where supported; shader mean otherwise; selected sample
  reads use compatible integer views where possible to preserve storage bits.
  Packed depth/stencil uses the reference depth and stencil passes. BGRX unused
  bytes follow the reference's zero convention. Helper draws execute on a private
  deferred context and restore immediate bindings.
- Qt controls select target, layer, resolve/sample, channel and display range.
  Capture changes reset these selections. Empty output clears the old image and
  disables export. Raw export preserves the last completed output across other
  inspection jobs.

The MSAA report explicitly says that pre-capture per-sample contents are not
reconstructed. Reading a sample is not proof that its initial contents were
present in the capture. Recorded writes determine the replayed result.

## Evidence

`tools/validate_frame_output_port.py` compares exact storage bytes, RGBA bytes,
selection metadata and conversion/MSAA metadata against the preserved Python
implementation. Native children receive only Qt and Windows runtime paths.
The script also generates independent sample patterns directly through D3D11
and compares them with replayed patterns, including unsigned integers above
2^24, signed integers, all four samples, two array layers, D16, D24S8, D32 and
D32S8, on hardware and WARP.

Initial evidence retained under `artifacts/`:

- `frame-output-parity-initial/validation.json`: 6,513 CPU acceptance/rejection
  cases and 657 aggregate checks of non-MSAA GPU output and presentation.
- `frame-output-msaa-depth/validation.json`: 731 checks including the CPU suite,
  MSAA sample-pattern comparisons, depth/stencil, sRGB, BGRA and BGRX.
- `frame-output-ui-initial.txt`: output availability, target switching, channels
  and range controls. Screenshots are retained alongside the UI evidence.

These are local evidence artifacts; captures and generated files stay out of Git.

Final verification of this implementation:

- `frame-output-complete/validation.json`: completed, all 1,560 checks passed;
  CPU coverage is 6,515 cases (1,852 accepted, 4,663 rejected). Added poisoned
  reference pixels, explicit second-chain selection and before/after-copy
  availability checks. `frame-output-cpu-debug` repeats CPU parity in Debug.
- `ctest-frame-output-release.log`: 27/27 CTest suites passed. Qt reports
  38 passed, none skipped. `frame-output-Release.txt` in the build directory
  includes repeated integer sample reads and exact OM identity/reference-count
  preservation. Debug frame-output tests and `frame-output-ui-debug.txt` pass.
- `validation-frame-output-golden/validation.json`: both real GF2/BF1 golden
  hashes and both suppressed-draw negative controls pass with the packaged CLI
  and Windows-only PATH. `frame-output-package-real/validation.json`: six real
  capture original/edited view comparisons pass.
- `frame-output-audit.json`: 318 successful native reports, 85 loaded module
  paths with no Python/GPA/RenderDoc runtime, and all three packaged executables
  identical to the verified Release builds.

Package: `out/FloraGPA-frame-output/FloraGPA.exe`.

## Remaining integration

This does not complete the Python application migration. Output display settings,
driver and API selection now save and restore through experiment UI state;
see `docs/OUTPUT_SESSION_MIGRATION.md`. Output pixel navigation identifies the
API/resource and selected subresource, and buffer pixels open their exact byte
range. The downstream pixel-history/coverage/debugger workflows remain pending.
Ordinary before-draw output now stops before the selected snapshot, while input
and pipeline inspectors retain their prepared boundary. Missing-binding recovery
follows the reference exception; see `docs/BEFORE_BOUNDARY_MIGRATION.md` for the
consumer distinction and dedicated comparison coverage.

The Texture tab still has its older preview path; MSAA output support here does
not imply that all texture inspection/editing consumers have been migrated.
Per-sample MSAA writes, planar texture handling and texture experiment editors
remain separate work. Immediate-binding preservation must not be generalized to
all private query/counter workflows without their own validation.

## Reproduce

```powershell
python tools/validate_frame_output_port.py `
  --reference D:/CDXrepo/FloraGPA `
  --oracle build/vs2022/Release/FloraFrameOutputTests.exe `
  --exe build/vs2022/Release/FloraGPA.Cli.exe `
  --qt-bin D:/Qt/6.11.2/msvc2022_64/bin `
  --out artifacts/frame-output-new-run
```

Use a new output directory each time. GPU validations must run serially.
`--cpu-only` limits the run to conversion/descriptor/inventory checks;
`--msaa-only` runs those checks and the MSAA GPU cases.
