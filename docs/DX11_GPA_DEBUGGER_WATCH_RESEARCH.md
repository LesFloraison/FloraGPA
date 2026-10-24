# GPA debugger lifecycle and UI request evidence

The pinned GPA installation exposes a shader-debug request path, but its concrete
debugger engine has not been recovered. This follow-up replaces sampled lifecycle
boundaries with calibrated hardware write-watch evidence for six bounded runs,
and extends the static investigation into `framedb_qt.dll`. It does not close P1,
establish VS/PS/CS trace parity, or permit removal of the current production
debugger. The independent DXBC executor remains the product implementation path.

## Binary identity and provenance

All image RVAs below are relative to image base `0x180000000`. Original files were
read from the local Intel GPA installation and verified unchanged after research.
Ghidra 12.1.4 with Java 21 used the separate `Dx11NativeResearch` project under
ignored `artifacts/`. No original DLL, capture, database or raw decompilation is
committed or included in a product package.

| Binary | SHA-256 |
| --- | --- |
| `dx11_player.dll` | `39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b` |
| `dx11_playback.dll` | `0a531905386af6c5f2c0357d4179cec208bb171a287b262f3c4a5539d483bf5e` |
| `com/intel/framedb/framedb_qt.dll` | `35a03995d50f390ed74722385583e7bb011f84e563a579c88086d8039ad1df80` |

The framedb file is 11,629,056 bytes, version `25,1,26603,47871`. A read-only
inventory scanned 438 local PE files; 436 contain broad debugger string matches,
often merely `IsDebuggerPresent`. These are discovery candidates, not evidence
of a shader debugger implementation. The new framedb import completed analysis
in approximately 496 seconds. Invalid-address decompiler warnings and unresolved
type descriptors remain in the raw log; successful analysis is not proof of
correct inferred types.

## Hardware watch and independent calibration

`FloraOriginalDebuggerWatch` is an explicit `EXCLUDE_FROM_ALL` research target,
not a runtime dependency or required test. It launches an owned child with the
Windows debug API and arms four hardware slots on all observed threads:

| Slot | Watch | Purpose |
| --- | --- | --- |
| DR0 | Eight-byte write at player `0x208540` | Host publication and clearing |
| DR1 | Execute at `0x89c70`, then `0x89f90` | Construction and destruction boundaries |
| DR2 | Eight-byte write at host `+0x3c0` | Candidate engine field writes |
| DR3 | Execute at `0x902b0` | Actual debugger request entry |

The watcher reads memory and adjusts thread debug/control registers; it does not
patch instructions or installed files. The child uses the existing temporary
request observation mechanism described in `DX11_GPA_RESEARCH.md`, including its
restoration checks. Unknown exceptions are forwarded. Each watch has a 120-second
deadline and a 10,000-event bound. Failure terminates the owned child. The serial
runner adds a 130-second outer process deadline without extending the inner one.

An independent fixture publishes the same field offsets, runs two concurrent
writer threads, performs transient nonzero/zero assignments with interlocked
exchanges, issues two requests,
and destroys the host. All three final calibration runs observed one constructor,
one destructor, 36 field writes across three writer threads, and two requests.
The fixture intentionally makes constructor and destructor code distinct under
linker identical-code folding. Ordinary watch hits update only the event thread;
changing every thread context at each hit previously lost pending debug traps.

Module attribution excludes unloaded images, so a reused address cannot inherit
an earlier module's identity. The JSON field named `written_engine` is the value read when processing a write
event. It is not an atomic record of the writing instruction's operand: another
thread can overwrite the field before all threads stop. Calibration checks write
event counts and some nonzero observations, not an exact nonzero-value count.
Instruction pointers on data watches identify the instruction after the write.

## Runtime results

All six final runs completed with exit code zero. Each observed one construction,
one destruction, two field writes and two requests. Both requests saw a null
engine. The only nonzero field observation occurred after destructor entry:
`0xfeeefeeefeeefeee`, written by `ntdll` during heap teardown. Optional vtable
decoding correctly retained its read failure instead of treating it as an engine.

