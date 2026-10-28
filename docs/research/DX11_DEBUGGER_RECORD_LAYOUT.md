# Original GPA debugger result record layout

This P1 continuation validates nonempty result containers beyond the empty-vector
codec probes in `../DX11_GPA_DEBUGGER_ADAPTER_RESEARCH.md`. It establishes byte
layout and serialization behavior, not the meaning of the fields or execution of
a shader. The engine constructor, non-null host `+0x3c0` assignment, implementation
vtable and supplying module remain unresolved. No product routing changes here.

The reproducible research tools are in commit
`2f3c3737a1d680d074ec3639d112e6477b696f56`. They are optional research tools, not
required release tests. Production continues to use project-owned analysis types
and the independently implemented DXBC executor, without loading GPA DLLs.

## Binary identity and static evidence

Both installed files are version `25.1.26603.47871`, image base `0x180000000`.

| Image | SHA-256 |
| --- | --- |
| `dx11_player.dll` | `39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b` |
| `dx11_playback.dll` | `0a531905386af6c5f2c0357d4179cec208bb171a287b262f3c4a5539d483bf5e` |

The wrapper checks installed hashes before and after Ghidra exports. It uses the
separate `Dx11NativeResearch` project with `-readOnly -noanalysis` and checks the
exported program identity. Fourteen functions were exported with disassembly,
incoming references and separately labelled decompiler hypotheses. Originals,
database and raw exports remain outside Git.

| Function | Player writer RVA | Playback reader RVA |
| --- | ---: | ---: |
| Result container | `0x10dd30` | `0x262690` |
| Recursive 0xd0-byte records | `0x10d0b0` | `0x261470` |
| 0x50-byte grouped records | `0x10cec0` | `0x2611b0` |
| String vector | `0x109890` | `0x2617d0` |
| Byte string | `0x1097d0` | `0x25c810` |
| Four-byte scalar | `0x109630` | `0x25c660` |
| Byte-span transfer | `0x10dbd0` | `0x262620` |

The result retains its previously identified 0xc0-byte container: recursive
vectors at `+0x20` and `+0x38`, grouped-record vector at `+0x50`, strings at
`+0x68` and `+0xa0`, and string vector at `+0x88`. The first 32 bytes are not
transmitted by this payload codec. The probe confirms those bytes stay unchanged.

The recursive record has these fields, serialized in table order:

| Memory offset | Observed transfer |
| --- | --- |
| `+0x00` | Four raw bytes |
| `+0x04` | Four raw bytes |
| `+0x08` | String: eight-byte length followed by that many bytes |
| `+0x28` | Four raw bytes |
| `+0x30..0xaf` | Exactly 128 raw bytes |
| `+0xb0` | One raw byte |
| `+0xb8` | Recursive vector of the same 0xd0-byte records |

Memory padding at `+0x2c..0x2f` and `+0xb1..0xb7` is not transmitted. Writer
`0x10d1c3` and reader `0x261611` establish the 128-byte width; recursive calls at
`0x10d20c` and `0x261658` establish the nested shape. The 0x50-byte grouped record
contains three recursive vectors at `+0`, `+0x18`, `+0x30`, followed by two
four-byte fields at `+0x48` and `+0x4c`. Counts are eight-byte little-endian values;
vector descriptors and object padding are not wire fields.

These offsets do not identify register numbers, types, dimensions, source lines,
execution steps, call stacks or validity flags. The one-byte field preserves
nonzero values other than one in this codec; that does not establish its domain
in real debugger output. Likewise, the 128-byte payload is not labelled as a
specific register or lane arrangement without consumer or execution evidence.

## Independent original-code probe

Twelve cases combine four shapes (leaf, recursively nested, grouped, mixed) with
three bit-pattern seeds. All passed. The mixed shape includes a 15-byte inline
string and a string containing an embedded NUL. Payloads include raw patterns
that would represent signed zero, infinities and distinct NaNs if interpreted
as floats; the probe never converts them to floats. Every byte survives the
original player writer and playback reader unchanged.

The research child owns every allocation. Vectors already have the exact target
size and capacity, nested descriptors point to separately guarded storage, and
strings use the 15-byte inline capacity. No original destructor receives this
storage. Guard and descriptor checks reject writes beyond the supplied objects
or changes to their allocation pointers. This does not test original container
growth, shrinking, heap strings, exceptions or allocator interoperability.

The probe constructs an expected packet independently of the original codec,
checks both transfers and bytes, decodes into prefilled destination memory, and
serializes that destination again. The validator separately parses the retained
wire bytes and reconstructs every guarded allocation from the fixture and its
recorded addresses. It checks payload, padding, untransmitted request bytes,
container pointers, string storage and guards, rather than trusting a success
boolean or a roundtrip alone. All 228 deliberately corrupted reports are
rejected, including altered raw payloads, pointers, guards and transfer widths.

## Empty reuse boundary

After each nonempty decode, a second packet supplies six zero counts/lengths.
All twelve observations consume exactly 48 bytes and leave the complete prior
object graph unchanged. They do not clear the previously populated vectors or
strings. Static evidence agrees: the recursive reader subtracts one from the
count at `0x2614ca` and branches past mutation at `0x2614d1` for zero; the string
reader has the corresponding check at `0x25c86a..0x25c871`. Grouped records and
the string vector also have zero-count skip paths.

This is an observed codec reuse behavior, not evidence that GPA's normal caller
reuses such objects, nor a requirement to reproduce stale values in the native
backend. Project-owned results must continue to distinguish absent information
and empty containers from previous results.

The first exploratory probe failed its transfer-boundary assertion even though
the packet bytes matched: the writer invokes its byte-span callback with length
zero for an empty string, whereas the reader skips that callback. The diagnostic
records preserve both lists. The corrected expectation retains exact bytes and
checks the distinct read and write boundaries. The first passing run used the
fixture label `steps`; final scripts use `groups` to avoid implying recovered
execution-step semantics. Only `final-observed` is the final runtime matrix.

## Reproduction and limits

```powershell
tools/ghidra/Invoke-DebuggerRecordResearch.ps1 -Headless <analyze-headless> -ProjectDirectory <existing-project> -OutputDirectory <new-root>/static-final
python tools/probe_original_debugger_records.py --out <new-root>/final
python tools/validate_debugger_records.py <new-root> --out <new-ledger.json>
```

Use new output directories. The child has a 30-second deadline and bounded
transport; a timeout terminates and waits for the owned process. Local final
validation used `--runtime final-observed` under
`artifacts/dx11-native/debugger-records`. The tracked ledger pins final exports,
runtime reports, earlier diagnostics and tool sources. Installed files were
unchanged. No capture, D3D device, GPU replay or shader engine was used; no
product regression or performance result is claimed by this research-only work.

This closes the synthetic nonempty-record layout gap. P1 engine recovery,
semantic trace mapping, full Pixel History, public PS debug parity and P7/P8
dependency removal and standalone release acceptance remain open.
