# Deferred buffer NO_OVERWRITE capture

Precompiled deferred buffer `NO_OVERWRITE` can now follow a known completed
write in the same command list. The journal owns a copy of the mapped CPU bytes
before application writes and the final bytes before Unmap. Lowering requires
that the first copy exactly equal the list's preceding known buffer contents.
Both copies count against the journal and frame payload budgets. No mapped
pointer enters a command list or frozen frame.

The first successful write remains `WRITE_DISCARD`. CopyResource,
CopySubresourceRegion or Update targeting that buffer invalidates the known
baseline; a new discard is required before accepting another no-overwrite.
This matters even when an application writes the same value as its CPU baseline:
after a GPU write, a byte diff could otherwise omit a real update. Shader-writable
and SO buffer descriptors remain outside this mapping profile. Missing or
different baselines, unsupported descriptors, unpaired maps and budget failures
refuse publication instead of guessing written ranges.

The existing encoder's sparse Map representation receives the verified before
and after bytes. Ordinary unchanged bytes, including same-value application
writes, need no patch when the baseline is proven. Capture owns this history and
dependency check; no Core, Replay or shared-format implementation changed.

Microsoft's [deferred rendering contract](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-render-multi-thread-render)
requires an initial discard before subsequent supported no-overwrite calls.
[Map](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-map)
documents the additional feature checks for dynamic constant/SRV buffers.
The injected constant-buffer fixture checks that feature explicitly. The main
vertex-buffer fixture uses the base vertex-buffer mapping contract.

## Native and dual-player evidence

The public-API-only `FloraDeferredMapScopeProbe --no-overwrite` first records a
discard and a CopySubresourceRegion consuming only a prefix. It then writes the
unconsumed tail through no-overwrite. On WARP, Intel and NVIDIA, the observed CPU
baseline equals the discard payload and the final tail is retained through
reverse and repeated list execution. Whole buffer bytes and the earlier prefix
are checked, with Debug Layer diagnostics rejected.

The independent native/wrapped vertex fixture draws three vertices into the
left half of an 8 × 8 target, updates only the other three vertices, then draws
the right half. A/B/A and B/A/B executions produce exact red/green and blue/white
images, respectively. Both WARP and hardware match the independently constructed
vertex bytes and pixels, preserve Execute restoration and release native objects
before encoding. A later immediate upload is captured as initial data, without
replacing the compiled versions. Nine unsafe-history mutations plus a payload
budget failure reject; the mapping fixture also proves an over-budget baseline
does not read an intentionally inaccessible pointer.

Eight files pass **116** serial dual-player checks: exact mixed-color CPU images,
real execution, event counts, inventory and VS/PS DXBC/disassembly. Removing the
last no-overwrite makes the final right half black. Removing all Draws leaves
the whole image black. The original preview is unchanged in both negative files.
The original-player development runner uses its default adapter; adapter
equivalence and new original-GPA GUI inspection are not claimed.

Independent-package startup injection mode 27 precompiles five dynamic constant
buffer color lists. Five captured frames pass **33** checks against their
per-Present CPU colors in both players. The harness/target use system-only PATH;
actual target modules exclude Qt, Core/Replay, GPA, RenderDoc and Python. Armed
cancellation, busy handling, explicit unsupported-update refusal, recovery,
disconnect and continued target execution pass. Only the capture controller,
runtime, CRT and licenses are shipped, not test binaries.

Both configurations build successfully after the test-helper correction.
Capture-only CTest passes **120/120**. The full application's related deferred,
boundary and capture UI suites pass **20/20**. GPU jobs run serially; this is a
complete capture-only run and a focused full-application run, not a full
270-test application sweep.

## Evidence and remaining scope

[capture-deferred-nooverwrite-baseline.json](capture-deferred-nooverwrite-baseline.json)
records exact regression totals, binaries and raw evidence hashes. Ignored
outputs remain under `artifacts/capture-deferred-nooverwrite-20261014`. Initial
full builds exposed a narrowing enum conversion in the new test-only fault
helper; the explicit UINT conversion fixes it, and the failed logs are retained.

This extends [the discard stage](CAPTURE_DEFERRED_DISCARD.md). Texture mapping,
unknown/GPU-mutated baselines, nested lists, compilation during capture, foreign
immediate execution and hidden states remain refused or unfinished. Wider
constant/index/SRV patterns have not been exhaustively tested. No new GF2 or
golden-frame replay run is claimed here; the existing GF2 acceptance retains its
original runtime identity. Historical Intel/NVIDIA pixel issues stay open and
overall independent capture remains partial.
