# Same-device original GPA replay validation

The pinned GPA 2025 R1 player now has a development-only explicit Intel replay
path. Three fresh original-player processes and three fresh Flora processes
execute the existing Intel GF2 home capture on Intel Iris Xe. Every output is
byte-identical between players, but still differs from captured RGBA by **10
pixels / 20 channels / maximum 6**. Formal acceptance remains **0/6**.

## Device selection evidence

Read-only Ghidra inspection identifies the GPU enumerator at RVA `0x11bd30` and
its selection predicate at `0x11c100`. The enumerated vector contains `0x178`-byte
objects, with PCI vendor/device fields at `0x80`/`0x84`. The predicate compares a
four-byte tag and an eight-byte identity beginning at object offset `0x94`.
The isolated validation adapter copies those 12 bytes from the unique Intel
entry, then releases the vector through `0x04f440`. It does not hardcode this
machine's identity. All calls remain guarded by the original DLL's SHA-256:
`39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b`.

After explicit playback, an audited DrawIndexed context supplies its actual
device through public COM getters. Every run identifies Intel Iris Xe, PCI
vendor `0x8086`, device `0x46a6`. Each executes 176 DrawIndexed calls and 497 ERG
dispatches; Open/Playback return success, no error callback fires, and Close
clears the player core. This verifies actual device selection, beyond a GUI label.

## Exact output and negative control

All six positive outputs hash to
`690fe4d68f3b7f2da906d9ed8c09301882c354a16ab114e8209a5632f14e336f`.
The ten differing pixels occupy x=936..939, y=325..327 (top-left origin).
The earlier first Flora replay exactly matched capture; its result is preserved
and remains unexplained. The new comparisons establish cross-player agreement
for these executions, not complete capture correctness or driver determinism.

A separate original-player negative suppresses all 176 DrawIndexed submissions.
Flora's Draw suppression negative yields the same output hash:
`4ca7171140d4edbe7e4cf812b5b8f7ed4a2b8792b1c82d1380e54f797548438e`.
Both differ from captured reference by 2,199,901 pixels, confirming that normal
results depend on execution rather than preview substitution. The controls have
different API scope: the original audit suppresses DrawIndexed, while Flora
suppresses all Draw submissions.

GPU checks ran serially with GF2 closed. No production source or driver setting
changed. No GPA dependency was added to capture, format or replay. Raw scripts,
decompilation, framebuffer outputs and logs remain ignored under
`artifacts/capture-original-gpu-selection-20261014`; the old workspace is intact.
The [baseline](capture-original-gpu-selection-baseline.json) pins the evidence
manifest, actual-device audits and exact comparisons. Original GUI inspection
and the remaining captured-output discrepancy are still open gates.
