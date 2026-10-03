# Probe registry, device configuration and DX11 callback results

> Historical batch record. Results, package paths, module counts and remaining-work
> statements below describe this batch. See [current status](CURRENT_STATUS.md)
> for present support and [the documentation index](README.md) for navigation.
> `artifacts/`, `out/`, `build/` and external-reference paths identify local
> evidence not distributed with a clone; original JSON baselines are preserved.

`MetricProbeRegistry.h/.cpp` migrates `metric_probe_registry.py`,
`metric_probe_config.py` and `dx11_metric_result.py` into the native application
library. These are the remaining registry/configuration/result dependencies of
the production MD iteration transport; that transport and its profiling
worker/Qt integration are still pending.

## Behavior

- Registration preserves both publisher lookups, ignores the second lookup's
  status while validating its handle, and keeps duplicate type registrations.
  Configuration creates probes lazily in registration order. A failed create
  retains a nonzero returned handle; successful zero handles remain eligible
  for another create. Existing handles are reused even after a failed create.
- Begin, End and release use unsigned type-key order and skip zero handles.
  Release clears registration groups first, stops on the first destroy failure
  and retains remaining handles. Exceptions preserve intermediate state. The
  backend owner must complete cleanup; destruction does not invent retries.
- Device configuration is exactly 12 bytes: little-endian uint32 type followed
  by a uint64 borrowed device identity. Nonzero pointers use type 3; zero uses
  type 0. Encoding does not dereference the pointer or acquire a COM reference.
  Direct device keys win over context aliases. Ambiguous aliases choose the
  first device in unsigned key order. Pointer validation applies only to the
  selected device, matching the reference's lookup order.
- `dx11MetricConfigurationForKey` accepts device records as
  `[uint32 key, borrowed pointer, context-key array]`. This retains typed key
  validation across the C++ JSON boundary; repeated keys form the same last-value
  mapping as the reference's input dictionary.
- DX11 result assembly preserves initial data when completion tokens or usable
  data are absent, accepts timing-only completion, validates rectangular numeric
  rows, keeps duplicate metric IDs and transposes the matrix into metric columns.
  Numeric values become binary64; booleans are rejected. Auxiliary flags are
  zero by the recovered DX11 protocol. Token presence is not a range-count or
  event-identity check; the acquisition owner must validate those separately.

## Verification

The packaged probe passed **802 Python/C++ scenarios**, including **2,542
registry actions**, and **1,035 saved original GPA observations**:

| Original evidence | Checks |
| --- | ---: |
| Registration/cache/release state and calls | 840 |
| Packed device configurations | 12 |
| Device/context lookup configurations | 32 |
| DX11 synthetic callback result assembly | 145 |
| DX11 callback assembly using saved real values | 6 |

Comparisons check states, ordering, errors, byte encodings and binary64 bits
(including nonfinite values). The result oracle's outer `status` field is
separated from its initial payload before calling the Python API. Catalog
dispatcher observations in those research files are outside these three
modules and are not counted as migrated here. Manifests, source modules and
the original binary have SHA-256 provenance; GPA code is not executed by this
validator or the native library.

The real Intel integration now routes the existing pass controller through the
new registry. It verifies registration-order creation `[22, 9]`, unsigned-key
execution/release `[9, 22]`, cached probes, exact live device configuration and
a context alias selecting that device. Two MD sets each collect three Dispatch
workloads; the new DX11 assembler preserves all three columns and zero flags.
A failure in the later probe leaves the earlier query active until owner
cleanup, after which acquisition succeeds again and all native samples close.

Full Release compilation and 12 related CTest suites passed. Existing
regressions passed 494 controller scenarios plus 1,908 original observations,
and 2,758 iteration scenarios plus 1,907 original observations. Packaged native
controller/registry and Intel tests each passed five Qt Test cases without
skips. GF2/BF1 golden frames and both draw-suppression negative controls passed.
Replay reports use package-local Qt and contain no Python, Tk or GPA modules.

All runtime checks use native binaries with the Qt package directory and
Windows system paths. Python is used only by the development comparison tool.
The independent MD transport is not yet exposed as a complete analyzer workflow;
no placeholder UI controls were added by this batch.

Local evidence (ignored by Git):

- `artifacts/build-probe-registry-release.log`
- `artifacts/build-probe-registry-integration.log`
- `artifacts/ctest-probe-registry-release.log`
- `artifacts/probe-registry-package-parity/validation.json`
- `artifacts/probe-registry-package-tests.txt`
- `artifacts/probe-registry-package-integration.txt`
- `artifacts/probe-registry-controller-regression/validation.json`
- `artifacts/probe-registry-iterations-regression/validation.json`
- `artifacts/probe-registry-package-golden/validation.json`
- `artifacts/probe-registry-delivery-audit.json`

The portable application is `out/FloraGPA-probe-registry/`; distribute the whole
directory. Test-only binaries are moved out after verification. These checks
run on the current host and do not establish behavior on every driver/device.
