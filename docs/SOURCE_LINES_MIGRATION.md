# Original shader source-line mapping

`ShaderSourceLines` now ports the recovered SPDB C13 and legacy SDBG instruction
line readers. Checkpoint catalogs and captured reports include `source_lines`
and unambiguous per-instruction `source_location` values. Source variables,
source function/inline stacks and the Qt debugger remain pending.

The native reader preserves embedded source bytes, encoding validity, checksums,
original DXBC offsets, C13 base records, module checksums and inlinee tables.
Captured filenames are labels; it never opens source files at captured paths.
Missing debug data stays unavailable. Invalid data is reported explicitly and
does not produce navigation locations. Ambiguous locations are omitted from
the offset map; expression locations take precedence over statement locations
as in the reference implementation.

SPDB checks include the MSF/DBI/module/subsection bounds, code contributions,
source checksums (MD5/SHA1/SHA256), original instruction boundaries and source
line ranges. Navigation uses the system compiler's original `#line` directives;
base C13 records remain available for the future inline-scope reader. Invalid
inlinee tables retain an issue without inventing inline records.

SDBG checks SDK table sizes, instruction IDs/opcodes, token/file bounds and full
instruction coverage. Compiler 47's displayed HS phase markers are excluded
when matching Compiler 43's SDBG instruction sequence. It does not trust the
known-inconsistent SDBG source directives from disassembly.

The existing bounded container/source readers were moved from
`ShaderInspector.cpp` into the internal `ShaderDebugData.h` and shared with this
reader. Whole-buffer UTF-8/UTF-16 replacement decoding now matches Python,
including incomplete final characters, invalid surrogate sequences and BOMs.
Invalid SDBG compiler/entry/target UTF-8 rejects source extraction. Windows
filename normalization uses a generated, locale-independent Unicode 15.1 full
casefold table matching the reference Python runtime; Qt's simple folding alone
does not expand characters such as `ß` and `İ`. Regeneration is development-only
through `tools/generate_unicode_casefold.py`.

## Packaged compiler correction

The initial package test found that Qt deploys a Windows 8.1-era
`D3DCompiler_47.dll` beside the application. On this host it omits many SPDB
`#line` directives and differs from the reference system compiler's SDBG output.
This caused real mapping errors even though the developer build passed.

`SystemDisassembly.h` now loads the absolute system-directory compiler DLL and
resolves `D3DDisassemble` explicitly for both checkpoint catalogs and source-line
decoding. Missing system support fails explicitly. Qt's deployed DLL may remain
loaded for its other consumers; source navigation uses the system backend.
The failed package results remain in `artifacts/source-lines-capture-v1`.

## Validation

`tools/validate_source_lines.py` runs 6,515 comparisons against the preserved
Python implementation, all passing in `artifacts/source-lines-system-final`:

- 182 source-map cases: multi-file SPDB, optimized/unoptimized GS/HS/DS,
  Compiler 40/41/42/43 SDBG, HS phase markers, stripped debug data, corrupted
  checksums and ranges, truncated containers, invalid metadata and text, and
  every Python source-line separator.
- 3,545 path cases, including all 1,530 Unicode full-fold mappings and 2,000
  deterministic Windows path combinations.
- 2,775 text cases covering all single bytes, UTF-16 byte order, incomplete
  UTF-8, surrogate errors, BOMs and deterministic random byte sequences.
- Eight inlinee-table cases and five ambiguous/expression-priority offset cases.

There are 6,390 exact successful results, five matching rejections and 120
results whose issue messages differ while their complete data, status and issue
counts match. Diagnostics retain their original text in the evidence; they are
not claimed to be byte-identical. The first Unicode/encoding/metadata failures
are preserved in the earlier `source-lines-v*` and `source-lines-metadata-v1`
directories rather than being overwritten.

The package test uses `validate_checkpoint_capture.py --source-only`. It compares
source maps and catalog locations in addition to the entire supported capture
report, raw registers and CSVs. Both SPDB and SDBG GS/HS/DS shaders execute on
Hardware and WARP, including every fixture HS phase, original instruction
checkpoints, complete traces and exact input selectors. The final evidence is
`artifacts/source-lines-capture-final/validation.json`.

All 60 package comparisons passed. The deliverable is
`out/FloraGPA-source-debug/FloraGPA.exe` with its CLI/worker and dependencies.
`artifacts/source-lines-runtime-audit.json` confirms system-compiler loading in
every successful capture report, package-local Qt, no Python/Tk/GPA/RenderDoc/DIA
modules, and matching Release/package executable hashes. This is validation on
the current host with isolated child paths, not a separate-machine test.

The complete Release build is recorded in `artifacts/build-source-lines-system.log`.
The checkpoint, post-transform, shader, core and Qt suites passed in
`artifacts/ctest-source-lines-final.log` (53 Qt cases without skips). After the
system-disassembly correction, the four affected native suites passed again in
`artifacts/ctest-source-lines-system.log`. GF2/BF1 golden frames and draw-suppression
controls also passed all four checks in `artifacts/source-lines-golden`.

This is a source-map layer and production-report integration, not a complete
source debugger. No Qt stepping, source-variable table, source call-stack view,
breakpoint or watch capability is claimed by this batch. The full migration goal
remains active.
