# Measured raster inputs for private output merging

## Missing connection

The private output merger needs observed depth, face orientation and coverage.
The pixel-input collector previously retained quad coverage but discarded the
selected invocation's `SV_Position.z` and `SV_IsFrontFace`. Vertex values or a
default face cannot replace those GPU observations.

`dxbc-pixel-raster-inputs/2` retains the existing 144-byte record size. Its final
four words now contain layer, selected raster sample, raw `SV_Position.z` bits
and a normalized front-face Boolean. Version 1 reserved the last two words.
The eight existing transported vectors and the atomic allocation header are
unchanged. Historical version-1 reports remain readable by the offline validator;
the current invocation join requires version 2 and does not reinterpret old
reserved zeros as measured depth or orientation.

The record shader observes the added system values even when the application
shader does not declare them. When the selected application input is a position
register, the helper adds the needed position components to its declaration and
signature while preserving the original register validity mask. It does not
claim that unused application input components became known debug registers.

The per-register join compares all identity words, including depth and face,
before constructing `input_provenance.selected_raster`. That object contains
`depth_bits`, `front_face`, `coverage_mask`, `sample` and `lane`. It travels through
the existing invocation, shader-output and primitive-history paths, including
equivalent sample observations. Mixed identities and malformed face values are
rejected. Original fragment eligibility stays unknown.

These are private pre-depth shader-input observations. The change does not prove
original fragment eligibility, early-depth behavior, writable shader scheduling,
or complete output-merger equivalence. In particular the measured depth is
precisely `SV_Position.z`; it is not a claim that every fixed-function depth bias,
conservative depth declaration or shader-written depth can be ignored. State
snapshot extraction and full merger/history integration remain separate work.

## Packed system semantics defect

The initial independent matrix passed 78 Qt checks and failed 20 sample-frequency
cases. The helper remapper assumed each system semantic occupied a separate
compiled input register. The compiled template actually declares:

```text
dcl_input_ps_sgv constant v2.x, sampleIndex
dcl_input_ps_sgv constant v2.y, is_front_face
```

The old remapping overwrote one register mapping with the other. The correction
preserves a shared temporary for components in the same compiled register and
keys helper declaration mappings by both source register and system semantic.
It still handles a system component supplied by the selected original register.
The retained HLSL, DXBC and disassembly demonstrate this packing independently
of the test results. Failure logs remain in the evidence; no assertion or skip
was weakened.

## Independent GPU probe

The new 96-case matrix uses WARP, NVIDIA and Intel; 1x/4x MSAA; both front-face
conventions; constant and sloped depth with a nondefault viewport depth range;
and application shaders with no input, position-only input, front-face input,
or sample-frequency inputs. A normal D3D11 draw writes raw depth, normalized
orientation, coverage and sample identity to an integer RTV. The test reads that
output and compares it to the separately generated private observation and join.
The normal draw does not use the input-transport shader or geometry bridge.

The corrected focused run passes 98 Qt checks (96 cases plus lifecycle), with
zero failures or skips, in 22.487 seconds. The validator rejects 960 modified
reports and checks the complete matrix, adapter, shader, sample, scope and raw
depth/front/coverage comparison. The fixed-depth probe also checks its independent
numeric expectation. Unit controls reject version-1 input in the new join,
inconsistent depth/face across passes and a non-Boolean face value.

## Regression and real-frame results

Five CTest entries pass 1,208 Qt checks with zero failures or skips in 309.40
seconds: pixel trace (24), pixel inputs (221), checkpoint inputs (635), private
geometry replay (203) and candidate discovery (125). The 102 saved input-capture
cases also retain exact agreement between the first eight record vectors and
the independent full MRT observations. GUI, CLI, Worker and the real-history
probe build successfully.

The complete history validator checks 72 owned history reports and nine real
capture/adapter combinations. All GF2 event 113, BF1 event 2010 and Helldivers
event 200 selections pass on WARP, NVIDIA and Intel. They retain 1, 10 and 1
private shader invocations respectively, with 100, 4,080 and 1,316 total quad
execution steps. Final selected storage matches normal replay byte-for-byte,
original storage is restored, and no unattributed changes are reported for
these selections. The history validator rejects 864 corrupt joins, including
malformed measured raster fields. Combined with the new probe validator, this
gives 1,824 corruption controls.

| Capture selection | WARP elapsed ms | NVIDIA elapsed ms | Intel elapsed ms |
| --- | ---: | ---: | ---: |
| GF2, event 113, `(799,480)` | 458 | 837 | 953 |
| BF1, event 2010, `(751,433)` | 5,564 | 8,104 | 8,141 |
| Helldivers, event 200, `(168,183)` | 2,710 | 7,862 | 3,333 |

These are bounded end-to-end probe observations on this host, including setup
and reference replay. They are not a full performance regression assessment or
proof of original per-fragment behavior. No RenderDoc comparison is required by
these probes.

## Reproduction

Build the relevant Release targets and run GPU tests serially. Use new output
paths; the evidence writers reject existing files.

```powershell
$env:FLORA_RASTER_MERGER_INPUTS_EVIDENCE = "$PWD/artifacts/dx11-native/raster-merger-rerun"
$env:FLORA_PIXEL_INPUT_EVIDENCE = "$PWD/artifacts/dx11-native/raster-input-regression-rerun"
ctest --test-dir build/vs2022 -C Release -R '^(dxbc_pixel_(inputs|trace)|native_pixel_(geometry_replay|checkpoint_inputs|candidates))$' --parallel 1 --output-on-failure
python tools/validate_raster_merger_inputs.py "$env:FLORA_RASTER_MERGER_INPUTS_EVIDENCE" --out raster-merger-validation.json
python tools/probe_pixel_input_template.py --out template-rerun
```

The template probe uses compiler flags 2048, matching normal optimized
`compileHlsl`, and saves source, DXBC, declarations and hashes. An earlier flags-0
diagnostic is retained separately and shows the same packed system inputs.

For real frames, run `FloraNativePixelDrawHistoryProbe FRAME EVENT RESOURCE X Y
0 0 0 0 VENDOR NEW_DIRECTORY outputs`; resource 80 selects GF2's target and
resource 0 discovers RTV zero for the other two frames. Use vendor 0, 4318 or
32902. The existing `validate_native_pixel_history_shader.py` accepts the saved
geometry/checkpoint directories and a real-report prefix with `-gf2-VENDOR`,
`-bf1-VENDOR` and `-hd-VENDOR` suffixes.

The adjacent ledger pins sources, binaries, raw reports, validation and logs.
Original GPU oracle output is retained in each probe report; historical captures
and large binary evidence remain outside Git. This work does not complete P5,
P6, original GPA debugger-engine recovery, or the P7/P8 dependency-removal gates.