| Capture | Settings | Watch duration (ms) | Live nonzero field observations |
| --- | --- | ---: | ---: |
| GF2, event 113 | Controlled | 6,453 | 0 |
| GF2, event 113 | Original initialization | 5,250 | 0 |
| BF1, event 876 | Controlled | 32,828 | 0 |
| BF1, event 876 | Original initialization | 30,781 | 0 |
| Helldivers, event 200 | Controlled | 26,875 | 0 |
| Helldivers, event 200 | Original initialization | 24,828 | 0 |

Durations include debugger overhead; some runs overlapped CPU-only Ghidra exports
and are not product performance measurements. GPU probes did not overlap.
Both configurations set single-threaded execution. Controlled initialization
also disables first playbacks and the GT query manager. The original-initialization
configuration leaves those two defaults intact. Adapter selection remains GPA's
undefined-GpuId default, not a certified product adapter. Before/after ordinary
replays succeeded, but these probes do not compare framebuffer pixels or prove
complete replay-state equality. No request produced a result callback.

The constructor's zero store at `0x89ce5` traps at `0x89cec`; host publication at
`0x87ce9` traps at `0x87cf0`. Destruction enters `0x89f90`; clearing the global at
`0x87055` traps at `0x87060`. The request reads manager `+0x98` at `0x902e4` and host
`+0x3c0` at `0x902eb`. Those instructions agree with the independently observed
host and request identities.

These runs narrow the sampled-boundary uncertainty for these exact lifecycles.
They do not exclude another initialization route, asynchronous UI execution,
another module/version or a debugger-sensitive path. The raw request selection
remains `[0, event, 0, 0, 0, 0, 0, 0]`; its zero discriminator is not validated as
a VS/PS/CS selector. The null-engine branch precedes further selection validation.
A successful wrapper status therefore remains insufficient evidence of a working
debugger.

## UI setting and task chain

The following findings are supported by disassembly and cross-references in the
pinned framedb image. They describe routing and object layout, not shader execution.

| RVA | Verified role |
| --- | --- |
| `0x789298` | `userSettings/isShaderDebuggerEnabled` string |
| `0x130530` | Settings constructor; stores the key at object `+0x898` |
| `0x134c90` | Reads that key through virtual slot `+0x70`, with fallback byte `+0xd33`, then converts to bool |
| `0x139c60` | Compares the current setting and writes changes through virtual slot `+0x68` |
| `0x12ea50` | Emits a Qt notification for the changed value |
| `0x2a3860` | Validates model selection and packs a 32-byte debug request |
| `0x36e6a0` | Returns context field `+0xa0` before request virtual dispatch |
| `0x3887c0` | Copies request/callback into a queued callable via virtual slot `+0xe0` |
| `0x38ae40` | Allocates a 400-byte task, calls base construction with kind `0x40`, and copies request into `+0xb0..0xcf` |
| `0x7c18f0` / `0x7c1998` | Primary/secondary `ShaderDebugTask` vtable addresses identified through RTTI and construction |
| `0x2a9190` | Result callback updates selected-program source/metadata from result `+0xa0` |

At `0x1310f2` the settings constructor zeros the dword containing fallback byte
`+0xd33`. This establishes a false fallback, not the value of a persisted user
setting and not proof that changing the setting constructs an engine.

The request builder accepts numeric discriminators 1, 8 and 10 and parses one,
two or six integer arguments respectively. It sets request word 1 from the matched
range and sends the eight-word request through virtual slot `+0x40`. Exact shader
stage and argument meanings remain unverified; no stage labels are inferred from
those numbers. The task factory's callable vtable is at `0x7c2dc0`, with its factory
at slot `+0x10`. The function at `0x37c110` is a deleting destructor, and
`0x37b690` adjusts the secondary object pointer before that destructor. Neither is
a shader execution function. `0x37da20` belongs to discard/error delivery.

The task's secondary entry `0x372710` adjusts the object by `-0x10`, invokes
primary slot `+0x58` (`0x386520`), then signals completion. The latter checks
discard/error state and calls queue execution `0x368e80`. Its successful branch
loads queue field `+0x1a8` at `0x3694b5`, passes request `task+0xb0` and result
storage `task+0xd0`, and invokes the adapter's virtual slot `+0x90` at `0x3694d2`.
The concrete target of that slot is not yet established. Completion path
`0x388c30` reaches `0x36dda0`, which delivers `task+0xd0` to callback object
`task+0xa8` through its virtual slot `+0x8`. This is a further static bridge,
not evidence that the downstream player engine is initialized.

