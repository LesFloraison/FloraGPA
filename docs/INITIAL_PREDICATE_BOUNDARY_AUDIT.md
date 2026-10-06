# Initial Predicate state: replay baseline versus application result

Historical baseline. The later [normalization audit](NORMALIZED_PREDICATION_AUDIT.md)
supersedes the eight missing-descriptor rejections below when an exact linked
capture-marker witness proves the saved execution condition. Those original
files now replay without reconstructing their absent descriptors or historical
query values. The measurements below describe the preceding implementation.

Reviewed 2026-10-06. This M4 change improves diagnostics and inspection; it does
not remove the original player's empty Begin/End initialization or manufacture
a missing Predicate descriptor. Runtime remains C++/Qt with no GPA/Python backend.

## Real original capture evidence

`FloraInitialPredicateProbe` creates one occlusion Predicate before the frame loop.
The four combinations of query result and comparison BOOL are exercised under
four histories. Every native and GPA-injected producer run checks twelve frames,
including independent CPU expected pixels: **16 files, 32 producer runs, 384 frames**.
Sources, executable and GPA DLL hashes are frozen with the captures. None of the
capture files is rewritten. The producer's late-query variants preserve the
conditional output in a staging texture before issuing later queries, then copy
that output back before Present.

| Modes | History at first conditional Draw | Saved Predicate descriptor | Result |
|---|---|---|---|
| 0–3 | Query completed before the captured frame | Absent | Both players fail; original open status 13 |
| 4–7 | Captured Begin/Draw/End before first use | Present | Both players match producer pixels |
| 8–11 | Preframe query plus an in-frame GetData observation | Absent | Both players fail; GetData does not supply the missing descriptor |
| 12–15 | First use precedes later captured Begin/End | Present | Both players match producer pixels using the original empty-query baseline |

For modes 0–3, 8–11 and 12–15, the saved first ERG SetPredication BOOL equals
`producer_query_result XOR producer_comparison`. For modes 4–7 it equals the
producer comparison. This is a measured truth table for these pinned files,
not a general rule inferred for every GPA version, hint Predicate or SO query.
In particular, rejecting all preframe Predicates or deleting empty initialization
would break four valid original captures. Matching output also does not recover
the application's historical query value.

Modes 8–11 really do save the successful four-byte BOOL in Context4 GetData
(`0x34fb`). The validator checks those bytes against the producer. The missing
piece in those files is the Predicate descriptor/identity record, not the BOOL;
the two kinds of missing information must not be conflated.

The existing reverse-engineering evidence in the reference workspace's
`analysis/PREDICATION.md` identifies the player's 24-byte `0x96` descriptor,
factory `0x3d7a0`, reader `0xb6170`, writer `0xbbd40` and native creator `0x574a0`.
The creator issues empty Begin/End. This batch adds original producer evidence
to that earlier static finding; it does not claim to have fully recovered the
shim's general normalization algorithm.

Original comparison uses the installed GPA 2025 R1 player recorded in the
producer manifest. Its adapter is not identified by the private-ABI wrapper.
Byte equality here is an observation, not a matched-device equivalence claim.

## Production behavior

- Offline validation reports `predicate_resource_missing` at the exact setter or
  Draw, including event, resource and record type. Wrong resource types and
  truncated descriptors report `predicate_resource_invalid`.
- A saved Predicate used before a captured interval receives the warning
  `predicate_replay_baseline`. This preserves replay, with the provenance visible.
- Inspection reports `status=replay_baseline`, `value=null` and
  `source=native_player_empty_begin_end`. It no longer labels a fabricated empty
  query as an observed application result. Qt displays **Replay baseline** and **—**.
- Captured Begin/End transitions to the actual replayed GPU result. Frame-time
  creation retains its separate unissued state. Repeated runs reset the provenance.
- Saved GetData records remain read-only observations. No ordinary query CPU
  control flow, missing descriptor or missing historical query is synthesized.

The synthetic Predicate fixture also now specifies its independent Append UAV
counter reset. Its later Dispatch must not depend on an undefined native count.

## Validation and reproduction

Four related serial CTest suites pass: predication (43 Qt rows), predicate_creation
(25 rows, original files configured), frame_validation and api_commands. The
new cases cover missing/wrong/truncated references, future interval boundaries,
repeated hardware/WARP runs and the transition from baseline to a captured result.
The existing parser tests retain all interface variants and truncated lengths.
Two Qt workflows pass, including the real isolated Worker, stale-result handling
and a visually checked baseline screenshot.

The final package comparison has 32 FloraGPA attempts and 32 original attempts.
Eight files match producer pixels in both repetitions; eight fail at the documented
missing descriptor. Another 32 hardware/WARP inspection processes verify query
values and provenance at first-use and completed-interval boundaries.

The full deployed-package gate passes **28 suites / 486 registrations / 476 hashes**:
452 replay-positive registrations (442 unique) and 34 rejection files. It retains
972 ordinary attempts, 668 control runs and 692 resource exports. All previous
470 registrations have unchanged preflight findings and deterministic image
hashes. Helldivers remains the single separately scoped variable case. Four
GF2/BF1 ordinary/disabled-Draw golden checks also pass in the isolated package
configuration. Results and binary/evidence hashes are pinned in
[initial-predicate-boundary-baseline.json](initial-predicate-boundary-baseline.json).

Local evidence:

- `artifacts/m4-initial-predicate-expanded/manifest.json`: original captures and frozen producer.
- `artifacts/m4-initial-predicate-expanded-before/`: previous package comparison.
- `artifacts/m4-initial-predicate-package/`: final package paired comparison.
- `artifacts/m4-initial-predicate-final-inspection/validation.json`: wire truth table and inspections.
- `artifacts/m4-predicate-ui.log` and `artifacts/m4-predicate-ui/`: Qt results/screenshots.

An intermediate attempt to run the undeployed build through the isolated batch
environment failed to load Qt DLLs (`0xc0000135`). It is retained under
`artifacts/m4-initial-predicate-after` and is **not** counted as replay evidence.
The deployed-package rerun above is the valid comparison.

```powershell
cmake --build --preset release --target FloraInitialPredicateProbe
python tools/capture_initial_predicates.py --producer build/vs2022/Release/FloraInitialPredicateProbe.exe --out artifacts/new-predicate-captures
python tools/validate_corpus.py --manifest artifacts/new-predicate-captures/manifest.json --captures-root artifacts/new-predicate-captures --exe out/FloraGPA-predicate-boundaries-20261006/FloraGPA.Cli.exe --out artifacts/new-predicate-comparison --oracle-tools D:/CDXrepo/FloraGPA/tools
python tools/validate_initial_predicates.py --captures artifacts/new-predicate-captures --comparison artifacts/new-predicate-comparison --exe out/FloraGPA-predicate-boundaries-20261006/FloraGPA.Cli.exe --qt-bin D:/Qt/6.11.2/msvc2022_64/bin --out artifacts/new-predicate-inspections
```

`validate_corpus.py` returns failure for the deliberately rejected files; the
registered compatibility gate separately verifies their exact expected outcomes.
Run GPU work serially. Captures remain outside Git.

M4/M5 remain incomplete. General shim normalization, other Predicate/query forms,
frame-before state not preserved by the capture, broader resource boundaries and
long-duration/deployment work remain open. Traditional-list M3 is still unaccepted;
the module ledger remains 72 ported / 117 partial / 15 pending.
