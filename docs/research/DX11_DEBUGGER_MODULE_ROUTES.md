# GPA module routes and debugger research boundaries

This P1 continuation follows dynamic module resolution from the pinned player.
It identifies concrete parser, logging and metrics-extension routes and a
research-host directory difference. It does not identify the implementation
consumed through host `+0x3c0`, prove its absence, or close the debugger gate.

Reproducible tools: commit `93fa887e9113b793ac05e03f0f3f36e9c803f0bf`.
The independent native DXBC implementation and product routing are unchanged.

## Identity and method

The installed `dx11_player.dll` is version `25.1.26603.47871`, image base
`0x180000000`, SHA-256
`39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b`.
The parser dependency is `ShaderParse.dll`, SHA-256
`7a5d68d32f72f3abfb6346126058ff0eec6e9dba0e31d244142c01f026596c82`.
All RVAs below belong to the player unless explicitly marked as parser RVAs.

The final recipe produces twelve Ghidra exports in the existing independent
research database using `-readOnly -noanalysis`. Installed binary hashes are
checked before and after. Each export must have the matching program identity,
complete selection count and required decoded instruction/call evidence. Raw
exports retain the explicit distinction between assembly and decompiler hypotheses.
They include 30 function selections, address/table evidence and symbol references.
Large exports, databases and original binaries remain outside Git.

## Verified paths

| Path | Decoded evidence | Independent evidence / boundary |
| --- | --- | --- |
| Common loader | `0xc7290` calls `LoadLibraryExW` at `0xc791f` | Loader policy is separate from a module's implementation |
| Parser wrapper | `0xfac50` calls the common loader at `0xface6` with flags 2, then `0xc6620` to resolve three endpoints | Six original constructor calls return the supplied object and retain the expected module/endpoint layout |
| Parser endpoints | Stores at wrapper `+0x30`, `+0x38`, `+0x40` | `Assemble`, `GetAsmParseError`, `Release`; parser PE RVAs `0x7690`, `0x7870`, `0x7860`, independently parsed from the file |
| Direct procedure fallback | `0xc6620` ultimately calls `GetProcAddress` at `0xc670f` | Six short/heap-name, missing-name and null-module cases agree with the Windows loader and preserve input bytes |
| Optional shim resolution | `0xc6620` checks `shimloader64.dll`, `ShimQueryInterface` and a returned resolver before its direct fallback | Static evidence only; the probe refuses an active shim, so it does not validate this branch |
| Profiler route | `0xfec70` loads `shader-profiler-shared.dll` and resolves `InitializeGTPin` | This is not evidence of the debugger engine or an assignment to host `+0x3c0` |
| Metrics extension loader | Table `0x1bf5c0` is `MetricsFramework::ExtensionLoader`; slots `+8`/`+0x10` target `0x14e730`/`0x14d690`, which call `0x14eeb0` | The latter loads a library and resolves `__extension_export_table__`; extension contents and a debugger relationship remain unproved |
| Main-process singleton queries | `0x11e380` and `0x11d7d0` query `StaticSingletonFactory` | Their actual type strings at `0x1fd240` and `0x1fd268` are `.PEAVIGPALogger@@` and `.?AVCLog@@`, independently read from the loaded image |
| Module reference release | `0xc7f40` calls `FreeLibrary` and clears the holder handle | First release succeeds, second returns false; all other bytes remain unchanged |
| Holder string cleanup | `0x384d0` frees allocated string storage, resets length/capacity and first byte | Exact cleanup snapshots and guards agree in all six cases |

The wrapper is `0x48` bytes: an opaque caller value at zero, a module holder at
`+8`, and three function addresses. Its holder contains an HMODULE and MSVC string.
The probe uses two guard patterns and three opaque values. The validator reconstructs
every byte of the `0x88`-byte guarded allocation at construction, release and cleanup;
endpoint addresses must equal the loaded module base plus the raw PE export RVA.
The retained module reference from explicit preloading remains alive throughout,
so successful wrapper release is not a claim that the DLL is fully unloaded.

## Executable versus player directory

`0xcea40` passes null to `GetModuleFileNameW`: decoded instructions `0xcea88`
and `0xcea8a` establish the argument and call. It returns the executable directory.
`0xce640` instead uses `GetModuleHandleExW` with flags 6 and its own code address
at `0xce687`/`0xce692`, then reads that module's path. It returns the player's
directory. Two original-code probes verify the returned strings against separate
Windows module-path queries and validate guarded cleanup.

In this run the first path is the bundled Python runtime directory; the second is
`C:/Program Files/IntelSWTools/GPA/`. The common loader's flags-2 path temporarily
selects the executable directory with `SetDllDirectoryW`, calls `LoadLibraryExW`,
then restores the prior directory. `os.add_dll_directory` alone did not make the
first parser-wrapper construction succeed in the Python-hosted child. That initial
child failed in 109 ms and its log/manifest remain retained.

The accepted probe explicitly preloads the hash-verified parser by absolute path.
It then checks the original wrapper's bindings and reference lifetime. No assertion
was removed from the constructor, endpoint, memory or deadline checks. This setup
does not validate normal GPA executable search behavior or demonstrate that a
missing parser explains the null debugger field. It establishes a concrete reason
to treat previous Python-hosted null-engine observations as environment-bounded.

## Results and next investigation

The final isolated child completed in 94 ms under a 30-second deadline: six wrapper
lifecycles, six resolver cases and two directory cases passed. It does not replay a
capture, create a D3D11 device or execute a shader. Independent validation checks all
twelve final Ghidra exports and rejects 48 altered observations or missing static
call edges. The initial failed child, the earlier passing probe without directory
checks, exploratory exports and final evidence are all retained separately.

The next runtime experiment should reproduce original executable-relative module
resolution in an isolated research host and inspect actual loaded modules while
watching `+0x3c0`. Merely adding more null samples from the same Python setup cannot
establish whole-program absence. Likewise neither the parser's endpoint names nor
the metrics loader's factory symbol identifies the missing debugger constructor,
implementation table or module. Computed resolution, extension contents, indirect
aliases and external writes remain outside this bounded investigation.

This changes the research boundary, not the requested native replacement scope:
full Pixel History/debugging parity and P7/P8 dependency-removal/release gates
remain required. No product regression result is claimed by these research tools.

## Reproduction

```powershell
tools/ghidra/Invoke-ModuleRouteResearch.ps1 -Headless <analyze-headless> -ProjectDirectory <existing-research-project> -OutputDirectory <new-root>/verified-final
python tools/probe_original_module_routes.py --out <new-root>/probe-complete
python tools/validate_module_route_research.py <new-root> --out <new-ledger.json>
```

`dx11-debugger-module-routes-evidence.json` records source and raw-evidence hashes.
The local GPA files are read-only research dependencies and are not distributed,
loaded by the product, or required by standalone release tests.
