#include "MdIterationSession.h"
#include "MdFrameRanges.h"
#include "MdIterationResults.h"
#include "MdIterations.h"
#include "MetricIterations.h"
#include "ZipArchive.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
namespace flora {
using Json = nlohmann::json;
namespace {
QByteArray read(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read scheduled artifact");
    return f.readAll();
}
void save(const QString &path, const Json &value) {
    QSaveFile f(path);
    const auto bytes = QByteArray::fromStdString(value.dump(2));
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit())
        throw std::runtime_error("Cannot save scheduled artifact");
}
} // namespace
Json scheduledUiSettings(const Json &ui) {
    if (ui.contains("scheduled_metrics"))
        return ui.at("scheduled_metrics");
    if (!ui.contains("hardware_metrics"))
        return Json::object();
    const auto &hardware = ui.at("hardware_metrics");
    auto result = hardware.value("metric_iterations", Json::object());
    if (hardware.contains("bridge"))
        result["bridge"] = hardware.at("bridge");
    result["ranges"] = hardware.value("events", Json(""));
    const auto scope = hardware.value("scope", Json());
    result["scope"] = scope == "全部帧指标范围" ? 0 : scope == "帧指标范围" ? 1 : -1;
    for (const auto key : {"samples", "warmup"}) {
        const auto value = hardware.value(key, Json("1"));
        bool ok = value.is_number_integer() && (value.is_number_unsigned() || value.get<int64_t>() >= 0) &&
                  value.get<uint64_t>() <= 100;
        int number = ok                  ? value.get<int>()
                     : value.is_string() ? QString::fromStdString(value.get<std::string>()).toInt(&ok)
                                         : 0;
        if (!ok || number < (key == std::string_view("samples") ? 1 : 0) || number > 100)
            throw std::invalid_argument("Invalid saved metric sample count");
        result[key] = number;
    }
    return result;
}
Json scheduledUiDocument(Json ui, const Json &settings) {
    ui["scheduled_metrics"] = settings;
    auto hardware = ui.value("hardware_metrics", Json::object());
    for (const auto key : {"symbols", "selected_pass", "mapping", "weight_path"})
        hardware["metric_iterations"][key] = settings.at(key);
    hardware["bridge"] = settings.at("bridge");
    hardware["events"] = settings.at("ranges");
    hardware["scope"] = settings.at("scope") == 0   ? "全部帧指标范围"
                        : settings.at("scope") == 1 ? "帧指标范围"
                                                    : "";
    for (const auto key : {"samples", "warmup"})
        hardware[key] = settings.at(key).dump();
    ui["hardware_metrics"] = std::move(hardware);
    return ui;
}
Json prepareScheduledRequest(const Frame &frame, const Experiment &experiment, const Json &catalog,
                             const Json &request) {
    auto cfg = scheduledMetricRequest(request);
    if (cfg.at("samples") == 0)
        throw std::invalid_argument("Samples must be 1..100");
    const auto plan = planMetrics(catalog, cfg.at("symbols"));
    const auto pass = metricIterationPass(cfg.at("requested_pass"), cfg.at("pass_mapping"));
    if (pass != metricAllPasses && pass >= plan.at("passes").size())
        throw std::invalid_argument("Selected pass is outside this metric plan");
    ReplayOptions options;
    experiment.apply(frame, options);
    const auto selection =
        selectFrameMetricRanges(effectiveFrame(frame, options), cfg.at("frame_ranges"), &experiment);
    if (!cfg.at("weights").empty() && cfg.at("weights").size() != selection.at("ranges").size())
        throw std::invalid_argument("Cached weight count differs from selected ranges");
    if (!cfg.at("frame_ranges").is_null()) {
        cfg["frame_ranges"] = Json::array();
        for (const auto &r : selection.at("ranges"))
            cfg["frame_ranges"].push_back(r.at("range_index"));
    }
    return {{"request", cfg}, {"selection", selection}, {"plan", plan}, {"frame_sha256", frame.sha256()}};
}
QStringList scheduledWorkerArguments(const QString &capture, const QString &jobDirectory,
                                     const QString &bridge, const Json &prepared) {
    const auto &cfg = prepared.at("request");
    const QDir folder(jobDirectory);
    QStringList args{"metric-iterations",
                     capture,
                     "--metrics-bridge",
                     bridge,
                     "--samples",
                     QString::fromStdString(cfg.at("samples").dump()),
                     "--warmup",
                     QString::fromStdString(cfg.at("warmup").dump()),
                     "--pass",
                     cfg.at("requested_pass") == metricAllPasses
                         ? "all"
                         : QString::fromStdString(cfg.at("requested_pass").dump()),
                     "--ready-file",
                     folder.filePath("start.ready")};
    for (const auto &symbol : cfg.at("symbols"))
        args << "--metric" << QString::fromStdString(symbol.get<std::string>());
    if (!cfg.at("frame_ranges").is_null())
        for (const auto &r : cfg.at("frame_ranges"))
            args << "--frame-range" << QString::fromStdString(r.dump());
    if (!cfg.at("pass_mapping").empty()) {
        QStringList mapping;
        for (const auto &p : cfg.at("pass_mapping"))
            mapping << QString::fromStdString(p.dump());
        args << "--pass-map" << mapping.join(',');
    }
    if (!cfg.at("weights").empty()) {
        save(folder.filePath("input-weights.json"), cfg.at("weights"));
        args << "--weights" << folder.filePath("input-weights.json");
    }
    return args;
}
Json acceptScheduledResult(const QString &directory, const Json &prepared, const QString &experimentFile,
                           const QString &experimentKey) {
    const QDir folder(directory);
    const auto path = folder.filePath("scheduled-profile.json");
    auto result = Json::parse(read(path).toStdString());
    const auto &cfg = prepared.at("request");
    const Json expected = {
        {"selection", prepared.at("selection")},
        {"requested_metrics", cfg.at("symbols")},
        {"requested_samples", cfg.at("samples")},
        {"warmup_count", cfg.at("warmup")},
        {"requested_pass", cfg.at("requested_pass")},
        {"pass_mapping", cfg.at("pass_mapping")},
        {"supplied_weights", cfg.at("weights")},
        {"frame_sha256", prepared.at("frame_sha256")},
        {"weight_source", cfg.at("weights").empty() ? "independent_weight_replay" : "caller_cache"}};
    for (auto it = expected.begin(); it != expected.end(); ++it)
        if (!metricIdentityEqual(result.value(it.key(), Json()), it.value()))
            throw std::invalid_argument("Scheduled result differs from the frozen request");
    const auto bytes = read(experimentFile);
    if (result.at("experiment").value("sha256", Json()) !=
        sha256(Bytes(reinterpret_cast<const uint8_t *>(bytes.constData()), size_t(bytes.size()))))
        throw std::invalid_argument("Scheduled experiment identity mismatch");
    const auto publisher = loadScheduledMetricResult(directory, result);
    result["experiment_key"] = experimentKey.toStdString();
    save(path, result);
    result["publisher_result"] = publisher;
    return result;
}
void exportScheduledResult(const QString &directory, const QString &destination) {
    const auto root = QFileInfo(directory).canonicalFilePath();
    if (root.isEmpty())
        throw std::invalid_argument("Scheduled output is unavailable");
    const auto target = QFileInfo(destination).absoluteFilePath();
    std::vector<ZipEntry> entries;
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QFileInfo file(it.next());
        const auto canonical = file.canonicalFilePath();
        if (!canonical.startsWith(root + '/', Qt::CaseInsensitive) ||
            file.absoluteFilePath().compare(target, Qt::CaseInsensitive) == 0 ||
            file.suffix().compare("dll", Qt::CaseInsensitive) == 0 ||
            file.suffix().compare("exe", Qt::CaseInsensitive) == 0)
            continue;
        entries.push_back({QDir(root).relativeFilePath(file.absoluteFilePath()), canonical, {}});
    }
    const QDir parent(QFileInfo(root).absolutePath());
    for (const auto name : {"process-tree.json", "worker.log", "input-weights.json"}) {
        const auto path = parent.filePath(name);
        if (QFileInfo(path).isFile() &&
            QFileInfo(path).absoluteFilePath().compare(target, Qt::CaseInsensitive) != 0)
            entries.push_back({name, path, {}});
    }
    writeZipArchive(destination, entries);
}
} // namespace flora
