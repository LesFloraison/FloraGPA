# Native VS typed buffer execution

Date: 2026-10-24. This P6 checkpoint extends the independent executor's typed
buffer SRV loads to the vertex stage. It does not replace production debugger
actions or establish vertex invocation extraction from real captures.

## Native stage and storage

The executor now passes the executing shader stage through typed memory access.
A VS `ld` uses a project-generated VS with the original literal/register index
form, resource swizzle and destination mask. A stream-output shader records the
raw result on the same isolated adapter and feature level as the replay device.
It does not substitute a CS operation for a VS operation. The shared helper's
backend identity is `dxbc-gpu/4`; shader-cache keys include the generated program.

The helper copies complete backing storage into a private immutable native buffer,
then recreates the typed SRV's format, first element and element count. Adapter
format support is checked before execution. The reference context is never used
by the helper. Existing bounded readback polling, cancellation, failure poisoning
and shader-cache limits apply. Private resources are released after each operation;
full-buffer upload cost remains a performance limitation requiring later profiling.

The CPU retains control flow, register state and validity. Loaded component
validity follows the source byte ranges and resource swizzle, while untouched
destination components retain their prior bits and validity. Missing bindings
produce unknown values; verified null views return known zero. Reversible register
changes remain available for stepping backward. This checkpoint adds no writes to
the private input resource or the original native buffer.

The [Microsoft `ld` instruction contract](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/ld--sm4---asm-)
defines VS support, typed component defaults, unsigned buffer indices and original
32-bit floating-point bit preservation. These are checked with an independently
compiled native VS, not inferred from the preceding CS results.

## Probe coverage

`FloraDxbcTypedVertexTests` compiles an original `vs_5_0` shader and executes it on
a reference device, then runs the independent CPU executor with private same-adapter
VS assistance. Input bytes include positive/negative subnormal representations,
negative zero and a NaN payload. No float tolerance is used: all four output words
must match exactly. The suite covers:

- WARP, NVIDIA and Intel, each selected explicitly at feature level 11.0.
- R32_FLOAT/UINT/SINT, R32G32B32A32_FLOAT, R16G16B16A16_UNORM,
  R8G8B8A8_SNORM/UINT, R8G8_UINT, R8_UINT, R10G10B10A2_UNORM,
  R11G11B10_FLOAT and R32G32B32_FLOAT.
- A three-element view starting at element one, with indices 0, 2, 3 and UINT32_MAX.
- Literal and constant-buffer indices, full swizzled output and partial output
  assignment preserving sentinel components.
- Per-component unknown propagation, preflight cancellation/retry, unbound versus
  unavailable bindings, and rejection of unsupported PS typed execution.
- Exact source-byte preservation, reversible register bits/validity, reference
  VS/GS/CB/SRV/SO/topology bindings, and D3D11 validation errors.

Each hardware row records compiler output SHA-256, input-byte SHA-256, exact CPU/GPU
words, validity and helper adapter/driver/feature-level identity. Raw logs remain
outside Git; `tools/summarize_dxbc_typed_vertices.py` records their identities and
test outcomes in `research/dx11-dxbc-typed-vertex-evidence.json`.

The VS suite passed 580 cases (576 hardware rows, two validity/lifecycle cases,
and QtTest setup/cleanup) in 104.142 seconds, with zero failures and zero skips.
The rebuilt existing suites then passed serially in 217.42 seconds:

| Suite | Passed | Failed | Skipped |
| --- | ---: | ---: | ---: |
| Typed CS buffer | 473 | 0 | 0 |
| Input snapshot | 42 | 0 | 0 |
| Hidden counter | 40 | 0 | 0 |
| Thread group | 38 | 0 | 0 |
| Raw/structured memory | 246 | 0 | 0 |
| Instruction execution | 375 | 0 | 0 |
| Regression subtotal | 1214 | 0 | 0 |
| Including VS suite | 1794 | 0 | 0 |

The final VS target and regression builds completed without compiler warnings.
No assertions, golden outputs or skip categories were weakened or expanded.

## Remaining scope

The tested stage/model is VS SM5 at feature level 11.0. This does not prove full
SM4/SM4.1 support, optional formats, VS UAV operations, PS typed loads, textures,
sampling or all original shader contexts. Other typed operations still require
CS execution; unsupported stage/operation combinations fail without a CS fallback.
The helper is native D3D11 and does not use GPA private entry points or RenderDoc.
Full source/GUI debugger integration, fragment history, dependency removal and
standalone release acceptance remain open.

## Reproduction

```powershell
cmake --build build/vs2022 --config Release --target FloraDxbcTypedVertexTests --parallel 4
ctest --test-dir build/vs2022 -C Release -R '^dxbc_typed_vertex$' --output-on-failure -j 1
```

Run GPU suites serially. The evidence script accepts repeated `--log`,
`--regression` and `--source` arguments followed by `--out`.
