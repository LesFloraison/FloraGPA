# M2: checked buffer copy commands

> Historical batch record. Results, package paths, module counts and remaining-work
> statements below describe this batch. See [current status](CURRENT_STATUS.md)
> for present support and [the documentation index](README.md) for navigation.
> `artifacts/`, `out/`, `build/` and external-reference paths identify local
> evidence not distributed with a clone; original JSON baselines are preserved.

`CopyResource` (`0x3e`), `CopySubresourceRegion` (`0x40`) and
`CopyStructureCount` (`0x3f`) now share a checked decoder and resource validator
between offline preflight and production replay. Previously only record shape
and context were checked before passing copy parameters to void-returning D3D11
calls. An invalid operation could reach the driver without a useful capture-level
diagnostic. The new failures identify the event, destination and source.

## Implemented checks

All three layouts check the common header, exact length, optional box flag and
immediate context. Linked execution remains unsupported. Operands must be real
buffer/texture resources; capture-end reference pixels are not GPU copy operands.
Copies to immutable destinations are rejected. Resource copies require matching
resource kinds; CopyResource requires different identities. The additional buffer checks are:

| Operation | Checks |
|---|---|
| CopyResource | Nonzero and equal ByteWidth |
| CopySubresourceRegion | Zero subresource indices, one-dimensional byte coordinates, source bounds, destination bounds using widened arithmetic |
| CopyStructureCount | Four-byte destination alignment and room, structured APPEND/COUNTER UAV, UAV bind flag, stride and view element range |

Ordinary buffer region copies may use unaligned byte offsets. Counter copies have
the separate four-byte alignment requirement. A null region box means the whole
source buffer; it is not treated as an empty box. Nonempty same-buffer region
copies remain explicitly outside this verified scope.

Bounded empty boxes, including an inverted axis, have no write effect. Production
records `empty_copy_regions` and avoids submitting an unnecessary GPU copy while
preserving the API event and its normal command count. Buffer coordinates are
still checked: a UINT_MAX destination offset is rejected even with an empty box.
The first hardware test of that malformed combination reached the driver and
reported device removal. The failed log is retained; it is not counted as a
successful empty-box test. Normal bounded empty boxes are also covered by an
unmodified original capture. No captured bytes or shader are patched.

The contracts used are Microsoft's
[CopyResource](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copyresource),
[CopySubresourceRegion](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copysubresourceregion)
and [CopyStructureCount](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copystructurecount).

## Independent original captures

The opt-in development target `FloraBufferCopyProbe` makes six native/GPA-shim
runs of twelve frames each. It initializes a destination explicitly each frame,
performs the selected copy, verifies all 64 destination bytes against an
independent CPU pattern, and renders a buffer-dependent green image. A magenta
preclear and shader mismatch output detect a missing copy or draw. The capture
files are original `CaptureNextFrame` outputs and stay outside Git.

| Mode | Operation |
|---|---|
| whole | Copy all 64 bytes |
| byte_region | Copy 11 bytes from source offset 1 to destination offset 3 |
| null_region | Copy the whole source via a null CopySubresourceRegion box |
| empty_region | Bounded empty box, destination remains zero |
| append_count | Reset APPEND count explicitly to 9; copy it to the last four bytes |
| structured_count | Reset COUNTER count explicitly to 9; copy it to offset zero |

The native program's reported byte/image checks are not fabricated HRESULTs:
the three copy APIs return no result. A failure in byte readback or rendering
terminates the producer and retains its error. C++ tests additionally inspect
the destination immediately after the first buffer copy and after complete
replay, on hardware and WARP. Negative fixtures cover truncation, trailing bytes,
linked records, invalid flags, absent/wrong references, immutable destinations,
size mismatches, invalid subresources, coordinates, alignment and counter views.

```powershell
cmake --build --preset release --target FloraBufferCopyProbe FloraCopyCommandTests
python tools/capture_copies.py --producer build/vs2022/Release/FloraBufferCopyProbe.exe --out artifacts/new-copy-originals
$env:FLORA_COPY_CAPTURES = "$PWD/artifacts/new-copy-originals"
ctest --preset release -R '^copy_commands$' --output-on-failure
```

## Remaining boundaries

This is not full texture-copy validation. Texture format compatibility (including
block reinterpretation), mip/layer/volume regions, MSAA/depth restrictions and
resolve semantics remain to be audited. Preflight emits
`texture_copy_validation_partial` for texture copy records; these are not
misclassified as fully validated buffer transfers. Their existing execution path
is retained behind the added general reference/usage checks.

The validator checks whether a UAV can carry a hidden counter, not whether every
capture saved its frame-initial counter value. The producer proves explicit
in-frame resets only. Missing initial content/counters, cross-device identity
aliases and outstanding original CPU map lifetimes remain separate work. Nothing
here fills unknown bytes or counters with an invented value. Clear/Resolve and
other context interface layouts are not certified by this batch. M2 and M3–M6
remain incomplete; module migration totals remain unchanged.

## Final acceptance results

Original files are registered in [copy-corpus.json](copy-corpus.json). Retained
result paths and source/binary hashes are pinned in
[buffer-copy-baseline.json](buffer-copy-baseline.json). The final package is
`out/FloraGPA-copy-final-20261003/`; GPU checks ran serially.

| Check | Result |
|---|---|
| Historical 91 files and prior 15 Present/buffer/pipeline files | 212 independent runs; all 105 prior stable image hashes unchanged; Helldivers retains documented variability |
| Six new original copy files | 12 independent and 12 original-player runs; all exact green-image matches |
| Native producer and GPA-shim producer | 144 frame checks; destination bytes and buffer-dependent image verified |
| C++ original boundary checks | All six copies on hardware and WARP; 64 destination bytes checked immediately after the copy and at frame end |
| Runtime dependencies | All 224 registered independent runs pass module audits excluding GPA/Python/RenderDoc |
| Corpus controls | 27 passed; the 12 new copy controls include ten magenta results after disabling necessary writes and two unchanged green results for the empty copy |
| Packaged goldens | GF2/BF1 and both draw-disabled negative controls passed using only system runtime paths |
| CTest | Seven existing suites passed; corrected copy suite passed all seven Qt cases; 55 main UI cases passed with zero skips |
| Development validator | Ten CPU tests passed |
| Malformed counter offset | Prior preflight had zero errors; final preflight blocks event 100, destination 2, source UAV 8 before GPU execution |

The first final-build CTest run failed only in the newly added texture-scope test:
the fixture attempted to overwrite its still-mapped file. It now writes a separate
reference-image file, and the complete copy suite passes. This test-only correction
did not change the production binaries. Both the failed run and corrected run are
retained in `artifacts/ctest-m2-copy-final.log` and
`artifacts/ctest-m2-copy-test-correction.log`; they are not reported as an initially
clean eight-suite run. The earlier malformed-empty-box driver failure is retained
in `artifacts/ctest-m2-copy-diagnostic.log`.

Original-player adapter selection remains unidentified. Exact image agreement
does not establish matched-device/driver equivalence. Original captures establish
bounded equal-edge empty-box behavior; inverted empty boxes have synthetic test
coverage only. These results do not certify full texture copy/resolve semantics,
every interface layout, or frame-initial hidden counters.
