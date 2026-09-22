#include "MdIterations.h"
#include "Experiment.h"
#include "MdFrameRanges.h"
#include "MdIterationTransport.h"
#include "MetricAcquisitionPriority.h"
#include "MetricsDiscovery.h"
#include "replay/Device.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QSaveFile>
#include <cmath>
#include <psapi.h>
#include <set>
namespace flora {
using Json = nlohmann::json;
namespace {
bool count(const Json &v) {
    return v.is_number_integer() && (v.is_number_unsigned() || v.get<int64_t>() >= 0) &&
           v.get<uint64_t>() <= 100;
}
void save(const QString &path, const QByteArray &bytes) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error("Cannot save " + path.toStdString());
}
void saveJson(const QString &path, const Json &value) {
    save(path, QByteArray::fromStdString(value.dump(2)));
}
QByteArray read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot open " + path.toStdString());
    return file.readAll();
}
std::string digest(const QByteArray &bytes) {
    return sha256(Bytes(reinterpret_cast<const uint8_t *>(bytes.constData()), size_t(bytes.size())));
}
void png(const QString &path, const Image &image) {
    if (!QImage(image.rgba.data(), int(image.width), int(image.height), int(image.width * 4),
                QImage::Format_RGBA8888)
             .save(path))
        throw std::runtime_error("Cannot save " + path.toStdString());
}
Json loadedModules() {
    std::vector<HMODULE> handles(256);
    DWORD needed{};
    for (;;) {
        if (!EnumProcessModules(GetCurrentProcess(), handles.data(), DWORD(handles.size() * sizeof(HMODULE)),
                                &needed))
            throw std::runtime_error("Cannot enumerate loaded modules");
        if (needed <= handles.size() * sizeof(HMODULE))
            break;
        handles.resize(needed / sizeof(HMODULE));
    }
    Json result = Json::array();
    for (size_t i = 0; i < needed / sizeof(HMODULE); ++i) {
        wchar_t buffer[32768]{};
        const auto n = GetModuleFileNameW(handles[i], buffer, 32768);
        if (!n || n >= 32768)
            throw std::runtime_error("Cannot identify loaded module");
        const auto path = QString::fromWCharArray(buffer, int(n));
        const auto name = QFileInfo(path).fileName().toLower();
        if (path.toLower().contains("intelswtools\\gpa") || name == "dx11_player.dll" ||
            name == "shader-profiler-shared.dll")
            throw std::runtime_error("GPA runtime dependency detected: " + path.toStdString());
        result.push_back(path.toStdString());
    }
    return result;
}
Json adapter(const Replay &replay) {
    const auto d = replay.adapterDescription();
    return {{"description", QString::fromWCharArray(d.Description).toStdString()},
            {"vendor_id", d.VendorId},
            {"device_id", d.DeviceId},
            {"revision", d.Revision},
            {"dedicated_video_memory", d.DedicatedVideoMemory},
            {"shared_system_memory", d.SharedSystemMemory}};
}
} // namespace
Json collectMetricCatalog(const QString &directory, const QString &bridge) {
    QDir out(directory);
    if (out.exists() || !QDir().mkpath(out.absolutePath()))
        throw std::invalid_argument("Catalog output directory must be new");
    auto device = createDx11Device(false, false, 0x8086);
    MetricsDiscovery md(device.device.Get(), bridge);
    const auto modules = loadedModules();
    auto provenance = md.provenance();
    provenance["loaded_modules"] = modules;
    saveJson(out.filePath("catalog.json"), md.catalog());
    saveJson(out.filePath("provenance.json"), provenance);
    Json report = {{"schema", "FloraGPA native result 1"}, {"completed", true}, {"loaded_modules", modules}};
    saveJson(out.filePath("report.json"), report);
    return report;
}
Json scheduledMetricRequest(const Json &request) {
    const auto samples = request.value("samples", Json(1)), warmup = request.value("warmup", Json(1));
    if (!count(samples))
        throw std::invalid_argument("Samples must be 0..100");
    if (!count(warmup))
        throw std::invalid_argument("Warmup must be 0..100");
    const auto symbols = request.value("symbols", Json::array());
    std::set<std::string> unique;
    if (!symbols.is_array() || symbols.empty())
        throw std::invalid_argument("Choose distinct metric symbols");
    for (const auto &symbol : symbols)
        if (!symbol.is_string() || !unique.insert(symbol.get<std::string>()).second)
            throw std::invalid_argument("Choose distinct metric symbols");
    const auto weights = request.value("weights", Json::array());
    if (!weights.is_array())
        throw std::invalid_argument("Cached weights must be finite numbers");
    for (const auto &w : weights)
        if (!w.is_number() || !std::isfinite(w.get<double>()))
            throw std::invalid_argument("Cached weights must be finite numbers");
    const auto selected = request.value("requested_pass", Json(metricAllPasses));
    const auto mapping = request.value("pass_mapping", Json::array());
    metricIterationPass(selected, mapping);
    auto ranges = request.value("frame_ranges", Json());
    if (ranges == "all")
        ranges = nullptr;
    return {{"samples", samples},    {"warmup", warmup},           {"symbols", symbols},
            {"weights", weights},    {"requested_pass", selected}, {"pass_mapping", mapping},
            {"frame_ranges", ranges}};
}
Json collectScheduledMetrics(const Frame &frame, const QString &directory, const Json &request,
                             const QString &experimentPath, const QString &bridge,
                             const std::function<bool()> &cancel,
                             const std::function<void(const Json &)> &progress) {
    const auto cfg = scheduledMetricRequest(request);
    QDir out(QFileInfo(directory).absoluteFilePath());
    if (out.exists())
        throw std::invalid_argument("Scheduled output directory must be new");
    if (!QDir().mkpath(out.absolutePath()))
        throw std::runtime_error("Cannot create scheduled output directory");
    QString frozen;
    if (!experimentPath.isEmpty()) {
        frozen = out.filePath("experiment.json");
        save(frozen, read(experimentPath));
    }
    ReplayOptions options;
    options.vendor = 0x8086;
    std::unique_ptr<Experiment> edits;
    Json identity;
    if (!frozen.isEmpty()) {
        edits = std::make_unique<Experiment>(frame);
        edits->load(frozen, frame);
        edits->apply(frame, options);
        const auto bytes = read(frozen);
        if (Json::parse(bytes.toStdString()) != edits->document())
            throw std::invalid_argument("Experiment changed while loading its identity");
        Json shaders = Json::object();
        for (const auto &[id, data] : options.shaders)
            shaders[std::to_string(id)] = sha256(data);
        identity = {{"sha256", digest(bytes)},
                    {"cursor", edits->document().at("cursor")},
                    {"revisions", edits->document().at("history").size()},
                    {"shaders", shaders}};
    }
    Replay replay(frame, options);
    MetricsDiscovery md(replay.nativeDevice(), bridge);
    const auto selection = selectFrameMetricRanges(replay.frame(), cfg.at("frame_ranges"), edits.get());
    const auto &ranges = selection.at("ranges");
    Json mapped = Json::array(), triples = Json::array();
    for (const auto &r : ranges) {
        mapped.push_back({r["start_event"], r["end_event"]});
        triples.push_back(r["frame_range"]);
    }
    if (!cfg["weights"].empty() && cfg["weights"].size() != ranges.size())
        throw std::invalid_argument("Cached weight count differs from selected ranges");
    saveJson(out.filePath("frame-ranges.json"), selection);
    saveJson(out.filePath("catalog.json"), md.catalog());
    const auto sourceHash = frame.sha256();
    for (unsigned i = 0; i < cfg["warmup"].get<unsigned>(); ++i)
        replay.run();
    replay.run();
    const auto baseline = replay.output();
    const auto baselineHash = sha256(baseline.rgba);
    png(out.filePath("baseline.png"), baseline);
    Json records = Json::array(), replays = Json::array(), metadata = Json::array();
    MdIterationTransport transport(
        md, replay.nativeDevice(), cfg.at("symbols"),
        [&](MdIterationTransport &session, uint32_t pass, const Json &requested) {
            if (requested != mapped)
                throw std::invalid_argument("Recovered range mapping differs from requested frame scope");
            const auto index = replays.size(), start = records.size();
            const auto name = QString("replay-%1").arg(index, 3, 10, QLatin1Char('0'));
            if (!out.mkdir(name))
                throw std::runtime_error("Cannot create scheduled replay directory");
            QDir folder(out.filePath(name));
            const auto description = md.selected();
            bool existing = false;
            for (auto &set : metadata)
                if (set["name"] == description["name"]) {
                    set = description;
                    existing = true;
                    break;
                }
            if (!existing)
                metadata.push_back(description);
            const auto record = [&, description, index, pass, name](const Json &info, MetricResult &result) {
                const auto filename =
                    name + "/range-" + QString::number(info.at("range_index").get<qulonglong>()) + ".bin";
                save(out.filePath(filename), QByteArray(reinterpret_cast<const char *>(result.raw.data()),
                                                        qsizetype(result.raw.size())));
                const auto n = description.at("metrics").size();
                Json values = Json::array(), information = Json::array();
                const auto &all = result.values.at("values");
                for (size_t i = 0; i < all.size(); ++i)
                    (i < n ? values : information).push_back(all[i]);
                Json row = {{"range_index", info.at("range_index")},
                            {"event", nullptr},
                            {"start_event", info.at("start_event")},
                            {"end_event", info.at("end_event")},
                            {"set", description.at("name")},
                            {"pass_index", index},
                            {"selected_pass", pass},
                            {"sample_index", 0},
                            {"available", result.values.at("available")},
                            {"unavailable_reasons", result.values.at("unavailable_reasons")},
                            {"values", values},
                            {"information", information},
                            {"raw_report", filename.toStdString()},
                            {"raw_sha256", sha256(result.raw)},
                            {"raw_size", result.raw.size()},
                            {"calculated_reports", result.values.at("reports")}};
                records.push_back(row);
                session.deliver(description, row);
            };
            MetricCommandCounterClient client{
                [&](auto consume) { session.begin(std::move(consume)); }, [&] { session.submit(); }, {}};
            FrameRangeCounter counter(client, ranges, record, true);
            const auto currentRecords = [&] {
                return Json(records.begin() + ptrdiff_t(start), records.end());
            };
            try {
                replay.run({}, {}, {}, [&](Id id, const auto &body) { counter.scope(id, body); });
                const auto image = replay.output();
                session.flush();
                const auto boundaries = counter.verify();
                Json actual = Json::array(), expected = Json::array();
                for (size_t i = start; i < records.size(); ++i)
                    actual.push_back(records[i]["range_index"]);
                for (const auto &r : ranges)
                    expected.push_back(r["range_index"]);
                Json valid = {{"replay_index", index},
                              {"selected_pass", pass},
                              {"set", description.at("name")},
                              {"metrics", session.passMetricIds(pass)},
                              {"image_matches", image.width == baseline.width &&
                                                    image.height == baseline.height &&
                                                    image.rgba == baseline.rgba},
                              {"rgba_sha256", sha256(image.rgba)},
                              {"range_mapping_valid", actual == expected},
                              {"boundaries", boundaries},
                              {"record_span", {start, records.size()}}};
                png(folder.filePath("output.png"), image);
                saveJson(folder.filePath("raw-results.json"),
                         {{"validation", valid}, {"records", currentRecords()}});
                replays.push_back(valid);
                if (valid["image_matches"] != true || valid["range_mapping_valid"] != true)
                    throw std::invalid_argument("Scheduled replay validation failed");
                if (progress)
                    progress({{"replay", index},
                              {"set", description.at("name")},
                              {"ranges", actual.size()},
                              {"image_matches", true}});
            } catch (...) {
                saveJson(folder.filePath("partial-records.json"), currentRecords());
                throw;
            }
        });
    Json result, failure;
    std::exception_ptr error;
    try {
        result =
            MetricIterationRunner(transport).executeFrame(replay.frame(), transport.requestedIds(), triples,
                                                          {{"samples", cfg["samples"]},
                                                           {"requested_pass", cfg["requested_pass"]},
                                                           {"pass_mapping", cfg["pass_mapping"]},
                                                           {"weights", cfg["weights"]}},
                                                          cancel);
        if (result.at("complete") != true)
            throw MetricIterationError(result);
    } catch (const MetricIterationError &e) {
        error = std::current_exception();
        failure = {{"type", "MetricIterationError"}, {"message", e.what()}};
    } catch (...) {
        error = std::current_exception();
        failure = metricPriorityFailure(error);
    }
    try {
        transport.close();
    } catch (...) {
        error = std::current_exception();
    }
    saveJson(out.filePath("scheduler-audit.json"),
             {{"protocol", result}, {"failure", failure}, {"adapter", transport.audit()}});
    saveJson(out.filePath("raw-records.json"), records);
    saveJson(out.filePath("publisher-values.json"), transport.publisherValues().report());
    transport.publisherValues().writeCsv(out.filePath("publisher-values.csv"));
    if (error)
        std::rethrow_exception(error);
    if (Frame(frame.path()).sha256() != sourceHash)
        throw std::invalid_argument("Source frame changed during scheduled collection");
    if (!identity.is_null() && digest(read(frozen)) != identity.at("sha256").get<std::string>())
        throw std::invalid_argument("Frozen experiment changed during collection");
    const auto modules = loadedModules();
    Json cells = Json::array();
    const auto &ids = transport.requestedIds(), &descriptors = transport.catalog();
    if (result.at("values").size() != ranges.size())
        throw std::invalid_argument("Scheduled result range count mismatch");
    for (size_t r = 0; r < ranges.size(); ++r) {
        const auto &info = ranges[r], &row = result["values"][r];
        if (row.size() != ids.size())
            throw std::invalid_argument("Scheduled result metric count mismatch");
        for (size_t m = 0; m < ids.size(); ++m) {
            const auto &d = descriptors[m], &v = row[m], &definition = d.at("definition");
            const bool measured = !v.at("values").empty();
            auto cell = metricSampleSummary(v.at("values"));
            cell.update({{"range_index", info["range_index"]},
                         {"start_event", info["start_event"]},
                         {"end_event", info["end_event"]},
                         {"metric", d["symbol"]},
                         {"metric_id", ids[m]},
                         {"label", definition.at("label")},
                         {"unit", d["symbol"] == "GpuTime" ? Json("us") : definition.at("unit")},
                         {"measured", measured},
                         {"values", v["values"]},
                         {"kind", measured ? v["kind"] : d["kind"]},
                         {"weight", measured ? v["weight"] : Json()}});
            cells.push_back(std::move(cell));
        }
    }
    const auto audit = transport.audit();
    if (audit["owned"] != 0 || audit["native_pool"]["cached"] != 0 || !audit["subscriptions"].empty() ||
        audit["local_lock_depth"] != 0)
        throw std::invalid_argument("Scheduled collector did not release all resources");
    Json profile = {
        {"schema_version", 1},
        {"mode", "recovered_metric_iterations"},
        {"auxiliary_semantics", "dx11_zero_query_flags"},
        {"frame", QDir::toNativeSeparators(
                      QFileInfo(QString::fromStdWString(frame.path().wstring())).absoluteFilePath())
                      .toStdString()},
        {"frame_sha256", sourceHash},
        {"experiment", identity},
        {"selection", selection},
        {"requested_metrics", cfg["symbols"]},
        {"descriptor_ids", "Independent MD catalog IDs; not GPA global IDs"},
        {"descriptors", descriptors},
        {"plan", transport.plan()},
        {"requested_samples", cfg["samples"]},
        {"warmup_count", cfg["warmup"]},
        {"requested_pass", cfg["requested_pass"]},
        {"pass_mapping", cfg["pass_mapping"]},
        {"weight_source", cfg["weights"].empty() ? "independent_weight_replay" : "caller_cache"},
        {"supplied_weights", cfg["weights"]},
        {"actual_iteration_count", result.at("iteration_count")},
        {"protocol", result},
        {"value_semantics", "publisher_binary64"},
        {"baseline_rgba_sha256", baselineHash},
        {"replays", replays},
        {"sets", metadata},
        {"records", records},
        {"metrics", cells},
        {"publisher_values", "publisher-values.json"},
        {"scheduler_audit", "scheduler-audit.json"},
        {"adapter", adapter(replay)},
        {"adapter_luid", md.catalog().at("luid")},
        {"provenance", md.provenance()},
        {"loaded_modules", modules},
        {"production_gpa_dependency", false},
        {"arbitration", "gpa_priority_v2"},
        {"complete_original_scheduling", false},
        {"limits",
         {"Only mapped pass 0 repeats; nonzero and AllPasses run one iteration. Unmeasured columns remain "
          "explicitly empty.",
          "Unavailable raw reports become null samples. Native status success is separate from acquisition "
          "completeness and metric availability.",
          "One immediate context with the recovered device OA priority mutex; same Windows user/session "
          "namespace, no multi-device scheduling.",
          "Lock timeouts abort collection. Uniform MD collectors share this OA namespace; it does not lock "
          "unrelated GPU workloads.",
          "Each pass replays the whole frame; counters surround complete independent Engine commands "
          "including binding/preparation.",
          "Each replay resets the receiver and closes subscriptions/counters. Publisher clock and Busy state "
          "persist across weight and metric passes.",
          "Compatibility uses actual MD set membership; global GPA catalog, parallel outputs and all "
          "original timing points are not recovered.",
          "Presentation equality does not establish equality of every intermediate resource or performance "
          "counter."}}};
    const std::vector<std::string> fields{
        "range_index", "start_event", "end_event",     "metric",        "label",
        "unit",        "measured",    "total_samples", "valid_samples", "invalid_samples",
        "median",      "minimum",     "maximum",       "mean",          "variation_percent"};
    save(out.filePath("scheduled-metrics.csv"), QByteArray::fromStdString(metricCsv(fields, cells)));
    saveJson(out.filePath("scheduled-profile.json"), profile);
    return profile;
}
} // namespace flora
