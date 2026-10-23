#include "NativeCounters.h"
#include "replay/NativeSample.h"
#include <QDir>
#include <QSaveFile>
#include <deque>

namespace flora {
namespace {
using Json = nlohmann::json;
struct Counter {
    const char *id;
    const char *name;
    uint64_t D3D11_QUERY_DATA_PIPELINE_STATISTICS::*member;
};
constexpr Counter pipelineCounters[]{
    {"input.vertices", "Input vertices", &D3D11_QUERY_DATA_PIPELINE_STATISTICS::IAVertices},
    {"input.primitives", "Input primitives", &D3D11_QUERY_DATA_PIPELINE_STATISTICS::IAPrimitives},
    {"vs.invocations", "Vertex shader invocations", &D3D11_QUERY_DATA_PIPELINE_STATISTICS::VSInvocations},
    {"gs.invocations", "Geometry shader invocations", &D3D11_QUERY_DATA_PIPELINE_STATISTICS::GSInvocations},
    {"gs.primitives", "Geometry shader primitives", &D3D11_QUERY_DATA_PIPELINE_STATISTICS::GSPrimitives},
    {"clipper.invocations", "Clipper invocations", &D3D11_QUERY_DATA_PIPELINE_STATISTICS::CInvocations},
    {"clipper.primitives", "Clipper primitives", &D3D11_QUERY_DATA_PIPELINE_STATISTICS::CPrimitives},
    {"ps.invocations", "Pixel shader invocations", &D3D11_QUERY_DATA_PIPELINE_STATISTICS::PSInvocations},
    {"hs.invocations", "Hull shader invocations", &D3D11_QUERY_DATA_PIPELINE_STATISTICS::HSInvocations},
    {"ds.invocations", "Domain shader invocations", &D3D11_QUERY_DATA_PIPELINE_STATISTICS::DSInvocations},
    {"cs.invocations", "Compute shader invocations", &D3D11_QUERY_DATA_PIPELINE_STATISTICS::CSInvocations}};
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
        {{"id", "gpu.duration"}, {"name", "GPU duration"}, {"unit", "seconds"}, {"type", "float64"}});
    for (const auto &counter : pipelineCounters)
        catalog.push_back(
            {{"id", counter.id}, {"name", counter.name}, {"unit", "count"}, {"type", "uint64"}});
    catalog.push_back({{"id", "samples.passed"},
                       {"name", "Samples passing depth and stencil"},
                       {"unit", "count"},
                       {"type", "uint64"}});
    auto drain = [&] {
        while (!pending.empty()) {
            checkCancellation(cancelled);
            auto &entry = pending.front();
            const auto sample = entry.sample->result();
            Json values{{"gpu.duration",
                         sample.timingAvailable
                             ? Json(double(sample.endTick - sample.startTick) / double(sample.frequency))
                             : Json(nullptr)},
                        {"samples.passed", sample.occlusion}};
            for (const auto &counter : pipelineCounters)
                values[counter.id] = sample.pipeline.*counter.member;
            rows.push_back({{"event", entry.event},
                            {"api", commandName(replay.frame().entry(entry.event).type)},
                            {"enabled", !replay.options().disabled.contains(entry.event)},
                            {"boundary", "submission_after_binding"},
                            {"values", std::move(values)},
                            {"timing_valid", sample.timingAvailable},
                            {"disjoint", sample.disjoint},
                            {"frequency_hz", sample.frequency},
                            {"start_tick", sample.startTick},
                            {"end_tick", sample.endTick}});
            pending.pop_front();
        }
    };
    replay.run([&](Id, size_t, size_t) { checkCancellation(cancelled); },
               [&](Id event, bool after, ID3D11DeviceContext *context, const auto &) {
                   checkCancellation(cancelled);
                   if (!after) {
                       pending.push_back(
                           {event, std::make_unique<NativeSample>(replay.nativeDevice(), context)});
                       pending.back().sample->begin();
                   } else {
                       if (pending.empty() || pending.back().event != event)
                           throw std::runtime_error("Unpaired native counter boundary");
                       pending.back().sample->end();
                       // Bound driver query storage without replaying the prefix once per event.
                       if (pending.size() >= 64)
                           drain();
                   }
               });
    drain();
    return {{"schema", 1},
            {"kind", "event_counters"},
            {"source", "native_dx11_queries"},
            {"catalog", catalog},
            {"events", rows},
            {"replay_count", 1},
            {"pending_sample_limit", 64},
            {"notes",
             {"Query batches are synchronized after at most 64 events; this can affect scheduling.",
              "Values describe the actual replay device, including device-specific predication behavior.",
              "Disjoint or otherwise invalid durations are null; integer query values are retained."}}};
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
} // namespace flora
