# M2: checked object observations and Present follow-up

This iteration removes 15 observed object-lifetime/getter record families from
the unchecked auxiliary fallback. It does not emulate the original application's
COM reference counts, reconstruct its CPU control flow, or implement Present.

## Checked records

| Records | Captured payload after the two-QWORD header | Replay treatment |
|---|---|---|
| `3012`, `324f`, `3256`, `3575` | HRESULT, 16-byte IID, 64-bit returned identity | QueryInterface / GetDevice observation |
| `3013`, `3014`, `3250`, `3251`, `3576`, `3577` | One uint32 returned count | AddRef / Release observation |
| `3019`, `3146` | Strict boolean, optional uint32 dimension | Resource GetType observation |
| `302e` | Strict boolean, optional 24-byte SRV description | GetDesc observation |
| `3261` | HRESULT, strict boolean, optional 72-byte x64 swap-chain description | GetDesc observation |
| `3597` | HRESULT, GUID, strict boolean, optional uint32 size, opaque uint64 pointer | GetPrivateData observation |

All values above are hexadecimal type IDs. The Reader requires exact length;
truncation, trailing bytes and invalid optional-field flags are rejected both in
production replay and offline preflight. Unknown types still return unhandled.
The old fallback silently accepted extra/truncated bytes; an inspector marking a
record `partial` was not an execution guard.

Pointers, returned reference counts and CPU output structures are observed values.
They do not become native replay pointers, allocation sizes or object-release
requests. A captured owner/returned identity need not be a materialized resource
record: actual GPU commands resolve their own saved resource identities and retain
their existing reference validation. A zero Release result does not destroy an
independently owned replay object. Failed getters are still exact captured wire
observations; untrusted descriptor/union contents are not used to create objects.
The record link is retained by the API inspector without inventing parent-call
execution or a general alias reconstruction scheme.

Production reports now count these records as `object_observation_records`.
GPU Draw/Dispatch/Map counts are unchanged. Capability classification is `metadata`,
not an execution or full COM-compatibility claim.

## Evidence

The layouts reuse the recovered native API inspector and are cross-checked with
existing original captures and Ghidra exports in the research workspace:

- `analysis/API_COMMANDS.md`, `analysis/api_commands/shim_wire.c`,
  `analysis/api_commands/queries.c` and the associated serialization tables.
- Shim RVA `0x3a480`: actual `IDXGISwapChain1::GetDesc`, not GetDesc1; slot 12
  serializes the 72-byte x64 description including padding.
- Shim RVA `0x2a2f50`: SRV GetDesc optional flag and 24-byte description.
- Shim RVA `0xf3d70`: GetPrivateData stores the returned size and original pointer
  value, not an arbitrarily sized copy of private data. No pointer dereference or
  size-driven allocation is appropriate in replay.
- QueryInterface/refcount shapes are also independently observed in the
  hash-registered game and micro-capture corpus. The existing checked CommandList
  and Factory observations use the same CPU-return/lifetime policy.

`tests/ObjectObservationTests.cpp` covers 30 positive layouts spanning all 15
families, every strict payload prefix and a trailing byte for each, invalid flags,
opaque UINT64_MAX identities, absent optional outputs, failed getters and zero/
UINT_MAX returned counts. Hardware/WARP tests verify repeated compute output,
UAV counter bytes and the still-bound native CS after the observations. Each
trailing-byte variant is also rejected by production replay and preflight with
an event ID. Present and mutating object calls remain outside this family.

## Why Present stays unverified

The current corpus has 70 Present calls, each followed only by captured object
observations. They contain S_OK, zero parent link, and either flags 0 or the
observed GF2 tearing flag. This explains why final image checks alone do not
exercise a later pipeline dependency after Present.

