#include "MetricAcquisitionPriority.h"
#include "MetricAnalysis.h"
#include "MetricsDiscovery.h"
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <system_error>
namespace flora {
using Json = nlohmann::json;
Json metricPriorityFailure(std::exception_ptr exception) {
    try {
        if (exception)
            std::rethrow_exception(exception);
    } catch (const MetricPriorityTimeout &e) {
        return {{"type", "TimeoutError"}, {"message", e.what()}};
    } catch (const std::invalid_argument &e) {
        return {{"type", "ValueError"}, {"message", e.what()}};
    } catch (const std::overflow_error &e) {
        return {{"type", "OverflowError"}, {"message", e.what()}};
    } catch (const std::system_error &e) {
        return {{"type", "OSError"}, {"message", e.what()}};
    } catch (const std::exception &e) {
        return {{"type", "RuntimeError"}, {"message", e.what()}};
    } catch (...) {
        return {{"type", "RuntimeError"}, {"message", "Unknown native exception"}};
    }
    return nullptr;
}
MetricAcquisitionPriority::MetricAcquisitionPriority(MetricPriorityClient client, QString output,
                                                     LockFactory factory, Save save)
    : client_(std::move(client)), path_(QDir(output).filePath("priority-audit.json")),
      factory_(std::move(factory)), save_(std::move(save)) {
    if (!factory_)
        factory_ = [](const Json &catalog) { return metricDeviceMutex(catalog); };
    if (!save_)
        save_ = [](const QString &path, const Json &report) {
            QSaveFile file(path);
            const auto bytes = report.dump(2);
            if (!file.open(QIODevice::WriteOnly) ||
                file.write(bytes.data(), qint64(bytes.size())) != qint64(bytes.size()) || !file.commit())
                throw std::runtime_error("Cannot save priority audit: " + path.toStdString());
        };
}
MetricAcquisitionPriority::MetricAcquisitionPriority(MetricsDiscovery &metrics, QString output,
                                                     LockFactory factory, Save save)
    : MetricAcquisitionPriority(
          {metrics.catalog(), [&metrics] { metrics.close(); }, [&metrics] { return metrics.closed(); }},
          std::move(output), std::move(factory), std::move(save)) {}
Json MetricAcquisitionPriority::report() const {
    return {{"schema_version", 1},
            {"policy", "uniform_set_then_iteration"},
            {"failure", failure_},
            {"passes", passes_},
            {"lock", lock_ ? lock_->audit() : Json()}};
}
void MetricAcquisitionPriority::save() { save_(path_, report()); }
void MetricAcquisitionPriority::enter() {
    try {
        lock_ = factory_(client_.catalog);
        save();
    } catch (...) {
        const auto original = std::current_exception();
        failure_ = metricPriorityFailure(original);
        if (lock_)
            lock_->close();
        save();
        std::rethrow_exception(original);
    }
}
void MetricAcquisitionPriority::run(const std::function<void(MetricAcquisitionPriority &)> &body) {
    enter();
    std::exception_ptr failure;
    try {
        body(*this);
    } catch (...) {
        failure = std::current_exception();
        failure_ = metricPriorityFailure(failure);
        client_.close();
    }
    close();
    if (failure)
        std::rethrow_exception(failure);
}
void MetricAcquisitionPriority::replay(const Json &index, const Json &name, const Json &sample,
                                       const std::function<void()> &body) {
    if (active_ || !lock_ || lock_->closed())
        throw std::invalid_argument("Priority acquisition is not idle");
    active_ = true;
    passes_.push_back({{"pass_index", index},
                       {"set", name},
                       {"sample_index", sample},
                       {"acquired", false},
                       {"complete", false},
                       {"failure", nullptr},
                       {"counters_closed_after_failure", false}});
    auto &entry = passes_.back();
    std::exception_ptr failure;
    try {
        lock_->setPriority(7);
        lock_->acquire();
        entry["acquired"] = true;
        save();
        body();
        entry["complete"] = true;
    } catch (...) {
        failure = std::current_exception();
        entry["failure"] = metricPriorityFailure(failure);
        try {
            client_.close();
            entry["counters_closed_after_failure"] = client_.closed();
        } catch (...) {
            failure = std::current_exception();
        }
    }
    try {
        lock_->setPriority(priorityEmpty);
        lock_->release();
    } catch (...) {
        failure = std::current_exception();
    }
    active_ = false;
    save();
    if (failure)
        std::rethrow_exception(failure);
}
void MetricAcquisitionPriority::close() {
    if (active_)
        throw std::invalid_argument("Cannot close arbitration during a replay");
    if (lock_)
        lock_->close();
    save();
}
Json validateMetricPriorityResult(const QString &folder, const Json &profile, bool required) {
    const auto protocol = profile.value("arbitration", Json()),
               reference = profile.value("priority_audit", Json());
    if (protocol.is_null() && reference.is_null() && !required)
        return nullptr;
    if (protocol != "gpa_priority_v2" || reference != "priority-audit.json")
        throw std::invalid_argument("Unknown uniform priority protocol");
    QFile file(QDir(folder).filePath("priority-audit.json"));
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read priority audit: " + file.fileName().toStdString());
    const auto audit = Json::parse(file.readAll().toStdString());
    const auto lock = audit.value("lock", Json::object()), passes = audit.value("passes", Json::array());
    const auto &luid = profile.at("adapter_luid");
    const auto &low = luid.at(0);
    if (!low.is_number_integer() ||
        (low.is_number_unsigned() ? low.get<uint64_t>() > UINT32_MAX
                                  : low.get<int64_t>() < 0 || low.get<int64_t>() > UINT32_MAX))
        throw std::invalid_argument("Expected uint32");
    const auto &high = luid.at(1);
    const auto highBits = high.is_number_unsigned() ? high.get<uint64_t>() : uint64_t(high.get<int64_t>());
    const auto matches = [](const Json &object, const char *key, const Json &value) {
        return metricIdentityEqual(object.value(key, Json()), value);
    };
    if (!matches(audit, "schema_version", 1) || !matches(audit, "policy", "uniform_set_then_iteration") ||
        !matches(audit, "failure", nullptr) ||
        !matches(lock, "resource", metricResourceName(1, low.get<uint32_t>(), uint32_t(highBits), "OA")) ||
        lock.value("closed", Json()) != Json(true) || !matches(lock, "depth", 0) ||
        !matches(lock, "priority", priorityEmpty) || !matches(lock, "timeouts", 0) ||
        lock.value("cross_process", Json()) != Json(true) ||
        lock.value("production_gpa_dependency", Json()) != Json(false))
        throw std::invalid_argument("Incomplete uniform priority ownership audit");
    Json expected = Json::array(), priorities = Json::array();
    for (const auto &p : profile.at("validation").at("passes")) {
        expected.push_back({{"pass_index", p.at("pass_index")},
                            {"set", p.at("set")},
                            {"sample_index", p.at("sample_index")},
                            {"acquired", true},
                            {"complete", true},
                            {"failure", nullptr},
                            {"counters_closed_after_failure", false}});
    }
    for (size_t i = 0; i < passes.size(); ++i) {
        priorities.push_back(7);
        priorities.push_back(priorityEmpty);
    }
    const auto attempts = lock.value("attempts", Json());
    if (expected.empty() || !metricIdentityEqual(passes, expected) ||
        !matches(lock, "priorities", priorities) || !attempts.is_number_integer() ||
        (!attempts.is_number_unsigned() && attempts.get<int64_t>() < 0) ||
        attempts.get<uint64_t>() < passes.size())
        throw std::invalid_argument("Uniform priority replay roster mismatch");
    return audit;
}
} // namespace flora
