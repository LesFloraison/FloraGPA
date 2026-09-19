# FloraGPA

Native Windows x64 DX11 frame analyzer. C++20, Visual Studio 2022, Qt 6 Widgets.

This repository migrates the independently recovered Python implementation in
`D:/CDXrepo/FloraGPA`. It does not load that implementation at runtime.
Migration status is tracked per source module in `docs/migration.json`.
Pending functionality is not represented as working functionality.

## Build

Install Qt 6.11.2 **MSVC 2022 x64** (the MinGW kit is not ABI-compatible).

```powershell
cmake --preset vs2022
cmake --build --preset release
ctest --preset release
```

Use `-DFLORA_BUILD_GUI=OFF` for the native core and CLI without Qt.
Capture files and GPU test outputs stay outside Git. Commits use the configured
personal Git identity and English titles and bodies. No remote is configured.