A successful flip-model Present can unbind the backbuffer from the pipeline;
TEST and DO_NOT_SEQUENCE have different behavior. See Microsoft's
[Present semantics](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present)
and [Present flags](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-present).
Consequently, moving Present into the passive family would conceal an execution
gap. Its old fallback remains explicitly `auxiliary_unverified`; no claim is made
that post-Present native binding state is accurate.

`tools/native/present_boundary_probe.cpp` is a standalone, development-only D3D11/
DXGI probe. It creates a headless composition flip-sequential swap chain, binds a
backbuffer RTV, calls Present with flags 0/TEST/DO_NOT_SEQUENCE, and records the
HRESULT and actual OM binding afterwards on hardware and WARP. It has no GPA,
Python or Qt runtime dependency and does not open a desktop window. Build and run
it from a normally configured repository (use a new/empty output directory):

```powershell
cmake --build --preset release --target FloraPresentBoundaryProbe
./build/vs2022/Release/FloraPresentBoundaryProbe.exe artifacts/new-present-probe
```

Both hardware and WARP returned S_OK for flags 0 and TEST. Flags 0 unbound the RTV;
TEST retained it. DO_NOT_SEQUENCE returned `0x887a0001` (invalid call) in this
composition setup and retained the binding; this is **not** successful execution
evidence for DO_NOT_SEQUENCE. Six observations are in
`artifacts/m2-present-probe/measured-run/present.json`. Both before/after bindings
are read from the native context, rather than inferred from the setter. The earlier direct compiler
run agrees. Its first build failed for a missing `<string>` include; the source
was corrected before either successful run. This probe is excluded from default
builds/tests and from the deployed application package.

This probe is execution-semantics evidence, not a GPA-format acceptance sample.
The next Present change still needs original captures, original-player comparison,
event-boundary state checks and post-Present work. It must address buffer identity/
rotation and binding changes without discarding the presented image or inventing
uncaptured buffer contents. Unknown layouts continue to require explicit diagnostics.

The remaining observed auxiliary families are Present (`3257`), Buffer.SetPrivateData
(`3017`) and Device.CreateBuffer (`3578`). Each needs its own semantics audit;
this reduction is not a percentage of original GPA functionality. Other record
families' incomplete resource validators, deferred execution and M4–M6 remain open.

## Validation on 2026-10-03

| Check | Result |
|---|---|
| Registered corpus | 91 files, 182 fresh-process native replays completed |
| Repeat images | 90 stable; all 90 unchanged from the preceding Map audit; Helldivers retains known variability |
| Observed command families | 45 execute, 39 metadata, 12 snapshot, 3 auxiliary-unverified |
| Newly checked observations | 1,374 per corpus pass across 70 files |
| Offline checks | 91 review_required, zero errors; warning occurrences reduced from 1,935 to 561 |
| Module and raw image hash audits | 182 passed; no GPA/Python runtime dependency found |
| Fresh original-player comparison | GF2 and BF1, two runs per implementation, matching golden images |
| Delivery CTest | Six suites passed; main UI 55 cases passed without skips |
| Packaged golden / disabled-draw negative controls | Four passed in an isolated environment |

The original player's default adapter configuration remains unidentified; matching
images do not establish identical device/driver configuration. The Helldivers
sharpening boundary and separately disabled-sharpening diagnostic controls remain
in the corpus report; production shaders and global image thresholds are unchanged.
These are observed corpus outcomes, not a claim that all 99 types are fully certified.

The package is `out/FloraGPA-object-audit-20261003/`. Its four executables and
Metrics DLL match the validated Release outputs. Compact, hash-linked results are
in [object-observation-baseline.json](object-observation-baseline.json). Raw local
evidence is retained in `artifacts/m2-object-corpus/`, `artifacts/m2-object-original/`,
`artifacts/m2-object-golden/`, `artifacts/m2-object-ui/` and
`artifacts/m2-present-probe/measured-run/`. Captures and generated outputs stay out
of Git. Python module migration totals remain 72 ported, 117 partial, 15 pending.
