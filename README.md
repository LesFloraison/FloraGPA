# FloraGPA

<img src="assets/icons/FloraGPA.svg" alt="FloraGPA icon" width="96" height="96">

An independent Windows x64 DX11 single-frame replay and analysis tool,
built with C++20, Visual Studio 2022 and Qt 6 Widgets.

FloraGPA reads existing GPA captures and provides event navigation, resource and
pipeline inspection, replay experiments, shader tools and GPU measurements.
Its current compatibility work targets **GPA 2025 R1 legacy DX11 / IGPA v3**.
Compatibility and analyzer migration are still in progress; this is not a
complete replacement for every GPA feature or capture format.

Ordinary replay runs in native C++ without Python or an Intel GPA installation.
Some optional analysis features require separately supplied components.

## Capabilities

- Replay captured DX11 work and navigate API events and annotations.
- Inspect textures, buffers, shaders, pipeline state and geometry at supported
  event boundaries; export images, raw bytes and reports.
- Apply supported shader, resource and pipeline experiments with undo/redo and
  frame-bound project files.
- Inspect Coverage and Quad diagnostics, collect DX11 timing/statistics, and
  use optional RenderDoc or Intel Metrics Discovery analysis.
- Run offline compatibility preflight before attempting GPU replay.

Support depends on the recorded layout, saved resource data, command path and
device capabilities. See [current support and limits](docs/CURRENT_STATUS.md)
for the capability matrix and evidence; module migration totals are not a
percentage of original GPA functionality.

## Build

Install these prerequisites:

- Windows x64 and Visual Studio 2022 with **Desktop development with C++**,
  the MSVC v143 x64 toolset and a Windows SDK.
- CMake 3.25 or newer.
- Qt **MSVC 2022 x64**, including Core, Gui, Widgets, Concurrent and Test for
  the default development build. The validated Qt version is 6.11.2;
  CMake requires Qt 6.11 or later. The MinGW kit is not ABI-compatible.

Run from the repository root, replacing the Qt path with your installation:

```powershell
$QtRoot = 'C:/Qt/6.11.2/msvc2022_64'
cmake --preset vs2022 "-DCMAKE_PREFIX_PATH=$QtRoot"
cmake --build --preset release
```

The tracked `CMakeLists.txt` and `CMakePresets.json` generate
`build/vs2022/FloraGPA.sln` and the Visual Studio projects. Generated projects
stay outside Git; a fresh clone does not need a pre-generated solution.
Python, GPA and the earlier Python implementation are not build dependencies.

Use `-DBUILD_TESTING=OFF` to omit development tests. Use
`-DFLORA_BUILD_GUI=OFF` to omit the Qt Widgets desktop application;
the CLI still uses Qt Core and Gui. Machine-specific overrides can live in
the ignored `CMakeUserPresets.json`.

## Package and run

For the default GUI build, use the same Qt installation for deployment:

```powershell
./tools/package.ps1 -QtRoot $QtRoot -OutputDirectory ./out/FloraGPA
./out/FloraGPA/FloraGPA.exe
```

The current packaging helper locates the VC143 redistributable under the
standard VS2022 Community installation directory. Other Visual Studio editions
or custom installation paths need a packaging-script path adjustment.

Keep the entire output directory together: GUI, CLI, workers, bridge, Qt plugins
and app-local VC runtime DLLs. Do not distribute only `FloraGPA.exe`.
To package while another copy is running, choose a fresh output directory.

Open a capture through **File > Open Capture…** or pass its path to the GUI.
Use **F5** to replay, **F6** for **Collect GPU Metrics**, and **Escape** to cancel.
See the [usage guide](docs/USAGE.md) for inspection and editing workflows.

## Preflight and command-line replay

These examples assume captures are in a local `captures/` directory.
Use a new or empty output directory for each command.

```powershell
$CaptureRoot = './captures'
$Frame = Join-Path $CaptureRoot 'sample.gpa_frame'
$Cli = './out/FloraGPA/FloraGPA.Cli.exe'
& $Cli validate-frame $Frame --out ./artifacts/preflight
& $Cli replay $Frame --out ./artifacts/replay
```

`validate-frame` checks structure, known blockers and command coverage without
creating a GPU device. A successful preflight does **not** certify replay output.
The GUI exposes the same diagnostics through its compact **Preflight** entry.
Replay exports its image when available and a report; captures without a
presentation image have separate output-inspection paths.

## Optional dependencies

| Feature | Additional requirements |
|---|---|
| Ordinary replay, native inspection, Coverage/Quad and DX11 queries | A suitable D3D11 device; supported paths can also use WARP |
| Pixel History, recorded VS/PS/CS debugging, Replay Mesh and Replay Metrics | A separately supplied compatible RenderDoc 1.45 release DLL |
| Intel hardware metrics | A supported Intel GPU and installed Metrics Discovery driver; the bridge is built with FloraGPA |
| External assembly or HLSL recovery fallback | An explicitly selected compatible shader tool |
| Development reference comparisons | Python, external reference sources and fixtures; original-kernel comparisons additionally require the pinned installed GPA build |

RenderDoc binaries, GPA binaries, reference sources and game captures are not
distributed with this repository. Third-party components already included in
the source/build are described in [THIRD_PARTY.md](THIRD_PARTY.md).

## Validation and known limits

```powershell
ctest --preset release --parallel 1
```

Run GPU checks serially. Optional external-fixture checks can skip when their
fixture variables are absent; a passing default run is not full corpus acceptance.
For GF2/BF1 checks, configure `-DFLORA_TEST_CAPTURE_DIR=<your-fixture-directory>`.
See [development validation](docs/USAGE.md#development-validation) for other
fixture requirements and reference comparisons.

Captured Deferred Context / Command List execution, broader interface layouts,
resource identity/version behavior and missing pre-frame information remain
compatibility work. Missing captured bytes cannot be reconstructed by assumption.
Helldivers retains documented workload variability. Original-player comparisons
also have explicitly recorded disagreements and configuration limits.

Package isolation has been exercised on the development host; deployment on a
separate clean Windows installation is not yet certified. Existing validation
records describe their own builds, not every future build or arbitrary capture.

## Documentation

- [Documentation index](docs/README.md)
- [Usage guide](docs/USAGE.md)
- [Current capabilities, acceptance and M1–M6 roadmap](docs/CURRENT_STATUS.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Development chronology (Chinese)](docs/MIGRATION_STATUS.md)

Detailed migration/audit documents are batch records. Their historical pending
items and counts must be read with the current support summary.
