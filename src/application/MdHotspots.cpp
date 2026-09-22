#include "MdHotspots.h"
#include "MetricAnalysis.h"
#include "MetricPublisher.h"
#include "MetricsDiscovery.h"
#include "replay/Device.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <tuple>
namespace flora {
using Json = nlohmann::json;
namespace {
[[noreturn]] void fail(const char *message) { throw std::invalid_argument(message); }
bool integerLess(const Json &a, const Json &b) {
    const bool negativeA = a.is_number_integer() && !a.is_number_unsigned() && a.get<int64_t>() < 0;
    const bool negativeB = b.is_number_integer() && !b.is_number_unsigned() && b.get<int64_t>() < 0;
    if (negativeA != negativeB)
        return negativeA;
    return negativeA ? a.get<int64_t>() < b.get<int64_t>() : a.get<uint64_t>() < b.get<uint64_t>();
}
bool contains(const Json &values, const Json &value) {
    return std::any_of(values.begin(), values.end(),
                       [&](const Json &v) { return metricIdentityEqual(v, value); });
}
bool finite(const Json &v) { return v.is_number() && std::isfinite(v.get<double>()); }
bool blankName(const std::string &name) {
    const auto text = QString::fromStdString(name);
    return std::all_of(text.begin(), text.end(), [](QChar ch) {
        const auto c = ch.unicode();
        // Python str.strip also treats U+001C..U+001F as whitespace.
        return (c >= 9 && c <= 13) || (c >= 28 && c <= 32) || c == 0x85 || c == 0xa0 || c == 0x1680 ||
               (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f ||
               c == 0x3000;
    });
}
void validateIndexed(const Json &profile) {
    if (profile.value("selection_mode", Json()) != "events" ||
        profile.value("baseline_matches", Json()) != true)
        fail("Requires accepted per-event measurements");
    // The shared matrix verifier additionally checks the complete pass schedule,
    // descriptor roster, exact source identities and boolean availability.
    for (const auto &row : profile.at("records"))
        if (!row.at("sample_index").is_number_integer() || !row.at("event").is_number_integer())
            fail("Metric record identity must be integer");
}
QByteArray read(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read event group artifact");
    return f.readAll();
}
void save(const QString &path, const QByteArray &bytes) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit())
        throw std::runtime_error("Cannot save event group artifact");
}
void saveJson(const QString &path, const Json &value) {
    save(path, QByteArray::fromStdString(value.dump(2)));
}
std::string digest(const QByteArray &bytes) {
    return sha256(Bytes(reinterpret_cast<const uint8_t *>(bytes.constData()), size_t(bytes.size())));
}
Json source(const QDir &directory, const QString &relative) {
    return {{"path", relative.toStdString()}, {"sha256", digest(read(directory.filePath(relative)))}};
}
} // namespace
Json normalizeMetricGroups(const Json &groups, const Json &events) {
    const auto supplied =
        groups.is_null() ? Json::array({{{"name", "selected"}, {"events", events}}}) : groups;
    if (!supplied.is_array() || supplied.empty())
        fail("Choose at least one event group");
    std::set<std::string> names;
    Json result = Json::array();
    for (const auto &g : supplied) {
        if (!g.is_object())
            fail("Event groups must be objects");
        const auto name = g.value("name", Json()), ids = g.value("events", Json());
        if (!name.is_string() || blankName(name.get<std::string>()) ||
            !names.insert(name.get<std::string>()).second)
            fail("Group names must be nonempty and distinct");
        if (!ids.is_array() || ids.empty())
            fail("Group events must be distinct enabled measured events");
        Json ordered = Json::array();
        for (const auto &id : ids) {
            if (!id.is_number_integer() || contains(ordered, id) || !contains(events, id))
                fail("Group events must be distinct enabled measured events");
            ordered.push_back(id);
        }
        std::sort(ordered.begin(), ordered.end(), integerLess);
        result.push_back({{"name", name}, {"events", ordered}});
    }
    return result;
}
Json aggregateMetricProfiles(const Json &profile, const Json &weightProfile, const Json &groups) {
    validateIndexed(profile);
    validateIndexed(weightProfile);
    const auto matrix = metricProfileMatrix(profile);
    metricProfileMatrix(weightProfile);
    if (weightProfile.at("sample_count") != 1 || weightProfile.at("sets").size() != 1)
        fail("Weights require one independently sampled metric set");
    for (const auto key :
         {"frame_sha256", "experiment", "baseline_rgba_sha256", "adapter_luid", "catalog_version"})
        if (!metricIdentityEqual(profile.at(key), weightProfile.at(key)))
            throw std::invalid_argument(std::string("Weight and metric identities differ: ") + key);
    const auto &events = profile.at("selected_events"), &weightEvents = weightProfile.at("selected_events");
    if (events.size() != weightEvents.size() ||
        std::any_of(events.begin(), events.end(),
                    [&](const Json &id) { return !contains(weightEvents, id); }))
        fail("Weight event roster differs");
    if (!metricIdentityEqual(profile.at("provenance"), weightProfile.at("provenance")))
        fail("Weight and metric backend identities differ");
    const auto definitions = normalizeMetricGroups(groups, events);
    const auto &weightSet = weightProfile.at("sets").at(0);
    const Json *descriptor = nullptr;
    size_t wi{};
    for (const auto symbol : {"GpuCoreClocks", "GpuTime"}) {
        for (size_t i = 0; i < weightSet.at("metrics").size(); ++i)
            if (weightSet["metrics"][i]["name"] == symbol) {
                descriptor = &weightSet["metrics"][i];
                wi = i;
                break;
            }
        if (descriptor)
            break;
    }
    if (!descriptor)
        fail("Weight set exposes neither GPU clocks nor duration");
    const auto weightKind =
        metricDescriptorKind(descriptor->at("metric_type"), descriptor->at("unit"), descriptor->at("name"));
    if (weightKind != 0 && weightKind != 1)
        fail("Unsupported weight descriptor");
    std::map<std::string, Json> weights;
    for (const auto &row : weightProfile.at("records")) {
        const auto &v = row.at("values").at(wi).at("value");
        weights[row.at("event").dump()] =
            row.at("available") == true && finite(v) && v.get<double>() >= 0 ? Json(v.get<double>()) : Json();
    }
    using Key = std::tuple<std::string, std::string, std::string>;
    std::map<Key, const Json *> vectors;
    for (const auto &set : matrix)
        for (const auto &metric : set.at("metrics"))
            for (const auto &range : metric.at("ranges"))
                vectors[{set.at("set").get<std::string>(), metric.at("metric").get<std::string>(),
                         range.at("event").dump()}] = &range.at("values");
    Json output = Json::array();
    for (const auto &set : profile.at("sets")) {
        for (const auto &d : set.at("metrics")) {
            const auto kind = metricDescriptorKind(d.at("metric_type"), d.at("unit"), d.at("name"));
            if (!metricIdentityEqual(Json(kind), d.at("gpa_kind")))
                fail("Metric kind metadata is inconsistent");
        }
        for (const auto &group : definitions) {
            const auto &ids = group.at("events");
            Json invalid = Json::array();
            std::vector<double> fixed;
            for (const auto &id : ids) {
                const auto &w = weights.at(id.dump());
                if (w.is_null())
                    invalid.push_back(id);
                fixed.push_back(w.is_null() ? 0. : w.get<double>());
            }
            for (const auto &d : set.at("metrics")) {
                Json values = Json::array();
                if (!invalid.empty())
                    for (size_t i = 0; i < profile.at("sample_count").get<size_t>(); ++i)
                        values.push_back(nullptr);
                else {
                    if (!d.at("gpa_kind").is_number_integer())
                        fail("Metric kind must be a byte");
                    std::vector<std::vector<double>> input;
                    for (const auto &id : ids) {
                        std::vector<double> v;
                        for (const auto &n : *vectors.at({set.at("name").get<std::string>(),
                                                          d.at("name").get<std::string>(), id.dump()}))
                            v.push_back(n.is_null() ? std::numeric_limits<double>::quiet_NaN()
                                                    : n.get<double>());
                        input.push_back(std::move(v));
                    }
                    const std::vector<int64_t> sameGroup(ids.size(), 0);
                    const auto merged =
                        aggregateMetricRanges(input, d.at("gpa_kind").get<int>(), fixed, sameGroup).at(0);
                    for (const auto v : merged.values)
                        values.push_back(std::isfinite(v) ? Json(v) : Json());
                }
                auto row = metricSampleSummary(values);
                row.update({{"group", group.at("name")},
                            {"set", set.at("name")},
                            {"metric", d.at("name")},
                            {"label", d.at("label")},
                            {"unit", d.at("unit")},
                            {"gpa_kind", d.at("gpa_kind")},
                            {"events", ids},
                            {"values", values},
                            {"invalid_weight_events", invalid}});
                output.push_back(std::move(row));
            }
        }
    }
    auto sorted = events;
    std::sort(sorted.begin(), sorted.end(), integerLess);
    Json published = Json::array();
    for (const auto &id : sorted)
        published.push_back({{"event", id}, {"value", weights.at(id.dump())}});
    return {{"schema_version", 1},
            {"mode", "event_group_aggregation"},
            {"frame_sha256", profile.at("frame_sha256")},
            {"experiment", profile.at("experiment")},
            {"baseline_rgba_sha256", profile.at("baseline_rgba_sha256")},
            {"groups", definitions},
            {"sample_count", profile.at("sample_count")},
            {"records", output},
            {"weight",
             {{"set", weightSet.at("name")},
              {"metric", descriptor->at("name")},
              {"unit", descriptor->at("unit")},
              {"samples", 1},
              {"values", published}}},
            {"ordering",
             "event ID ascending within each group; binary64 rounding can differ from native unstable sort"},
            {"semantics", "Fixed independent weights; corresponding iterations combine before statistics; "
                          "any unavailable contributor invalidates that iteration"},
            {"limits",
             {"Per-event measurements combined, not one continuous-interval hardware report.",
              "Separate weight pass and metric-set replays are not simultaneous.",
              "Full original GPA hotspot scheduling, derived metrics and multipass equivalence remain "
              "unverified."}}};
}
Json aggregatePublisherMetricProfiles(const Json &profile, const Json &weights, const Json &publisher,
                                      const Json &weightPublisher, const Json &groups) {
    auto result = aggregateMetricProfiles(publisherMetricProfile(profile, publisher),
                                          publisherMetricProfile(weights, weightPublisher), groups);
    result["value_semantics"] = "publisher_binary64";
    result["weight"]["value_semantics"] = "publisher_binary64";
    result["limits"].push_back("Publisher transforms precede both weight extraction and range aggregation.");
    result["limits"].push_back("Weight and metric acquisitions have separate clock calibration and BusyState "
                               "lifetimes; full original scheduling remains unverified.");
    return result;
}
Json loadPublisherMetricAggregates(const QString &directory, const Json &result) {
    QDir dir(directory);
    if (result.value("publisher_aggregates", Json()) != "publisher-aggregates.json")
        fail("Missing canonical publisher aggregate reference");
    Json profiles = Json::object(), publishers = Json::object(), sources = Json::object();
    for (const auto name : {"weights", "metrics"}) {
        const auto path = QString(name) + "/profile.json";
        if (result.at("sources").at(name).at("path") != path.toStdString() ||
            result.at("sources").at(name).at("sha256") != digest(read(dir.filePath(path))))
            fail("Publisher aggregate source profile identity mismatch");
        profiles[name] = Json::parse(read(dir.filePath(path)).toStdString());
        publishers[name] = loadMetricPublisherResult(dir.filePath(name), profiles[name]);
        sources[name] = source(dir, QString(name) + "/publisher-values.json");
    }
    auto expected =
        aggregatePublisherMetricProfiles(profiles["metrics"], profiles["weights"], publishers["metrics"],
                                         publishers["weights"], result.at("groups"));
    for (const auto key : {"frame_sha256", "experiment", "baseline_rgba_sha256", "sample_count", "groups"})
        if (!metricIdentityEqual(expected.at(key), result.at(key)))
            fail("Publisher aggregate capture identity mismatch");
    expected["sources"] = result.at("sources");
    expected["publisher_sources"] = sources;
    const auto saved = Json::parse(read(dir.filePath("publisher-aggregates.json")).toStdString());
    if (!metricIdentityEqual(saved, expected))
        fail("Publisher aggregate differs from accepted source samples");
    return saved;
}
std::string metricGroupsCsv(const Json &result) {
    return metricCsv({"group", "set", "metric", "label", "unit", "gpa_kind", "total_samples", "valid_samples",
                      "invalid_samples", "median", "minimum", "maximum", "mean", "variation_percent"},
                     result.at("records"));
}
Json collectMetricGroups(const Frame &frame, const QString &directory, const Json &request,
                         const QString &experiment, const QString &bridge,
                         const std::function<bool()> &cancel,
                         const std::function<void(const Json &)> &progress) {
    auto cfg = request;
    if (!cfg.contains("samples"))
        cfg["samples"] = 3;
    cfg = uniformMetricRequest(cfg);
    if (!cfg["symbols"].is_null() || !cfg["interval"].is_null() || !cfg["frame_ranges"].is_null())
        fail("Event groups require per-event metric sets");
    const auto check = [&] {
        if (cancel && cancel())
            throw std::runtime_error("Cancelled");
    };
    check();
    QDir out(QFileInfo(directory).absoluteFilePath());
    if (out.exists() || !QDir().mkpath(out.path()))
        fail("Group output directory must be new");
    QString frozen;
    if (!experiment.isEmpty()) {
        frozen = out.filePath("experiment.json");
        save(frozen, read(experiment));
    }
    std::string weightSet;
    {
        auto device = createDx11Device(false, false, 0x8086);
        MetricsDiscovery md(device.device.Get(), bridge);
        for (const auto symbol : {"GpuCoreClocks", "GpuTime"}) {
            for (const auto &s : md.catalog().at("sets")) {
                for (const auto &m : s.at("metrics"))
                    if (m.at("name") == symbol) {
                        weightSet = s.at("name").get<std::string>();
                        break;
                    }
                if (!weightSet.empty())
                    break;
            }
            if (!weightSet.empty())
                break;
        }
    }
    if (weightSet.empty())
        fail("No GPU clock or duration weight metric");
    auto weights = cfg;
    weights["sets"] = {weightSet};
    weights["samples"] = 1;
    check();
    const auto notify = [&](const char *phase) {
        return [&, phase](Json event) {
            event["phase"] = phase;
            if (progress)
                progress(event);
        };
    };
    const auto weight = collectUniformMetrics(frame, out.filePath("weights"), weights, frozen, bridge, cancel,
                                              notify("weights"));
    const auto groups = normalizeMetricGroups(request.value("groups", Json()), weight.at("selected_events"));
    check();
    const auto profile =
        collectUniformMetrics(frame, out.filePath("metrics"), cfg, frozen, bridge, cancel, notify("metrics"));
    check();
    auto result = aggregateMetricProfiles(profile, weight, groups);
    result["sources"] = Json::object();
    for (const auto name : {"weights", "metrics"})
        result["sources"][name] = source(out, QString(name) + "/profile.json");
    if (cfg["publisher_values"] == true) {
        auto converted = aggregatePublisherMetricProfiles(
            profile, weight, loadMetricPublisherResult(out.filePath("metrics"), profile),
            loadMetricPublisherResult(out.filePath("weights"), weight), groups);
        converted["sources"] = result["sources"];
        converted["publisher_sources"] = Json::object();
        for (const auto name : {"weights", "metrics"})
            converted["publisher_sources"][name] = source(out, QString(name) + "/publisher-values.json");
        saveJson(out.filePath("publisher-aggregates.json"), converted);
        save(out.filePath("publisher-aggregates.csv"), QByteArray::fromStdString(metricGroupsCsv(converted)));
        result["publisher_aggregates"] = "publisher-aggregates.json";
    }
    check();
    save(out.filePath("aggregates.csv"), QByteArray::fromStdString(metricGroupsCsv(result)));
    saveJson(out.filePath("aggregates.json"), result);
    return result;
}
} // namespace flora
