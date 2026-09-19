# FloraGPA

Native Windows x64 DX11 frame analyzer. C++20, Visual Studio 2022, Qt 6 Widgets.

This repository migrates the independently recovered Python implementation in
`D:/CDXrepo/FloraGPA`. It does not load that implementation at runtime.
Migration status is tracked per source module in `docs/migration.json`.
Pending functionality is not represented as working functionality.

## Build

Install Qt 6.11.2 **MSVC 2022 x64** (the MinGW kit is not ABI-compatible).

```powershell
cmake --preset vs2022 -DFLORA_TEST_CAPTURE_DIR=D:/CDXrepo/FloraGPA
cmake --build --preset release
ctest --preset release
```

Use `-DFLORA_BUILD_GUI=OFF` to omit Qt Widgets and the desktop application.
The CLI still uses Qt Core and Gui for file handling and PNG output.
Capture files and GPU test outputs stay outside Git. Commits use the configured
personal Git identity and English titles and bodies. No remote is configured.

The preset uses `D:/Qt/6.11.2/msvc2022_64`. Override `CMAKE_PREFIX_PATH` for a
different installation. Open `build/vs2022/FloraGPA.sln` in Visual Studio 2022.
GPU tests require the external GF2 capture and a working D3D11 adapter; they
explicitly skip when `FLORA_TEST_CAPTURE_DIR` is empty.

## Run and deploy

```powershell
.\tools\package.ps1
.\out\FloraGPA\FloraGPA.exe
.\out\FloraGPA\FloraGPA.Cli.exe replay D:\captures\sample.gpa_frame --out D:\results\sample
```

The entire `out/FloraGPA` directory is the application package, including Qt
plugins and app-local VC143 runtime DLLs. Keep the worker beside the GUI.
No Python environment, GPA installation or original source directory is used
by the application. Package portability has been checked on this host with
Windows-only child process paths, not yet on a separate clean Windows machine.

Use **F5** to replay, **F6** for GPU timings, **Escape** to cancel. Select API
events to inspect their pipeline and before/after output. Resources provide
texture mip/layer/slice previews, shader source/DXBC/reflection and buffer bytes.
The Buffer tab reads initial/before/after values and byte ranges as hex, ASCII or
32-bit words. Geometry provides IA tables and a rotatable wireframe, with CSV/OBJ
export. Resource names come from captured debug metadata.
Shader **Compile & Apply** and event enable/disable changes use an experiment
history with undo/redo and frame-bound JSON projects.

See [migration status](docs/MIGRATION_STATUS.md) for implemented features and
the remaining parity gaps. This build is not the completed migration.

## Reference comparisons

These scripts use Python only as a development oracle. Neither the GUI nor the
worker launches or loads Python.

```powershell
python tools/validate_native.py --exe out/FloraGPA/FloraGPA.Cli.exe --captures D:/CDXrepo/FloraGPA --out artifacts/golden-run --isolated-env
python tools/validate_geometry.py --reference D:/CDXrepo/FloraGPA/standalone --exe out/FloraGPA/FloraGPA.Cli.exe --captures D:/CDXrepo/FloraGPA --qt-bin D:/Qt/6.11.2/msvc2022_64/bin --out artifacts/geometry-run
python tools/validate_buffers.py --reference D:/CDXrepo/FloraGPA/standalone --exe out/FloraGPA/FloraGPA.Cli.exe --captures D:/CDXrepo/FloraGPA --qt-bin D:/Qt/6.11.2/msvc2022_64/bin --out artifacts/buffer-run
```

Run GPU checks serially and use a new output directory each time.
