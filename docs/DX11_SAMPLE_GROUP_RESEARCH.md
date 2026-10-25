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

## Native execution checkpoint

The initial six-flow WARP comparison passed four flows and failed the explicit
branches and staggered arrival cases. The old scheduler issued 64 scalar GPU
operations for the branch case, and 33 operations for staggered arrival. Their
raw component differences remain in `sample-group-first.txt`; the independent
oracle is unchanged.

`bounded-sampling-rendezvous/3` now gathers runnable invocations at sampling
boundaries, issues separate cohorts for distinct instruction/call/loop identities,
and drains the staged results before allowing any subsequent instruction. Its
4,096-instruction gathering limit permits a sampled value to unblock a polling
thread. It remains a deterministic diagnostic schedule, not a reconstruction of
the actual GPU schedule. Long divergence that exceeds the gathering bound,
arbitrary calls and mixed dynamic bindings still need dedicated evidence.

The complete comparison has 288 hardware rows. Every raw output component and
validity byte must match, including untouched sentinel slots. Original input
parameters and texture bytes/validity remain unchanged. Uniform, early-return and
staggered cases use one private sampling dispatch, branches use two, and each
loop case uses three. Barrier loops release exactly six barriers. Four group
dimensions and the original flat indices are retained in every helper dispatch.

Six additional cases test scheduler progress and cancellation on all three
adapters. A producer samples and then sets a raw UAV flag; the consumer polls it
with an atomic OR. This shader is executed only by the diagnostic scheduler; it
is not dispatched as a hardware spin-wait test. Required sampling, loop and
atomic opcodes are checked after compilation. All progress cases finish in
4,112 instructions. Cancellation during gathering leaves the output unchanged,
performs no GPU sample and rejects reuse of the failed group. A new group then
completes successfully with the same private storage.

The initial groupshared polling fixture was optimized away by the compiler and
correctly failed the intended wait/cancellation assertions. The retained fixture
uses externally observable UAV atomics and checks the compiled operation list.
These fixture-development failures are kept under `sample-group-progress-*`.
They are separate from the two executor mismatches above. No output tolerance or
skip was added to make a case pass.

Run `dxbc_sampling_group`, `dxbc_sample_grad`, `dxbc_group` and `dxbc_execution`
serially after rebuilding their targets. The group evidence validator requires
all 288 configurations, exact shader hashes and adapter LUIDs, batch counts,
progress/cancellation records and all three regression suites. The independent
validator rejects missing cases, changed input parameters/sampler descriptors
and writes into untouched slots. Complete P6 and standalone release acceptance
remain open.

The final serial CTest run passes all four suites: 296 sampling group tests,
262 explicit-gradient tests, 38 cooperative group tests and 375 scalar execution
tests, totaling 971 passes with zero failures/skips in 284.94 seconds. GUI, CLI,
Worker and all four test targets build in Release. The ledger is
`research/dx11-dxbc-sampling-group-evidence.json`; raw logs remain in
`artifacts/dx11-native/sample-group-*`. Negative checks reject altered batch
counts, missing comparisons, changed shader hashes and failed retries.
