#include "UniformMetricSession.h"
#include "MdFrameRanges.h"
#include "MdProfile.h"
#include "MetricAcquisitionPriority.h"
#include "MetricAnalysis.h"
#include "MetricPublisher.h"
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <algorithm>
#include <array>
#include <set>
namespace flora {
using Json = nlohmann::json;
namespace {
const std::array<std::string, 7> scopes = {"当前事件", "整帧",       "指定事件",      "区间整体",
                                           "整帧整体", "帧指标范围", "全部帧指标范围"};
Json indices(const std::string &text) {
    auto value = QString::fromStdString(text);
    value.replace(QChar(0xff0c), ',');
    Json result = Json::array();
    for (const auto &part : value.split(',')) {
        bool ok{};
        const auto number = part.trimmed().toULongLong(&ok);
        if (!ok || part.trimmed().startsWith('-'))
            throw std::invalid_argument("Expected comma-separated API or range indices");
        result.push_back(number);
    }
    return result;
}
QByteArray read(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read uniform metric artifact");
    return f.readAll();
}
int savedCount(const Json &value, int minimum) {
    bool ok = value.is_number_integer() && (value.is_number_unsigned() || value.get<int64_t>() >= 0) &&
              value.get<uint64_t>() <= 100;
    int n = ok                  ? value.get<int>()
            : value.is_string() ? QString::fromStdString(value.get<std::string>()).toInt(&ok)
                                : 0;
    if (!ok || n < minimum || n > 100)
        throw std::invalid_argument("Invalid saved metric sample count");
    return n;
}
} // namespace
Json uniformUiSettings(const Json &ui, bool requested) {
    if (ui.contains("intel_metrics"))
        return ui["intel_metrics"].value(requested ? "request" : "sets", Json::object());
    const auto hardware = ui.value("hardware_metrics", Json::object());
    const auto source = requested ? hardware.value("metric_request", Json::object()) : hardware;
    const auto scope = hardware.value("scope", std::string("当前事件"));
    const auto found = std::find(scopes.begin(), scopes.end(), scope);
    Json result = {{"scope", found == scopes.end() ? 0 : int(found - scopes.begin())},
                   {"events", hardware.value("events", std::string())},
                   {"samples", savedCount(hardware.value("samples", Json(1)), 1)},
                   {"warmup", savedCount(hardware.value("warmup", Json(1)), 0)},
                   {"sets", hardware.value("sets", Json::array({"RenderBasic"}))},
                   {"symbols", source.value("symbols", std::string())},
                   {"search", source.value("search", std::string())},
                   {"publisher_values", source.value("publisher_values", Json(false)) == true},
                   {"value_view", source.value("value_view", Json()) == "原版数值转换" ? 1 : 0}};
    if (hardware.contains("bridge"))
        result["bridge"] = hardware["bridge"];
    return result;
}
Json uniformUiDocument(Json ui, const Json &sets, const Json &requested) {
    auto hardware = ui.value("hardware_metrics", Json::object());
    for (const auto key : {"bridge", "sets", "events", "publisher_values"})
        hardware[key] = sets.at(key);
    const auto scope = sets.at("scope").get<int>();
    hardware["scope"] = scope >= 0 && scope < 7 ? scopes[size_t(scope)] : "";
    for (const auto key : {"samples", "warmup"})
        hardware[key] = sets.at(key).dump();
    hardware["value_view"] = sets.at("value_view") == 1 ? "原版数值转换" : "原始 MD 值";
    hardware["metric_request"] = {
        {"symbols", requested.at("symbols")},
        {"search", requested.at("search")},
        {"publisher_values", requested.at("publisher_values")},
        {"value_view", requested.at("value_view") == 1 ? "原版数值转换" : "原始 MD 值"}};
    ui["hardware_metrics"] = std::move(hardware);
    return ui;
}
Json prepareUniformRequest(const Frame &frame, const Experiment &experiment, const Json &catalog,
                           const Json &settings, bool requested, Id currentEvent) {
    Json request = {{"samples", settings.at("samples")},
                    {"warmup", settings.at("warmup")},
                    {"publisher_values", settings.at("publisher_values")}};
    Json plan;
    if (requested) {
        auto value = QString::fromStdString(settings.at("symbols").get<std::string>());
        value.replace(QChar(0xff0c), ',');
        Json symbols = Json::array();
        for (const auto &s : value.split(','))
            symbols.push_back(s.trimmed().toStdString());
        plan = planMetrics(catalog, symbols);
        request["sets"] = Json::array();
        request["symbols"] = symbols;
    } else
        request["sets"] = settings.at("sets");
    const auto scope = settings.at("scope").get<int>();
    if (scope == 0)
        request["events"] = {currentEvent};
    else if (scope == 2)
        request["events"] = indices(settings.at("events"));
    else if (scope == 3)
        request["interval"] = indices(settings.at("events"));
    else if (scope == 4)
        request["interval"] = {nullptr, nullptr};
    else if (scope == 5)
        request["frame_ranges"] = indices(settings.at("events"));
    else if (scope == 6)
        request["frame_ranges"] = "all";
    else if (scope != 1)
        throw std::invalid_argument("Choose a collection scope");
    request = uniformMetricRequest(request);
    Json names = request["sets"];
    if (requested) {
        names = Json::array();
        for (const auto &p : plan["passes"])
            names.push_back(p["set"]);
    }
    for (const auto &name : names) {
        size_t matches{};
        for (const auto &s : catalog.at("sets"))
            if (s["name"] == name)
                ++matches;
        if (matches != 1)
            throw std::invalid_argument("Choose existing hardware metric sets");
    }
    ReplayOptions options;
    experiment.apply(frame, options);
    const auto &effective = effectiveFrame(frame, options);
    Json interval, rangeInfo;
    std::set<Id> selected, eligible;
    for (const auto &[id, e] : effective.entries())
        if (e.category == 7 && isDraw(e.type) && experiment.enabled(id))
            eligible.insert(id);
    if (scope == 5 || scope == 6) {
        rangeInfo =
            selectFrameMetricRanges(effective, scope == 6 ? Json() : request["frame_ranges"], &experiment);
        if (scope == 5)
            request["frame_ranges"] = Json::array();
        for (const auto &r : rangeInfo["ranges"]) {
            for (const auto &id : r["enabled_draw_events"])
                selected.insert(id.get<Id>());
            if (scope == 5)
                request["frame_ranges"].push_back(r["range_index"]);
        }
    } else if (scope == 3 || scope == 4) {
        interval =
            selectMetricInterval(effective, &experiment, request["interval"][0], request["interval"][1]);
        for (const auto &id : interval["enabled_draw_events"])
            selected.insert(id.get<Id>());
    } else {
        if (request["events"].empty())
            selected = eligible;
        else
            for (const auto &id : request["events"])
                selected.insert(id.get<Id>());
        if (selected.empty() ||
            std::any_of(selected.begin(), selected.end(), [&](Id id) { return !eligible.contains(id); }))
            throw std::invalid_argument("Selected events are absent or disabled");
        if (scope != 1)
            request["events"] = selected;
    }
    return {{"request", request},
            {"frame_sha256", frame.sha256()},
            {"plan", plan},
            {"sets", names},
            {"selection",
             {{"selected_events", selected},
              {"selection_mode", scope >= 5   ? "frame_ranges"
                                 : scope >= 3 ? "interval"
                                              : "events"},
              {"interval", interval},
              {"frame_ranges", rangeInfo}}}};
}
QStringList uniformWorkerArguments(const QString &capture, const QString &directory, const QString &bridge,
                                   const Json &prepared) {
    const auto &r = prepared.at("request");
    QStringList args{"metric-profile",   capture,
                     "--metrics-bridge", bridge,
                     "--ready-file",     QDir(directory).filePath("start.ready"),
                     "--samples",        QString::fromStdString(r["samples"].dump()),
                     "--warmup",         QString::fromStdString(r["warmup"].dump())};
    for (const auto &n : r["sets"])
        args << "--set" << QString::fromStdString(n.get<std::string>());
    if (!r["symbols"].is_null())
        for (const auto &n : r["symbols"])
            args << "--metric" << QString::fromStdString(n.get<std::string>());
    for (const auto &id : r["events"])
        args << "--event" << QString::fromStdString(id.dump());
    if (r["publisher_values"] == true)
        args << "--publisher-values";
    if (!r["interval"].is_null()) {
        args << "--interval";
        for (size_t i = 0; i < 2; ++i)
            if (!r["interval"][i].is_null())
                args << (i ? "--end-event" : "--start-event")
                     << QString::fromStdString(r["interval"][i].dump());
    }
    if (r["frame_ranges"] == "all")
        args << "--all-frame-ranges";
    else if (!r["frame_ranges"].is_null())
        for (const auto &i : r["frame_ranges"])
            args << "--frame-range" << QString::fromStdString(i.dump());
    return args;
}
Json acceptUniformResult(const QString &directory, const Json &prepared, const QString &experimentFile,
                         const QString &key) {
    QDir dir(directory);
    auto profile = Json::parse(read(dir.filePath("profile.json")).toStdString());
    validateMetricPriorityResult(directory, profile, true);
    Json expected = prepared.at("selection");
    expected["frame_sha256"] = prepared.at("frame_sha256");
    expected["sample_count"] = prepared["request"]["samples"];
    expected["warmup_count"] = prepared["request"]["warmup"];
    for (auto it = expected.begin(); it != expected.end(); ++it)
        if (!metricIdentityEqual(profile.value(it.key(), Json()), it.value()))
            throw std::invalid_argument("Uniform result differs from frozen request");
    Json sets = Json::array();
    for (const auto &s : profile.at("sets"))
        sets.push_back(s.at("name"));
    if (sets != prepared["sets"])
        throw std::invalid_argument("Uniform metric set identity mismatch");
    if (!prepared["plan"].is_null() && profile.value("metric_request", Json()) != prepared["plan"])
        throw std::invalid_argument("Uniform metric plan identity mismatch");
    const auto frozen = read(experimentFile);
    if (profile.at("experiment").at("sha256") !=
        sha256(Bytes(reinterpret_cast<const uint8_t *>(frozen.constData()), size_t(frozen.size()))))
        throw std::invalid_argument("Uniform experiment identity mismatch");
    for (const auto field : {"frame_matches", "experiment_matches", "image_matches", "event_mapping_valid"})
        if (profile.at("validation").at(field) != true)
            throw std::invalid_argument("Uniform metric validation failed");
    metricProfileMatrix(profile);
    Json publisher;
    if (prepared["request"]["publisher_values"] == true)
        publisher = loadMetricPublisherResult(directory, profile);
    profile["experiment_key"] = key.toStdString();
    const auto bytes = QByteArray::fromStdString(profile.dump(2));
    QSaveFile f(dir.filePath("profile.json"));
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit())
        throw std::runtime_error("Cannot save accepted metric profile");
    if (!publisher.is_null())
        profile["publisher_result"] = publisher;
    return profile;
}
} // namespace flora
