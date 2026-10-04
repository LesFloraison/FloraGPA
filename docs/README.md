# Documentation

Start with the [project README](../README.md) for prerequisites, building,
packaging and basic replay. Documentation is primarily English; the development
chronology remains in Chinese.

## User and contributor entry points

| Need | Read |
|---|---|
| Build, package and start | [Project README](../README.md#build) |
| Replay, inspect and edit | [Usage guide](USAGE.md) |
| Current capabilities and known limits | [Current status](CURRENT_STATUS.md) |
| Component responsibilities | [Architecture](ARCHITECTURE.md) |
| Run tests and external comparisons | [Development validation](USAGE.md#development-validation) |
| Interpret corpus results and diagnostics | [Compatibility infrastructure](COMPATIBILITY_BASELINE.md) |
| Deferred versions, original expansion and M3 boundaries | [Deferred version audit](DEFERRED_VERSION_AUDIT.md) |
| Find dated changes | [Development chronology (Chinese)](MIGRATION_STATUS.md) |
| Inspect module-level migration bookkeeping | [Module ledger](migration.json) |
| Included dependencies | [Third-party components](../THIRD_PARTY.md) |

## Reading evidence

[Current status](CURRENT_STATUS.md) is the maintained capability summary.
The documents below record individual implementation/validation batches. Their
counts, package names and remaining-work statements describe those batches;
read the supersession notes before treating them as present-day limitations.
Original JSON baselines are preserved, not retroactively updated.

Files under `artifacts/`, `build/`, `out/` and external reference workspaces are
local evidence, not downloadable repository assets. A recorded SHA-256 identifies
an artifact but does not distribute it. Original captures, the recovered Python
reference, GPA and optional RenderDoc binaries are not bundled.
Third-party documentation is preserved verbatim and may refer to files from its
upstream repository that are not vendored here.

## Historical implementation and acceptance records

- [M2 registered corpus gate and M3 entry](M2_ACCEPTANCE_GATE.md), [full baseline](m2-gate-baseline.json) and [reproducible suite registry](m2-gate-suites.json).
- [Native discard acceptance and information boundaries](DISCARD_AUDIT.md), [focused baseline](discard-baseline.json) and [original corpus](discard-corpus.json).
- [Context-state identity loss and diagnostics](CONTEXT_STATE_DISCOVERY.md), [focused baseline](context-state-baseline.json) and [original corpus](context-state-corpus.json).
- [Unused SO lifetime acceptance](SO_LIFETIME_AUDIT.md), [acceptance baseline](so-lifetime-acceptance-baseline.json) and [accepted original corpus](so-lifetime-accepted-corpus.json).
- [SO lifetime investigation](SO_LIFETIME_DISCOVERY.md), [focused baseline](so-lifetime-baseline.json) and [original positive/blocked corpus](so-lifetime-corpus.json).
- [Pipeline getter acceptance](PIPELINE_GETTER_AUDIT.md), [acceptance baseline](pipeline-getter-baseline.json), [positive corpus](pipeline-getter-corpus.json) and [missing-SO negative](pipeline-getter-missing-so-corpus.json).
- [Pipeline getter discovery](PIPELINE_GETTER_DISCOVERY.md), [discovery baseline](pipeline-getter-discovery-baseline.json) and [blocked original corpus](pipeline-getter-discovery-corpus.json).
- [LOD shader resource usage](MIN_LOD_USAGE_AUDIT.md), [acceptance baseline](min-lod-usage-baseline.json) and [usage/negative corpus](min-lod-usage-corpus.json).
- [Resource minimum LOD replay](RESOURCE_LOD_AUDIT.md), [acceptance baseline](resource-lod-baseline.json), [positive corpus](min-lod-corpus.json) and [missing-state control](min-lod-missing-state-corpus.json).

- [Resource minimum LOD discovery](MIN_LOD_DISCOVERY.md), [blocked corpus](min-lod-discovery-corpus.json) and [discovery baseline](min-lod-discovery-baseline.json).

- [Texture export across queued refreshes](TEXTURE_EXPORT_FIX.md).

- [Context1 resource transfers](TRANSFER1_AUDIT.md), [acceptance baseline](transfer1-baseline.json), [original corpus](transfer1-corpus.json) and [retained context-omission controls](transfer1-context-corpus.json).

The following index keeps the existing document locations stable for citations.
Use the topical links in the usage/current-status guides for a shorter route.

### Replay compatibility audits

- [M2: checked buffer copy commands](BUFFER_COPY_AUDIT.md)
- [M2: buffer creation boundaries and private-data observations](BUFFER_CREATION_AUDIT.md)
- [M2: creation-time buffer SRV, RTV and UAV replay](BUFFER_VIEW_CREATION_AUDIT.md)
- [M2: class linkage and instance creation](CLASS_CREATION_AUDIT.md)
- [M2: predicate creation boundaries](PREDICATE_CREATION_AUDIT.md)
- [M2: ClearView and resource byte boundaries](CLEAR_VIEW_AUDIT.md)
- [DX11 compatibility baseline](COMPATIBILITY_BASELINE.md)
- [M2: geometry-stage and stream-output shader creation](GEOMETRY_CREATION_AUDIT.md)
- [Helldivers 2 capture compatibility — 2026-10-03](HELLDIVERS_REPLAY_FIX.md)
- [M2: Map / Unmap observation audit](MAP_OBSERVATION_AUDIT.md)
- [Native replay baseline](NATIVE_BASELINE.md)
- [M2: checked object observations and Present follow-up](OBJECT_OBSERVATION_AUDIT.md)
- [M2: captured shader, input layout and pipeline state creation](PIPELINE_CREATION_AUDIT.md)
- [M2: pipeline setters at captured event boundaries](PIPELINE_SETTER_AUDIT.md)
- [M2: checked Present boundaries and native binding semantics](PRESENT_REPLAY_AUDIT.md)
- [M2: remove the obsolete auxiliary replay fallback](STRICT_DISPATCH_AUDIT.md)
- [M2: checked texture copies and ResolveSubresource](TEXTURE_COPY_AUDIT.md)
- [M2: creation-time Texture2D and SRV replay](TEXTURE_CREATION_AUDIT.md)
- [M2: creation-time Texture1D and Texture3D replay](TEXTURE_DIMENSIONS_AUDIT.md)
- [M2: captured RTV, DSV and texture UAV creation](VIEW_CREATION_AUDIT.md)

### Analyzer and migration records

- [Captured annotations and explicit context membership](ANNOTATION_MIGRATION.md)
- [Before-event replay boundaries](BEFORE_BOUNDARY_MIGRATION.md)
- [Native checkpoint capture and export](CHECKPOINT_CAPTURE_MIGRATION.md)
- [Native checkpoint model migration](CHECKPOINT_MODEL_MIGRATION.md)
- [Native GS / HS / DS checkpoint workspace](CHECKPOINT_UI_MIGRATION.md)
- [Persistent constant-buffer setter editing](CONSTANT_BUFFER_SETTER_MIGRATION.md)
- [Native coverage execution](COVERAGE_EXECUTION_MIGRATION.md)
- [Native coverage shader transformations](COVERAGE_SHADER_MIGRATION.md)
- [Qt coverage inspection](COVERAGE_UI_MIGRATION.md)
- [Draw resources workspace](DRAW_RESOURCES_UI.md)
- [Native checkpoint and trace instrumentation](DXBC_CHECKPOINT_MIGRATION.md)
- [Native DXBC output-log instrumentation](DXBC_OUTPUT_LOG_MIGRATION.md)
- [Event texture edits](EVENT_TEXTURE_MIGRATION.md)
- [Native experiment execution reports](EXPERIMENT_REPORT_MIGRATION.md)
- [Native external shader tools and assembly editing](EXTERNAL_SHADER_MIGRATION.md)
- [Frame output migration](FRAME_OUTPUT_MIGRATION.md)
- [Repeated native GPU timing](GPU_PROFILE_MIGRATION.md)
- [Native event and command-range GPU statistics](GPU_STATISTICS_MIGRATION.md)
- [Native DXBC to HLSL recovery](HLSL_RECOVERY_MIGRATION.md)
- [Native hull shader outputs](HULL_OUTPUT_MIGRATION.md)
- [Persistent IA setter editing](IA_SETTER_MIGRATION.md)
- [Complete-command metric ranges](MD_FRAME_RANGES_MIGRATION.md)
- [Native event-group metrics](MD_HOTSPOTS_MIGRATION.md)
- [Offline scheduled metric results](MD_ITERATION_RESULTS_MIGRATION.md)
- [Native MD iteration transport](MD_ITERATION_TRANSPORT_MIGRATION.md)
- [Scheduled Intel metric collection](MD_ITERATIONS_MIGRATION.md)
- [Scheduled Intel metrics in Qt](MD_ITERATIONS_UI_MIGRATION.md)
- [Uniform Intel metric collection](MD_PROFILE_MIGRATION.md)
- [Native synchronous and FIFO counter acquisition](METRIC_ADAPTERS_MIGRATION.md)
- [Metric iteration analysis and request planning](METRIC_ANALYSIS_MIGRATION.md)
- [Native publisher subscriptions and scheduled counter pool](METRIC_COLLECTOR_MIGRATION.md)
- [Native publisher clock, report values and query drain](METRIC_CORE_MIGRATION.md)
- [Numeric metric iterations and FrameFile range indexing](METRIC_ITERATIONS_MIGRATION.md)
- [Metric pass controller and callback consumer](METRIC_PASS_CONTROLLER_MIGRATION.md)
- [Metric priority arbitration and publisher sidecar acceptance](METRIC_PRIORITY_MIGRATION.md)
- [Probe registry, device configuration and DX11 callback results](METRIC_PROBE_REGISTRY_MIGRATION.md)
- [Native Metrics Discovery foundation](METRICS_DISCOVERY_MIGRATION.md)
- [Native debugger configuration and Qt controls](NATIVE_DEBUG_CONFIG_MIGRATION.md)
- [Output binding migration](OUTPUT_BINDING_MIGRATION.md)
- [Native VS/DS writes and GS emissions](OUTPUT_LOG_MIGRATION.md)
- [Output session and pixel navigation](OUTPUT_SESSION_MIGRATION.md)
- [Persistent pipeline setters](PIPELINE_SETTER_MIGRATION.md)
- [Qt Pixel History](PIXEL_HISTORY_UI_MIGRATION.md)
- [Native planar Map and Update writes](PLANAR_WRITE_MIGRATION.md)
- [Native post-transform geometry](POST_TRANSFORM_MIGRATION.md)
- [Native private depth preparation for Quad](QUAD_DEPTH_MIGRATION.md)
- [Native Quad diagnostic execution](QUAD_EXECUTION_MIGRATION.md)
- [Native final-geometry submission for Quad diagnostics](QUAD_FINAL_MIGRATION.md)
- [Native resources for Quad diagnostics](QUAD_RESOURCE_MIGRATION.md)
- [Native serial submission for Quad diagnostics](QUAD_SERIAL_MIGRATION.md)
- [Native shader preparation for Quad diagnostics](QUAD_SHADER_MIGRATION.md)
- [Native graphics UAV relocation for Quad](QUAD_UAV_MIGRATION.md)
- [Native Qt Quad view](QUAD_UI_MIGRATION.md)
- [Native replay inventory and raw texture access](RDC_ASSETS_MIGRATION.md)
- [Optional native RenderDoc recapture and command provenance](RDC_CAPTURE_MIGRATION.md)
- [Native headless replay analysis](RDC_CLI_MIGRATION.md)
- [Native RenderDoc shader debugging](RDC_DEBUG_MIGRATION.md)
- [Native RenderDoc Pixel History backend](RDC_HISTORY_MIGRATION.md)
- [Native recorded counter sessions and publisher conversion](RECORDED_METRICS_MIGRATION.md)
- [Recorded shader debugging in the Qt workspace](REPLAY_DEBUG_UI_MIGRATION.md)
- [Native post-shader replay mesh](REPLAY_MESH_MIGRATION.md)
- [Native replay metrics](REPLAY_METRICS_MIGRATION.md)
- [Native SDBG source assignments](SDBG_VARIABLES_MIGRATION.md)
- [Native multi-file shader projects](SHADER_PROJECT_MIGRATION.md)
- [Persistent shader setters](SHADER_SETTER_MIGRATION.md)
- [Original shader source-line mapping](SOURCE_LINES_MIGRATION.md)
- [Native source navigation and watch expressions](SOURCE_NAVIGATION_MIGRATION.md)
- [Native source stacks and HS scope ownership](SOURCE_STACK_MIGRATION.md)
- [Native CodeView source variables](SOURCE_VARIABLES_MIGRATION.md)
- [Native Texture inspection and export](TEXTURE_INSPECTION_MIGRATION.md)
- [Uniform Intel metrics in Qt](UNIFORM_METRICS_UI_MIGRATION.md)
- [Global view experiments](VIEW_MIGRATION.md)
- [Native VS identity and unique output tables](VS_IDENTITY_MIGRATION.md)

### External backend validation

- [RenderDoc 1.46 compatibility test — 2026-09-23](RENDERDOC146_VALIDATION.md)

### Machine-readable evidence

- [buffer-copy-baseline.json](buffer-copy-baseline.json)
- [buffer-corpus.json](buffer-corpus.json)
- [buffer-creation-baseline.json](buffer-creation-baseline.json)
- [buffer-view-baseline.json](buffer-view-baseline.json)
- [buffer-view-corpus.json](buffer-view-corpus.json)
- [capture-corpus.json](capture-corpus.json)
- [class-creation-baseline.json](class-creation-baseline.json)
- [class-creation-corpus.json](class-creation-corpus.json)
- [clear-view-baseline.json](clear-view-baseline.json)
- [clear-view-corpus.json](clear-view-corpus.json)
- [compatibility-baseline.json](compatibility-baseline.json)
- [copy-corpus.json](copy-corpus.json)
- [creation-discovery-corpus.json](creation-discovery-corpus.json)
- [geometry-creation-baseline.json](geometry-creation-baseline.json)
- [geometry-creation-corpus.json](geometry-creation-corpus.json)
- [map-observation-baseline.json](map-observation-baseline.json)
- [migration.json](migration.json)
- [object-observation-baseline.json](object-observation-baseline.json)
- [original-comparison-baseline.json](original-comparison-baseline.json)
- [pipeline-corpus.json](pipeline-corpus.json)
- [pipeline-creation-baseline.json](pipeline-creation-baseline.json)
- [pipeline-creation-corpus.json](pipeline-creation-corpus.json)
- [pipeline-setter-baseline.json](pipeline-setter-baseline.json)
- [present-corpus.json](present-corpus.json)
- [predicate-creation-baseline.json](predicate-creation-baseline.json)
- [predicate-creation-corpus.json](predicate-creation-corpus.json)
- [present-replay-baseline.json](present-replay-baseline.json)
- [strict-dispatch-baseline.json](strict-dispatch-baseline.json)
- [resource-boundary-baseline.json](resource-boundary-baseline.json)
- [texture-copy-baseline.json](texture-copy-baseline.json)
- [texture-copy-corpus.json](texture-copy-corpus.json)
- [texture-creation-baseline.json](texture-creation-baseline.json)
- [texture-creation-corpus.json](texture-creation-corpus.json)
- [texture-dimensions-baseline.json](texture-dimensions-baseline.json)
- [texture-dimensions-corpus.json](texture-dimensions-corpus.json)
- [view-creation-baseline.json](view-creation-baseline.json)
- [view-creation-corpus.json](view-creation-corpus.json)
