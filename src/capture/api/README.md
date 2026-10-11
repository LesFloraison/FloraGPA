# Native COM forwarding description

`interfaces.json` describes public COM method signatures from the Windows SDK
10.0.26100.0 D3D11/DXGI headers. It contains API declarations and array directions,
not SDK implementation code. The ordinary installed Windows SDK supplies the
actual declarations, GUIDs and libraries used by the compiler.

`FloraCaptureApiGenerator` is a C++20 build tool using the repository's pinned
JSON dependency. It generates `object_proxies.cpp` in the build directory. It is
not shipped in the capture package. Building and running capture require no
Python or Qt. Native compilation checks every generated override against the
SDK; generated source is not checked into Git.

Each family includes inherited methods, delegates IUnknown to one object owner,
unwraps input interfaces and wraps successful interface outputs. The native
QueryInterface result controls availability; implementing a newer interface in
C++ does not expose it on an older native object. Bounded shader getter arrays
retain the caller's input capacity. The OM KEEP sentinels preserve ignored array
arguments. SetPrivateDataInterface deliberately stores the supplied application
interface so GetPrivateData preserves its identity.

Review array metadata when changing this description. The SDK annotates
IDXGIDevice::CreateSurface's output as a single pointer, but NumSurfaces is its
actual output array length; the checked description records that override.

Forwarding coverage is not recording coverage. Unknown successful interfaces
are returned unchanged and diagnose an API/IID gap. Calls made through those
native interfaces cannot be tracked, including their subsequent identity
queries. Such sessions cannot produce a complete capture. Public video,
interop, debug, keyed-mutex and vendor interfaces are not all covered here.
Initial state, command recording, hidden GPU state and frame serialization
remain separate pending work.
