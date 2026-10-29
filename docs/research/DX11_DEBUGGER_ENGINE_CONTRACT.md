# Original debugger receiver and cleanup contracts

This P1 increment validates the caller of the unresolved host `+0x3c0` object.
It does not find an original engine assignment, constructor, implementation table
or supplying module. The receiver used here is explicitly synthetic. Product
code, debug routing and required release tests are unchanged.

Research tools: commit `34f838b670b101e1df5bee4cddcd3b707ac3514b`.

## Binary and decoded evidence

The installed `dx11_player.dll` is version `25.1.26603.47871`, preferred image base
`0x180000000`, SHA-256
`39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b`.
The recipe checks this identity before and after Ghidra exports, using the separate
research database with `-readOnly -noanalysis`. Five functions and two table
locations are exported. Assembly and decompiler hypotheses remain distinct.

| Location | Decoded caller behavior |
| --- | --- |
| `0x902e4` / `0x902eb` | Load the owner through request-manager `+0x98`, then load owner `+0x3c0`. |
| `0x902f7` to `0x902fe` | If that receiver is null, return AL = 1 without entering the engine or delivering a result. |
| `0x905d6` to `0x905fb` | Call receiver table slot `+0`: RCX is the receiver, RDX addresses a status word, R8 addresses request `+0x20`, R9D contains request `+0x10`, and the fifth argument addresses the initialized result container. |
| `0x90605` / `0x9060c` | Compare the returned status word with the global at `0x208560`; matching identities take the result-delivery path. |
| `0x90845` to `0x90851` | Invoke the request's result callback with userdata in RCX and the result address in RDX. |
| `0x8a026` to `0x8a03b` | Host cleanup calls a non-null receiver's table slot `+8` with EDX = 1, then clears host `+0x3c0`. |

These are register and memory contracts for this build, not recovered C++ type
names. In particular, the return from the slot-zero call itself is not used to
determine success; the caller examines its output status word. Nonzero status
objects, ownership rules and their semantics remain unqualified.

## Independent dynamic observation

The existing bounded host probe has an explicit `--synthetic-engine-contract`
mode. Default mode still leaves the engine absent. The new mode refuses to
replace a non-null original receiver. During one observed request it supplies a
guarded, child-owned object with two callbacks, then restores the null field in
`finally`. The slot-zero callback records the five arguments, leaves the original
empty result unchanged, and copies the compared global status identity. That
identity is zero in every retained case. It executes no shader interpreter.

After a further original replay, the probe supplies the same object during host
close. The cleanup callback records receiver and flags without freeing storage
owned by Python. After close, the probe reads only its own guarded storage and
the module singleton; it never dereferences the freed host allocation. The
original installation and capture files remain unchanged. The existing temporary
request-table observer is restored. No instruction bytes are patched.

The final matrix has 12 cases: GF2 event 113, BF1 event 876 and Helldivers event
200, each on explicitly selected NVIDIA and Intel adapters, with zero and distinct
opaque selection words. Selection word 1 retains the event. Other marker words
are passed unchanged; this does not establish their shader-selection semantics.

Every case observes exactly one receiver call, one result callback and one
cleanup callback. The latter has flags 1 and the same installed receiver. The
result callback receives the same 0xa0-byte initialized empty container seen by
the receiver. The two string-capacity words at offsets `0x60` and `0x98` are 15;
all other bytes are zero. Guards and the receiver pointer remain intact. The
request returns AL = 1, the outer request reports status zero, and the subsequent
original replay completes on the same adapter. This replay check establishes
completion and adapter continuity, not resource-by-resource state equivalence.

The null-engine control in each case also reports outer status zero, but delivers
no result. Therefore successful request dispatch alone is not evidence of shader
debugging. A separate default-mode GF2/NVIDIA lifecycle confirms the opt-in
extension did not change that observer's routing.

## Validation and reproduction

The independent validator checks the Ghidra instruction evidence, adapter and
request identities, selection bytes, argument pointers, result initialization,
guarded storage, cleanup and the default-mode control. It accepts all 12 final
cases and rejects 96 corrupted reports. Python compilation and staged whitespace
checks pass. No C++ build or product regression-suite result is claimed for these
optional research-only tools.

```powershell
tools/ghidra/Invoke-EngineContractResearch.ps1 -Headless <analyze-headless> -ProjectDirectory <existing-research-project> -OutputDirectory <new-root>/verified
python tools/run_original_engine_contract.py --captures <capture-directory> --out <new-root>/final
python tools/probe_original_debugger_host.py <GF2-frame> --event 113 --vendor 4318 --host-graph --out <new-root>/default-control
python tools/validate_original_engine_contract.py <new-root> --out <new-root>/validation.json
```

GPU jobs run serially. Each child has a 120-second deadline, and the matrix stops
on a failed process. The retained root is
`artifacts/dx11-native/debugger-engine-contract`; its authoritative matrix is
`final-v2`, so its validation command additionally uses `--matrix final-v2`.
Preliminary single cases and the earlier matrix are retained separately and are
not counted twice. The final rerun follows a probe-description clarification;
no runtime assertion was relaxed. The accompanying evidence manifest pins raw
artifacts and committed research sources. Original binaries, captures and Ghidra
databases remain outside Git.

This closes the selected caller/cleanup contract only. An original non-null
assignment, engine constructor, real implementation table and module are still
missing. The previously observed constant-false public availability getter is
consistent with these null lifecycles but does not prove whole-program absence.
P1 engine recovery and full P5/P6/P7/P8 parity/removal/release gates remain open.
