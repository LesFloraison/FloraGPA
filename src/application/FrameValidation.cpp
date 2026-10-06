#include "FrameValidation.h"
#include "ApiCommands.h"
#include "ContextInspector.h"
#include "core/BufferCreation.h"
#include "core/ClassCreation.h"
#include "core/ClearView.h"
#include "core/Commands.h"
#include "core/ConstantBufferBindings.h"
#include "core/ContextStateRecords.h"
#include "core/CopyCommands.h"
#include "core/DiscardRecords.h"
#include "core/IaBindings.h"
#include "core/InspectionRecords.h"
#include "core/MapRecords.h"
#include "core/NormalizedPredication.h"
#include "core/OutputBindings.h"
#include "core/PipelineBindings.h"
#include "core/PipelineCreation.h"
#include "core/PredicateCreation.h"
#include "core/Predication.h"
#include "core/PresentRecords.h"
#include "core/ReplayCapabilities.h"
#include "core/ResourceLod.h"
#include "core/SrvBindings.h"
#include "core/StreamOutput.h"
#include "core/TextureCreation.h"
#include "core/TextureStorage.h"
#include "core/UavCounters.h"
#include <map>
namespace flora {
using Json = nlohmann::json;
Json validateFrame(const std::filesystem::path &path, const std::function<bool()> &cancelled) {
    const auto utf8 = path.u8string();
    Json report{{"schema", "FloraGPA frame validation 1"},
                {"capture", std::string(utf8.begin(), utf8.end())},
                {"profile", "GPA 2025 R1 legacy DX11 / IGPA v3"},
                {"scope", "offline_original_capture"},
                {"gpu_validation", "not_run"},
                {"replay_proven", false},
                {"completed", false},
                {"findings", Json::array()},
                {"coverage", Json::array()}};
    unsigned errors = 0, warnings = 0;
    auto finding = [&](const Entry *e, const char *severity, const char *kind, const std::string &reason,
                       Id referenced = 0) {
        errors += std::string(severity) == "error";
        warnings += std::string(severity) == "warning";
        report["findings"].push_back({{"severity", severity},
                                      {"kind", kind},
                                      {"reason", reason},
                                      {"entry_id", e ? Json(e->id) : Json(nullptr)},
                                      {"category", e ? Json(e->category) : Json(nullptr)},
                                      {"record_type", e ? Json(e->type) : Json(nullptr)},
                                      {"event_id", e && e->category == 7 ? Json(e->id) : Json(nullptr)},
                                      {"resource_id", e && e->category == 5 ? Json(e->id)
                                                      : referenced          ? Json(referenced)
                                                                            : Json(nullptr)}});
    };
    try {
        Frame frame(path, cancelled);
        report["entries"] = frame.entries().size();
        report["source_sha256"] = frame.sha256(cancelled);
        report["unused_stream_output_lifetimes"] = Json::array();
        report["unused_constant_buffer_lifetimes"] = Json::array();
        const auto lod = auditResourceLod(frame);
        report["resource_lod_initial_observations"] = Json::array();
        for (const auto &[resource, initial] : lod.initial)
            report["resource_lod_initial_observations"].push_back(
                {{"resource_id", resource},
                 {"observation_event", initial.observation},
                 {"min_lod", initial.value},
                 {"source", "first resource LOD record is a getter; no preceding saved setter or frame-time "
                            "creation"}});
        for (const auto &issue : lod.issues)
            finding(&frame.entry(issue.event), "error", "resource_lod_unresolved", issue.reason,
                    issue.resource);
        std::map<std::pair<unsigned, unsigned>, Json> coverage;
        size_t scanned = 0;
        std::optional<MapRecordAudit> mapAudit;
        std::optional<BufferCreationAudit> creationAudit;
        std::optional<TextureCreationAudit> textureCreationAudit;
        std::optional<ClassCreationAudit> classCreationAudit;
        std::optional<PredicateCreationAudit> predicateCreationAudit;
        std::optional<PipelineCreationAudit> pipelineCreationAudit;
        auto reference = [&](const Entry &e, Id id, int category) {
            if (!id)
                return;
            auto it = frame.entries().find(id);
            if (it == frame.entries().end() || it->second.category != category)
                finding(&e, "warning", "reference_requires_review",
                        "Missing or unexpected-category reference; runtime recovery/use must be checked", id);
        };
        std::set<Id> definedCounters;
        std::set<Id> completedPredicates, activePredicates;
        std::optional<PredicateBinding> predicateBinding;
        const auto normalizedPredication = auditNormalizedPredication(frame);
        Id normalizedBinding = 0;
        for (const auto &[id, e] : frame.entries()) {
            if (cancelled && cancelled()) {
                report["cancelled"] = true;
                break;
            }
            ++scanned;
            auto cap = e.category == 7 ? replayCapability(e.type) : ReplayCapability{"not_evaluated", "", ""};
            auto &row = coverage[{e.category, e.type}];
            if (row.is_null())
                row = {{"category", e.category},
                       {"record_type", e.type},
                       {"count", 0},
                       {"first_entry", id},
                       {"name", e.category == 7 ? commandName(e.type) : resourceName(e.type)},
                       {"handling", cap.handling},
                       {"implementation", cap.implementation},
                       {"evidence", cap.evidence},
                       {"decoded_records", 0},
                       {"unchecked_records", 0}};
            row["count"] = row["count"].get<size_t>() + 1;
            try {
                bool checked = false;
                if (e.category == 7) {
                    if (isMapObservation(e.type) || e.type == 0x246) {
                        if (!mapAudit)
                            mapAudit = auditMapRecords(frame);
                        const auto &map = mapAudit->at(id);
                        try {
                            requireMapRecord(*mapAudit, id);
                            if (e.type == 0x246)
                                mappedWriteLayout(frame, map);
                        } catch (const std::exception &error) {
                            finding(&e, "error", e.type == 0x246 ? "map_write_rejected" : "map_observation_rejected",
                                    error.what(), map.resource);
                            if (e.type == 0x246)
                                report["findings"].back()["data_id"] = map.data;
                        }
                    }
                    const auto command = inspectCommand(frame, id);
                    if (command["status"] == "invalid")
                        throw std::runtime_error(command.value("error", "Invalid command wire layout"));
                    checked = command["status"] == "decoded";
                    auto predicateReference = [&](Id resource, bool binding) {
                        if (!resource)
                            return;
                        if (!frame.entries().contains(resource)) {
                            if (binding && normalizedBinding == resource)
                                return;
                            finding(&e, "error", "predicate_resource_missing",
                                    "Predicate descriptor is absent from the capture; saved control or "
                                    "GetData observations do not identify a native predicate descriptor",
                                    resource);
                            return;
                        }
                        try {
                            readPredicate(frame, resource);
                        } catch (const std::exception &error) {
                            finding(&e, "error", "predicate_resource_invalid", error.what(), resource);
                            return;
                        }
                        if (!predicateCreationAudit)
                            predicateCreationAudit = auditPredicateCreations(frame);
                        const auto created = predicateCreationAudit->creationEvents.find(resource);
                        if (created != predicateCreationAudit->creationEvents.end()) {
                            if (binding && !completedPredicates.contains(resource))
                                finding(&e, "error", "predicate_interval_missing",
                                        "Frame-created predicate has no completed captured Begin/End interval",
                                        resource);
                            return;
                        }
                        if (binding && !completedPredicates.contains(resource) &&
                            !activePredicates.contains(resource))
                            finding(&e, "warning", "predicate_replay_baseline",
                                    "Uses the original player's empty-query baseline before a captured "
                                    "Begin/End interval. Saved control may be normalized; this is not a "
                                    "restored application query result", resource);
                    };
                    if (const auto operation = predicateOperation(e.type)) {
                        const auto p = readPredicateCommand(e.type, frame.payload(id));
                        requireImmediateContext(frame, p.context);
                        if (*operation == PredicateOperation::Set) {
                            normalizedBinding = 0;
                            if (const auto proof = normalizedPredication.find(id);
                                proof != normalizedPredication.end()) {
                                normalizedBinding = p.resource;
                                finding(&e, "warning", "captured_predicate_condition",
                                        "Linked failed marker lookup proves normalized conditional execution. "
                                        "Predicate descriptor and original query value remain unavailable",
                                        p.resource);
                                report["findings"].back()["witness_event"] = proof->second.witness;
                            }
                        }
                        predicateReference(p.resource, *operation == PredicateOperation::Set);
                        if (*operation == PredicateOperation::Set)
                            predicateBinding = PredicateBinding{p.resource, p.value};
                        else if (*operation == PredicateOperation::Begin) {
                            activePredicates.insert(p.resource);
                            completedPredicates.erase(p.resource);
                        } else if (activePredicates.erase(p.resource))
                            completedPredicates.insert(p.resource);
                    } else if (e.type == 0x242) {
                        predicateBinding.reset();
                        normalizedBinding = 0;
                    } else if (isDraw(e.type)) {
                        const auto state = frame.state(frame.event(id).state);
                        predicateReference(predicateBinding ? predicateBinding->resource : state.predicate,
                                           true);
                    }
                    if (isOutputCommand(e.type)) {
                        const auto output = readOutputCommand(e.type, frame.payload(id));
                        validateOutputRange(e.type, output);
                        requireImmediateContext(frame, output.context);
                        if (output.uavs && output.initialCounts)
                            for (size_t slot = 0; slot < output.uavs->size(); ++slot)
                                if (output.initialCounts->at(slot) != UINT32_MAX)
                                    definedCounters.insert(output.uavs->at(slot));
                    }
                    auto requireCounter = [&](Id view) {
                        if (const auto counter = describeCounter(frame, view);
                            counter && !definedCounters.contains(view)) {
                            finding(&e, "error", "counter_initial_value_missing",
                                    "No captured counter value is established before this use; KEEP and "
                                    "buffer contents do not recover a hidden UAV counter", view);
                            report["findings"].back()["storage_id"] = counter->resource;
                        }
                    };
                    if (isDraw(e.type)) {
                        const auto event = frame.event(id);
                        for (const auto &counter : boundCounters(frame, event, frame.state(event.state)))
                            requireCounter(counter.view);
                    } else if (e.type == 0x3f) {
                        Reader copy(frame.payload(id));
                        copy.skip(28);
                        requireCounter(copy.read<Id>());
                        copy.end();
                    } else if (e.type == 0x357d) {
                        const auto creation = readTextureCreation(e.type, frame.payload(id));
                        if (!creation.result && creation.resource)
                            definedCounters.erase(creation.resource);
                    }
                    if (isDiscardRecord(e.type)) {
                        const auto discard = readDiscardRecord(e.type, frame.payload(id));
                        try {
                            const auto target = validateDiscardRecord(frame, discard);
                            finding(&e, "warning", "discard_contents_undefined",
                                    "Discard does not define replacement bytes. Native execution may "
                                    "invalidate the selected contents; reads before defining writes are not "
                                    "certified by structural validation",
                                    target.resource);
                        } catch (const std::exception &error) {
                            finding(&e, "error",
                                    !frame.entries().contains(discard.target) ? "discard_target_missing"
                                    : ambiguousDiscardRectangles(discard)
                                        ? "discard_rectangle_presence_unresolved"
                                        : "discard_rejected",
                                    error.what(), discard.target);
                        }
                    }
                    if (isContextStateRecord(e.type)) {
                        const auto record = readContextStateRecord(e.type, frame.payload(id));
                        validateContextStateOwner(frame, id, record);
                        if (const auto gap = contextStateReplayGap(record); !gap.empty())
                            finding(&e, "error", contextStateGapKind(record), gap, record.state);
                    } else if (std::string(cap.handling) == "unsupported")
                        finding(&e, "error", "implementation_gap",
                                "No native replay path for " + commandName(e.type));
                    else if (!checked)
                        finding(&e, "warning", "decoder_gap",
                                "Replay path exists but API wire inspection is incomplete");
                    if (isDraw(e.type)) {
                        auto ev = frame.event(id);
                        try {
                            requireImmediateContext(frame, ev.context);
                        } catch (const std::exception &ex) {
                            finding(&e, "error", "context_unsupported", ex.what(), ev.context);
                        }
                        frame.state(ev.state);
                        reference(e, ev.argumentBuffer, 5);
                    } else if (finishCommandListVersion(e.type)) {
                        try {
                            acceptFinishCommandList(frame, readFinishCommandList(e.type, frame.payload(id)));
                        } catch (const std::exception &ex) {
                            finding(&e, "error", "command_list_unsupported", ex.what());
                        }
                    } else {
                        if (constantBufferStage(e.type)) {
                            const auto binding = readConstantBufferSetter(e.type, frame.payload(id));
                            std::optional<UnusedConstantBufferLifetime> lifetime;
                            std::string blocked;
                            if (std::any_of(binding.buffers.begin(), binding.buffers.end(), [&](Id buffer) {
                                    return buffer && !frame.entries().contains(buffer);
                                })) {
                                try {
                                    lifetime = proveUnusedConstantBufferLifetime(frame, id);
                                    report["unused_constant_buffer_lifetimes"].push_back(
                                        {{"event_id", id},
                                         {"closing_event_id", lifetime->closingEvent},
                                         {"stage", lifetime->stage},
                                         {"resource_ids", lifetime->resources},
                                         {"native_binding_recovered", false}});
                                    report["record_handling_overrides"].push_back(
                                        {{"event_id", id},
                                         {"handling",
                                          std::any_of(binding.buffers.begin(), binding.buffers.end(),
                                                      [&](Id buffer) {
                                                          return buffer && frame.entries().contains(buffer);
                                                      })
                                              ? "execute"
                                              : "metadata"},
                                         {"reason", "Absent CB slots close before GPU use; saved slots "
                                                    "remain native bindings"},
                                         {"closing_event_id", lifetime->closingEvent}});
                                } catch (const std::exception &error) {
                                    blocked = error.what();
                                }
                            }
                            bool unresolved = false;
                            for (const auto buffer : binding.buffers) {
                                if (!buffer)
                                    continue;
                                const auto entry = frame.entries().find(buffer);
                                if (entry == frame.entries().end() || entry->second.category != 5 ||
                                    entry->second.type != 0x83) {
                                    unresolved = true;
                                    if (lifetime)
                                        finding(&e, "warning", "constant_buffer_unused_binding",
                                                "Absent CB binding closes at event " +
                                                    std::to_string(lifetime->closingEvent) +
                                                    " before GPU use; descriptor/storage and native state "
                                                    "inside this interval are not reconstructed",
                                                buffer);
                                    else
                                        finding(
                                            &e, "error", "constant_buffer_resource_unresolved",
                                            "Constant-buffer setter requires a saved buffer resource; "
                                            "native binding cannot be reconstructed from this reference. " +
                                                blocked,
                                            buffer);
                                }
                            }
                            if (!unresolved)
                                try {
                                    validateConstantBufferBinding(frame, binding);
                                } catch (const std::exception &error) {
                                    finding(&e, "error", "constant_buffer_binding_rejected", error.what());
                                }
                        }
                        if (isStreamOutputTargets(e.type)) {
                            const auto targets = readStreamOutputTargets(frame.payload(id));
                            std::optional<UnusedStreamOutputLifetime> lifetime;
                            std::string blocked;
                            if (targets.buffers &&
                                std::any_of(targets.buffers->begin(), targets.buffers->end(), [&](Id buffer) {
                                    return buffer && !frame.entries().contains(buffer);
                                })) {
                                try {
                                    lifetime = proveUnusedStreamOutputLifetime(frame, id);
                                    report["unused_stream_output_lifetimes"].push_back(
                                        {{"event_id", id},
                                         {"closing_event_id", lifetime->closingEvent},
                                         {"resource_ids", lifetime->resources},
                                         {"native_binding_recovered", false}});
                                    report["record_handling_overrides"].push_back(
                                        {{"event_id", id},
                                         {"handling", "metadata"},
                                         {"reason", "Proven unused absent-only SO target identities; native "
                                                    "targets are not materialized"},
                                         {"closing_event_id", lifetime->closingEvent}});
                                } catch (const std::exception &error) {
                                    blocked = error.what();
                                }
                            }
                            if (targets.buffers)
                                for (const auto buffer : *targets.buffers)
                                    if (buffer && !frame.entries().contains(buffer))
                                        if (lifetime)
                                            finding(&e, "warning", "stream_output_unused_binding",
                                                    "Absent-only SO binding closes at event " +
                                                        std::to_string(lifetime->closingEvent) +
                                                        " before GPU use; descriptor/storage are not "
                                                        "reconstructed. "
                                                        "Replay ending inside this interval or requiring "
                                                        "native command-state observation is unsupported",
                                                    buffer);
                                        else
                                            finding(&e, "error", "stream_output_resource_unresolved",
                                                    "SOSetTargets references an absent buffer entry; " +
                                                        blocked,
                                                    buffer);
                        }
                        if (isPipelineSetter(e.type)) {
                            const auto binding = readPipelineSetter(e.type, frame.payload(id));
                            if (const auto missing = missingPipelineShader(frame, binding))
                                finding(&e, "warning", "shader_binding_not_saved",
                                        "Shader identity is absent; native state remains unresolved until a "
                                        "later setter, ClearState or validated draw snapshot",
                                        missing);
                            else
                                validatePipelineBinding(frame, binding);
                        }
                        if (isCopyCommand(e.type)) {
                            const auto copy = readCopyCommand(e.type, frame.payload(id));
                            try {
                                if (validateCopyCommand(frame, copy) == CopyValidation::TextureReferences)
                                    finding(&e, "info", "texture_copy_validation_partial",
                                            "Texture copy uses a special reinterpretation, packed or planar "
                                            "format; full transfer semantics require further audit",
                                            copy.destination);
                            } catch (const std::exception &error) {
                                finding(&e, "error", "copy_command_rejected", error.what(), copy.destination);
                            }
                        }
                        if (e.type == 0x358e) {
                            if (!predicateCreationAudit)
                                predicateCreationAudit = auditPredicateCreations(frame);
                            const auto &c = predicateCreationAudit->records.at(id);
                            if (!c.error.empty())
                                finding(&e, "error", "predicate_creation_rejected", c.error, c.resource);
                            else if (c.result)
                                report["record_handling_overrides"].push_back(
                                    {{"event_id", id},
                                     {"resource_id", c.resource},
                                     {"handling", "metadata"},
                                     {"reason", "Noncreating predicate call"}});
                        }
                        if (isClassCreation(e.type)) {
                            if (!classCreationAudit) {
                                classCreationAudit = auditClassCreations(frame);
                                for (const auto &[source, target] : classCreationAudit->identities.aliases)
                                    if (source != target)
                                        report["class_linkage_aliases"].push_back(
                                            {{"api_identity", source},
                                             {"snapshot_identity", target},
                                             {"source",
                                              "paired shader/instance creation and resource snapshot"}});
                            }
                            const auto &c = classCreationAudit->records.at(id);
                            if (!c.error.empty())
                                finding(&e, "error", "class_creation_rejected", c.error, c.resource);
                            else if (!c.note.empty())
                                finding(&e, "info", "unmaterialized_class_creation", c.note, c.resource);
                            if (c.error.empty() && (c.result || !c.note.empty()))
                                report["record_handling_overrides"].push_back(
                                    {{"event_id", id},
                                     {"resource_id", c.resource},
                                     {"handling", "metadata"},
                                     {"reason", c.result ? "Noncreating class call" : c.note}});
                        }
                        if (isPipelineCreation(e.type)) {
                            if (!pipelineCreationAudit)
                                pipelineCreationAudit = auditPipelineCreations(frame);
                            const auto &creation = pipelineCreationAudit->records.at(id);
                            if (!creation.error.empty())
                                finding(&e, "error", "pipeline_creation_rejected", creation.error,
                                        creation.resource);
                            else if (!creation.note.empty())
                                finding(&e, "info", "unmaterialized_pipeline_creation", creation.note,
                                        creation.resource);
                            if (creation.error.empty() && (creation.result != 0 || !creation.note.empty()))
                                report["record_handling_overrides"].push_back(
                                    {{"event_id", id},
                                     {"resource_id", creation.resource},
                                     {"handling", "metadata"},
                                     {"reason", creation.result != 0
                                                    ? "Failed/validation-only pipeline creation"
                                                    : creation.note}});
                        }
                        acceptTextureCreationObservation(e.type, frame.payload(id));
                        if (isTextureCreation(e.type)) {
                            if (!textureCreationAudit)
                                textureCreationAudit = auditTextureCreations(frame);
                            const auto &creation = textureCreationAudit->records.at(id);
                            if (!creation.error.empty())
                                finding(&e, "error", "texture_creation_rejected", creation.error,
                                        creation.resource);
                            else if (creation.result == 0 && !creation.hasInitial && !isViewCreation(e.type))
                                finding(&e, "info", "texture_initial_contents_undefined",
                                        "Creation has no saved initial-data observations; a later first-use "
                                        "blob is not a creation-time initializer",
                                        creation.resource);
                        }
                        acceptQueryMetadata(e.type, frame.payload(id));
                        acceptInspectionRecord(e.type, frame.payload(id));
                        acceptPassiveObjectRecord(e.type, frame.payload(id));
                        if (e.type == 0x3578) {
                            if (!creationAudit)
                                creationAudit = auditBufferCreations(frame);
                            const auto &creation = creationAudit->records.at(id);
                            if (!creation.error.empty())
                                finding(&e, "error", "buffer_creation_rejected", creation.error,
                                        creation.resource);
                            else if (creation.result == 0 && !creation.hasInitial)
                                finding(&e, "info", "buffer_initial_contents_undefined",
                                        "CreateBuffer requested storage without initial data; later captured "
                                        "writes define its contents",
                                        creation.resource);
                        }
                        if (e.type == 0x3017 || e.type == 0x3167) {
                            const auto data = readPrivateDataObservation(frame.payload(id));
                            if (data.result >= 0 && data.size)
                                finding(&e, "info", "private_data_payload_not_saved",
                                        "API record stores an opaque pointer, not private-data bytes; saved "
                                        "resource names are separate metadata",
                                        data.owner);
                        }
                        if (e.type == 0x3257) {
                            try {
                                validatePresentRecord(frame, id);
                            } catch (const std::exception &error) {
                                Id chain = 0;
                                auto raw = frame.payload(id);
                                if (raw.size() >= 16) {
                                    Reader header(raw);
                                    header.skip(8);
                                    chain = header.read<Id>();
                                }
                                finding(&e, "error", "present_unsupported", error.what(), chain);
                            }
                        }
                        if (e.type == 0x257) {
                            const auto clear = readClearView(frame.payload(id));
                            try {
                                validateClearView(frame, clear);
                            } catch (const std::exception &error) {
                                finding(&e, "error", "clear_view_rejected", error.what(), clear.view);
                            }
                        } else if (isWritableCommand(e.type))
                            validateWritableCommand(frame, id);
                        if (e.type >= 0x3278 && e.type <= 0x327e)
                            validateAnnotationCommand(e.type, frame.payload(id));
                        if (e.type == 0x247 || e.type == 0x255) {
                            Reader operand(frame.payload(id));
                            operand.skip(16);
                            const auto destination = operand.read<Id>();
                            try {
                                const auto layout = updateSourceLayout(frame, id);
                                const auto dst = frame.resource(layout.destination);
                                if (dst.type != 0x83 &&
                                    (textureInfo(dst).format == 104 || textureInfo(dst).format == 105))
                                    finding(&e, "error", "capture_data_missing",
                                            "Legacy P010/P016 Update requires an explicit complete source "
                                            "replacement",
                                            dst.id);
                            } catch (const std::exception &error) {
                                finding(&e, "error", "update_command_rejected", error.what(), destination);
                            }
                        }
                    }
                } else if (e.category == 3 && e.type == 3) {
                    const auto s = frame.state(id);
                    checked = true;
                    for (const auto &stage : s.stages) {
                        reference(e, stage.shader, 5);
                        for (auto x : stage.cb)
                            reference(e, x, 5);
                        for (auto x : stage.srv)
                            reference(e, x, 5);
                        if (stage.classCount > stage.classes.size())
                            throw std::runtime_error("Class instance count exceeds snapshot capacity");
                    }
                    for (auto x : s.rtv)
                        reference(e, x, 5);
                    for (auto x : s.csUav)
                        reference(e, x, 5);
                    reference(e, s.dsv, 5);
                    reference(e, s.ib, 5);
                    for (auto x : s.vb)
                        reference(e, x, 5);
                    // rtCount spans RTV and OM UAV slots, including the extended
                    // D3D11.1 snapshot. Device-specific limits are checked at replay.
                    if (s.rtCount > 64 || s.omStart > 64 || s.soCount > 4)
                        throw std::runtime_error("Output count exceeds snapshot capacity");
                } else if (e.category == 9 && e.type == 1) {
                    frame.data(id);
                    checked = true;
                } else if (e.category == 9 && e.type == 0x81) {
                    frame.shader(id);
                    checked = true;
                } else if (e.category == 5) {
                    const auto r = frame.resource(id);
                    if (e.type == 0x83) {
                        checked = true;
                        if (r.data && frame.data(r.data).size() != r.desc[0])
                            throw std::runtime_error("Buffer initial data length does not match descriptor");
                    } else if (e.type >= 0x84 && e.type <= 0x87) {
                        const auto info = textureInfo(r);
                        if (r.data) {
                            if (e.type != 0x87 && info.samples == 1 && info.format != 104 &&
                                info.format != 105) {
                                try {
                                    textureInitialSubresources(r, frame.data(r.data));
                                } catch (const std::exception &error) {
                                    finding(&e, "error", "texture_initial_data_invalid", error.what());
                                    report["findings"].back()["data_id"] = r.data;
                                }
                            } else
                                frame.data(r.data);
                        }
                        checked = true;
                        if (info.samples > 1 && r.data)
                            finding(&e, "warning", "capture_data_limit",
                                    "MSAA GenData is not per-sample initialization; verify in-frame writes "
                                    "before use");
                        if (info.format >= 103 && info.format <= 105)
                            finding(&e, "warning", "capture_data_limit",
                                    "Planar capture initial data/UV completeness requires layout-specific "
                                    "verification");
                    } else if (e.type >= 0x90 && e.type <= 0x95) {
                        frame.shader(r.data);
                        checked = true;
                    } else if (contextVersion(e.type)) {
                        const auto c = describeContext(frame, id, false);
                        checked = true;
                        if (c.deferred)
                            finding(&e, "warning", "implementation_gap",
                                    "Deferred context inventory is decoded; production execution is not "
                                    "implemented");
                    } else if (e.type == 0x9a) {
                        inspectCommandList(frame, id);
                        checked = true;
                        finding(&e, "warning", "implementation_gap",
                                "Command list inventory is decoded; execution is unverified");
                    } else if (e.type == 0x96) {
                        readPredicate(frame, id);
                        checked = true;
                    }
                }
                const auto key = checked ? "decoded_records" : "unchecked_records";
                row[key] = row[key].get<size_t>() + 1;
            } catch (const std::exception &ex) {
                finding(&e, "error", "record_rejected", ex.what());
            }
        }
        size_t unchecked = 0;
        for (auto &[key, row] : coverage) {
            unchecked += row["unchecked_records"].get<size_t>();
            if (row["unchecked_records"].get<size_t>() && key.first == 5)
                finding(&frame.entry(row["first_entry"].get<Id>()), "warning", "offline_coverage_gap",
                        "This resource family has no complete offline descriptor validator; GPU replay "
                        "remains necessary");
            report["coverage"].push_back(std::move(row));
        }
        report["unchecked_records"] = unchecked;
        report["scanned_records"] = scanned;
        report["completed"] = scanned == frame.entries().size() && !report.value("cancelled", false);
        report["limits"] = {
            "GPU resource creation, shader execution, binding recovery and pixel correctness are not tested",
            "Unrecognized non-command payloads and descriptor-specific semantics remain outside offline "
            "coverage",
            "Reports inspect original capture data, not an experiment project"};
    } catch (const OperationCancelled &) {
        report["cancelled"] = true;
        report["completed"] = false;
    } catch (const std::exception &ex) {
        finding(nullptr, "error", "container_rejected", ex.what());
    }
    report["errors"] = errors;
    report["warnings"] = warnings;
    report["status"] = report.value("cancelled", false) ? "cancelled"
                       : errors                         ? "blocked"
                       : warnings                       ? "review_required"
                                                        : "checked";
    return report;
}
} // namespace flora
