# Original GPA shader-debug availability

The pinned local GPA build exposes `ShadersPresenter::isShaderDebuggerAvailable`
through a constant false getter. This is a verified public-property path, not
evidence that the unresolved debugger engine is absent from every module or
unreachable through another route. P1 remains open.

## Binary and static evidence

The original file is `com/intel/framedb/framedb_qt.dll`, version
`25, 1, 26603, 47871`, SHA-256
`35a03995d50f390ed74722385583e7bb011f84e563a579c88086d8039ad1df80`.
Its preferred image base is `0x180000000`; all addresses below are RVAs.
The existing independent Ghidra research project was opened read-only without
analysis. The installed image hash was checked before and after observation.

| Location | Evidence |
| --- | --- |
| `0x774f00` | Presenter metaobject; string/data/dispatcher pointers resolve to the following three locations. |
| `0x7754c0` | Metadata strings name `ShadersPresenter`, `debugShader` and `isShaderDebuggerAvailable`. |
| `0x775d50` | Metadata revision 12, 25 local methods and 22 local properties. Method 23 is no-argument `debugShader`; property 21 is the boolean availability property. |
| `0xb66d0` | Static metacall dispatcher. Method 23 reaches the tail jump at `0xb69bf` to `0x118980`. Read-property call kind 1, property 21 reaches `0xb6fc3`. |
| `0xb6fc3` | Calls `0xd22e0`, then `0xb6fc8` stores AL to the property output byte. |
| `0xd22e0` | Exact bytes `32 c0 c3`: `XOR AL,AL; RET`. It does not read the supplied object. |
| `0x118980` | Debug request forwarding function. Converts the string at object `+0x180`, passes object `+0x38` as RDX, `*(object+0x18)+0x98` as RCX and the converted string as R8. |
| `0x1189bf` | Calls the previously identified request builder at `0x2a3860`. |

The metadata pointer layout includes the two-word superclass representation;
assuming an eight-byte superclass field would select the wrong pointers.
Raw PE parsing verifies the exact layout rather than importing a presumed Qt
structure. The property name/index interpretation is independently exercised
through the actual original dispatcher below.

The tiny false getter has many cross-references because functions can share this
implementation. Those references alone do not identify a debugger-specific
function. The presenter metadata and concrete property dispatch establish the
specific association. Ghidra pseudocode types remain hypotheses; the retained
instruction listings and dynamic reads support the conclusions above.

The forwarded object at `+0x38` is not equated with the separate `shaderType`
property at `+0x30`. Previously accepted request discriminators 1, 8 and 10 still
lack verified shader-stage meanings. The independent persisted
`isShaderDebuggerEnabled` setting cannot change the verified constant getter's
instructions; no patched or bypassed availability path is used as evidence.

## Independent guarded probe

`tools/probe_original_debug_availability.py` first verifies the complete image
hash, metadata identities and getter bytes. A bounded child loads the original
module, verifies the loaded getter bytes and invokes both that getter and the
actual static metacall dispatcher. No frame is opened, application object
constructed, GPU work submitted or shader executed.

Nine cases cross object arguments `0`, `1`, `UINT64_MAX` with output seeds
`0x00`, `0x55`, `0xff`. The getter returns zero and property dispatch writes exactly
one zero byte while preserving the surrounding 32 guard bytes and output-pointer
array. The non-object arguments are safe only for this hash-pinned constant
branch; this probe must not be generalized to unverified original functions.
The final child exits successfully in 94 ms within a 30-second bound.

`tools/validate_debug_availability.py` independently checks the complete case
matrix, raw outputs, metadata association, exact assembly edges, source identity
and static-export hashes. All nine cases pass, and twelve deliberately corrupted
reports are rejected. The first validator run rejected the file-version field
because it expected dotted formatting; the actual resource uses comma-separated
components. The validator now requires that exact original string. No original
result was changed.

The tracked ledger `dx11-debugger-availability-evidence.json` pins the original
probe reports, Ghidra exports and scripts. Original binaries, raw exports and
Ghidra databases remain outside Git under ignored research artifacts.

## Reproduction

```powershell
tools/ghidra/Invoke-DebuggerAvailabilityResearch.ps1 -Headless D:/CDXrepo/utils/Ghidra/Analyze-Headless.bat -ProjectDirectory artifacts/dx11-native/ghidra -OutputDirectory artifacts/dx11-native/debugger-availability-rerun/static
python tools/probe_original_debug_availability.py --out artifacts/dx11-native/debugger-availability-rerun/probe
python tools/validate_debug_availability.py artifacts/dx11-native/debugger-availability-rerun --out <new-ledger.json>
```

Use fresh evidence destinations and the already imported, hash-matching research
program. The scripts reject changed binaries and incomplete exports.

## Consequence and remaining work

Normal presenter-property reads in this build cannot demonstrate an enabled
debugger. This narrows the next investigation but does not close it. The player
request entry at `0x902b0` still requires a verified nonzero assignment,
constructor, implementation vtable and module for host `+0x3c0`. Neither this
request forwarding path nor Shader Profiler is a recovered shader-debug engine.
Product code does not load this private module. Full P1/P5/P6 parity and P7/P8
dependency removal and release acceptance remain incomplete.
