# Native DXBC typed buffer checkpoint

Date: 2026-10-24. This P6 checkpoint executes CS typed buffer loads and stores
against private native snapshots. It does not replace production debug actions,
establish complete shader execution, or close the RenderDoc removal gate.

## Representation and execution

`BufferView::format` retains a native DXGI typed-buffer format. Zero continues to
mean raw or structured storage. The independent layout table records element size,
component bit ranges and the DXBC return class; it does not implement a competing
CPU texel conversion. Native SRV/UAV descriptors determine the element range and
format. The snapshot retains all backing bytes and shares one private resource
between aliases. Source identity is `independent-replay-buffer-inputs/2`.

The executor recognizes `dcl_resource`, `dcl_uav_typed`, `ld`, `ld_uav_typed` and
`store_uav_typed`. A typed buffer declaration and compatible return class are
required. Conflicting raw/structured and typed declarations fail. Other resource
dimensions and non-CS typed execution fail explicitly until their own semantics
are implemented and validated. No fallback or adapter substitution occurs.

`GpuOperations` (`dxbc-gpu/3`) emits a project-owned single-operation CS shader
and executes it on its isolated device with the replay adapter's LUID and feature
level. WARP must be explicitly selected. Both literal and register-sourced indices
retain their operand form. Actual D3D11 format conversion supplies the returned
bits and written storage, including packed formats and missing channels. Each
operation checks the device's format support. Typed UAV loads/stores additionally
query `D3D11_FEATURE_FORMAT_SUPPORT2`; unsupported operations fail explicitly.
This is independent implementation using native D3D11, not imported interpreter
code or a product dependency on GPA.

The helper creates only private resources and views. Loads return requested
components; stores copy the full backing buffer back and reject any change
outside the selected element. Out-of-view stores must leave all bytes unchanged.
The interpreter records an in-view write even when its bytes do not change.
It records reversible byte/validity changes only after the operation succeeds.
Existing device cancellation, bounded polling, shader-cache bounds and failure
poisoning remain in force. Full-buffer copies bound correctness but can be costly;
this checkpoint is not a performance acceptance result.

## Unknown values and validation

GPU readback does not establish whether an input was defined. A typed load's
component validity depends on all snapshot bytes overlapping that component;
default missing channels and defined out-of-view results do not depend on input
storage. Resource swizzles and destination masks are applied to validity as well
as bits. A typed store derives each byte's validity from contributing source
components. Shared packed bytes remain unknown if any contributing component is
unknown. Full `.xyzw` typed stores are required by the instruction contract.

Observed snapshot bits still describe an independent replay, not proof of original
capture initialization or scheduling. Typed buffer atomics, additional resource
queries, typed textures, sampling, vertex/pixel invocation extraction, quad/helper
semantics and full stage/source/UI parity remain separate work. Unknown resources
remain distinct from verified null bindings.

The probe suite compares full backing bytes with a separate replay executing the
original compiler-generated shader. It exercises three adapters, SRV loads, UAV
stores, mandatory R32 UAV loads, view offsets, ordinary/out-of-view/UINT32_MAX
indices and literal versus constant-buffer operands. It checks trace reversal,
original resource preservation, D3D11 validation messages, partial validity,
unchanged writes, invalid stores, cancellation and retry. Optional multi-channel
typed UAV loads are not established by the mandatory R32 load tests.

The hardware matrix covers these formats:

| Format | DXGI value | Probe operations |
| --- | ---: | --- |
| R32_FLOAT, R32_UINT, R32_SINT | 41, 42, 43 | SRV load, UAV store, UAV load |
| R32G32B32A32_FLOAT | 2 | SRV load, UAV store |
| R16G16B16A16_UNORM | 11 | SRV load, UAV store |
| R8G8B8A8_SNORM, R8G8B8A8_UINT | 31, 30 | SRV load, UAV store |
| R8G8_UINT, R8_UINT | 50, 62 | SRV load, UAV store |
| R10G10B10A2_UNORM, R11G11B10_FLOAT | 24, 26 | SRV load, UAV store |
| R32G32B32_FLOAT | 6 | SRV load |

