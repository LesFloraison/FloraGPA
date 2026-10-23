#include "NativeCounters.h"
#include "core/Commands.h"
#include "replay/NativeSample.h"
#include <QDir>
#include <QSaveFile>
#include <cmath>
#include <deque>
#include <set>

namespace flora {
namespace {
using Json = nlohmann::json;
struct Counter {
    const char *id;
    const char *name;
    const char *description;
    uint64_t D3D11_QUERY_DATA_PIPELINE_STATISTICS::*member;
};
constexpr Counter pipelineCounters[]{
    {"input.vertices", "Input vertices", "Vertex references processed by the input assembler.",
     &D3D11_QUERY_DATA_PIPELINE_STATISTICS::IAVertices},
    {"input.primitives", "Input primitives", "Primitives processed by the input assembler.",
     &D3D11_QUERY_DATA_PIPELINE_STATISTICS::IAPrimitives},
    {"vs.invocations", "Vertex shader invocations", "Executions of the vertex shader.",
     &D3D11_QUERY_DATA_PIPELINE_STATISTICS::VSInvocations},
    {"gs.invocations", "Geometry shader invocations", "Executions of the geometry shader.",
     &D3D11_QUERY_DATA_PIPELINE_STATISTICS::GSInvocations},
    {"gs.primitives", "Geometry shader primitives", "Primitives emitted by the geometry shader.",
     &D3D11_QUERY_DATA_PIPELINE_STATISTICS::GSPrimitives},
    {"clipper.invocations", "Clipper invocations", "Primitives submitted to rasterization.",
     &D3D11_QUERY_DATA_PIPELINE_STATISTICS::CInvocations},
    {"clipper.primitives", "Clipper primitives", "Primitives remaining after clipping.",
     &D3D11_QUERY_DATA_PIPELINE_STATISTICS::CPrimitives},
    {"ps.invocations", "Pixel shader invocations",
     "Driver-reported pixel shader executions; helper-pixel accounting may vary.",
     &D3D11_QUERY_DATA_PIPELINE_STATISTICS::PSInvocations},
    {"hs.invocations", "Hull shader invocations", "Executions of the hull shader.",
     &D3D11_QUERY_DATA_PIPELINE_STATISTICS::HSInvocations},
    {"ds.invocations", "Domain shader invocations", "Executions of the domain shader.",
     &D3D11_QUERY_DATA_PIPELINE_STATISTICS::DSInvocations},
    {"cs.invocations", "Compute shader invocations", "Executions of the compute shader.",
     &D3D11_QUERY_DATA_PIPELINE_STATISTICS::CSInvocations}};
void save(const QString &path, const QByteArray &bytes) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error("Cannot publish native counters");
}
} // namespace
Json collectNativeEventCounters(Replay &replay, const CancelCheck &cancelled) {
    if (replay.options().measurement || replay.options().timings || replay.options().before)
        throw std::invalid_argument("Event counters require their own complete replay boundaries");
    struct Pending {
        Id event;
        std::unique_ptr<NativeSample> sample;
    };
    std::deque<Pending> pending;
    Json rows = Json::array(), catalog = Json::array();
    catalog.push_back(
        {{"id", "gpu.duration"},
         {"name", "GPU duration"},
         {"unit", "seconds"},
         {"type", "float64"},
         {"description", "Elapsed GPU timestamp interval; unavailable for disjoint or invalid samples."}});
    for (const auto &counter : pipelineCounters)
        catalog.push_back({{"id", counter.id},
                           {"name", counter.name},
                           {"unit", "count"},
                           {"type", "uint64"},
                           {"description", counter.description}});
    catalog.push_back({{"id", "samples.passed"},
                       {"name", "Samples passing depth and stencil"},
                       {"unit", "count"},
                       {"type", "uint64"},
                       {"description", "Samples accepted by depth and stencil testing."}});
    for (auto &counter : catalog) {
        counter["category"] = "D3D11 queries";
        counter["byte_width"] = 8;
        counter["identity"] = "flora.dx11.counter/" + counter.at("id").get<std::string>();
    }
    auto drain = [&] {
        bool synchronize = true;
        while (!pending.empty()) {
            checkCancellation(cancelled);
            auto &entry = pending.front();
            // Every sample in this batch has already ended on the same context.
            // The first completion fence therefore covers all remaining query reads.
            const auto sample = entry.sample->result(synchronize);
            synchronize = false;
            Json values{{"gpu.duration",
                         sample.timingAvailable
                             ? Json(double(sample.endTick - sample.startTick) / double(sample.frequency))
                             : Json(nullptr)},
                        {"samples.passed", sample.occlusion}};
            for (const auto &counter : pipelineCounters)
                values[counter.id] = sample.pipeline.*counter.member;
            rows.push_back(
                {{"event", entry.event},
                 {"api", commandName(replay.frame().entry(entry.event).type)},
                 {"enabled", !replay.options().disabled.contains(entry.event)},
                 {"boundary", isDraw(replay.frame().entry(entry.event).type) ? "submission_after_binding"
                                                                             : "resource_write_command"},
                 {"values", std::move(values)},
                 {"timing_valid", sample.timingAvailable},
                 {"disjoint", sample.disjoint},
                 {"frequency_hz", sample.frequency},
                 {"start_tick", sample.startTick},
                 {"end_tick", sample.endTick}});
            pending.pop_front();
        }
    };
    const ReplayBoundaryObserver measure = [&](Id event, bool after, ID3D11DeviceContext *context,
                                               const auto &) {
        checkCancellation(cancelled);
        if (!after) {
            pending.push_back({event, std::make_unique<NativeSample>(replay.nativeDevice(), context)});
            pending.back().sample->begin();
        } else {
            if (pending.empty() || pending.back().event != event)
                throw std::runtime_error("Unpaired native counter boundary");
            pending.back().sample->end();
            // Bound driver query storage without replaying the prefix once per event.
            if (pending.size() >= 64)
                drain();
        }
    };
    replay.run([&](Id, size_t, size_t) { checkCancellation(cancelled); }, measure,
               [&](Id event, bool after, ID3D11DeviceContext *context, const auto &objects) {
                   if (isWritableCommand(replay.frame().entry(event).type))
                       measure(event, after, context, objects);
               });
    drain();
    return {
        {"schema", 1},
        {"kind", "event_counters"},
        {"source", "native_dx11_queries"},
        {"catalog", catalog},
        {"events", rows},
        {"replay_count", 1},
        {"pending_sample_limit", 64},
        {"notes",
         {"Query batches are synchronized after at most 64 events; this can affect scheduling.",
          "Values describe the actual replay device, including device-specific predication behavior.",
          "Disjoint or otherwise invalid durations are null; integer query values are retained.",
          "Draw and dispatch intervals exclude input preparation; resource-write intervals include command "
          "implementation work.",
          "Catalog identities belong to FloraGPA; external vendor-provider catalogs are not enumerated."}}};
}
void exportNativeEventCounters(const Json &report, const std::filesystem::path &directory) {
    const auto root = QString::fromStdWString(directory.wstring());
    if (!QDir().mkpath(root))
        throw std::runtime_error("Cannot create native counter output directory");
    save(root + "/counters.json", QByteArray::fromStdString(report.dump(2) + "\n"));
    std::string csv = "event,counter,value,unit,valid\r\n";
    for (const auto &row : report.at("events"))
        for (const auto &counter : report.at("catalog")) {
            const auto id = counter.at("id").get<std::string>();
            const auto &value = row.at("values").at(id);
            csv += std::to_string(row.at("event").get<Id>()) + ',' + id + ',' +
                   (value.is_null() ? "" : value.dump()) + ',' + counter.at("unit").get<std::string>() + ',' +
                   (value.is_null() ? "false" : "true") + "\r\n";
        }
    save(root + "/counters.csv", QByteArray::fromStdString(csv));
}
Json nativeCounterRows(const Json &report) {
    if (report.at("schema") != 1 || report.at("kind") != "event_counters" ||
        report.at("source") != "native_dx11_queries" || !report.at("catalog").is_array() ||
        !report.at("events").is_array())
        throw std::runtime_error("Invalid native counter report");
    const auto &catalog = report.at("catalog");
    std::set<std::string> identifiers;
    for (const auto &counter : catalog) {
        const auto id = counter.at("id").get<std::string>();
        if (id.empty() || !identifiers.insert(id).second || !counter.at("name").is_string() ||
            (counter.at("type") != "float64" && counter.at("type") != "uint64") ||
            (counter.at("unit") != "seconds" && counter.at("unit") != "count"))
            throw std::runtime_error("Invalid native counter catalog");
    }
    Json rows = Json::array();
    std::set<Id> events;
    for (const auto &event : report.at("events")) {
        const auto &id = event.at("event");
        if (!id.is_number_integer() || (!id.is_number_unsigned() && id.get<int64_t>() <= 0) || id == 0 ||
            !events.insert(id.get<Id>()).second || !event.at("values").is_object() ||
            event.at("values").size() != catalog.size() || !event.at("enabled").is_boolean())
            throw std::runtime_error("Invalid native counter event");
        for (const auto &counter : catalog) {
            const auto key = counter.at("id").get<std::string>();
            const auto &value = event.at("values").at(key);
            if (counter.at("type") == "uint64") {
                if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<int64_t>() < 0))
                    throw std::runtime_error("Invalid native integer counter");
            } else if (!value.is_null() &&
                       (!value.is_number() || !std::isfinite(value.get<double>()) || value.get<double>() < 0))
                throw std::runtime_error("Invalid native duration counter");
            if (key == "gpu.duration" && (!event.at("timing_valid").is_boolean() ||
                                          event.at("timing_valid").get<bool>() == value.is_null()))
                throw std::runtime_error("Native counter validity mismatch");
            rows.push_back({{"event", id},
                            {"counter", key},
                            {"name", counter.at("name")},
                            {"unit", counter.at("unit")},
                            {"value", value},
                            {"valid", !value.is_null()},
                            {"enabled", event.at("enabled")}});
        }
    }
    return rows;
}
} // namespace flora
