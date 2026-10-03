# Resource LOD and shader SRV declarations

This batch follows `57c4d58`. It addresses false missing-LOD diagnoses for
bound textures that the active shader does not declare. The source producer is
`tools/native/min_lod_usage_probe.cpp`; original captures and native/injected
producer results are under `artifacts/m2-minlod-usage/` and stay outside Git.

## Original evidence

All ten modes create a four-mip RESOURCE_CLAMP texture, set its minimum LOD to
one before capture, and deliberately save no setter or getter inside the frame.
Mip colors are red, green, blue and white. Each native and shim-injected run
checks twelve frames against CPU-generated 8×8 RGBA pixels. Shader bytecode and
Microsoft disassembly are retained alongside the producer output.

| Mode | Captured workload | Expected output | Before this change |
|---|---|---|---|
| 0 | Constant PS with clamp SRV at t0 | Blue | Replay correct; preflight false error |
| 1 | Constant PS with clamp SRV at t1 | Blue | Replay correct; preflight false error |
| 2 | Constant PS, clamp SRV bound to unused VS t0 | Blue | Replay correct; preflight false error |
| 3 | Reflection-stripped constant PS, clamp SRV t0 | Blue | Runtime and preflight false rejection |
| 4 | Reflection-stripped PS samples clamp t0 | Green | Correct missing-state rejection |
| 5 | PS samples clamp t0 | Green | Correct missing-state rejection |
| 6 | Null PS, clamp SRV t0 | Black | Already correct; boundary control |
| 7 | PS samples ordinary t0; unused clamp SRV t1 | Red | Replay correct; preflight false error |
| 8 | Constant PS first, then PS samples clamp t0 | Green | Rejects correctly, but also flags the first draw incorrectly |
| 9 | Samples ordinary t0, then constant PS with clamp t0 | Blue | Replay correct; preflight false error |

The previous package completed six files with correct pixels and rejected four.
Its preflight reported six false access sites across modes 0, 1, 2, 7, 8 and 9,
plus the unused stripped-shader site in mode 3. Modes 4, 5 and 8 really depend
on missing initial state and must continue to reject, at the actual sampling draw.

The original player completed twenty runs. Seven files matched the producer;
the three sampling files returned mip-0 red instead of producer green. Original
adapter/configuration identity remains unproven. These are observed differences,
not a general assertion about GPA UI playback. Raw evidence is
`artifacts/m2-minlod-usage-before/validation.json`.

## Implementation

`shaderSrvDeclarations` reads checked SHDR/SHEX program tokens for SM4.0, SM4.1
and SM5.0. Typed, raw and structured SRV declarations carry immediate t-register
indices. Those executable declarations survive removal of reflection data. The
encoding follows Microsoft's [tokenized program format](https://github.com/microsoft/DirectX-Headers/blob/main/include/directx/D3D12TokenizedProgramFormat.hpp);
a local source snapshot is retained in `artifacts/minlod-usage-token-format.hpp`.
This is declaration analysis, not general bytecode validation or dynamic branch
analysis. Native shader creation remains responsible for executable validation.

Offline LOD auditing intersects saved SRV bindings with the saved program's
declared slots. Runtime auditing queries the actual native shader and native
views, then uses declarations cached against the COM identity of that created
shader. Shader replacements therefore change the runtime access decision without
rewriting the captured preflight findings. No texture content or initial LOD is
invented, and no shader is changed automatically.

Unknown shader models, absent executable programs and interface programs retain
all slots conservatively. Invalid declaration lengths, operand forms and slot
indices produce explicit errors. Empty shader stages have no SRV accesses.
UAV/RTV/DSV guards remain conservative and are outside this batch's acceptance;
so are class resource offsets, branch-dependent non-use, and nonzero-LOD full
texture storage export. Production captured command-list replay remains pending.

## Verification gates

The regression adds hardware and WARP rows for every original capture. Positive
rows require both zero preflight errors and exact producer pixels across repeats;
negative rows require one error at the actual sampling draw and runtime rejection.
Shader replacement tests change an unused stripped program into a sampling
program and vice versa. Parser negatives cover every truncated container prefix,
illegal slots, extended/wrong operands, non-immediate indices, invalid declaration
lengths and duplicate executable chunks. Unknown-model/interface controls retain
every slot.

All 37 related CTest suites pass. The LOD suite has 51 passing cases with no
skips; Qt/Worker has 56 passing cases with no skips. Declaration tests also use
an independently compiled compute program with typed, raw and structured SRVs
at slots 3, 5 and 127, with and without reflection. Those CPU parser tests do not
establish complete captured compute-LOD coverage.

Build the development producer with the `FloraMinLodUsageProbe` target. Its
arguments are `<new-output-dir> <mode>` for a native run, or
`<new-output-dir> <mode> <capture-path> <shimloader64.dll>` with
`GPA_LOCAL_INJECT=true` for original capture. Use a separate directory for each
run. `FLORA_MINLOD_USAGE` selects the resulting numbered corpus directories for
the `resource_lod` CTest; the earlier fixtures use `FLORA_MINLOD_CAPTURES` and
`FLORA_MINLOD_BOUNDARIES`. GPU tests must run serially. Captures, the installed
GPA DLL and producer output are development evidence, not runtime dependencies.

The [acceptance baseline](min-lod-usage-baseline.json) pins 332 enrolled files:
328 positive and four missing-state rejections. The 656 ordinary positive runs
include 327 stable files and known-variable Helldivers; all 320 previously stable
hashes are unchanged. There are 353 diagnostic controls and 208 resource exports
(204 strict byte goldens, four Helldivers boundary diagnostics). Four separate
golden/negative checks also pass. New positives have zero preflight errors and
exact producer image hashes. New sampling negatives identify resource 23 at
event 22, or event 33 for late sampling; the previous control remains 22 / 23.

The final package is out/FloraGPA-minlod-usage-20261004/, with all five binaries
matching the final Release build. Ordinary successful replay dependency audits
pass without GPA/Python. Final reports use artifacts/m2-minlod-usage-regression-*
and artifacts/m2-minlod-usage-final-*; per-suite results are archived in
artifacts/m2-minlod-usage-qt-results/. The mixed positive/negative
[usage corpus](min-lod-usage-corpus.json) intentionally makes the generic runner
return nonzero for three required rejections; acceptance checks each failure
location and never treats arbitrary failure as a passing negative.

M2 remains incomplete. Continue auxiliary/interface and remaining resource access
audits; captured command lists, broader recovery, release and advanced analyzer
work retain the M3-M6 gates.
