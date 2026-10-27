# Debugger host alias store investigation

This P1 continuation follows the owner aliases established in
`DX11_DEBUGGER_HOST_INITIALIZATION.md`. It eliminates concrete false-positive
field matches. It does not recover the debugger engine, prove its absence, or
change product execution/debug routing.

Reproducible research tools: commit `345388b915bfdb9101456bdfd9154ef5b0908131`.

## Binary and search boundary

The installed `dx11_player.dll` is version `25.1.26603.47871`, image base
`0x180000000`, SHA-256
`39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b`.
The original file is checked before and after research. Ghidra uses the separate
`Dx11NativeResearch` database with `-readOnly -noanalysis`; each export must match
that identity. Original files and the database are not committed.

An access through host subobjects `+8`, `+0x48` or `+0xf8` could reach the candidate
engine at host `+0x3c0` using displacements `0x3b8`, `0x378` or `0x2c8` respectively.
These are search hypotheses, not proof that an arbitrary register holds a host
subobject. The decoded-instruction scan finds 33 first-operand memory-store
candidates and 25 LEA candidates. It does not cover arbitrary pointer arithmetic,
bulk copies, unrecognized code, external modules or every alias path.

| Candidate family | Count | Verified distinguishing evidence |
| --- | ---: | --- |
| Direct RSP stores | 20 | Stack operands in the exported instruction list |
| RBP stores in `0x114bd0` | 2 | `0x114bdc` derives RBP from RSP minus `0x3f0` |
| Host lifecycle stores at offset `0x3b8` | 3 | Existing host constructor, teardown and initializer use the host base itself; these are the neighboring field |
| D3D11 manager initialization | 1 | `0x34411` writes manager offset `0x2c8`, within the independently identified manager constructor `0x340f0` |
| State-block constructor/destructor | 6 | `0xb2870` and `0xb3170` use the state-block object, confirmed by concrete tables and the original-code probe below |
| Cached-array copying in `0x6cf70` | 1 | `0x6d3b8` sets RCX to the input object plus `0x38ac` before the loop containing store `0x6d787`; scalar `0x378` alone is not the destination's object offset |

The LEA candidates include six stack addresses, fifteen manager collection
addresses and four state-block addresses. The retained functions show the base
register assignments, manager table references and state-block table stores.
The manager implementation table at `0x17ae40` references `0x4c530` at slot
`+0x28` and `0x3ebd0` at `+0xc0`; those functions preserve their input manager in
R12 and R14. State-block copy constructor `0x35d60` installs table `0x179318`.
These classifications do not establish whole-program alias closure.

## Concrete state-block path

`0xb2870` installs `Playback::StateBlockD3D11` table `0x179318`, writes its second
argument at `+8`, and initializes fields through `+0x488`. The non-deleting
destructor `0xb3170` ultimately installs base table `0x1792c8`. The deleting
wrapper at `0x3a790` calls that destructor at `0x3a79f`.

The actual constructor caller is `0x61640`: instruction `0x61660` advances RCX
by `0x10`, then `0x61667` calls `0xb2870`. Its outer tables are `0x17f8a8` and
`0x17f8f0`, identified as the state-block base specialization and
`Playback::D3D11Manager::StateBlock`. This is an embedded state block; it is not
the host's candidate debugger implementation.

`probe_original_state_block_layout.py` calls the original constructor and
non-deleting destructor in a child process with a 30-second deadline. Two guard
patterns and three marker values produce six independent lifecycles. The probe
checks every byte of the `0x490`-byte object and both `0x40`-byte guards, including
unwritten padding. The returned constructor pointer must equal the input.
Runtime RTTI independently identifies `.?AVStateBlockD3D11@Playback@@`; the
complete-object locator, type, hierarchy and table RVAs are retained.

All six lifecycles passed in one 94-ms child run. No device, capture, shader or
debugger request is executed. The validator verifies all twelve final Ghidra
exports, the original memory images, source/file hashes and deadline completion.
Thirty altered memory reports are rejected. The result does not rely only on a
reported success boolean or a decompiler's type name.

An initial caller export selected nearby function `0x60fb0`, which does not
contain call `0x61667`; the validator rejected the missing call evidence. The
corrected final recipe selects `0x61640`. Initial exports and the failed validator
log are retained, and the call-edge requirement remains unchanged.

## Reproduction and remaining work

```powershell
tools/ghidra/Invoke-HostAliasResearch.ps1 -Headless <analyze-headless> -ProjectDirectory <existing-research-project> -OutputDirectory <new-root>/verified-final
python tools/probe_original_state_block_layout.py --out <new-root>/layout
python tools/validate_host_alias_research.py <new-root> --out <new-ledger.json>
```

The tracked evidence ledger pins the local raw exports and probe records. This
research narrows candidate stores around known host subobjects. A non-null
assignment to host `+0x3c0`, its constructor, implementation table and module are
still unresolved. P1 is not closed; full native Pixel History, public PS debug
parity, removal and standalone release acceptance remain required.
