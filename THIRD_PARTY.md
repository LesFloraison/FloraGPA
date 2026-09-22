# Third-party components

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
  explicitly selected for optional recapture. No RenderDoc binary or Python
  runtime is redistributed with FloraGPA.
- RenderDoc 1.45 public replay headers are vendored under
  `third_party/renderdoc/replay/` for the isolated native analysis worker.
  MIT, Copyright (c) 2015-2026 Baldur Karlsson and (c) 2014 Crytek; per-file
  notices are preserved. `replay-manifest.json` records the local upstream
  snapshot hashes and tested binary commit. `REPLAY_HEADERS.md` documents the
  two small compatibility changes. This uses the version-specific C++ replay
  ABI; the optional external DLL must report release version 1.45.
