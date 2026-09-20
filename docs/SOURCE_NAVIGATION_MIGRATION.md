# Native source navigation and watch expressions

`DebugExpression` and `SourceTrace` port the recovered expression engine, native
source-frame environments and invocation-local source navigation to C++.
They are library APIs at this stage. The Qt debugger, its saved configuration
and SDBG assignment reconstruction are not yet integrated. The current deployed
application remains `out/FloraGPA-source-stack/FloraGPA.exe`; this batch does not
claim new debugger controls in that package.

## Expression semantics

The bounded parser accepts the original read-only expression language:
arithmetic, integer bitwise operations, comparisons, short-circuit booleans,
ternary selection, mapped members/indices, vector swizzles, constructors,
`all`, `any`, `abs`, `min` and `max`. Limits remain 2,048 characters, 256 tokens
and 40 recursive parser levels. Assignment, memory access, user-defined calls
and mutation are rejected. No Python evaluator or shader execution is involved.

Integer expressions preserve 32-bit wrap, signed division/remainder, uint
promotion and checked shift counts. Float arithmetic rounds after each operator;
double keeps double precision. Numeric bits, signed zero, NaN/infinity behavior
and value text are compared with the reference. Unsupported 64-bit expression
types remain explicit errors rather than being silently narrowed.

Native environments select locals by the active source frame and deepest live
lexical block. An unavailable inner declaration still shadows an outer value.
Equal-depth duplicate names remain ambiguous. Aggregate/vector lookups use the
decoded type groups; scalar fields named `x` and `y` alone do not imply a vector.
The SDBG environment adapter accepts verified assignment-history values and
rejects same-name declarations without inventing lexical ancestry. The adapter
does not reconstruct SDBG assignments; that dependency remains pending.

## Navigation semantics

Source navigation supports forward/reverse mapped steps, step-over, step-out,
line breakpoints, conditional breakpoints, hit-count rules (`equal`, `at_least`,
`multiple`), ordered rule export/copy, per-frame watches and a single-record
value cache. Functions stop at a matching breakpoint before selecting their
usual step target, including breakpoints in a descendant inline frame.

Navigation remains within the current diagnostic invocation and HS phase.
Loop reentry and source-frame identity changes count as new source positions.
Missing original call scopes, ambiguous future frames and truncated tails remain
errors. Hit counts require the visible invocation origin and a full trace;
single checkpoints cannot supply missing history. Failed rule validation does
not replace an existing rule, and at most 256 source breakpoints are retained.

The value loader is an in-process callback over native resolved values. The
navigation layer performs no GPU work or resource writes. UI selection,
configuration persistence and loader wiring are separate pending consumers.

## Validation

- `artifacts/debug-expression-final/validation.json`: 15,821 expression and
  environment comparisons; 13,158 exact successful results and 2,663 consistent
  rejections. Tests include deterministic integer/float cases, parser limits,
  Unicode decimal digits/whitespace, shadowing, partial vectors, SDBG ambiguity,
  nonfinite bit patterns and 1,098 original Hardware/WARP snapshot records.
  Successful results compare type, scalar/vector values (float64 raw bits),
  formatted text and scalar truth. Error wording is not required to be identical.
- `artifacts/source-trace-final/validation.json`: 73 scenarios and 28,998
  operations; 22,112 exact successes and 6,886 consistent rejections, including
  callback load counts. Coverage includes 1,114 captured records, nested inline
  functions, GS/HS/DS, phase and invocation boundaries, forward/reverse stepping,
  condition interruption, hit counts, missing origins, cache behavior, invalid
  rules, the 256-breakpoint limit and synthetic SDBG adapter cases.
- `artifacts/ctest-source-navigation-final.log`: checkpoint, post-transform,
  shaders and core suites passed. Native cases independently verify integer
  wrap, signed division/remainder, float rounding, lazy branches, aggregate
  shadowing, conditional step-over, invocation boundaries, unchanged rules after
  validation failures and missing-origin hit counts.
- `artifacts/build-source-navigation-final.log`: full Release build passed.

The development validators use Python only as an oracle. Current C++ consumers
use Qt and the existing native model APIs. No replay implementation or deployed
UI behavior changed in this batch; the prior source-stack package's golden-frame
and runtime-module evidence remains associated with that package, not relabeled
as fresh evidence for these new library APIs.

This work does not establish completion of the full Python migration. The
manifest retains partial status until the remaining consumers are integrated.
