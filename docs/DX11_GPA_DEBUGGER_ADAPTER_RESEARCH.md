# GPA DX11 debugger adapter and wire protocol

This follow-up resolves the concrete DX11 adapter behind the queue field
`+0x1a8` and virtual slot `+0x90` identified in
`DX11_GPA_DEBUGGER_WATCH_RESEARCH.md`. It reaches the already observed player
request entry through an identified protocol command. The adapter is a client
of the player, not the missing shader execution engine. P1's engine constructor,
implementation vtable and supplying module remain unresolved; P6 and final
RenderDoc removal are not certified by these findings.

## Identity and method

The same pinned GPA files and image base `0x180000000` were used:

| Binary | SHA-256 |
| --- | --- |
| `dx11_player.dll` | `39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b` |
| `dx11_playback.dll` | `0a531905386af6c5f2c0357d4179cec208bb171a287b262f3c4a5539d483bf5e` |
| `com/intel/framedb/framedb_qt.dll` | `35a03995d50f390ed74722385583e7bb011f84e563a579c88086d8039ad1df80` |

Ghidra 12.1.4/Java 21 processed the existing independent research database with
`-readOnly -noanalysis`. Installed binaries were hash-checked before and after
the verified recipe. All addresses below are RVAs within the named image.
Decompiler types remain hypotheses; dispatch targets, offsets and scalar widths
were checked against disassembly and runtime table/codec observations.

The new `ExportFieldStores.java` scans candidate first-operand memory stores or
LEA address calculations for a requested displacement. For framedb `+0x1a8` it
finds 133 stores and 108 address calculations. Many are unrelated objects or
stack slots. These counts are not an exhaustive alias analysis and do not prove
the absence of another assignment. The first implementation failed because
unresolved operands can have a null Ghidra reference type; that failed log remains
in the evidence directory. The final scanner uses an explicit set of candidate
x86 write mnemonics and requires subsequent object-provenance verification.

## Concrete factory-to-request chain

| Image / RVA | Verified role |
| --- | --- |
| framedb `0x35e050` | Constructs `PlaybackFanQueue`, including a null `+0x1a8` adapter field |
| framedb `0x3624b0` | Legacy frame initialization obtains a factory/device, calls device slot `+0x18`, and stores the returned shared pointer in queue `+0x1a8` at `0x3629af` / `0x3629b6` |
| framedb `0x5c90f0` | Requests the `GetAPIFactory` export through loader `0x5c76c0` |
| framedb `0x5c76c0` | For the legacy descriptor with API words 1 and 11 and stream flag zero, loads `dx11_playback` and calls the requested factory export |
| playback `0x7400` | `GetAPIFactory` returns the singleton at `0x3b8020` |
| playback `0x2b5058`, slot `+0x8` | Factory vtable points to `0x49a20`, which creates `legacy::Device` |
| playback `0x4c700` | Device constructor installs vtable `0x2b9098` |
| playback `0x2b9098`, slot `+0x18` | Points to `0x4c950`, which allocates a shared `legacy::Playback` and calls constructor `0x56530` |
| playback `0x56530` | Installs `legacy::Playback` vtable `0x2b9e78` |
| playback `0x2b9e78`, slot `+0x90` | Points to debugger client method `0x5f2e0` |
| player `0xa2710`, case 9 | Decodes the request and calls handler `0xab500` |
| player `0xab500` | Supplies result/error callbacks to wrapper `0x88950`, reaching the previously observed request at `0x902b0` |

The 0x408-byte allocation at playback `0x4c950` includes the shared ownership
control block; the playback object begins 0x10 bytes into that allocation.
Queue cleanup `0x3606c0` clears the adapter/control pair and releases it.
Other frame teardown paths also clear the pair. The separate stream path at
`0x366740` uses `GetStreamAPIFactory` and may restore cached objects; its existence
must not be confused with the legacy `.gpa_frame` path. StreamPlayback's concrete
debugger implementation was not investigated here.

The original DLL runtime probe directly calls `GetAPIFactory` and verifies its
returned singleton/vtable and the factory, device and playback slot addresses.
It does not instantiate an original device or playback object. Object construction
and queue assignment in the table above are static call-chain evidence; full UI
initialization through this chain has not been exercised by this probe.

At playback `0x5f2e0`, the request is copied, its event word is converted through
`0x4b300` and `0x257f0`, and command 9 is written through `0x6be00`. The event
conversion's complete mapping is not recovered by the synthetic codec test.
The result is decoded and the original eight request words are copied into its
first 32 bytes. No shader interpreter or engine construction occurs in this
inspected client method.

## Independently observed wire behavior

