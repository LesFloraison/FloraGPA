# Traditional Execute dispatch search and M3 acceptance gate

Reviewed 2026-10-05. This batch investigates the unresolved traditional-list
execution path in the pinned GPA 2025 R1 binaries. It rules out a concrete
player-side candidate and documents a previously missed forwarding thunk. It
does not enable production replay or establish that no other implementation
could exist. See the [baseline](execute-dispatch-search-baseline.json).

## Findings

The read-only scanner disassembles the complete executable section ranges of
three hash-pinned files. It records literal memory reads and indirect call/jump
operands at offset `0x1d0`, the x64 base-context Execute slot. It also retains
skipped data, stack-relative reads and function-range lookup results. Files are
neither loaded nor modified.

| Binary | Literal call/jump candidates | All retained reads | Skipped data records |
|---|---:|---:|---:|
| `dx11_player.dll` | 1 | 26 | 1,257 |
| `shimd3d64.dll` | 17 | 346 | 1,092 |
| `dx11_playback.dll` | 0 | 15 | 624 |

These counts describe this instruction pattern, not all possible indirect
execution. Computed offsets, register dispatch and skipped bytes prevent a
whole-program absence claim. A matching offset does not by itself prove that
the receiver is an `ID3D11DeviceContext`.

The player's candidate at `0x4da2f`, function `0x4d7c0`, is an internal MSAA
resource helper. The decompiled path references `DX11ResolveMS.h`, records work
on a helper deferred context, finishes a newly produced list with FALSE, then
executes that local object with TRUE. The argument comes from that Finish output,
not the saved traditional Execute operand. It therefore supplies no file-to-list
identity rule. Other reviewed non-stack pointer loads at this displacement are
object fields rather than demonstrated list execution sites.

The complete section scan finds a shim leaf thunk that an unwind-function-only
scan omitted: `0x2e41a0`, tail jump at `0x2e41a7`. Its vtable is `0x4cf238`, slot
58; the PE RTTI names it `DX11DeviceContextWrapper`. The fourteen instruction
bytes replace RCX with `[RCX+8]`, load its vtable, and forward via slot `0x1d0`.
RDX/R8 arguments are unchanged. This proves a native forwarding layer, not the
meaning of a serialized capture operand. Its target and RTTI are pinned by the
scanner alongside the raw bytes.

The shim candidates also include the five previously recovered interface
wrappers and a generic switch/function fragment at `0x2d9a3c`. The latter has a
matching displacement under case `0x30c5`; the match must not be relabeled as
serialized Execute merely because of its offset.

The player's known API Execute and ERG Execute vtable entries both still resolve
to `0x166a0`, whose bytes are `c2 00 00` (`ret 0`). This is a positive byte-level
check of those exact slots, supplementing the earlier actual native-record probe;
it is not an inference from a missing symbol or empty search result.

## Why this affects the roadmap

The accepted M3 criterion requires an unmodified original capture containing real
traditional lists. Existing application-deferred captures in the registered corpus
use the expanded immediate path. Controlled research copies demonstrated native
list building, but their outer original playback needs intervention and their
manual system Execute calls do not recover file operand identity. They remain
ineligible as M3 execution acceptance.

The initial dependency collectors and C++ scheduler are now verified prerequisites.
Adding more metadata tests cannot establish the missing operand binding or turn
these research copies into original captures. The present environment still has
no qualifying original traditional-list execution sample and no verified active
player path from which to derive that binding.

The full M1–M6 goal and M3 acceptance definition are unchanged. M3 is **not
complete**, and its unsupported paths remain explicit. Under the user's fixed
M1 → M2 → M3 → M4 → M5 order, moving to M4 requires a priority exception from the
user. A concrete proposed exception is:

1. Keep M3 open, preserve all execution rejections and record the missing original
   sample/operand evidence as its acceptance blocker.
2. Proceed with M4 resource recovery and boundary behavior, then M5 stability for
   the already supported capture scope. Do not call these results M3 acceptance.
3. Resume traditional-list execution when a qualifying capture and reference
   execution path become available; retain M6 as the later analyzer/version phase.

This is a proposal, not an applied change of scope or stage order. No new capture
format, guessed pointer binding or experimental override is enabled.

## Verification and reproduction

Five scanner tests cover indirect call, tail jump, register-load candidates,
stack reads, other offsets/writes/address calculations, and explicit undecodable
trailing bytes. All 48 related evidence tests pass. Only development tools and
documentation changed; CTest, GPU, GUI and packaging were not rerun in this batch.
Corpus and module migration counts are unchanged.

```powershell
python tools/audit_original_execute_sites.py --out <fresh-audit>
python -m unittest discover -s tests -p test_original_execute_sites.py -v
```

The scanner uses the existing development `pefile` and `capstone` packages.
Runtime FloraGPA remains independent of Python and GPA. Ghidra runs were read-only
against the existing project; local decompilations and scan reports remain outside
Git, identified by the baseline hashes.
