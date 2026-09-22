#include "application/MdRecordedQueries.h"
#include "application/MetricPublisher.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
struct Fake final : MetricRecordedTransport, MetricCommandLists {
    Json cfg, metadata, trace = Json::array();
    struct Counter {
        uint64_t context{}, execution{}, polls{};
        std::string state;
    };
    std::map<uint64_t, Counter> owned;
    std::map<uint64_t, uint64_t> commands;
    uint64_t token{}, execution{}, commandIndex{}, clockReads{};
    std::string fault;
    bool never{}, clockFailed{}, clockChanged{};
    explicit Fake(Json value) : cfg(std::move(value)), metadata(cfg.value("metadata", Json())) {}
    void fail(const std::string &name) const {
        if (("," + fault + ",").find("," + name + ",") != std::string::npos)
            throw std::runtime_error("Injected " + name);
    }
    bool supportsRecorded() const override { return cfg.value("supported", true); }
    const Json &selected() const override { return metadata; }
    Json provenance() const override { return {{"fake", true}}; }
    Json clockPair() override {
        trace.push_back({"clock"});
        fail("clock");
        const auto index = clockReads++;
        if (cfg.contains("clocks"))
            return cfg.at("clocks").at(std::min<size_t>(index, cfg.at("clocks").size() - 1));
        return {{"maximum_ns", clockChanged ? 1000001 : 1000000},
                {"frequency_hz", 1000},
                {"status", clockFailed ? 1 : 0},
                {"gpu_ns", 100 + index * 10},
                {"cpu_ns", 1000 + index * 10}};
    }
    int32_t contextType(uint64_t context) override {
        trace.push_back({"context_type", context});
        return 1;
    }
    uint64_t finish(uint64_t context, bool restore) override {
        trace.push_back({"finish", context, restore});
        fail("finish");
        const auto keys = cfg.value("command_keys", Json::array({500, 100, 300, 900, 200, 800, 600, 400}));
        const auto key =
            commandIndex < keys.size() ? keys.at(commandIndex).get<uint64_t>() : 1000 + commandIndex;
        ++commandIndex;
        commands[key] = context;
        return key;
    }
    void release(uint64_t command) override {
        trace.push_back({"release_command", command});
        fail("release_command");
        if (!commands.erase(command))
            throw std::runtime_error("Unknown command");
    }
    uint64_t recordedBegin(ID3D11DeviceContext *context) override {
        const auto key = uint64_t(reinterpret_cast<uintptr_t>(context));
        trace.push_back({"begin", key});
        fail("begin");
        for (const auto &[id, counter] : owned)
            if (counter.context == key && counter.state == "begun")
                throw std::runtime_error("Overlapping native counter");
        owned[++token] = {key, 0, 0, "begun"};
        return token;
    }
    void recordedEnd(uint64_t id) override {
        trace.push_back({"end", id});
        fail("end");
        auto &counter = owned.at(id);
        if (counter.state != "begun")
            throw std::runtime_error("Invalid native end");
        counter.state = "ended";
    }
    uint64_t recordedExecute(ID3D11CommandList *command, std::span<const uint64_t> tokens,
                             bool restore) override {
        const auto key = uint64_t(reinterpret_cast<uintptr_t>(command));
        trace.push_back({"execute", key, std::vector<uint64_t>(tokens.begin(), tokens.end()), restore});
        fail("execute");
        for (auto id : tokens)
            if (owned.at(id).context != commands.at(key) || owned.at(id).state == "begun")
                throw std::runtime_error("Invalid execution roster");
        ++execution;
        for (auto id : tokens) {
            auto &counter = owned.at(id);
            counter.execution = execution;
            counter.polls = 0;
            counter.state = "executed";
        }
        return execution;
    }
    std::optional<MetricResult> recordedPoll(uint64_t id, uint64_t currentExecution, bool flush) override {
        trace.push_back({"poll", id, currentExecution, flush});
        fail("poll");
        auto &counter = owned.at(id);
        if (!currentExecution || counter.execution != currentExecution)
            throw std::runtime_error("Stale execution");
        ++counter.polls;
        const auto ready = cfg.value("ready_after", 2u);
        if (!never && (flush || (ready && counter.polls >= ready)))
            counter.state = "ready";
        if (counter.state != "ready")
            return {};
        if (cfg.contains("result"))
            return MetricResult{cfg.at("result"),
                                {uint8_t(id), uint8_t(currentExecution), uint8_t(counter.context), 0}};
        const auto key = currentExecution * 100 + id;
        const bool available = id % 3 != 0;
        return MetricResult{{{"available", available},
                             {"unavailable_reasons", available ? Json::array() : Json::array({"ReportLost"})},
                             {"reports", 1},
                             {"values",
                              {{{"type", 1}, {"value", 1000 + id}},
                               {{"type", 2}, {"value", 2.5}},
                               {{"type", 1}, {"value", id * 3}},
                               {{"type", 1}, {"value", key}},
                               {{"type", 1}, {"value", key + 100}},
                               {{"type", 3}, {"value", !available}}}}},
                            {uint8_t(id), uint8_t(currentExecution), uint8_t(counter.context), 0}};
    }
    void recordedRelease(uint64_t id) override {
        trace.push_back({"release", id});
        fail("release");
        if (!owned.erase(id))
            throw std::runtime_error("Unknown token");
    }
    Json native() const {
        Json counters = Json::array(), lists = Json::array();
        for (const auto &[id, c] : owned)
            counters.push_back({id, c.context, c.execution, c.polls, c.state});
        for (const auto &[id, context] : commands)
            lists.push_back({id, context});
        return {{"counters", counters}, {"commands", lists}};
    }
};
Json read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read fixture");
    return Json::parse(file.readAll().toStdString());
}
std::string bytes(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read export");
    return file.readAll().toHex().toStdString();
}
Json run(const Json &job) {
    Fake fake(job);
    if (job.value("op", "recorded") == "publisher") {
        MetricPublisherValues publisher(fake, job.value("recorded", false));
        Json rows = Json::array();
        for (const auto &step : job.at("steps")) {
            Json row;
            try {
                if (step.at("op") == "update")
                    publisher.update(step.value("force", false));
                else if (step.at("op") == "append")
                    publisher.append(step.at("metadata"), step.at("row"));
                else if (step.at("op") == "fault")
                    fake.fault = step.at("value").get<std::string>();
                else if (step.at("op") == "clock_fail")
                    fake.clockFailed = step.at("value").get<bool>();
                else if (step.at("op") == "clock_changed")
                    fake.clockChanged = step.at("value").get<bool>();
                row["return"] = nullptr;
            } catch (const std::exception &e) {
                row["error"] = e.what();
            }
            row["report"] = publisher.report();
            row["csv"] = publisher.csv();
            row["trace"] = fake.trace;
            fake.trace.clear();
            rows.push_back(std::move(row));
        }
        return rows;
    }
    MdRecordedQueries session(fake, job.value("timeout", 10000u), job.value("publisher", false), &fake);
    std::map<std::string, uint64_t> handles;
    Json rows = Json::array();
    QTemporaryDir exports;
    unsigned exportIndex{};
    for (const auto &step : job.at("steps")) {
        Json row;
        const auto context = step.value("context", uint64_t(10));
        const auto key = step.contains("handle") ? handles[step.at("handle").get<std::string>()]
                                                 : step.value("key", uint64_t(0));
        const auto op = step.at("op").get<std::string>();
        try {
            if (op == "begin")
                session.begin(context, step.value("key0", uint64_t(1)),
                              step.contains("key1") ? std::optional(step.at("key1").get<uint64_t>())
                                                    : std::nullopt,
                              step.value("tag", 6u));
            else if (op == "end")
                session.end(context);
            else if (op == "finish") {
                const auto command = session.finish(context, step.value("restore", false));
                handles[step.at("save").get<std::string>()] = command;
                row["return"] = command;
            } else if (op == "execute")
                row["return"] = session.execute(key, step.value("restore", false));
            else if (op == "release")
                session.release(key);
            else if (op == "drain")
                session.drain(step.value("wait", true));
            else if (op == "close")
                session.close();
            else if (op == "fault")
                fake.fault = step.at("value").get<std::string>();
            else if (op == "never")
                fake.never = step.at("value").get<bool>();
            else if (op == "clock_fail")
                fake.clockFailed = step.at("value").get<bool>();
            else if (op == "clock_changed")
                fake.clockChanged = step.at("value").get<bool>();
            else if (op == "metadata")
                fake.metadata = step.at("value");
            else if (op == "report")
                row["return"] = session.report();
            else if (op == "export") {
                const auto folder = exports.filePath(QString::number(++exportIndex));
                row["return"] = session.exportReport(folder);
                row["export"] = {{"raw", bytes(folder + "/raw-values.csv")},
                                 {"json", read(folder + "/recorded-profile.json")},
                                 {"publisher", QFile::exists(folder + "/publisher-values.csv")
                                                   ? Json(bytes(folder + "/publisher-values.csv"))
                                                   : Json()}};
            }
            if (!row.contains("return"))
                row["return"] = nullptr;
        } catch (const std::exception &e) {
            row["error"] = e.what();
        }
        row["state"] = {{"closed", session.closed()},
                        {"failed", session.failed()},
                        {"abandoned", session.abandoned()},
                        {"owned", session.ownedCount()},
                        {"commands", session.commandCount()}};
        row["records"] = session.records();
        row["native"] = fake.native();
        row["trace"] = fake.trace;
        fake.trace.clear();
        rows.push_back(std::move(row));
    }
    return rows;
}
} // namespace
class RecordedMetricsTests : public QObject {
    Q_OBJECT
  private slots:
    void timeoutAborts() {
        Fake fake({{"metadata",
                    {{"name", "Empty"},
                     {"metrics", Json::array()},
                     {"information", Json::array()},
                     {"report_size", 4}}}});
        MdRecordedQueries session(fake, 1, false, &fake);
        session.begin(10, 1);
        session.end(10);
        const auto list = session.finish(10);
        session.execute(list);
        fake.never = true;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, session.drain());
        QVERIFY(session.closed() && session.failed() && fake.owned.empty() && fake.commands.empty());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, session.report());
    }
    void destructorReleases() {
        Fake fake({{"metadata", {{"name", "Empty"}}}});
        {
            MdRecordedQueries session(fake, 10000, false, &fake);
            session.begin(10, 1);
        }
        QVERIFY(fake.owned.empty());
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 4 && args[1] == "--probe") {
        Json result = Json::array();
        for (const auto &job : read(args[2]))
            try {
                result.push_back(run(job));
            } catch (const std::exception &e) {
                result.push_back({{"error", e.what()}});
            }
        QFile output(args[3]);
        if (!output.open(QIODevice::WriteOnly))
            return 2;
        output.write(QByteArray::fromStdString(result.dump(2)));
        return 0;
    }
    RecordedMetricsTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "RecordedMetricsTests.moc"