`tools/probe_original_debugger_wire.py` runs in a bounded child process and calls
the original pinned codec routines against an in-memory transport with two
callbacks. It opens no capture, connects to no server and executes no GPU work.
The transport bounds individual and total writes to 4 KiB. The parent enforces
a 30-second deadline and preserves exit, timeout and child reports. Original
files remain unchanged; this tool is never a product dependency or required
release test.

The request serializer at playback `0x264ff0` emits 17 little-endian uint32 values
from an eight-word, 32-byte request, in this exact zero-based index order:

```text
0, 1, 2, 3, 4, 5, 6, 2, 3, 4, 5, 2, 3, 4, 5, 6, 7
```

With the four-byte command prefix, the observed packet is 72 bytes. It is not a
32-byte struct copy. Player decoder `0x10ba30` consumes the same sequence. The
repeated stores overwrite earlier values at shared offsets; a distinct-value
synthetic packet confirms that the last store wins. This establishes byte layout,
not a shader-stage meaning for the overlapped fields or valid selection values.

The client response helper `0x263d30` reads one-byte markers until it sees zero,
then reads one status byte. Both a direct zero marker/status zero and a sequence
of nonzero markers followed by zero/status one were observed. Arbitrary malformed
streams, transport failure recovery and all marker semantics remain outside this
probe's scope. Its bounded parent prevents the original loop from hanging the
research session indefinitely.

The result reader at playback `0x262690` and writer at player `0x10dd30` agree on
these fields within the 0xc0-byte result object:

| Offset | Container/layout evidence | Runtime validation in this probe |
| --- | --- | --- |
| `+0x00..0x1f` | Request copy performed by the client, outside payload codec | Not exercised by the codec-only call |
| `+0x20`, `+0x38` | Vector descriptors; static element stride `0xd0` | Empty vectors |
| `+0x50` | Vector descriptor; static element stride `0x50` | Empty vector |
| `+0x68` | MSVC byte-string object | Empty and inline sentinel string |
| `+0x88` | Vector of MSVC strings; static element stride `0x20` | Empty vector |
| `+0xa0` | MSVC byte-string object | Empty and inline sentinel string |

Counts and string lengths are serialized as eight-byte values. The two nonempty
sentinel strings are `assembly` and `source`; those chosen fixture values do not
prove the actual fields' semantic names. The containers start empty, with inline
string capacity 15. This avoids claiming behavior for reused containers, heap
ownership or nonempty trace records. Register validity, instruction steps, source
positions, call stacks and shader execution semantics are still unverified.

Nine original-binary cases passed: request serialization, cross-module request
roundtrip, conflicting repeated-field decoding, two status patterns, two result
decodes and two cross-module result roundtrips. The default-result path visible
in the player dispatcher is consistent with the earlier null-engine requests
returning success without a callback. That connection is a static inference,
not an end-to-end valid-shader trace test.

## Reproduction and remaining work

```powershell
tools/ghidra/Invoke-DebuggerAdapterResearch.ps1 `
  -Headless D:/CDXrepo/utils/Ghidra/Analyze-Headless.bat `
  -ProjectDirectory artifacts/dx11-native/ghidra `
  -OutputDirectory artifacts/dx11-native/debugger-adapter-new/verified
python tools/probe_original_debugger_wire.py `
  --out artifacts/dx11-native/debugger-adapter-new/wire-cross-module
```

Use fresh output directories and the research database already containing the
three pinned binaries. The recipe checks hashes, exporter completion, missing
functions and script errors. Seventeen exports completed in the verified run.
The first five-case codec run is retained separately from the final nine-case
cross-module run. Raw exports, process reports and the failed initial scanner
remain under ignored `artifacts/dx11-native/debugger-adapter/`.

`tools/summarize_debugger_adapter.py --root
artifacts/dx11-native/debugger-adapter --out
docs/research/dx11-debugger-adapter-evidence.json` validates and summarizes the
retained session, including the failed scanner log. It rejects altered hashes
and incomplete exports. An intentionally altered copied export was rejected before
ledger output; the original evidence subsequently passed validation again.
Original binaries, capture files, raw decompilation and
the Ghidra database remain outside Git. Product code and tests were not changed;
no new product regression-suite result is claimed by this research checkpoint.

The next engine-recovery target remains the player host field `+0x3c0`, including
initialization routes/settings not covered by the earlier synchronous probes.
The concrete client adapter and protocol are now identified, so they should no
longer be treated as candidate shader interpreters. The independent C++ DXBC
executor still needs the remaining VS/PS/CS semantics and production integration;
full Pixel History and standalone removal/acceptance gates remain open.
