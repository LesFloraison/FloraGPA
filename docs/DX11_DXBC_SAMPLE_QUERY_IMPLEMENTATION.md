# Native DXBC sample-query execution

This implementation extends the independent executor with `sampleinfo` and
`samplepos`. It uses the native observations established in
`DX11_SAMPLE_QUERY_RESEARCH.md`, keeping SRV and rasterizer results separate.
It does not complete shader input reconstruction, sampling, derivatives,
fragment history, production debugger integration or standalone acceptance.

## Observation contract

The snapshot scans shader instructions for resource and rasterizer sample
queries. A shader without these queries does not launch query diagnostics.
Snapshots retain query count, position bits, actual resource sample count and
quality, plus rasterizer multisample and forced-count state. Identical SRV
selections share their immutable query observations. Input source identity is
`independent-replay-resource-inputs/6`.

`Replay::observeSampleQueries` uses the replay device and an isolated deferred
command list. SRV queries reference the original read-only view; they never
fetch texels. Rasterizer queries use a private render/depth target with the
observed sample count, quality and format, and a private rasterizer retaining
multisample/forced-count settings. The source target is never drawn into.
The diagnostic writes count and positions to a private structured UAV, with
space for all 32 supported indices and an out-of-range sentinel query. A known
initial marker detects missing diagnostic execution. No original UAV slot must
be free. The command list restores bindings, and predicate isolation prevents
the original predicate from suppressing diagnostic work.

The native count and positions are observed before interpretation; CPU execution
then applies resource swizzles, destination masks and integer/float count mode.
The trace's GPU-assisted flag records the source of the values, not a dispatch
per executed query. Missing snapshots stay unknown, verified unbound SRVs return
known zeroes, and an unknown sample index leaves position xy unknown while zw
remain known zero. Out-of-range positions are zero according to the instruction
contract. Resource and rasterizer observations are never interchanged, including
the local Intel center-quality discrepancy recorded by the independent probe.

No-RTV/no-DSV rasterizer snapshots are explicitly unobserved. Independent
fresh-device and prior-draw sequences found order-dependent count/position values
on the local Intel and NVIDIA devices. The helper does not run a substitute
target or normalize those values. The interpreter keeps query-dependent
components unknown, preserving only the instruction's specified zero components.
The matrix separately counts these unknown-propagation checks; they are not
verified no-target query semantics and do not close that P6 acceptance item.
The forced-count/no-target combination remains in the same unresolved category.

The input probe's version-5 report includes query observations and availability.
Native parity checks reobserve them on both the original and reference replay
before comparing output bytes. An unobserved query prevents a parity claim.

Compiler-generated SRV `samplepos` instructions carry an additional zero word
after the scalar index, including immediate, constant-buffer and thread-ID forms.
The original rasterizer probe has no such tail. The shared parser accepts the
observed zero tail after a complete scalar SRV index; other trailing data is
rejected. Checkpoint, output-log, system-ID and hull
rewrites preserve the instruction's original encoding. Format tests compile PS
and VS shaders, reject malformed/truncated variants, and disassemble transformed
programs. No interpreter source or token parser was copied from RenderDoc.

Observation checks cancellation before submission, while polling and after
readback. The default deadline is five seconds, limited to 60 seconds. The
enclosing Worker remains responsible for terminating a blocked driver call.
No persistent cross-frame query cache or adapter fallback is introduced.

## Validation status

The local sequence investigation used Intel Graphics driver `32.0.101.8826`
and NVIDIA GeForce RTX 5090 Laptop GPU driver `32.0.16.1742`, alongside WARP.
Adapter identities and raw observations remain pinned in the sequence ledger.
Deliberately corrupting a sequence's raw output causes its validator to fail
without creating a success report.

The final query suite passes 375 tests with zero failures/skips in 106.805 seconds:
288 hardware matrix cases verify query results, 72 separately verify unknown
propagation for the unresolved no-target case, six verify cancellation/state
recovery, six compare compiled CS queries with independent native dispatches,
and one validates missing/malformed inputs, plus suite setup/cleanup. The matrix
covers WARP/NVIDIA/Intel, counts 1/2/4/8, quality 0/standard/center, 2D/array SRVs,
bound rasterizers/null inputs and both multisample-enable states. Hand-encoded
VS/PS/CS programs verify integer/float counts, swizzles, masks and index bounds;
compiler-generated PS programs exercise actual instruction encodings.

The six CS subprocess comparisons verify one four-thread dispatch for 2D and
array MS SRVs on each device. Both the original and reference replay reobserve
the sample-query inputs. Each 64-byte output matches exactly and original
observed inputs remain unchanged. This is stronger evidence than interpreting
the observation table alone, but does not establish all stages and rasterizer
configurations on hardware.

State tests cancel after command submission, verify bindings, preserve a hidden
UAV counter initialized to 17, bypass and restore a false predicate, and compare
original MSAA samples before/after successful retry. No original target is
written. The expanded format suite passes 16 tests, including constant-buffer
and shader-input indices and malformed zero-tail variants.

The initial product build exposed an incorrect fixture field name. Earlier runs
failed on instruction encoding, invalid no-target assumptions and use of the
non-MSAA readback API in a state-test fixture. The first CS comparison exposed
the overly narrow constant-only padding rule. Diagnostic loop/unroll experiments
and all failed logs are retained outside Git; no earlier golden was changed.
Adjacent regression validation is recorded in the accompanying ledger.
The latest nine-suite total is 1,639 passes, zero failures and zero skips; GUI,
CLI and Worker Release builds pass. Negative evidence checks reject an altered
native-parity result and a no-target record incorrectly labeled as verified.

Remaining scope includes no-target rasterizer semantics, forced sample counts,
depth-only targets, broader format/count coverage, native VS query comparisons,
shader input reconstruction, production debugger integration, and P5/P7/P8.
These results are a bounded implementation checkpoint, not release acceptance.

## Reproduction

Build `FloraDxbcSampleQueryTests` and `FloraDxbcFormatTests` in Release. The query
target builds its independent D3D11 probe and native input-comparison subprocess.
Run GPU tests serially:

```powershell
ctest --test-dir build/vs2022 -C Release -R "^dxbc_(sample_query|format)$" --output-on-failure -j 1
python tools/summarize_dxbc_sample_queries.py --log artifacts/dx11-native/sample-query-latest-sample-query.txt --regression artifacts/dx11-native/sample-query-format-expanded.txt --source src/replay/SampleQueries.cpp --source src/application/DxbcExecution.cpp --out artifacts/dx11-native/sample-query-checkpoint.json
```

The test log defaults to the build directory; preserve it under the chosen
artifact name before running the summarizer. Append earlier `--log` arguments
to retain failed iterations, and append `--regression`, `--source` and `--build`
arguments for the complete evidence set. The committed ledger pins that set and
keeps `sample_query_semantics_complete` and `complete_release_acceptance` false.
Raw probe output, temporary GPA fixtures and binaries are not committed.
