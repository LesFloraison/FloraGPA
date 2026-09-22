#include "MdProfile.h"
#include "Experiment.h"
#include "MdFrameRanges.h"
#include "MdSamplePool.h"
#include "MdScheduledPool.h"
#include "MetricAcquisitionPriority.h"
#include "MetricAnalysis.h"
#include "MetricPublisher.h"
#include "MetricsDiscovery.h"
#include <Psapi.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QSaveFile>
#include <Windows.h>
#include <set>
namespace flora {
using Json = nlohmann::json;
namespace {
QByteArray read(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read profile input");
    return f.readAll();
}
void save(const QString &path, const QByteArray &bytes) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit())
        throw std::runtime_error("Cannot save profile artifact");
}
void saveJson(const QString &path, const Json &value) {
    save(path, QByteArray::fromStdString(value.dump(2)));
}
std::string digest(const QByteArray &v) {
    return sha256(Bytes(reinterpret_cast<const uint8_t *>(v.constData()), size_t(v.size())));
}
void png(const QString &path, const Image &image) {
    if (!QImage(image.rgba.data(), int(image.width), int(image.height), int(image.width * 4),
                QImage::Format_RGBA8888)
             .save(path))
        throw std::runtime_error("Cannot save profile image");
}
Json modules() {
    std::vector<HMODULE> handles(512);
    DWORD needed{};
    for (;;) {
        if (!EnumProcessModules(GetCurrentProcess(), handles.data(), DWORD(handles.size() * sizeof(HMODULE)),
                                &needed))
            throw std::runtime_error("Cannot enumerate profile modules");
        if (needed <= handles.size() * sizeof(HMODULE))
            break;
        handles.resize(needed / sizeof(HMODULE));
    }
    Json result = Json::array();
    for (size_t i = 0; i < needed / sizeof(HMODULE); ++i) {
        wchar_t buffer[32768]{};
        const auto n = GetModuleFileNameW(handles[i], buffer, 32768);
        if (!n || n >= 32768)
            throw std::runtime_error("Cannot identify profile module");
        const auto path = QString::fromWCharArray(buffer, int(n));
        const auto name = QFileInfo(path).fileName().toLower();
        if (path.toLower().contains("intelswtools\\gpa") || name == "dx11_player.dll" ||
            name == "shader-profiler-shared.dll")
            throw std::runtime_error("GPA runtime dependency detected");
        result.push_back(path.toStdString());
    }
    return result;
}
Json difference(const Image &a, const Image &b) {
    if (a.width != b.width || a.height != b.height)
        return {{"dimensions_match", false}};
    unsigned count{};
    std::array<int, 4> bounds{int(a.width), int(a.height), -1, -1};
    Json examples = Json::array();
    for (size_t i = 0; i < a.rgba.size(); i += 4) {
        if (std::equal(a.rgba.begin() + i, a.rgba.begin() + i + 4, b.rgba.begin() + i))
            continue;
        const int x = int(i / 4 % a.width), y = int(i / 4 / a.width);
        ++count;
        bounds = {std::min(bounds[0], x), std::min(bounds[1], y), std::max(bounds[2], x),
                  std::max(bounds[3], y)};
        if (examples.size() < 16)
            examples.push_back(
                {{"x", x},
                 {"y", y},
                 {"baseline", std::vector<uint8_t>(a.rgba.begin() + i, a.rgba.begin() + i + 4)},
                 {"instrumented", std::vector<uint8_t>(b.rgba.begin() + i, b.rgba.begin() + i + 4)}});
    }
    return {{"dimensions_match", true},
            {"changed_pixels", count},
            {"bounds", count ? Json(bounds) : Json()},
            {"examples", examples}};
}
bool count(const Json &v, unsigned low) {
    return v.is_number_integer() && (v.is_number_unsigned() || v.get<int64_t>() >= 0) &&
           v.get<uint64_t>() >= low && v.get<uint64_t>() <= 100;
}
const std::vector<std::string> statsFields = {
    "set",    "event",   "start_event",   "end_event",     "metric",
    "label",  "unit",    "total_samples", "valid_samples", "invalid_samples",
    "median", "minimum", "maximum",       "mean",          "variation_percent"};
