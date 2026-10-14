# Frame-end MSAA diagnostics

The current six-file GF2 gate still fails as recorded in
[the current-runtime validation](CAPTURE_GF2_RESOLVE_CURRENT.md). Those files
retain initial MSAA samples and the final backbuffer, but not the application's
intermediate frame-end sample planes. Replay-only probes cannot supply that
missing reference. This change adds an explicitly separate development runtime
to collect it on future diagnostic captures. It is not a correctness fix.

## Runtime boundary

`FloraCaptureDiagnosticRuntime` is an `EXCLUDE_FROM_ALL` target. Build it explicitly
with `cmake --build --preset capture-only-release --target FloraCaptureDiagnosticRuntime`.
For development injection, copy it into a new isolated directory as
`FloraCaptureRuntime.dll`, beside `FloraCapture.exe`. Its IPC responses identify
`development_runtime: final_msaa_readbacks`. Never substitute it into an existing
evidence package or a release package.

The diagnostic DLL compiles its own session object with a diagnostic definition;
the ordinary session object in the capture archive is not selected by the linker.
The production DLL compiles without that definition. The normal packaging script
selects the production DLL explicitly. Both targets undergo the same transitive
capture dependency audit. Capture still does not import Core, Replay, Qt,
RenderDoc or an original GPA library.

After recording finishes, on the application's capture-owner thread, the
diagnostic runtime reads retained MSAA color textures through the existing
capture-native sample readback primitive. It preserves each layer/sample as raw
integer storage bits. It does not resolve samples, use previews as reference,
modify the captured initial data, or assign replay/encoder IDs. Depth/stencil is
outside this diagnostic export's scope.

The side directory is `<requested-frame>.gpa_frame.final-msaa`, with runtime
resource IDs, shape, quality, row widths and plane files in `manifest.json`.
Existing directories are refused. Budget, cancellation, missing leases and I/O
failures stop this diagnostic capture; partial diagnostic output stays separate
from GPA publication. `readbacks_complete` reports only these reads and
`acceptance_established` remains false. A subsequent cancellation/publication
failure may leave complete diagnostic reads without a published frame. Consumers
must pair them with a Completed controller result and file hashes.

Before freezing the frame, the runtime reads the backbuffer again and requires
byte equality with its pre-diagnostic snapshot. This detects immediate output
mutation; it does not prove that synchronization cannot perturb future frames.
The extra copies, compute work, stalls and disk writes are intentional diagnostic
overhead and are not enabled in production.

## Verified behavior

The native tests use independent CPU expected values for a two-layer RGBA16
texture at 2x and 4x MSAA, including raw bit patterns and in-frame sample changes.
WARP and hardware each verify 12 final planes, preserving distinct initial data.
They reject budget exhaustion, cancellation, missing leases, no color resources
and an existing evidence directory (10 refusals per backend). A repeated draw
after inspection confirms application bindings and output, internal reads do not
enter the captured command list, native leases are released, and the D3D debug
layer reports no warnings/errors.

The final diagnostic DLL also passes two startup injection paths with eight
successful captures, recovery, cancellation and disconnect. All 48 exported
RGBA8 planes match the independent live-target CPU oracle. The eight files and
four initialization/application-draw removal controls pass both players: 12 files,
279 checks. The release package remains separate from both diagnostic packages;
the first diagnostic package is retained because its initial link used the
production export-library name and produced a linker warning. The final package
uses its own export definition and builds without that warning.

## GF2 observation

One additional diagnostic session uses the equipment scene, Intel Iris Xe,
`-force-d3d11 -force-device-index 1`, and unchanged game quality. The controller
is launched with a Windows-system-only PATH. File
`frame-13714340-3f05-473a-96db-1d0728dd38be.gpa_frame` is captured at Present 1108.
The session continues to Present 1244, disconnects and exits normally before any GPU
replay starts. The status identifies the development DLL and reports no object
interface gaps or lost diagnostics.

Three retained textures (3456 x 1944, 2x MSAA, one layer) yield six planes totaling
268,738,560 bytes. Their capture runtime IDs map to wire resource IDs solely in
the external analysis: each encoder-generated initialization annotation contains
one CopyResource destination. No numeric-ID equality is assumed.

| Runtime resource | Wire resource | Storage family | Exact sample comparisons |
| ---: | ---: | --- | ---: |
| 3244 | 413 | RGBA16 | 4 / 4 |
| 3251 | 417 | RGBA8 | 4 / 4 |
| 3788 | 623 | RGBA16 | 4 / 4 |

Each sample is exported in two fresh Flora replay processes at the final Present
event 6419, using integer typed views and no display conversion. All 12 exports
match the application's corresponding raw bytes exactly. Separately, three
fresh-process executions in each player match the captured final backbuffer
exactly; the original-player audit verifies Intel execution and normal close.

This is one diagnostic observation, not a replacement acceptance file. It does
not reproduce or explain the two discrepant files in the formal six-frame set.
It does not establish full-frame state equivalence, detailed original-player
inspection of this new file, or absence of observation effects. The existing
failure thresholds and retained evidence are unchanged.

## Evidence and regression

Raw outputs, scripts, binaries and hashes are outside Git at
`C:/Users/Myrrr/FloraGPA-evidence/capture-final-msaa-20261014`.
The checked-in [baseline](capture-final-msaa-baseline.json) binds this stage to
those identities and records the completed regression counts. All 161
capture-only CTests pass, including format and boundary checks. The capture-only
build uses no Qt or replay target; full and replay-only configuration also pass
their source/target boundary checks. GPU validation is serial. Replay and format
implementation are unchanged, so this stage does not rerun the replay golden
suite or claim a new replay baseline.
