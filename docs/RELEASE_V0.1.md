# FloraGPA v0.1 — stable baseline

Version **0.1.0**, tag **v0.1.0**, release date **2026-10-10**.

This baseline supports the verified Windows x64, GPA 2025 R1 legacy DX11 /
IGPA v3 paths. It is not a promise of compatibility with every DX11 capture,
GPA version, graphics device or original Graphics Frame Analyzer feature.

## Run

Download `FloraGPA-v0.1.0-windows-x64.zip` from the GitHub release, extract the
whole directory and start `FloraGPA.exe`. Keep the DLLs, plugins and workers
together. A suitable Windows D3D11 device and graphics driver are required.
Ordinary replay does not require an Intel GPA, Python, Qt or Visual Studio
installation. Qt and the application-local VC runtime are included.

Open a capture with **File > Open Capture…**. Use **F5** to replay, select an
API event to navigate, and press **Escape** to cancel. The compact **Preflight**
entry exposes structural diagnostics; passing preflight does not certify GPU
output. Images, resource bytes and analysis reports can be exported.

## Included capabilities

- Native DX11 replay of supported immediate-context work, annotations and event
  navigation, with explicit diagnostics for unsupported or incomplete inputs.
- Texture, buffer, pipeline, geometry and shader inspection at supported event
  boundaries; image, byte and report exports.
- Supported shader/resource/pipeline experiments, undo/redo and frame-bound
  project files.
- Native Coverage/Quad diagnostics and supported DX11 timing/statistics queries.
- Optional RenderDoc and Intel Metrics Discovery integration.

The final replay implementation is based on `3186c82`, including the correction
that preserves execution after pixel discard in SM4.0 mip dependency analysis.
Discard does not incorrectly become a shader return; required omitted state
continues to produce a located refusal. Game shaders are not patched.

## Known limits and optional components

- Traditional Deferred Context / Command List production replay is incomplete.
  Structure inspection is not evidence that every list can be executed.
- Missing pre-frame resource bytes, counters and other omitted capture state
  cannot be reconstructed by assumption. Some completed replays have known
  capture-side fidelity limitations; completion and fidelity are distinct.
- Helldivers 2 has documented replay variability around its in-place sharpening
  workload. This release does not promise a fixed final-image hash or silently
  disable the event.
- Pixel History, recorded shader debugging, Replay Mesh and Replay Metrics
  require a separately supplied compatible **RenderDoc 1.45** DLL. Intel hardware
  metrics require a supported Intel GPU and its Metrics Discovery driver.
  These external binaries are not bundled or required for ordinary replay.
- Package isolation is checked on the development Windows host. A separate
  clean Windows installation and broader driver/device coverage are not certified.
- Earlier GUI GDI-count observations have no established allocation-owner or
  unbounded-leak conclusion. No leak-free claim is made.

## Validation provenance

Three evidence scopes are intentionally separate:

1. **Earlier full baseline:** the early-return package completed the 43-suite,
   629-registration / 619-unique-capture matrix, with 587 completed registrations
   and 42 located refusals. Fidelity classifications were 155 passed, 32
   capture-side mismatch, 19 information missing and 423 unassessed. These are
   mostly specialist samples, not counts of supported games.
2. **Latest discard correction:** retained development logs report 200 LOD test
   rows, nine Windows UI rows and the preflight CTest passing. Its historical
   full-matrix run and source-build attempt were incomplete; they are not claimed
   as complete acceptance of this version.
3. **Final release:** the separately published `RELEASE_VALIDATION.json` records
   the committed source build, focused checks, GF2/BF1 golden and negative
   controls, Helldivers boundary/control checks, desktop recovery and relocated
   package checks actually completed for the distributed binary.

`BUILD_INFO.json` in the portable package identifies its source commit and build
toolchain. `SHA256SUMS.txt` on the release identifies the final downloadable
assets. Captures, local evidence, test executables and developer tools are not
part of the application package.

## Source and maintenance

The tag provides CMake source for VS2022 x64, C++20 and Qt 6.11 or newer
(validated kit: Qt 6.11.2 MSVC2022 x64). Generated `.sln` files are created by
CMake and are intentionally not versioned. See the repository README for build
commands and `THIRD_PARTY.md` for included components and notices.

This release does not change the project's existing licensing terms.
Blocking fixes will use v0.1.x; expanded compatibility and analysis work belong
to subsequent development. Published tags and binaries will not be replaced.