const std::vector<std::string> requestFields = {"metric",       "label",       "unit",      "set",
                                                "event",        "start_event", "end_event", "pass_index",
                                                "sample_index", "value",       "available", "value_type"};
} // namespace
Json uniformMetricRequest(const Json &request) {
    const auto samples = request.value("samples", Json(1)), warmup = request.value("warmup", Json(1));
    if (!count(samples, 1))
        throw std::invalid_argument("Samples must be 1..100");
    if (!count(warmup, 0))
        throw std::invalid_argument("Warmup must be 0..100");
    const auto symbols = request.value("symbols", Json()),
               sets = request.value("sets", Json::array({"RenderBasic"}));
    const auto events = request.value("events", Json::array()), interval = request.value("interval", Json()),
               ranges = request.value("frame_ranges", Json());
    if (!symbols.is_null() && sets != Json::array())
        throw std::invalid_argument("Metric symbols and explicit metric sets are mutually exclusive");
    if (symbols.is_null() && !sets.is_null()) {
        std::set<std::string> names;
        if (!sets.is_array() || sets.empty())
            throw std::invalid_argument("Choose distinct hardware metric sets");
        for (const auto &name : sets)
            if (!name.is_string() || !names.insert(name.get<std::string>()).second)
                throw std::invalid_argument("Choose distinct hardware metric sets");
    }
    if (!events.is_array())
        throw std::invalid_argument("Events must be an array");
    for (const auto &event : events)
        if (!event.is_number_integer() || (!event.is_number_unsigned() && event.get<int64_t>() < 0))
            throw std::invalid_argument("Selected events are absent or disabled");
    if (!interval.is_null() && !events.empty())
        throw std::invalid_argument("Per-event selection and interval collection are mutually exclusive");
    if (!ranges.is_null() && (!events.empty() || !interval.is_null()))
        throw std::invalid_argument("Frame ranges cannot be combined with events or interval");
    if (!interval.is_null() && (!interval.is_array() || interval.size() != 2))
        throw std::invalid_argument("Interval requires start and end");
    return {{"samples", samples},     {"warmup", warmup},
            {"sets", sets},           {"symbols", symbols},
            {"events", events},       {"interval", interval},
            {"frame_ranges", ranges}, {"publisher_values", request.value("publisher_values", Json(false))}};
}
Json collectUniformMetrics(const Frame &frame, const QString &directory, const Json &request,
                           const QString &experimentPath, const QString &bridge,
                           const std::function<bool()> &cancel,
                           const std::function<void(const Json &)> &progress) {
    const auto cfg = uniformMetricRequest(request);
    QDir out(QFileInfo(directory).absoluteFilePath());
    if (out.exists() || !QDir().mkpath(out.absolutePath()))
        throw std::invalid_argument("Profile output directory must be new");
    auto checkCancel = [&] {
        if (cancel && cancel())
            throw std::runtime_error("Cancelled");
    };
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
    MetricAcquisitionPriority priority(md, out.path());
    Json profile;
    priority.run([&](auto &arbitration) {
        Json plan, sets = cfg.at("sets");
        if (!cfg["symbols"].is_null()) {
            plan = planMetrics(md.catalog(), cfg["symbols"]);
            sets = Json::array();
            for (const auto &p : plan["passes"])
                sets.push_back(p["set"]);
            saveJson(out.filePath("metric-plan.json"), plan);
        }
        if (sets.is_null()) {
            sets = Json::array();
            for (const auto &s : md.catalog()["sets"])
                sets.push_back(s["name"]);
        }
        if (sets.empty())
            throw std::invalid_argument("Driver exposes no DX11 hardware metric sets");
        const auto sourceHash = frame.sha256();
        std::map<Id, Json> available;
        for (const auto &[id, e] : replay.frame().entries())
            if (e.category == 7 && isDraw(e.type) && (!edits || edits->enabled(id))) {
                const auto event = replay.frame().event(id);
                const auto state =
                    effectiveBindings(replay.frame(), id, replay.frame().state(event.state), options);
                Json shaders = Json::object();
                const char *stages[] = {"vs", "hs", "ds", "gs", "ps", "cs"};
                for (size_t i = 0; i < 6; ++i)
                    shaders[stages[i]] = state.stages[i].shader;
                available[id] = {{"event", id}, {"api", commandName(e.type)}, {"shaders", shaders}};
            }
        Json interval, rangeInfo;
        std::set<Id> selected;
        if (!cfg["frame_ranges"].is_null()) {
            rangeInfo = selectFrameMetricRanges(
                replay.frame(), cfg["frame_ranges"] == "all" ? Json() : cfg["frame_ranges"], edits.get());
            for (const auto &r : rangeInfo["ranges"])
                for (const auto &id : r["enabled_draw_events"])
                    selected.insert(id.get<Id>());
            saveJson(out.filePath("frame-ranges.json"), rangeInfo);
        } else if (!cfg["interval"].is_null()) {
            interval =
                selectMetricInterval(replay.frame(), edits.get(), cfg["interval"][0], cfg["interval"][1]);
            for (const auto &id : interval["enabled_draw_events"])
                selected.insert(id.get<Id>());
        } else {
            if (cfg["events"].empty())
                for (const auto &[id, row] : available)
                    selected.insert(id);
            else
                for (const auto &id : cfg["events"])
                    selected.insert(id.get<Id>());
            if (selected.empty() ||
                std::any_of(selected.begin(), selected.end(), [&](Id id) { return !available.contains(id); }))
                throw std::invalid_argument("Selected events are absent or disabled");
        }
        for (const auto &name : sets)
            md.select(name.get<std::string>());
        saveJson(out.filePath("catalog.json"), md.catalog());
        for (unsigned i = 0; i < cfg["warmup"].get<unsigned>(); ++i) {
            checkCancel();
            replay.run();
        }
        checkCancel();
        replay.run();
        const auto baseline = replay.output();
        png(out.filePath("baseline.png"), baseline);
        Json passes = Json::array(), rows = Json::array(), metadata = Json::array();
        std::unique_ptr<MetricPublisherValues> publisher;
        std::unique_ptr<MdScheduledPool> scheduled;
        std::unique_ptr<MdSamplePool> sampled;
        std::unique_ptr<MdCounter> drained;
        if (cfg["publisher_values"].get<bool>()) {
            publisher = std::make_unique<MetricPublisherValues>(md);
            if (md.supportsSamples() && md.supportsReuse())
                scheduled = std::make_unique<MdScheduledPool>(md, *publisher);
            else if (md.supportsSamples())
                sampled = std::make_unique<MdSamplePool>(md, *publisher);
            else if (md.supportsDrain())
                drained = std::make_unique<MdCounter>(md, *publisher);
        }
        const bool asynchronous = bool(scheduled) || bool(sampled);
        MetricCommandCounterClient client{[&](auto consume) {
                                              if (scheduled)
                                                  scheduled->begin(std::move(consume));
                                              else if (sampled)
                                                  sampled->begin(std::move(consume));
                                              else if (drained)
                                                  drained->begin();
                                              else
                                                  md.begin();
                                          },
                                          [&] {
                                              if (scheduled)
                                                  scheduled->submit();
                                              else if (sampled)
                                                  sampled->submit();
                                              else
                                                  md.submit();
                                          },
                                          [&] {
                                              return scheduled ? scheduled->end()
                                                     : sampled ? sampled->end()
                                                     : drained ? drained->end()
                                                               : md.end();
                                          }};
        auto finish = [&] {
            if (scheduled)
                scheduled->finish();
            else if (sampled)
                sampled->finish();
        };
        auto close = [&] {
            if (scheduled)
                scheduled->close();
            else if (sampled)
                sampled->close();
        };
        for (const auto &name : sets)
            for (unsigned sample = 0; sample < cfg["samples"].get<unsigned>(); ++sample) {
                checkCancel();
                const auto index = passes.size();
                arbitration.replay(index, name, sample, [&] {
                    md.select(name.get<std::string>());
                    const auto description = md.selected();
                    if (!sample)
                        metadata.push_back(description);
                    const auto subdir = QString("pass-%1").arg(index, 2, 10, QLatin1Char('0'));
                    if (!out.mkdir(subdir))
                        throw std::runtime_error("Cannot create metric pass directory");
                    QDir folder(out.filePath(subdir));
                    Json passRows = Json::array();
                    auto record = [&](MetricResult &r, Json row, const QString &filename) {
                        save(folder.filePath(filename),
                             QByteArray(reinterpret_cast<const char *>(r.raw.data()),
                                        qsizetype(r.raw.size())));
                        Json values = Json::array(), info = Json::array();
                        const auto n = description.at("metrics").size();
                        for (size_t i = 0; i < r.values["values"].size(); ++i)
                            (i < n ? values : info).push_back(r.values["values"][i]);
                        row.update({{"set", name},
                                    {"pass_index", index},
                                    {"sample_index", sample},
                                    {"available", r.values["available"]},
                                    {"unavailable_reasons", r.values["unavailable_reasons"]},
                                    {"values", values},
                                    {"information", info},
                                    {"raw_report", (subdir + '/' + filename).toStdString()},
                                    {"raw_sha256", sha256(r.raw)},
                                    {"raw_size", r.raw.size()},
                                    {"calculated_reports", r.values["reports"]}});
                        passRows.push_back(row);
                        if (publisher)
                            publisher->append(description, row);
                    };
                    auto scopeIdentity = [](const Json &info, const char *api) {
                        return Json{{"event", nullptr},
                                    {"api", api},
                                    {"shaders", Json::object()},
                                    {"start_event", info.at("start_event")},
                                    {"end_event", info.at("end_event")},
                                    {"command_count", info.at("commands").size()}};
                    };
                    Json boundary;
                    bool mapping{};
                    auto commandProgress = [&](Id, size_t, size_t) { checkCancel(); };
                    try {
                        if (!rangeInfo.is_null()) {
                            FrameRangeCounter counter(
                                client, rangeInfo["ranges"],
                                [&](const Json &info, MetricResult &r) {
                                    auto row = scopeIdentity(info, "FrameFile metric range");
                                    row["range_index"] = info["range_index"];
                                    record(
                                        r, row,
                                        QString("range-%1.bin").arg(info["range_index"].get<qulonglong>()));
                                },
                                asynchronous);
                            replay.run(commandProgress, {}, {},
                                       [&](Id id, const auto &body) { counter.scope(id, body); });
                            if (asynchronous)
                                finish();
                            boundary = counter.verify();
                            std::set<std::tuple<Id, Id, Id>> actual, expected;
                            for (const auto &r : passRows)
                                actual.emplace(r["range_index"].get<Id>(), r["start_event"].get<Id>(),
                                               r["end_event"].get<Id>());
                            for (const auto &r : rangeInfo["ranges"])
                                expected.emplace(r["range_index"].get<Id>(), r["start_event"].get<Id>(),
                                                 r["end_event"].get<Id>());
                            mapping = passRows.size() == rangeInfo["ranges"].size() && actual == expected;
                        } else if (!interval.is_null()) {
                            MetricIntervalCounter counter(
                                client, interval,
                                [&](MetricResult &r) {
                                    record(r, scopeIdentity(interval, "API interval"), "interval.bin");
                                },
                                asynchronous);
                            replay.run(commandProgress, {}, {},
                                       [&](Id id, const auto &body) { counter.scope(id, body); });
                            boundary = counter.verify();
                            mapping = passRows.size() == 1;
                        } else {
                            replay.run(commandProgress, [&](Id id, bool after, auto *, const auto &) {
                                if (!selected.contains(id))
                                    return;
                                const auto row = available.at(id);
                                const auto file = QString("event-%1.bin").arg(id);
                                if (!after)
                                    client.begin(asynchronous ? MetricCommandCounterClient::Consumer(
                                                                    [&, row, file](MetricResult &r) {
                                                                        record(r, row, file);
                                                                    })
                                                              : MetricCommandCounterClient::Consumer{});
                                else if (asynchronous)
                                    client.submit();
                                else {
                                    auto r = client.end();
                                    record(r, row, file);
                                }
                            });
                            if (asynchronous)
                                finish();
                            std::set<Id> actual;
                            for (const auto &r : passRows)
                                actual.insert(r["event"].get<Id>());
                            mapping = actual == selected && passRows.size() == selected.size();
                        }
                    } catch (...) {
                        if (asynchronous)
                            close();
                        throw;
                    }
                    if (asynchronous)
                        close();
                    const auto image = replay.output();
                    png(folder.filePath("output.png"), image);
                    Json valid = {{"set", name},
                                  {"pass_index", index},
                                  {"sample_index", sample},
                                  {"image_matches", image.width == baseline.width &&
                                                        image.height == baseline.height &&
                                                        image.rgba == baseline.rgba},
                                  {"rgba_sha256", sha256(image.rgba)},
                                  {"event_mapping_valid", mapping}};
                    if (!boundary.is_null())
                        valid["interval_boundaries"] = boundary;
                    if (valid["image_matches"] != true)
                        valid["image_difference"] = difference(baseline, image);
                    saveJson(folder.filePath("raw-results.json"),
                             {{"validation", valid}, {"records", passRows}});
                    passes.push_back(valid);
                    for (const auto &r : passRows)
                        rows.push_back(r);
                    if (progress)
                        progress({{"set", name},
                                  {"events", passRows.size()},
                                  {"image_matches", valid["image_matches"]}});
                });
            }
        arbitration.close();
        Json validation = {
            {"passes", passes},
            {"frame_matches", Frame(frame.path()).sha256() == sourceHash},
            {"experiment_matches",
             identity.is_null() || digest(read(frozen)) == identity["sha256"].get<std::string>()},
            {"image_matches", true},
            {"event_mapping_valid", true}};
        for (const auto &p : passes) {
            if (p["image_matches"] != true)
                validation["image_matches"] = false;
            if (p["event_mapping_valid"] != true)
                validation["event_mapping_valid"] = false;
        }
        saveJson(out.filePath("validation.json"), validation);
        const auto loaded = modules(), provenance = md.provenance();
        auto provenanceFile = provenance;
        provenanceFile["loaded_modules"] = loaded;
        saveJson(out.filePath("provenance.json"), provenanceFile);
        for (const auto key : {"frame_matches", "experiment_matches", "image_matches", "event_mapping_valid"})
            if (validation[key] != true)
                throw std::invalid_argument("Hardware metrics validation failed; no profile published; see " +
                                            out.filePath("validation.json").toStdString());
        const auto adapter = replay.adapterDescription();
        profile = {
            {"schema_version", 3},
            {"arbitration", "gpa_priority_v2"},
            {"priority_audit", "priority-audit.json"},
            {"sample_count", cfg["samples"]},
            {"warmup_count", cfg["warmup"]},
            {"sample_schedule", "metric_set_then_iteration"},
            {"statistics_precision", "binary64; raw integer values retained exactly"},
            {"backend", "Intel driver Metrics Discovery DX11 counter API"},
            {"frame",
             QFileInfo(QString::fromStdWString(frame.path().wstring())).canonicalFilePath().toStdString()},
            {"frame_sha256", sourceHash},
            {"adapter",
             {{"description", QString::fromWCharArray(adapter.Description).toStdString()},
              {"vendor_id", adapter.VendorId},
              {"device_id", adapter.DeviceId},
              {"revision", adapter.Revision},
              {"dedicated_video_memory", adapter.DedicatedVideoMemory},
              {"shared_system_memory", adapter.SharedSystemMemory}}},
            {"catalog_version", md.catalog()["version"]},
            {"adapter_luid", md.catalog()["luid"]},
            {"provenance", provenance},
            {"loaded_modules", loaded},
            {"production_gpa_dependency", false},
            {"shader_instrumentation", false},
            {"baseline_rgba_sha256", sha256(baseline.rgba)},
            {"baseline_matches", true},
            {"baseline_scope", edits ? "same_frozen_experiment" : "original_capture"},
            {"experiment", identity},
            {"selected_events", selected},
            {"selection_mode", !rangeInfo.is_null()  ? "frame_ranges"
                               : !interval.is_null() ? "interval"
                                                     : "events"},
            {"interval", interval},
            {"frame_ranges", rangeInfo},
            {"sets", metadata},
            {"records", rows},
            {"validation", validation}};
        profile["limits"] = Json::array(
            {"Each metric set uses a separate complete replay; values across passes are not simultaneous.",
             "Event counters surround Draw/Dispatch after effective bindings; interval counters surround "
             "complete commands including replay preparation and submission gaps. Counter polling adds "
             "synchronization.",
             "Each interval sample is computed from one raw report per metric set; it is not a sum or "
             "average of per-event percentages.",
             "Repeats run consecutively for each metric set. This schedule is explicit and is not the "
             "complete GPA multipass scheduler.",
             "Sample statistics use recovered binary64 arithmetic; original integer values remain exact in "
             "raw records. Unavailable samples are excluded with explicit counts.",
             "Metric names, units and formula results come from the current Intel driver; not verified "
             "equivalent to every GPA displayed metric.",
             "GPU frequency is observed, not locked. Timing and ratios vary with workload and device state.",
             "Non-finite values are unavailable; unsupported scalar ABI types are rejected. Hardware threads "
             "are not SIMD lanes or shader invocations.",
             "Report error/loss/overflow/context flags mark all metric values unavailable; raw diagnostics "
             "are preserved.",
             "Strict output validation applies to the presentation image, not every intermediate resource."});
        const auto matrix = metricProfileMatrix(profile);
        profile["iteration_matrix"] = "metric-iterations.json";
        saveJson(out.filePath("metric-iterations.json"),
                 {{"schema_version", 1},
                  {"axes", {"metric_set", "metric", "range", "iteration"}},
                  {"sample_count", cfg["samples"]},
                  {"sample_schedule", profile["sample_schedule"]},
                  {"sets", matrix},
                  {"semantics", "Binary64 iteration vectors; unavailable/nonfinite/boolean samples are null; "
                                "raw integers remain in profile records"}});
        profile["sample_statistics"] = summarizeMetricRecords(profile);
        auto table = [&](const char *stem, const Json &data, const std::vector<std::string> &fields,
                         const Json &csvRows) {
            saveJson(out.filePath(QString(stem) + ".json"), data);
            save(out.filePath(QString(stem) + ".csv"), QByteArray::fromStdString(metricCsv(fields, csvRows)));
        };
        if (!plan.is_null()) {
            profile["metric_request"] = plan;
            const auto requested = requestedMetricResults(profile, plan);
            table("requested-metrics", requested, requestFields, requested["samples"]);
        }
        table("sample-statistics", profile["sample_statistics"], statsFields, profile["sample_statistics"]);
        Json csvRows = Json::array();
        for (const auto &r : rows) {
            const Json *description = nullptr;
            for (const auto &s : metadata)
                if (s["name"] == r["set"])
                    description = &s;
            for (size_t i = 0; i < r["values"].size(); ++i) {
                const auto &d = description->at("metrics")[i], &v = r["values"][i];
                const bool valid = r["available"] == true && !v["value"].is_null();
                csvRows.push_back({{"event", r["event"]},
                                   {"api", r["api"]},
                                   {"pass", r["pass_index"]},
                                   {"set", r["set"]},
                                   {"metric", d["name"]},
                                   {"label", d["label"]},
                                   {"unit", d["unit"]},
                                   {"value", valid ? v["value"] : Json()},
                                   {"available", valid},
                                   {"value_type", v["type"]},
                                   {"scope", profile["selection_mode"]},
                                   {"start_event", r.value("start_event", Json())},
                                   {"end_event", r.value("end_event", Json())},
                                   {"command_count", r.value("command_count", Json())},
                                   {"sample_index", r["sample_index"]}});
            }
        }
        save(out.filePath("metrics.csv"),
             QByteArray::fromStdString(metricCsv({"event", "api", "pass", "set", "metric", "label", "unit",
                                                  "value", "available", "value_type", "scope", "start_event",
                                                  "end_event", "command_count", "sample_index"},
                                                 csvRows)));
        if (publisher) {
            auto published = publisher->report();
            const auto analysis = publisherMetricAnalysis(profile, published);
            if (scheduled)
                published["query_drain"] = scheduled->report();
            else if (sampled)
                published["query_drain"] = sampled->report();
            else if (drained)
                published["query_drain"] = {
                    {"mode", "synchronous_wait_after_each_end"},
                    {"records", drained->audit()},
                    {"core", "recovered QueryDrain and QueryBatch"},
                    {"counter_limit", 1},
                    {"validity",
                     "Validated report decoding; original raw diagnostic availability is retained"},
                    {"complete_original_scheduling", false}};
            profile["publisher_values"] = "publisher-values.json";
            profile["publisher_statistics"] = "publisher-statistics.json";
            saveJson(out.filePath("publisher-values.json"), published);
            table("publisher-statistics", analysis["statistics"], statsFields, analysis["statistics"]);
            saveJson(out.filePath("publisher-iterations.json"), {{"schema_version", 1},
                                                                 {"value_semantics", "publisher_binary64"},
                                                                 {"sets", analysis["iterations"]}});
            if (analysis.contains("requested"))
                table("publisher-requested-metrics", analysis["requested"], requestFields,
                      analysis["requested"]["samples"]);
            publisher->writeCsv(out.filePath("publisher-values.csv"));
        }
        saveJson(out.filePath("profile.json"), profile);
    });
    return profile;
}
} // namespace flora
