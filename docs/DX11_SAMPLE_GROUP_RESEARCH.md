# Explicit-gradient sampling through compute control flow

This checkpoint extends the independent D3D11 observations from
[the gradient investigation](DX11_SAMPLE_GRAD_RESEARCH.md). It does not accept
the native executor or complete P6. RenderDoc and Intel GPA are not used by
the probe.

`FloraSampleGroupProbe` compiles original HLSL with the Windows compiler, runs
one dispatch on the explicitly selected adapter, and saves the compiled DXBC,
disassembly, exact input texture and parameter bits, observed sampler
descriptor, and complete output. The debug device must report no errors.
Readback has a five-second deadline. Output directories must be new.

The matrix contains 96 cases per adapter: four group dimensions (`64,1,1`,
`8,4,2`, `4,4,4`, `32,2,2`), six control flows, two sampling offsets, and point
or anisotropic filtering. Each lane has four output slots, initially
`0xdeadbeef` in every component. The flows are uniform execution, early return,
two explicit branches, one to three loop iterations per lane, three iterations
with groupshared communication and uniform barriers, and unequal work before
a common sampling instruction. Disassembly checks establish that the compiler
retains the required branches, loops, and barriers.

The checked-in ledger records 288 observations across WARP, NVIDIA, and Intel:
33,240 written slots and 58,920 untouched slots. All write participation and
sentinels are checked. Another 14,730 point-filtered results match analytical
texel selection using power-of-two derivative lengths exactly. Anisotropic
and skew-gradient values remain observations, not analytically asserted truth.
`GetDesc` canonicalizes unused point-filter anisotropy to zero on these devices;
the report records that observed descriptor. All three devices receive
byte-identical compiled shaders. The raw outputs and compiler binaries remain
outside Git under `artifacts/dx11-native/sample-group-baseline`.

Reproduce from a configured Release build, running GPU commands serially:

```powershell
cmake --build build/vs2022 --config Release --target FloraSampleGroupProbe
New-Item -ItemType Directory artifacts/dx11-native/sample-group-repeat
foreach ($vendor in @(0, 4318, 32902)) {
    & build/vs2022/Release/FloraSampleGroupProbe.exe `
        "artifacts/dx11-native/sample-group-repeat/$vendor" $vendor
    if ($LASTEXITCODE -ne 0) { throw "Independent sampling probe failed" }
}
python tools/summarize_sample_groups.py `
    --root artifacts/dx11-native/sample-group-repeat `
    --binary build/vs2022/Release/FloraSampleGroupProbe.exe `
    --out artifacts/dx11-native/sample-group-repeat/summary.json
```

The next acceptance step is exact whole-output comparison with the independently
compiled shader executed by `dxbc_exec::Group`. Participation, branch identity,
dynamic loop iteration, original flat lane index and group dimensions must be
preserved. A matching scalar sample alone does not establish that acceptance.
