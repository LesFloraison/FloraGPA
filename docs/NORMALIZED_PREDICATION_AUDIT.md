# Replay captured conditions when the Predicate descriptor is absent

Reviewed 2026-10-06. Eight unchanged original captures that previously failed
now replay correctly from saved conditional-execution evidence. This restores
control flow on the GPU; it does not reconstruct an absent Predicate descriptor,
the application's old query value or its original comparison argument.

## Original implementation evidence

Read-only Ghidra analysis of the pinned GPA 2025 R1 `shimd3d64.dll`
(`cb0b99d113511cbf7ca9f949d97aa02e4a1f8f110b1f3ad3165d8a81035dee31`)
locates the same conversion in all five SetPredication wrappers:

| Interface | Wrapper RVA |
|---|---|
| Context | `0x23f6a0` |
| Context1 | `0x1f7d70` |
| Context2 | `0x1abe60` |
| Context3 | `0x15ee60` |
| Context4 | `0x1112c0` |

After forwarding the application's native call, the capture path calls GetData
for a four-byte result. **Only when GetData returns S_OK** does it call
Predicate GetPrivateData for the marker at RVA `0x54ac80`, whose wire bytes are
`00df6068758ab648a15ea89f698bf81f`. Begin/End wrappers write the current capture
identifier to this marker. If the lookup fails, or its returned identifier differs
from the current capture, the setter stores:

```text
saved_comparison = uint32(original_comparison != completed_query_word)
```

Otherwise it keeps the original comparison. The native application receives its
original argument; the conversion changes the subsequently serialized argument.
The missing-descriptor originals contain a linked `0x3166` GetPrivateData record
with `DXGI_ERROR_NOT_FOUND`. Its parent identifies the enclosing SetPredication.
This proves both successful GetData and the conversion branch, even in files
without a separate application GetData call. It explains the previously measured
four-combination truth table; that observation alone was not sufficient proof.

Ghidra output, scripts and hashes are retained under
`artifacts/m4-predicate-normalization/`, with the index in `evidence.json`.
The reference workspace and GPA DLLs were not modified. A successful marker
lookup is deliberately not accepted: its data is represented by an opaque pointer,
so the file does not supply the bytes needed for the identifier comparison.

## Native implementation and scope

`auditNormalizedPredication` accepts a missing-descriptor ERG `0x248` binding only
when all of these conditions hold:

- The setter is top-level, uses an immediate context and stores canonical 0/1.
- Exactly one linked Predicate GetPrivateData child has the same resource ID,
  the exact marker GUID and HRESULT, a valid size-presence flag, zero returned
  size, a nonzero opaque data pointer and no trailing/truncated bytes.
- No saved resource occupies the ID. No captured Begin/End or frame-time
  creation establishes a conflicting identity or an unknown query lifecycle.
- The proof belongs to this setter. It does not authorize earlier snapshots,
  another setter, another resource, or stale snapshots after ClearState.

The implementation never dereferences the captured pointer. It uses a private
empty FALSE occlusion query as a native condition carrier. With the proven
normalized comparison, native predication executes exactly when the saved
condition permits it. This preserves Draw, Clear, Copy and Dispatch behavior
without shader patches or manually skipping events. The carrier is an internal
implementation object, not a fabricated captured resource descriptor.

The offline report includes `captured_predicate_condition`, the setter/resource
and witness event. The inspector returns `status=captured_condition`, a null
query value, `descriptor_available=false`, and a separate boolean for whether
the currently bound condition allows execution. Qt displays **Captured condition**
and lists the entry as **Condition ID**. Before the setter establishes it, query
inspection still fails explicitly; a later valid boundary can be inspected in
the same UI session. Existing experiment editing/undo/redo can change this
setter's saved comparison or unbind it; it cannot borrow another unproven ID.

This is a GPA 2025 R1 recorded-control recovery rule, not a general query-history
reconstruction. A missing witness, successful marker lookup, noncanonical saved
value, frame-created identity, or unknown interval retains explicit rejection.
Original noncanonical BOOL arguments cannot be reconstructed from a normalized
0/1 field. The producer oracles cover canonical source comparisons.

## Verification

The existing original corpus is reused without rewriting any capture. Modes
0–3 and 8–11 now pass exact producer-image checks on hardware and WARP, twice
per Replay instance: **32 unobserved native runs**. Modes 4–7 and 12–15 retain
their existing behavior. The self-owned producer's 384 native/injected frames
and frozen sources remain the independent oracle.

Fresh paired package runs verify all 16 files twice. All FloraGPA results match
producer RGBA exactly. The original player still fails to open the eight
missing-descriptor files with status 13; the other eight files match both
implementations. This is not a claim that the failing original player validates
our recovery. Its adapter is also unidentified, so matched-device equivalence
is not asserted. Another **48 hardware/WARP inspection processes** check the
wire witnesses, first-use conditions and completed-query boundaries.

Five serial CTests pass with the original corpora configured. The normalized
suite includes 16 malformed/conflicting-evidence variants, 50 invalid witness
lengths, three out-of-scope uses, conditional Draw/Clear/Copy/Dispatch, repeated
replays, inspection isolation, and edit/undo/redo. Existing Predicate/creation,
API parsing and offline validation tests pass. Three Qt workflows verify the
new condition, failure then retry, baseline inspection and stale-result handling;
the rendered condition pane has been inspected.

## Reproduction and remaining work

```powershell
$env:FLORA_INITIAL_PREDICATE_CAPTURES='D:/CDXrepo/FloraGPA-Cpp/artifacts/m4-initial-predicate-expanded'
ctest --test-dir build/vs2022 -C Release -R '^normalized_predication$' --output-on-failure -j 1
python tools/validate_initial_predicates.py --captures artifacts/m4-initial-predicate-expanded --comparison artifacts/m4-normalized-predicate-final --exe out/FloraGPA-captured-conditions-final-20261006/FloraGPA.Cli.exe --qt-bin D:/Qt/6.11.2/msvc2022_64/bin --out artifacts/new-normalized-inspection --recovered-conditions
```

GPU jobs are serial. Captures, binaries, decompiled code and generated evidence
stay outside Git. GPA/Python remain development-only dependencies. Full Query
CPU control flow, unproven normalization branches, other versions, absent
historical state, traditional Command Lists, long-duration recovery and clean
machine deployment remain open. M3/M4/M5 are not marked complete; the migration
ledger remains 72 ported / 117 partial / 15 pending.
