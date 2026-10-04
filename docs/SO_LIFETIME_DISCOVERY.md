# SO lifetime evidence and original capture delivery

> Subsequent [lifetime implementation](SO_LIFETIME_AUDIT.md) admits proven unused
> absent-only intervals. The discovery outcomes and baseline below are historical.

Reviewed 2026-10-04; production implementation remains `5ebcd8f`. This M2
development batch restores a usable original-capture validation path and adds
nine original SO lifetime files. It does **not** implement missing-buffer lifetime
elision. The [baseline](so-lifetime-baseline.json) pins results and local evidence;
the [corpus](so-lifetime-corpus.json) includes both successes and explicit blockers.

## Original capture delivery

The previously successful frozen producer also stopped delivering captures.
Native/injected workload success and successful Present calls were insufficient.
Read-only traces of the installed GPA 2025 R1 shim showed CaptureNextFrame's
pending flag staying set, an active swap chain in its registry, and no primary
swap chain selected. Visible/foreground-window attempts and a different output
drive did not resolve this. The underlying reason automatic selection stopped
running is not established; low disk space is not a proven cause.

Read-only Ghidra analysis identified the original active-swap-chain selector.
`tools/native/original_capture_control.h` invokes that original method under its
own recursive lock after the probe has presented frames, verifies it selected
the probe's swap chain, then uses public CaptureNextFrame. It verifies the DLL's
complete SHA-256, selector prologue and public export RVA before private access.
Null-module and wrong-module checks reject before accessing private state.
The selector is a private, version-specific development ABI, not a general SDK.

| Pinned shim detail | Value |
|---|---|
| SHA-256 | `cb0b99d113511cbf7ca9f949d97aa02e4a1f8f110b1f3ad3165d8a81035dee31` |
| Selector RVA | `0x3265f0` |
| CaptureNextFrame RVA | `0x2ede80` |
| Registry RVA / lock / primary-entry pointer | `0x65d918` / registry `+0x30` / registry `+0x60` |

No DLL instructions, registry fields, shaders or capture bytes are patched.
Original code performs its normal selection mutation. This helper is linked only
into the optional `FloraSoLifetimeProbe` development target; FloraGPA runtime
continues to require neither GPA nor Python. The first new harness attempt omitted
`GPA_LOCAL_INJECT=true` and failed to load the shim; that failed output is retained.
The corrected harness clears that variable for native runs and sets it only for
injected runs. All nine final captures are unmodified original output.

## Nine original cases

Each source workload completes twelve native and twelve injected frames. The
probe asserts getter object identities, exact red final pixels, and (where
applicable) all 64 SO buffer bytes and native SO statistics. Sources, executable,
shim hashes and per-run files are frozen under
`artifacts/m2-so-lifetime-originals-v2/producer` and `manifest.json`.

| Mode | Original workload | Saved resources / FloraGPA outcome |
|---|---|---|
| 0 | Bind at offset 4, getter, unbind, final draw | Missing buffer 20; rejects event 19 |
| 1 | Bind/getter, ClearState, restore graphics, draw | Missing buffer 20; rejects event 19 |
| 2 | Two targets at offsets 4/20, getter, unbind | Missing buffers 20/21; rejects event 19 |
| 3 | Rebind offset 4 to 20, getters, unbind | Missing buffer 20; preflight identifies setters 19/23 |
| 4 | Bind/getter, four explicit null SO slots, draw | Missing buffer 20; rejects event 19 |
| 5 | Bind/getter, unbind, copy/read back | Saved buffer; exact 64 zero bytes |
| 6 | Mixed targets, replace with first target APPEND, SO draw | Both descriptors saved; writes one point at offset 16 |
| 7 | SO+VB buffer, explicit offset 16, SO draw | Writes one point at offset 16 |
| 8 | SO-only buffer, explicit offset 4, SO draw | Writes one point at offset 4 |

All nine original-player files replay twice with exact producer pixels. Original
adapter/configuration equivalence remains unproven, and these original-player
exports do not verify intermediate SO storage or hidden cursor state. In
particular, mode 6 saved **both** descriptors: it is a mixed-binding/APPEND
control, not an original missing-descriptor mixed-binding counterexample.

The existing independent player succeeds on modes 5–8 on hardware and WARP.
Exact SO/staging bytes and one written/needed primitive for modes 6–8 pass twice
per adapter. Modes 0–4 retain both preflight and runtime diagnostics; their
rejections are implementation compatibility blockers, not proof that their final
images cannot be recovered. Missing descriptor/storage facts remain explicit.

## Avoiding false acceptance

The final red triangle is independent of SO. Also, modes 6–8 already have the
final SO byte pattern before the recorded draw because the producer repeats its
workload. Therefore final images and unchanged buffer bytes alone cannot prove
the recorded SO draw executed correctly. The accepted checks additionally verify
the independent player's actual SO written/storage counts at the draw event.
Twelve hardware/WARP disabled-draw controls retain the same final red image but
have no SO execution-history entry. Twelve before-draw exports explicitly record
the prefilled-buffer fact; they are not counted as new write proofs.

The focused run contains 32 strict buffer exports, twelve before-draw observations,
sixteen successful ordinary hardware/WARP replays, twenty located missing-buffer
rejections and twelve disabled-draw controls. A separate serial corpus comparison
contains eighteen independent attempts and eighteen original-player runs.
Existing stream-output/getter CTest suites pass 19/30 cases with no skips.
The production binary is unchanged, so this does not claim a new full historical
matrix, UI acceptance or release-package rebuild.

## Next implementation gate

1. Prove a closed, unconsumed SO binding interval in the original missing-buffer
   files. Keep the older event-43/buffer-44 getter file as an independent case.
2. Restrict any relaxation to justified semantics. A mixed setter can reset a
   saved buffer's hidden cursor; dropping the whole setter may break a later
   APPEND/DrawAuto. Getter metadata cannot replace that side effect.
3. Account for binding hazards, aliases, ClearState, state snapshots, disabled or
   edited closing commands, event-prefix replay and native-state observers. An
   observer inside an unresolved interval must not report stale bindings as valid.
4. Keep GPU consumption, unknown commands, deferred/list contexts and unresolved
   resource versions explicitly rejected until their own evidence exists. Do not
   fabricate descriptors/storage to make a setter pass.
5. Add malformed/truncated/reference counterexamples and relevant full regression
   when production semantics change. These nine unmodified originals remain the
   acceptance inputs; edited research copies are only negative controls.

Reproduce in a developer environment (new output directories required):

```powershell
cmake --build --preset release --target FloraSoLifetimeProbe
python tools/capture_so_lifetimes.py --exe build/vs2022/Release/FloraSoLifetimeProbe.exe --shim "C:/Program Files/IntelSWTools/GPA/shimloader64.dll" --out artifacts/new-so-lifetimes
python tools/validate_so_lifetimes.py --exe out/FloraGPA-pipeline-getters-20261004/FloraGPA.Cli.exe --manifest docs/so-lifetime-corpus.json --captures-root artifacts --out artifacts/new-so-checks
```

The pinned corpus references the preserved final captures, not newly generated
files. Fresh captures receive new hashes and event IDs and need a new catalog;
never replace the old files to satisfy a hash. A normal clone does not include
the local capture files, proprietary DLLs or prior package.
