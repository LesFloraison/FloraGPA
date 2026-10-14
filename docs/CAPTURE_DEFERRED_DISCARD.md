# Deferred buffer discard execution

Capture now lowers completed dynamic-buffer `WRITE_DISCARD`/`Unmap` pairs from
precompiled command lists at each actual `ExecuteCommandList` position. Each
execution owns another copy of the recorded payload and retains list generation,
execution ordinal and source-call provenance. It does not deduplicate repeated
lists or use the latest immediate-context upload in place of retained data.

The existing deferred journal copies mapped memory before native Unmap and
removes borrowed pointers. This change consumes that immutable history. It
checks the dynamic buffer descriptor, writable CPU access, subresource zero,
zero flags, exact byte count, successful completion and matching Unmap. Open,
overlapping, failed or incomplete mappings refuse. Supported descriptors have
no miscellaneous flags or structure stride and only vertex/index/constant/SRV
bind flags. Texture maps and `NO_OVERWRITE` remain refused; neither written
ranges nor rename dependencies are guessed.

Unmap inherits the matched Map's capture-owned context metadata; it does not
invent a GPU state transition. The encoder already represents ordinary Map and
Unmap records, so no format, Core or Replay code changes. Resource snapshots
still occur at frame start/first execution use, separately from compile-time
write payloads. Capture-only builds remain independent of Qt and replay.

## Native semantics and validation

Microsoft documents that deferred mappings use dynamic resources, starting with
discard within a list; subsequent supported mappings may use no-overwrite.
See [Immediate and Deferred Rendering](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-render-multi-thread-render)
and [Map](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-map).
The native experiment additionally tests the actual resource version visible
outside each execution; documentation alone is not taken as that proof.

`FloraDeferredMapScopeProbe` uses only public D3D11/DXGI calls. Two contexts
compile different four-word discard payloads, then the immediate context writes
a third value. Reverse/repeated list execution recovers each list's bytes in
its CopyResource destination **and** the resource read outside the list.
The entire four-word payload is compared, with both Execute restore flags and
Debug Layer warning/error rejection. WARP, explicitly selected Intel and NVIDIA
are tested separately.

`FloraCaptureDeferredDiscardTests` compares unwrapped and wrapped native
execution on WARP and default hardware. Two lists carry factors/tags `(3,1)`
and `(5,2)` in a dynamic constant buffer. Their noncommutative compute sequence
produces exact uint results 22 for A/B/A and 37 for B/A/B, with three dispatches
and the expected final buffer contents. Graphics converts those results into
independently expected red/green pixels. A later immediate upload `(17,99)` is
captured as initial data but cannot replace the compiled versions. Finish and
Execute restoration, source-call order, native object release and thirteen
malformed-history refusals are checked.

Eight generated files pass **132** serial dual-player checks, including event
counts, exact CPU-expected images, DXBC/assembly and resource inventory. Removing
the final A discard changes A/B/A's result to 37/green; removing Draw leaves
black. Both controls retain the original red preview, proving actual execution.
The original comparison uses the pinned installed GPA player through a separate
development runner; this stage does not claim new original-GPA GUI inspection.

Startup injection mode 26 precompiles five constant-buffer color lists. The
independent capture package produces five files, all matching the per-Present
CPU color in both players (**33** checks). Armed cancellation, busy requests,
an explicit unsupported-update failure, recovery, disconnect and subsequent
target exit pass. The harness/target receive Windows system paths only. Their
actual module inventory excludes Qt, Replay, original GPA, RenderDoc and Python.
The development test binaries are not shipping package contents.

Both build configurations succeed. Capture-only CTest passes **117/117**;
the full application's related deferred, boundary and capture UI suites pass
**17/17**, with GPU work serialized. Replay and shared format were unchanged,
so this stage does not claim another golden-frame replay run.

## Evidence and remaining scope

[capture-deferred-discard-baseline.json](capture-deferred-discard-baseline.json)
records builds, regression totals, binaries and ignored evidence hashes under
`artifacts/capture-deferred-map-scope-20261014`.
The first fixture incorrectly used capture-registry readback for its unwrapped
native oracle; it was corrected to direct D3D11 staging readback. The first
dual-player harness expected the report key `Unmap` instead of the existing
`unmap_observations`; exact pixels had already passed. Both failed logs remain.

The strongest new shader-consumption evidence is dynamic constant buffers.
Broader bind combinations, textures, no-overwrite, nested lists, compilation
during capture, foreign execution threads and hidden predicate/SO/counter state
remain outside this evidence or explicitly refused. The existing GF2 six-frame
acceptance was captured with the prior runtime; no new GF2 claim is made here.
Earlier Intel/NVIDIA pixel issues remain open, and overall delivery is partial.
