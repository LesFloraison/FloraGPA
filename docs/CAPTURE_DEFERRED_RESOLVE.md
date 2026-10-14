# Deferred DX11 Resolve capture

Capture records `ResolveSubresource` in immutable deferred journals and emits
the existing legacy category 7/type `0x42` record at immediate submission.
Repeated list executions retain their order and provenance. The runtime checks
resource leases, descriptors, selected subresources, typed format and native
resolve capability before initial GPU readback. The CPU encoder independently
checks the resource graph before publishing. Capture adds no Replay/Core, Qt or
original-GPA dependency; production Replay and Format sources are unchanged.

The admitted profile is the existing RGBA8_UNORM/RGBA16_FLOAT, 2x/4x quality-zero
source and single-sample DEFAULT destination, including corresponding typeless
storage, array layers and destination mips. Predicate, additional format/quality,
unverified storage and other MSAA mutation restrictions remain explicit failures.
Deferred Resolve currently retains the conservative deferred pipeline snapshot;
unrelated unsupported bound state can therefore reject the list. This is not a
claim of arbitrary deferred-state coverage.

The implementation follows Microsoft's [Resolve resource rules](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-resolvesubresource)
and [command-list execution/state restoration rules](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-executecommandlist).

## Native and dual-player evidence

`FloraCaptureDeferredResolveTests` compiles two lists on one worker and joins it
before submission. A resolves two layers into separate destination mips; B draws
into half of one layer with alternating samples enabled. Source GPU data is
initialized **after** list compilation. A/B/A executes with TRUE/FALSE/TRUE state
restoration, checked through native shader getters. The captured initial samples
and untouched destination mip bytes must remain unchanged across all submissions.

WARP and hardware each cover eight combinations of format, sample count and
typed/typeless pairing. Each releases all native owners before encoding, reports
zero Debug Layer warnings/errors and passes 72 malformed-encoder refusals.
Each also passes 48 submission refusals using altered immutable journal metadata
(bad subresource, format, predicate and extra arguments) or byte/plane budgets.
Those refusals leave the valid native command list forwarding and producing the
same destination bytes. They are not tests issuing those invalid arguments to D3D.

The validator executes 64 files in FloraGPA and the pinned original GPA player;
all 1,510 checks pass. Both players agree exactly with independent CPU output,
including the high and low bytes of RGBA16 values. Event counts, Resolve argument
order, shader DXBC/disassembly and module checks pass. Removing all resolves,
the modifying Draw or only the last A changes executed output while preserving
the captured reference image. Container loading or preview comparison alone
does not satisfy these checks.

## Startup injection and independent package

Native target modes 42/43 use direct D3D11 device/swap-chain creation and DXGI
factory/flip creation respectively, both through Present1. The target contains
no capture model, encoder or Replay link. On each frame a joined worker compiles
two sample-masked Draws and two resolves, then the immediate context executes
that same list with TRUE and FALSE restoration. An ordinary-texture display Draw
exposes the resolved bytes. Inputs use exactly representable integer averages;
the CPU comparison rule is fixed before execution.

The packaged controller/runtime runs with only Windows system directories in
PATH. Two launches produce eight captures. Busy/cancel handling, 15 malformed
IPC identities, deliberate Update failure without publication, subsequent
successful capture, armed disconnect, native object identity and final exact
CPU pixels pass. Both runs require explicit swap-chain selection because the
foreground choice is ambiguous; automatic selection is not claimed.

Eight captures plus six controls (removing initial-sample helpers, application
Draws or Resolve records) pass all 323 dual-player checks. Both source samples
and Resolve execution affect output. Module inventories exclude Qt, Replay/Core,
GPA, RenderDoc and Python from the captured target.

The preceding deferred-MSAA package explicitly rejects the same new mode 42 at
`ExecuteCommandList`, citing missing Resolve compilation tracking. It publishes
no frame or partial file, disconnects and exits with exact native CPU output.
This separates the new capability from a fixture that was already accepted.

## Regression and limits

Both capture-only and full application Release builds pass. All 159 capture-only
CTests and 80 relevant full-application CTests pass, including the Qt capture UI.
The three new GPU tests also enforce `RUN_SERIAL`. Source/binary hashes and the
raw evidence manifest are recorded in
[the baseline](capture-deferred-resolve-baseline.json). Production replay binary
SHA-256 remains `215ebafed657953f0671c32a0948e12c8c965437a3eaa33e6d38aedcff5e2ddc`;
no new golden-frame run is claimed. All GPU checks run serially.

Raw evidence is retained outside Git at
`C:/Users/Myrrr/FloraGPA-evidence/capture-deferred-resolve-20261014`.
This stage does not repeat the GF2 six-frame gate or original GPA detailed GUI
inspection. Earlier accepted files retain their original runtime identities;
the newer GF2 candidate's pixel variation and GUI inspection remain unresolved.
General delivery remains incomplete.