The symbol exporter prints up to 24 nearby pointer candidates for each vtable.
That window can cross a table boundary; only individually verified slots may be
used as object-layout evidence. Decompiler types and names remain hypotheses.

Two misleading discovery paths were checked explicitly. Playback's `Unable to
debug shader.` string at `0x2bbd88` leads to `0x57550`, a shared message-catalog
constructor. Player's imported `IsDebuggerPresent` has inspected callers at
`0x148c74` (failure/exception handling) and `0x148f30` (ATL critical-section failure
diagnostics). Neither establishes engine construction or a debugger enable gate.
This does not rule out indirect or cross-module debugger detection.

## Reproduction and retained failures

Build the explicit target in the existing Release configuration, then use fresh
output directories. GPU-dependent original-player probes run serially.

```powershell
cmake --build build/vs2022 --config Release --target FloraOriginalDebuggerWatch
python tools/run_original_debugger_watch.py `
  --watcher build/vs2022/Release/FloraOriginalDebuggerWatch.exe `
  --capture-root D:/CDXrepo/captures `
  --out artifacts/dx11-native/debugger-write-watch-new/verified-final
tools/ghidra/Invoke-DebuggerWatchResearch.ps1 `
  -Headless D:/CDXrepo/utils/Ghidra/Analyze-Headless.bat `
  -ProjectDirectory artifacts/dx11-native/ghidra `
  -OutputDirectory artifacts/dx11-native/debugger-write-watch-new/static
python tools/scan_gpa_debugger_modules.py `
  --out artifacts/dx11-native/debugger-write-watch-new/module-candidates.json
```

The Ghidra recipe assumes player/playback are already imported by
`Invoke-Dx11Research.ps1`; add `-ImportFramedb` once for a project without framedb.
Subsequent exports use `-readOnly -noanalysis`, verify completion markers and
original hashes, and record output hashes. Do not run two headless operations
against the same project simultaneously. Avoid shell metacharacters in regex
arguments passed through the Windows `.bat` launcher.

The final Release research-target build and `capture_boundaries` test passed.
Three concurrent calibrations and all six final original-player runs passed.
Eleven static exports completed. Product sources did not change in this follow-up;
the preceding 2,957-test product checkpoint is not presented as a new test run.

Raw evidence lives in `artifacts/dx11-native/debugger-write-watch/`. The compact
ledger `research/dx11-debugger-watch-evidence.json` is reproduced with
`tools/summarize_debugger_watch.py --root <that-directory> --static
<that-directory>/static-complete --out <ledger-path>`. It verifies source/report
hashes, completed exports, lifecycle counts and retained failures. It summarizes
the retained research session, including historical failures; a fresh session
without those historical files is not a substitute for this ledger.
A negative integrity check altered a copied export and confirmed rejection before
ledger output; the original export set still passed verification afterward.

Retained failures include the first GF2 teardown-pointer dereference, a fixture
failure caused by folded constructor/destructor functions, and a concurrent
calibration failure ending in unhandled `0x80000004`. The earlier six successful
GPA watches predate the concurrent-context fix and are explicitly superseded.
An initial headless regex containing a pipe was interpreted by the batch shell
and failed; rerunning with plain `ShaderDebug` completed. None of these failures
was deleted or relabeled as a pass.

The next unresolved P1 step at this checkpoint was the concrete implementation behind the queue's
`+0x1a8` adapter and virtual slot `+0x90`, including any engine construction it
enables. The engine's nonzero assignment,
implementation vtable and supplying module remain unknown. P5, remaining P6,
production debugger integration, P7 removal and P8 standalone acceptance stay open.

The subsequent `DX11_GPA_DEBUGGER_ADAPTER_RESEARCH.md` resolves that adapter to
`legacy::Playback` in `dx11_playback.dll`, identifies protocol command 9 and
verifies the request/result codecs against both original DLLs. It still does not
recover the downstream execution engine.

Windows debug-loop behavior follows Microsoft's
[debugger main-loop documentation](https://learn.microsoft.com/en-us/windows/win32/debug/writing-the-debugger-s-main-loop).
Debugger presence is observable through
[IsDebuggerPresent](https://learn.microsoft.com/en-us/windows/win32/api/debugapi/nf-debugapi-isdebuggerpresent),
so the watcher must not be treated as behaviorally invisible.
