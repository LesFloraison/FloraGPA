# Optional native RenderDoc recapture and command provenance

The optional RenderDoc path from the recovered Python implementation is now
available for native CLI replay. It uses RenderDoc's public application API 1.6.0
to capture FloraGPA's own D3D11 device. GPA decoding, resources, experiments and
command execution still come from FloraGPA's native implementation. Normal
replay does not load RenderDoc, and native recapture does not launch Python,
qrenderdoc or RenderDoc's replay API.

```powershell
.\out\FloraGPA-rdc-capture\FloraGPA.Cli.exe replay D:/captures/frame.gpa_frame --renderdoc "C:/Program Files/RenderDoc/renderdoc.dll" --out D:/results/recapture
```

The output directory must be new or empty. `report.json` records the actual
`rdc_capture` path and loaded modules. Existing replay driver, event boundary,
disabled-command and experiment options apply. The RenderDoc DLL is an optional
external installation; no RenderDoc binary is packaged with FloraGPA. The
application API header and MIT license are versioned with the project.

## Capture lifecycle and identity

The selected DLL loads before D3D11 device creation. It must expose application
API 1.6.0 and match the loaded file identity. Capture hotkeys and overlay are
disabled; referenced resources are retained. The capture title identifies
FloraGPA independent replay. Completion requires exactly one new capture and an
existing output file. Exceptions discard an active capture before device cleanup.
RenderDoc remains loaded until process exit because its process hooks and wrapped
D3D objects may outlive a capture.

Original resource objects receive `GPA resource <ID> (type 0x<type>)` names.
Original Draw/Dispatch calls have `GPA <ID>: <Command>` scopes. Clear, Copy,
Resolve, GenerateMips, UpdateSubresource and captured Map/Unmap writes use
`GPA API <ID>: <Command>` scopes. Lazy resource setup and diagnostic/experiment
preparation are outside those scopes. Disabled commands and a selected
before-event boundary do not acquire executed-command markers.

`RdcEvents` ports the exact annotation-based mapping algorithm. Matching action
kinds and structured CPU API calls determine provenance. It retains many native
events belonging to one command while refusing automatic selection when there
is more than one. Duplicate markers are ambiguous; unknown/malformed GPA-like
groups block inheritance from outer scopes. Unmarked helper work remains
unmapped. Traversal has explicit depth/count/name and numeric limits.

## Remaining Pixel History work

This is a prerequisite for Pixel History, not its completed UI. The native
RenderDoc replay-controller adapter, PixelHistory result conversion, structured
CPU-write supplementation and Qt history view still need migration. `RdcEvents`
currently consumes normalized native action/event data and is verified through
a development probe; the production replay-controller consumer is not connected.
The main-window Pixel History tab therefore remains disabled. Python/qrenderdoc
are used only as external development oracles for reopening captures and
comparing this batch's mappings.

The separate RenderDoc-backed pixel/vertex/compute debugging workflows and
counter consumers also remain pending. This batch does not substitute output
image differences for fragment history or change the full migration objective.

## Validation

The Release package is `out/FloraGPA-rdc-capture`. Validation on this host:

- All 43 Release CTest suites passed (`artifacts/ctest-rdc-capture-release.log`),
  including native provenance and the existing 54 Qt UI cases with no skips.
- Packaged recapture passed 99 checks over nine captures: Hardware/WARP command
  and MSAA fixtures, Unicode output paths, disabled/before-event boundaries,
  and GF2/BF1. Captured replay images, dimensions, resource selection and command
  counts match ordinary replay. Every RDC reopened successfully in the external
  reference oracle. Missing/wrong libraries fail; failure during active capture
  discards the capture (`artifacts/rdc-capture-package/validation.json`).
- Native provenance exactly matched the original Python implementation on 700
  generated annotation trees and all nine real captures, including structured
  CPU events (`artifacts/rdc-events-package/validation.json`). The real oracle's
  index is also compared against the normalized development adapter. BF1 has
  5,491 uniquely selectable GPA commands across 77,279 native API events.
- GF2/BF1 golden images and suppressed-draw negative controls all passed in
  the isolated packaged environment (`artifacts/rdc-capture-golden/validation.json`).
- All 18 packaged native replay reports passed the module audit. Qt is local
  to the package; Python/Tk/GPA backends are absent; RenderDoc loads only for
  explicit recapture from the selected external DLL. All three executables match
  the Release build, and the RenderDoc header license is packaged
  (`artifacts/rdc-capture-runtime-audit.json`).

These checks establish this recapture/provenance scope on the current machine,
not complete Pixel History, arbitrary frame compatibility or portability to a
separate clean Windows machine. Development scripts use Python/qrenderdoc as
oracles; the application and its recapture path do not.

Reproduce optional recapture and event-mapping checks:

```powershell
python tools/validate_rdc_capture.py --reference D:/CDXrepo/FloraGPA --exe build/vs2022/Release/FloraGPA.Cli.exe --qt-bin D:/Qt/6.11.2/msvc2022_64/bin --out artifacts/rdc-check
$events = @(Get-ChildItem artifacts/rdc-check -Filter '*-events.json' | ForEach-Object FullName)
python tools/validate_rdc_events.py --reference D:/CDXrepo/FloraGPA --exe build/vs2022/Release/FloraRdcTests.exe --qt-bin D:/Qt/6.11.2/msvc2022_64/bin --out artifacts/rdc-map-check --real $events
```

The recapture check requires the optional installed RenderDoc and its Python
development oracle. It runs the oracle hidden and sequentially, outside the
application runtime. Use fresh output directories. The native `rdc_events` CTest
has no RenderDoc installation dependency.