The separate validity test uses R32G32B32A32_UINT (3). The layout table includes
additional compatible formats; this matrix does not claim those unprobed formats
have completed hardware acceptance. Stores include signed, unsigned, normalized,
floating-point, saturation and NaN source bits; all comparisons keep exact bytes.

## Sources

- [Microsoft typed UAV load instruction](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/ld-uav-typed--sm5---asm-)
  specifies addressing, format conversion and missing components.
- [Microsoft typed UAV store instruction](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/store-uav-typed--sm5---asm-)
  specifies full component masks, conversion and out-of-bounds writes.
- [Microsoft typed UAV load support](https://learn.microsoft.com/en-us/windows/win32/direct3d11/typed-unordered-access-view-loads)
  distinguishes mandatory R32 formats from optional additional formats.
- [D3D11.3 functional specification](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)
  supplies buffer/view and format contracts. Local token declarations use the
  pinned public Microsoft header identified in `DX11_DXBC_FORMAT_RESEARCH.md`.

## Reproduction

The first full run passed all 468 hardware rows and then failed in the validity
fixture: it supplied a temporary register as an external input, which the executor
correctly rejected. The retained diagnostic run reports `Invalid DXBC execution
input register`. The corrected fixture uses a declared constant buffer with
partially known components. The original byte-comparison assertions remain exact.
A diagnostic build also exposed Qt moc's rejection of a function-try-block in a
slot; ordinary internal try/catch now reports exception messages without aborting
the suite. Neither issue became a baseline exception or a skipped test.

The final serial CTest run passed all six suites in 214.37 seconds:

| Suite | Passed | Failed | Skipped |
| --- | ---: | ---: | ---: |
| Typed buffer | 473 | 0 | 0 |
| Input snapshot | 42 | 0 | 0 |
| Hidden counter | 40 | 0 | 0 |
| Thread group | 38 | 0 | 0 |
| Raw/structured memory | 246 | 0 | 0 |
| Instruction execution | 375 | 0 | 0 |
| Total | 1214 | 0 | 0 |

The 473 typed cases comprise 468 hardware rows, three validity/lifecycle cases and
QtTest setup/cleanup. Existing input tests now require successful typed-buffer
execution with exact GPU output instead of expecting that binding to be unsupported;
the separate texture-unavailable/null-binding assertions remain intact. The final
builds completed without compiler warnings. The input observation probe also builds.

Two NVIDIA real-frame input observations now retain the formerly unsupported
R32G32B32A32_FLOAT SRV at VS slot 0:

| Capture / event | GPA resource / view | Snapshot view bytes | Remaining unsupported bindings | Time |
| --- | --- | ---: | ---: | ---: |
| BF1 / 876 | 859 / 872 | 1048576 | 0 | 4445 ms |
| Helldivers / 200 | 138 / 166 | 8388608 | 0 | 3734 ms |

These observations capture native bindings and bytes only. They do not execute
the VS or establish actual vertex invocation semantics. Capture/shader identities,
backing-byte hashes and adapter LUIDs are in
`research/dx11-dxbc-typed-buffer-evidence.json`. Regression records retain the same
adapters' driver versions and helper identities. The new path calls native D3D11
and contains no RenderDoc or GPA private entry points. The final package/module
audit remains required.

```powershell
cmake --build build/vs2022 --config Release --target FloraDxbcTypedBufferTests FloraDxbcInputSnapshotTests --parallel 4
ctest --test-dir build/vs2022 -C Release -R '^dxbc_(typed_buffer|input_snapshot)$' --output-on-failure -j 1
```

`tools/summarize_dxbc_typed_buffers.py` accepts repeated `--log`, `--regression`,
`--observation` and `--source` arguments and an `--out` path. It preserves failed attempts,
per-probe comparison hashes, final QtTest outcomes and source SHA-256 identities.
Raw logs and generated captures remain outside Git.
