# RenderDoc replay headers

Source: local upstream `renderdoc-1.x_origin/renderdoc/api/replay` snapshot,
version 1.45. The snapshot has no Git metadata; no upstream tag/commit identity
is asserted. `replay-manifest.json` records each original file hash, normalized
vendored file hash, and the separately observed installed binary commit.
All per-file MIT notices are retained; the aggregate license includes Crytek.

Two compatibility changes preserve public type layouts and virtual interfaces:

- `shader_types.h`: `ShaderVariable::GetPointer()` initializes `PointerVal` by
  member assignment because its defaulted constructors preclude aggregate
  initialization in C++20.
- `renderdoc_replay.h`: a private `FLORA_RENDERDOC_DYNAMIC_ALLOCATORS` define
  removes `dllimport` only from the two allocation entry-point declarations.
  The worker implements these bridges by calling the selected DLL's alloc/free
  exports. API strings/arrays retain RenderDoc allocator ownership. All other
  declarations remain upstream `dllimport`; entry points used by the worker are
  explicitly resolved with `GetProcAddress`. No import library or installed SDK
  is needed to build FloraGPA.

The worker exports RenderDoc's replay-program marker and runs separately from
the recapture worker. It checks the loaded DLL identity and release version
before opening a capture. The selected DLL stays loaded until process exit.
