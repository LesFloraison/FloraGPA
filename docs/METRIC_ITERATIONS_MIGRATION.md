# Numeric metric iterations and FrameFile range indexing

The native application library now implements `metric_outer_passes.py`,
`metric_iterations.py`, `metric_range_mapping.py`, `metric_query_flags.py` and
`frame_metric_index.py`. The first four retain the recovered numeric protocol
over a caller-owned transport. The frame index is also available through the
production CLI and worker:

```powershell
.\out\FloraGPA-metric-iterations\FloraGPA.Cli.exe metric-index D:\captures\sample.gpa_frame --out D:\results\index
```

This writes `metric-index.json` and the normal native `report.json`. The output
directory must be new or empty. No Python or GPA installation is used by this
command. Its category scope is explicitly `[2]`; it does not claim to recover
every FrameFile category or metadata-provider side effect.

## Preserved behavior

- Pass selection validates uint32 selectors and the preparation flag, handles
  zero groups and all-pass selection, and represents large selections lazily.
  The outer receiver preserves cumulative duplicate metric rows, moved-from
  empty timestamp groups, partial results, cancellation checks and exact
  prepare/replay ordering. Successful runs finish with empty preparation;
  early returns leave resource cleanup to the transport owner.
- The iteration runner retains weight-name precedence, supplied weights,
  mapped passes, the pass-zero sample count, and direct non-metric replay.
  A failed weight assertion performs exactly two additional calls with the
  previous result. This can return outer `success` with `complete: false`.
  `collect` rejects incomplete acquisition while preserving its partial result.
  Consumers must inspect completeness before publishing data.
- Numeric assembly retains duplicate overwrite order, unrequested initial
  columns, group order, binary64 statistics, boolean numeric conversion and
  strict integer identity/range validation. Timestamp pairs preserve uint64
  precision; the first parallel group replaces previous timestamps, including
  an empty first group. Query flags use catalog labels, consume only kind-zero
  descriptors, limit masks to 64 bits, merge/deduplicate labels, preserve prior
  samples and reject malformed shapes before aggregation.
- Range mapping preserves captured order and overlap endpoints. An existing
  empty category-2 table wins over fallback. Missing overlaps yield `(0, 0)`;
  endpoints outside the internal-ID table yield `UINT32_MAX`. These are
  protocol values, not proof that a range can execute on a GPU.
- Frame indexing reads ordered API entries with the checked native reader and
  the original static inventory of 1,547 supported decoder types. Generic
  records use the low uint32 target at payload offset 8 and require a supported
  context resource. Trailing calls remain in the internal-ID list without
  creating an extra range. Unsupported/non-context records retain exclusion
  reasons; truncated object headers and oversized accepted IDs fail explicitly.

`MetricIterationTransport` separates the numeric protocol from GPU acquisition.
The full profiling pass controller, production metric transport, worker flow
and Intel metric Qt controls still require migration/integration. This batch
does not add placeholder analyzer controls or claim an end-to-end profiling UI.

## Validation

The packaged probe passed **2,758** exact Python/C++ comparisons, including
**1,907** saved original GPA observations. Floating values use binary64 bit
comparisons against Python with NaNs classified separately; saved GPA numeric
fields use a tolerance of `1e-14`. The saved cases cover:

| Original observation group | Cases |
| --- | ---: |
| Range mapping | 510 |
| Outer receiver | 112 |
| Iteration policy | 63 |
| Iteration failures | 40 |
| Sample times | 293 |
| Query flags | 869 |
| FrameFile index | 20 |

Additional cases check all 65,536 uint16 classifications, the complete static
decoder inventory, randomized ranges/flags/duplicate columns, strict bounds,
validation order, cancellation, transport failures and partial-result handling.
The test harness decodes saved wire responses into the injectable transport;
this is test input reconstruction, not a production GPA wire connection.
Reference modules, saved observation manifests, binaries where provided and
FrameFile oracle fixtures have recorded or verified SHA-256 provenance.

Both real captures also pass production CLI JSON comparison and module audits:

| Capture | Internal APIs | Category-2 ranges | Excluded APIs |
| --- | ---: | ---: | ---: |
| GF2 | 864 | 75 | 56 |
| BF1 | 17,995 | 1,313 | 29 |

Full Release compilation passed, followed by rebuilding the final CLI/worker
and probe changes. Ten related CTest suites passed, including real Intel
Metrics Discovery integration. The existing analysis regression passed 3,043
Python comparisons and 479 saved GPA checks. Packaged iteration and Intel
tests each passed five Qt Test cases without skips. GF2/BF1 golden replays and
both draw-suppression negative controls passed. CLI runtime audits found no
Python, Tk, GPA or RenderDoc modules and verified package-local Qt DLLs.

Initial harness corrections fixed nested FrameFile fallback lookup and the
saved range oracle's non-category-2 fallback selection. Implementation fixes
preserved boolean numeric conversion and the reference's validation order.
The final checks above include those corrections; earlier failed artifacts
remain available locally rather than being presented as successful runs.

Local evidence is ignored by Git:

- `artifacts/build-iterations-release-final.log`
- `artifacts/build-iterations-delivery.log`
- `artifacts/ctest-iterations-release.log`
- `artifacts/iterations-package-parity/validation.json`
- `artifacts/iterations-package-tests.txt`
- `artifacts/iterations-package-integration.txt`
- `artifacts/iterations-analysis-regression/validation.json`
- `artifacts/iterations-package-golden/validation.json`
- `artifacts/iterations-delivery-audit.json`

The complete portable directory is `out/FloraGPA-metric-iterations/`.
Tests run with package-local Qt and Windows system paths; test-only binaries
are moved out of the final package. This is validation on the current host,
not a separate clean Windows installation. Module status counts describe
inventory entries, not a percentage of the total migration workload.
