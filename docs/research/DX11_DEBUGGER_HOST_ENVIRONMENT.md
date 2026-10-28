# GPA executable-relative loading and debugger observations

This P1 experiment confirms the executable-directory limitation identified in
`DX11_DEBUGGER_MODULE_ROUTES.md`. A colocated research executable enables the
original relative parser load without explicitly preloading the parser. It does
not initialize the missing debugger object in the completed lifecycle probes.
One Helldivers/Intel configuration remains incomplete after two bounded timeouts.
The engine constructor, non-null assignment, implementation table and module
remain unresolved. Neither P1 nor the native replacement/release gates are closed.

## Research boundary and identities

The player remains version `25.1.26603.47871`, SHA-256
`39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b`.
The parser remains SHA-256
`7a5d68d32f72f3abfb6346126058ff0eec6e9dba0e31d244142c01f026596c82`.
The previous pinned Ghidra exports supply the function/RVA evidence; this study
adds runtime validation, not a new claim about unanalyzed code.

`FloraOriginalResearchHost` is an explicit `EXCLUDE_FROM_ALL` development target.
It embeds the existing Python 3.12.14 runtime through an absolute DLL path so the
same Python lifecycle probe runs under a different Windows executable identity.
It is not a port of the GPA implementation or a production Python dependency.
Its dynamic entry points use the documented stable ABI, including the deprecated
`Py_SetPythonHome` compatibility entry point, to avoid requiring Python headers
at build time. See [Python's initialization API](https://docs.python.org/3.12/c-api/init.html).
The local runtime and launcher hashes are recorded in each manifest.

Each capture experiment copies all 72 top-level GPA DLLs, totaling 159,997,656
bytes, into a fresh ignored directory and marks the copies read-only. The host
is copied beside them. Installed files, copies, captures, runtime and probe
sources are hashed. The original installation is never modified. No original
DLL, capture, database or executable is added to Git or the product package.

This controls top-level executable-relative DLL resolution. It does not reproduce
GPA's GUI executable, exported application factories, configuration files, plugin
subdirectories, services, asynchronous application setup or valid shader-debug
selection. It must not be described as a complete original application host.

## Relative-load result

Both environments use the same copied player, working directory and probe. The
original directory getters are checked against the actual process image and
loaded player paths. The probe refuses an already loaded parser, including a
parser imported as a side effect of loading the player.

| Observation | Ordinary Python executable | Colocated research executable |
| --- | --- | --- |
| Getter `0xcea40` | Python runtime directory | Copied GPA directory |
| Getter `0xce640` | Copied GPA directory | Copied GPA directory |
| Original constructor `0xfac50` | Null parser handle | Loads copied `ShaderParse.dll` |
| Wrapper endpoints | Null | `Assemble`, `GetAsmParseError`, `Release` |
| Endpoint PE RVAs | No bindings | `0x7690`, `0x7870`, `0x7860` |
| Parser after original release | Absent | Absent |

All three capture experiment directories reproduce this pair of results. The
independent validator parses the parser's PE exports, reconstructs all guarded
wrapper bytes, validates original cleanup and compares module snapshots. Unlike
the preceding explicit-preload experiment, no extra parser reference is retained.
No parser function executes a shader in this test.

## Serial lifecycle matrix

The matrix comprises three captures, two hosts, two initialization settings and
two explicitly selected GPUs: 24 distinct cases. Both settings retain
`single-threaded=true`; the controlled setting additionally enables
`no-gtquerymanager` and `no-first-playbacks`. The other setting leaves those two
original defaults in place. It is not the unmodified full GPA application.

The existing hardware watcher is calibrated three times per capture. All nine
calibrations observe the required 36 field writes from three fixture threads,
including transient non-null values. Each successful original lifecycle observes
construction, the initial zero write, publication, two request entries,
destruction and the teardown write. Teardown allocator values are not counted as
live debugger objects.

| Capture / event | Complete distinct cases | Incomplete distinct cases | Complete watch duration |
| --- | ---: | ---: | ---: |
| GF2 / 113 | 8 | 0 | 3,219–5,484 ms |
| BF1 / 876 | 8 | 0 | 20,985–52,109 ms |
| Helldivers / 200 | 7 | 1 | 15,328–19,844 ms |

The successful probes verify the actual replay device before and after requests:
NVIDIA vendor/device `4318/11352`, LUID `69256`, or Intel `32902/32103`, LUID `74756`.
Every successful lifecycle retains API table `0x17ae40` and D3D11 query table
`0x17d668`, with their owner/API aliases verified at the five live boundaries.
The transient DLL-event inventory contains only the copied `dx11_player.dll` and
`d3dcompiler_47.dll` from the 72-file GPA inventory. The standalone parser test is
a separate process; it does not preload the parser into these replay lifecycles.

All 23 completed lifecycles observe a null host `+0x3c0` at both request entries
`0x902b0` and no nonzero live field write. Their two requests return status zero
without result callbacks. The existing selection discriminator remains zero and
is still not established as a valid shader selection. These are dispatch and
lifetime observations, not successful shader debugging or framebuffer parity.

## Retained timeouts

The colocated, controlled, Intel Helldivers case fails twice under the unchanged
120-second hardware-watch deadline. The parent enforces a separate 130-second
bound. Both watcher processes terminate their child; no process was left running.

| Attempt | Elapsed | Last saved boundary | Actual adapter verified | Requests observed |
| --- | ---: | --- | --- | ---: |
| Initial | 120,343 ms | Opened, before first replay completion | No | 0 |
| One recovery attempt | 120,422 ms | After both requests, before second replay completion | Yes, Intel | 2 |

The recovery's two request entries also see a null engine. Neither attempt has
a complete lifecycle or verified normal close. They remain separate failed
records, not successful samples, skips or a new product-test exemption. There
were no further retries. The two previously unstarted default-initialization
cases were then run separately and completed on both NVIDIA and Intel.

The evidence ledger therefore reports 23 complete lifecycles and two failed
attempts across 24 distinct cases. `complete_lifecycle_matrix` and
`every_attempt_completed` are both false. The timeout cause is unresolved; these
observations do not establish a loader, driver or debugger-engine cause.
Research process times include debugger overhead and are not product performance
or memory-growth measurements.

## Validation and reproduction

The launcher and existing watcher build successfully in Release. The runner
checks missing arguments, a missing runtime and a DLL without the Python ABI.
Independent validation accepts six environment reports, nine calibrations and
23 complete lifecycles, rejects 158 corrupted observations, and separately
classifies the two timeout reports. Complete-lifecycle checks are never applied
to fabricated completion records for the failures. No product regression-suite
or independent release acceptance is claimed by this research-only change.

```powershell
cmake --build build/vs2022 --config Release --target FloraOriginalResearchHost FloraOriginalDebuggerWatch --parallel 4
python tools/run_original_host_environment.py --host <host.exe> --watcher <watcher.exe> --capture <GF2-frame> --event 113 --out <new-root>/gf2
python tools/run_original_host_environment.py --host <host.exe> --watcher <watcher.exe> --capture <BF1-frame> --event 876 --out <new-root>/bf1
python tools/run_original_host_environment.py --host <host.exe> --watcher <watcher.exe> --capture <Helldivers-frame> --event 200 --out <new-root>/helldivers
```

GPU commands run serially. Runners stop on a failure and retain their manifests.
For the observed timeout, `tools/complete_original_host_environment.py` accepts
the failed manifest and a new output directory. It makes exactly one retry before
the remaining cases and stops again if that retry fails. To run an unstarted case
independently, use the recorded watcher command with a new output directory and
the explicit adapter/default-initialization arguments:

```powershell
<watcher.exe> <new-case>/watch.json <copied-host.exe> <python-home> <python312.dll> <absolute-probe_original_debugger_host.py> <frame.gpa_frame> --event 200 --gpa <copied-module-directory> --out <new-case> --vendor 4318 --host-graph --child --original-initialization
```

Repeat with vendor `32902` only after the first process exits. Every command and
exit status for this study is retained in the main and supplemental manifests.
The final validation command is:

```powershell
python tools/validate_host_environment.py artifacts/dx11-native/host-environment-final --recovery artifacts/dx11-native/host-environment-final/helldivers-recovery/runs.json --recovery artifacts/dx11-native/host-environment-final/helldivers-remaining/runs.json --out <new-ledger.json>
```

`dx11-debugger-host-environment-evidence.json` pins the retained evidence and tool
sources. The preliminary four-case GF2 study remains separately in
`artifacts/dx11-native/host-environment-initial`; it is not included in the final
matrix counts. The final experiment narrows the module-search uncertainty while
leaving application initialization and the debugger implementation unresolved.
Further research must trace an actual non-null assignment or a concrete caller
contract, rather than infer an engine from parser availability or profiler names.
