#include "GpuStatistics.h"
#include "ExperimentReport.h"
#include "StreamOutputInspector.h"
#include <QDir>
#include <QSaveFile>
#include <sstream>
namespace flora {
using Json = nlohmann::json;
namespace {
Json so(const D3D11_QUERY_DATA_SO_STATISTICS &s) {
    return {{"primitives_written", s.NumPrimitivesWritten},
            {"primitives_storage_needed", s.PrimitivesStorageNeeded}};
}
Json autoResult(const DrawAutoParameters &p) {
    auto value = drawAutoJson(p);
    value["parameters"] = {{"vertex_count", p.vertexCount}, {"start_vertex", 0}};
    return value;
}
} // namespace
std::map<std::string, uint64_t> compatibleReplayCounts(const Replay &replay, Id start, Id end,
                                                       std::map<std::string, uint64_t> counts) {
    // Retain the original report families while leaving native engine diagnostics intact.
    for (auto name :
         {"unresolved_output_setters", "unresolved_input_layout_setters", "unresolved_srv_setters"})
        counts.erase(name);
    for (auto name : {"ClearUnorderedAccessViewUint", "ClearUnorderedAccessViewFloat"})
        if (auto it = counts.find(name); it != counts.end()) {
            counts["ClearUnorderedAccessView"] += it->second;
            counts.erase(it);
        }
    for (const auto &[id, e] : replay.frame().entries()) {
        if (e.category != 7 || id < start || id > end)
            continue;
        const bool off = replay.options().disabled.contains(id);
        if (!off && e.type == 0x34fb) {
            ++counts["query_metadata_records"];
            auto &auxiliary = counts.at("state_or_auxiliary_records");
            if (--auxiliary == 0)
                counts.erase("state_or_auxiliary_records");
        }
        if (off)
            ++counts["experiment_disabled_" + commandName(e.type)];
        if (!off && (constantBufferStage(e.type).has_value() || e.type == 0x34ff || e.type == 0x3500 ||
                     e.type == 0x3522 || e.type == 0x25e))
            ++counts["state_or_auxiliary_records"];
        if (isDraw(e.type)) {
            ++counts[commandName(e.type) + "_records"];
            if (!off && !replay.options().suppressDraws && (e.type == 0x39 || e.type == 0x3a)) {
                const auto event = replay.frame().event(id);
                const auto indices = uint64_t(event.args[0]) * (e.type == 0x3a ? event.args[1] : 1);
                if (indices)
                    counts["submitted_indices"] += indices;
            }
        }
    }
    return counts;
}
Json gpuStatisticsReport(const Replay &replay) {
    if (!replay.options().measurement || !replay.measurementResult())
        throw std::runtime_error("No completed GPU statistics sample");
    const auto &m = *replay.options().measurement;
    const auto &s = *replay.measurementResult();
    const auto &p = s.pipeline;
    auto desc = replay.adapterDescription();
    std::ostringstream level;
    level << "0x" << std::hex << unsigned(replay.featureLevel());
    Json adapter{{"description", replay.adapter()},
                 {"vendor_id", desc.VendorId},
                 {"device_id", desc.DeviceId},
                 {"revision", desc.Revision},
                 {"dedicated_video_memory", desc.DedicatedVideoMemory},
                 {"shared_system_memory", desc.SharedSystemMemory}};
    const bool warp = replay.options().warp;
    Json streams = Json::array();
    for (size_t i = 0; i < s.streams.size(); ++i) {
        auto value = so(s.streams[i]);
        value["stream"] = i;
        streams.push_back(std::move(value));
    }
    Json out{
        {"source", "native_dx11_queries"},
        {"single_replay_sample", true},
        {"statistics_shader_instrumentation", false},
        {"replay_generation", replay.generation()},
        {"query_completion", "native_event_query"},
        {"execution_device",
         {{"selected", warp ? "warp" : "hardware"},
          {"software", warp},
          {"measurement_scope", warp ? "software_device" : "hardware_device"},
          {"feature_level", level.str()},
          {"adapter", adapter}}},
        {"so_count_reconstruction", replay.reconstructsSoCounts()},
        {"experiment", experimentReport(replay)},
        {"pipeline",
         {{"ia_vertices", p.IAVertices},
          {"ia_primitives", p.IAPrimitives},
          {"vs_invocations", p.VSInvocations},
          {"gs_invocations", p.GSInvocations},
          {"gs_primitives", p.GSPrimitives},
          {"clip_invocations", p.CInvocations},
          {"clip_primitives", p.CPrimitives},
          {"ps_invocations", p.PSInvocations},
          {"hs_invocations", p.HSInvocations},
          {"ds_invocations", p.DSInvocations},
          {"cs_invocations", p.CSInvocations}}},
        {"stream_output", {{"legacy_query", so(s.legacySo)}, {"streams", streams}, {"overflow", s.overflow}}},
        {"occlusion_samples", s.occlusion},
        {"timing",
         {{"available", s.timingAvailable},
          {"disjoint", s.disjoint},
          {"frequency_hz", s.frequency},
          {"start_tick", s.startTick},
          {"end_tick", s.endTick},
          {"elapsed_ms", s.elapsedMs ? Json(*s.elapsedMs) : Json(nullptr)}}},
        {"event", m.start},
        {"api", m.singleEvent ? commandName(replay.frame().entry(m.start).type) : "CommandRange"},
        {"scope", m.singleEvent ? "single_event" : "command_range"},
        {"notes",
         {"SO primitives needing storage can be nonzero with null targets; overflow is not evidence of an "
          "out-of-bounds write.",
          "Invocation counts and timing describe this replay device; WARP measurements are software "
          "measurements.",
          "Native counters are reported unchanged. WARP can count CS invocations for a predicated-off "
          "Dispatch whose UAV writes are suppressed.",
          "DrawAuto replay can include SO count queries and CPU synchronization even without edits; its "
          "timing is not an uninstrumented application measurement."}}};
    if (m.singleEvent) {
        out["predication"] = {{"resource", s.predicate.resource}, {"value", s.predicate.value}};
        out["event_enabled"] = !replay.options().disabled.contains(m.start);
        auto it = replay.drawAutoResults().find(m.start);
        out["draw_auto_recomputed"] =
            it == replay.drawAutoResults().end() ? Json(nullptr) : autoResult(it->second);
    } else {
        Json disabled = Json::array(), autos = Json::object();
        uint64_t commands = 0, draws = 0;
        auto counts = compatibleReplayCounts(replay, m.start, m.end, s.replayCounts);
        for (const auto &[id, e] : replay.frame().entries()) {
            if (e.category != 7 || id < m.start || id > m.end)
                continue;
            ++commands;
            const bool off = replay.options().disabled.contains(id);
            if (off)
                disabled.push_back(id);
            if (isDraw(e.type))
                ++draws;
        }
        for (const auto &[id, parameters] : replay.drawAutoResults())
            if (id >= m.start && id <= m.end)
                autos[std::to_string(id)] = autoResult(parameters);
        out["range"] = {{"start", m.start},
                        {"end", m.end},
                        {"inclusive", true},
                        {"command_count", commands},
                        {"draw_dispatch_count", draws},
                        {"disabled_commands", disabled}};
        out["replay_counts"] = counts;
        out["draw_auto_recomputed"] = autos;
        out["notes"].push_back("The interval brackets complete replay commands, including binding, lazy "
                               "resource uploads, experiment helpers and CPU submission gaps. It is not a "
                               "sum of isolated draw timings or original application GPU time.");
    }
    return out;
}
void exportGpuStatistics(const Json &report, const std::filesystem::path &directory) {
    const auto root = QString::fromStdWString(directory.wstring());
    if (!QDir().mkpath(root))
        throw std::runtime_error("Cannot create statistics output directory");
    auto save = [&](const char *name, const QByteArray &bytes) {
        QSaveFile file(root + '/' + name);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            throw std::runtime_error("Cannot save GPU statistics");
    };
    save("statistics.json", QByteArray::fromStdString(report.dump(2) + "\n"));
    QByteArray csv = QByteArray::fromHex("efbbbf") + "metric,value,unit\r\n";
    auto row = [&](const std::string &name, const Json &value, const char *unit) {
        csv += QByteArray::fromStdString(name + "," + (value.is_null() ? "" : value.dump()) + "," + unit +
                                         "\r\n");
    };
    for (auto key : {"ia_vertices", "ia_primitives", "vs_invocations", "gs_invocations", "gs_primitives",
                     "clip_invocations", "clip_primitives", "ps_invocations", "hs_invocations",
                     "ds_invocations", "cs_invocations"})
        row(key, report.at("pipeline").at(key), "count");
    row("occlusion_samples", report.at("occlusion_samples"), "samples");
    for (const auto &stream : report.at("stream_output").at("streams"))
        for (auto key : {"primitives_written", "primitives_storage_needed"})
            row("so_stream" + std::to_string(stream.at("stream").get<unsigned>()) + "_" + key, stream.at(key),
                "primitives");
    row("so_overflow", report.at("stream_output").at("overflow"), "bool");
    row("elapsed_ms", report.at("timing").at("elapsed_ms"), "ms");
    save("statistics.csv", csv);
}
} // namespace flora
