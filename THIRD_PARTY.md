# Third-party components

- Microsoft Detours v4.0.1: MIT, pinned commit
  `e4bfd6b03e50de46b47abfbd1e46b384f0c5f833`. Capture-only native injection;
  source and license are in `third_party/detours/`. Its source hashes and the
  capture JSON dependency are checked by `src/capture/dependencies.json` during
  configuration. Upstream: https://github.com/microsoft/Detours.

- Intel Metrics Discovery public interface: MIT, Copyright (c) 2019-2026 Intel
  Corporation. Vendored header and license are in `third_party/metrics-discovery/`.
  Pinned upstream commit: `b798d05c3c535d3840eccbba23670584c26dd1a9`;
  `source.json` records original paths. Header SHA-256:
  `6cf0c5d6be3ac6c329f3e1241a2da520af89f3b37ac476fbab072c45ef8e9333`.
  `FloraGPA.Metrics.dll` is built from our native bridge source. Intel's driver
  DLL is discovered from the loaded Intel DX11 driver in Windows DriverStore;
  it is neither bundled nor required for normal replay.

- Qt 6.11.2: dynamically linked MSVC2022 x64 Core, Gui, Widgets and Concurrent;
  Test is used only for development. Installed through the Qt maintenance tool.
  Qt license texts are included with the portable build. Qt source and licensing:
  https://www.qt.io/download-qt-installer-oss and https://code.qt.io/cgit/qt/.
  `windeployqt` also deploys Qt Svg/Network dependencies of its selected plugins.
  The LGPL-3.0-only and GPL-3.0-only texts were taken from the Qt Base v6.11.2
  `LICENSES` directory. The portable directory includes the VC143 app-local
  runtime from the installed VS2022 redistributable directory.
- nlohmann/json 3.12.0: vendored single header, MIT license at
  `third_party/nlohmann/LICENSE.MIT`. Upstream tag:
  https://github.com/nlohmann/json/tree/v3.12.0.
  Header SHA-256:
  `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63`.
- Windows D3D11, DXGI, D3DCompiler and BCrypt are operating-system APIs.
  No Intel GPA libraries are included.
- RenderDoc public application API header: MIT, Copyright (c) 2015-2026 Baldur
  Karlsson. Vendored unmodified as `third_party/renderdoc/renderdoc_app.h`, with
  license at `third_party/renderdoc/LICENSE.MIT`. Source: the local upstream
  RenderDoc 1.45 source snapshot's `renderdoc/api/app/renderdoc_app.h`.
  Header SHA-256: `b7005e7dc34c3635046868bbd76d81b9b055aede0f56daa0bd39fedee0639ffb`.
  FloraGPA requests public application API 1.6.0 and only loads RenderDoc when
  requested for analysis recapture. The portable package bundles the official
  RenderDoc 1.45 x64 release DLL and its native `renderdoccmd.exe` helper,
  pinned in `runtime-manifest.json`, without
  QRenderDoc, Python, Qt5 or vendor-specific optional plugins. The runtime is not
  tracked in Git. Its SHA-256 is
  `313f450bfa1f7f7d5f50cc7ee80d9f99b68d3fe873e25eb865ff0433e359f7b3`.
  The official archive URL/hash, upstream commit, MIT license, acknowledgements
  and upstream third-party notice files accompany it under `RenderDoc/`.
  Ordinary replay remains independent of this backend. No Python runtime is
  redistributed with FloraGPA.
  The optional `-PatchedRenderDocRoot` packaging argument replaces only the DLL
  with a local build of the same upstream commit plus
  `third_party/renderdoc/patches/dx12-d16-history.patch` and
  `third_party/renderdoc/patches/dx12-dxbc-workgroup.patch`. The first preserves
  D16 depth typing and decodes its stored UNORM bits in DX12 Pixel History;
  the second initializes every simulated DXBC compute lane's Group ID in the
  DX12 debugger. Both export patch-version markers for the isolated adapter.
  Build both with `tools/build_renderdoc_d16.ps1` in a clean dedicated upstream
  v1.45 checkout (the script retains its original name).
  Patched packages include both patches, build/source hashes, the original runtime
  manifest and an active manifest containing the replacement DLL hash.
- RenderDoc 1.45 public replay headers are vendored under
  `third_party/renderdoc/replay/` for the isolated native analysis worker.
  MIT, Copyright (c) 2015-2026 Baldur Karlsson and (c) 2014 Crytek; per-file
  notices are preserved. `replay-manifest.json` records the local upstream
  snapshot hashes and tested binary commit. `REPLAY_HEADERS.md` documents the
  two small compatibility changes. This uses the version-specific C++ replay
  ABI; the optional external DLL must report release version 1.45.
