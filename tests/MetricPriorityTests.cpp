#include "application/MetricAcquisitionPriority.h"
#include "application/MetricPriority.h"
#include "application/MetricPublisher.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#define NOMINMAX
#include <Windows.h>
#include <iostream>
#include <map>
#include <thread>
using namespace flora;
using Json = nlohmann::json;
namespace {
uint32_t u32(const Json &v) {
    if (!v.is_number_integer() ||
        (v.is_number_unsigned() ? v.get<uint64_t>() > UINT32_MAX
                                : v.get<int64_t>() < 0 || v.get<int64_t>() > UINT32_MAX))
        throw std::invalid_argument("Expected uint32");
    return v.get<uint32_t>();
}
std::string hash(std::span<const uint8_t> bytes) {
    return QCryptographicHash::hash(
               QByteArrayView(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size())),
               QCryptographicHash::Sha256)
        .toHex()
        .toStdString();
}
Json tableProbe(const Json &job) {
    auto buffer = MetricPriorityTable::initial();
    if (job.contains("corrupt"))
        buffer.at(job["corrupt"].get<size_t>()) ^= 1;
    MetricPriorityTable table(buffer);
    Json out = Json::array();
    std::map<std::string, std::unique_ptr<MetricPriorityState>> states;
    for (const auto &step : job.at("steps")) {
        Json result;
        try {
            const auto op = step.at("op");
            const auto row = step.value("row", size_t(0));
            const auto alive = [&](uint32_t pid) {
                const auto values = step.value("alive", Json::array());
                return std::find(values.begin(), values.end(), Json(pid)) != values.end();
            };
            if (op == "allocate")
                result = table.allocate(step.at("name").get<std::string>());
            else if (op == "name")
                result = table.name(row);
            else if (op == "get")
                result = table.get(row, step.at("field").get<size_t>());
            else if (op == "put")
                table.put(row, step.at("field").get<size_t>(), u32(step.at("value")));
            else if (op == "insert")
                table.insert(row, u32(step.at("pid")), u32(step.at("tid")), u32(step.at("priority")));
            else if (op == "update")
                table.update(row, u32(step.at("pid")), u32(step.at("tid")), u32(step.at("priority")));
            else if (op == "remove")
                table.remove(row, u32(step.at("pid")), u32(step.at("tid")));
            else if (op == "maximum")
                result = table.maximum(row);
            else if (op == "cleanup")
                table.cleanup(row, alive);
            else if (op == "empty")
                result = table.emptyAfterCleanup(row, alive);
            else if (op == "state") {
                const auto global = step.value("global", Json(0));
                states[step.at("id").get<std::string>()] = std::make_unique<MetricPriorityState>(
                    table, row, global.is_null() ? std::nullopt : std::optional(global.get<size_t>()),
                    u32(step.at("pid")), u32(step.at("tid")), u32(step.at("priority")),
                    step.value("global_strategy", false));
            } else {
                auto &state = *states.at(step.at("id").get<std::string>());
                if (op == "set")
                    state.setPriority(u32(step.at("value")));
                else if (op == "eligible")
                    result = state.eligible();
                else if (op == "highest")
                    result = state.highest();
                else if (op == "grant")
                    result = state.canGrant();
                else if (op == "owned")
                    result = state.owned();
                else if (op == "acquire")
                    result = state.tryAcquire(alive);
                else if (op == "release")
                    state.release();
                else if (op == "unregister")
                    state.unregister();
                else
                    throw std::invalid_argument("Unknown table operation");
            }
            result = {{"result", result}};
        } catch (const std::exception &e) {
            result = {{"error", e.what()}};
        }
        result["sha256"] = hash(buffer);
        result["priorities"] = Json::object();
        for (const auto &[id, state] : states)
            result["priorities"][id] = state->priority();
        out.push_back(result);
    }
    return out;
}
struct Scenario {
    Json job, trace = Json::array();
    std::map<std::string, int> calls;
    bool metricsClosed{};
    void hit(const std::string &name) {
        trace.push_back(name);
        const auto count = ++calls[name];
        if (job.value("failure", "") == name && count == job.value("failure_call", 1)) {
            if (name == "acquire")
                throw MetricPriorityTimeout("injected acquire");
            throw std::runtime_error("injected " + name);
        }
    }
};
struct FakeLock : MetricPriorityLock {
    Scenario &s;
    bool isClosed{};
    int depth{};
    uint32_t priority{priorityEmpty};
    Json priorities = Json::array();
    explicit FakeLock(Scenario &scenario) : s(scenario) {}
    void setPriority(uint32_t value) override {
        s.hit(value == 7 ? "set7" : "withdraw");
        priority = value;
        priorities.push_back(value);
    }
    bool acquire() override {
        s.hit("acquire");
        depth = 1;
        return true;
    }
    void release() override {
        s.hit("release");
        depth = 0;
    }
    void close() override {
        s.hit("close");
        isClosed = true;
        depth = 0;
    }
    bool closed() const override { return isClosed; }
    Json audit() const override {
        return {{"closed", isClosed}, {"depth", depth}, {"priority", priority}, {"priorities", priorities}};
    }
};
Json acquisitionProbe(const Json &job) {
    Scenario s{job};
    MetricPriorityClient client{Json::object(),
                                [&] {
                                    s.hit("metrics_close");
                                    s.metricsClosed = true;
                                },
                                [&] { return s.metricsClosed; }};
    MetricAcquisitionPriority acquisition(
        client, "unused",
        [&](const Json &) {
            s.hit("factory");
            return std::make_unique<FakeLock>(s);
        },
        [&](const QString &, const Json &report) {
            s.hit("save");
            s.trace.push_back(report);
        });
    Json error;
    try {
        acquisition.run([&](MetricAcquisitionPriority &a) {
            if (job.value("outer_failure", false))
                throw std::runtime_error("injected outer");
            for (int i = 0; i < job.value("passes", 2); ++i)
                a.replay(i, "A", i, [&] {
                    s.hit("work");
                    if (job.value("nested", false))
                        a.replay(9, "nested", 0, [] {});
                    if (job.value("close_active", false))
                        a.close();
                });
        });
    } catch (...) {
        error = metricPriorityFailure(std::current_exception());
    }
    return {{"error", error},
            {"report", acquisition.report()},
            {"trace", s.trace},
            {"metrics_closed", s.metricsClosed}};
}
Json run(const Json &j) {
    const auto op = j.at("op");
    if (op == "name")
        return metricResourceName(u32(j.at("kind")), u32(j.at("low")), u32(j.at("high")),
                                  j.at("suffix").get<std::string>());
    if (op == "table")
        return tableProbe(j);
    if (op == "acquisition")
        return acquisitionProbe(j);
    if (op == "priority_result")
        return validateMetricPriorityResult(QString::fromStdString(j.at("folder").get<std::string>()),
                                            j.at("profile"), j.value("required", false));
    if (op == "publisher_result")
        return loadMetricPublisherResult(QString::fromStdString(j.at("folder").get<std::string>()),
                                         j.at("profile"));
    if (op == "default_path")
        return metricPriorityDefaultPath().toStdString();
    throw std::invalid_argument("Unknown probe operation");
}
int worker(const QStringList &args) {
    MetricPriorityOptions options;
    options.path = args[2];
    options.mutexName = args[3];
    options.timeout = .15;
    SharedMetricPriorityMutex lock(args[4].toStdString(), options);
    HANDLE guard = CreateMutexW(nullptr, FALSE, reinterpret_cast<LPCWSTR>(args[3].utf16()));
    bool holding{};
    std::cout << Json({{"ready", true}, {"audit", lock.audit()}}).dump() << std::endl;
    for (std::string line; std::getline(std::cin, line);) {
        const auto j = Json::parse(line);
        Json output;
        try {
            Json result;
            const auto op = j.at("operation");
            if (op == "priority") {
                lock.setPriority(u32(j.at("value")));
                result = true;
            } else if (op == "acquire")
                result = lock.tryAcquire();
            else if (op == "wait")
                result = lock.acquire();
            else if (op == "release") {
                lock.release();
                result = true;
            } else if (op == "eligible")
                result = lock.eligible();
            else if (op == "highest")
                result = lock.highest();
            else if (op == "audit")
                result = lock.audit();
            else if (op == "hold_guard") {
                result = WaitForSingleObject(guard, 5000);
                holding = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
            } else if (op == "release_guard") {
                result = bool(ReleaseMutex(guard));
                holding = false;
            } else if (op == "close") {
                lock.close();
                result = true;
            } else
                throw std::runtime_error("Unknown worker operation");
            output = {{"result", result}};
        } catch (...) {
            output = {{"error", metricPriorityFailure(std::current_exception())}};
        }
        std::cout << output.dump() << std::endl;
        if (j.at("operation") == "close")
            break;
    }
    if (holding)
        ReleaseMutex(guard);
    CloseHandle(guard);
    return 0;
}
} // namespace
class MetricPriorityTests : public QObject {
    Q_OBJECT
  private slots:
    void coreStorage() {
        auto bytes = MetricPriorityTable::initial();
        MetricPriorityTable table(bytes);
        QCOMPARE(bytes.size(), priorityTableSize);
        const auto global = table.allocate("GLOBAL"), row = table.allocate("oa");
        table.insert(row, 100, 1, 7);
        MetricPriorityState state(table, row, global, 100, 1, 7);
        state.setPriority(7);
        QVERIFY(state.tryAcquire([](uint32_t pid) { return pid == 100; }));
        QVERIFY(state.tryAcquire([](uint32_t pid) { return pid == 100; }));
        state.release();
        QVERIFY(!state.owned());
        table.insert(row, 200, 2, 8);
        state.setPriority(7);
        QVERIFY(!state.tryAcquire([](uint32_t pid) { return pid == 100; }));
        QCOMPARE(table.get(row, 0x80), 8u);
        state.setPriority(7);
        QVERIFY(state.tryAcquire([](uint32_t pid) { return pid == 100; }));
    }
    void isolatedStorage() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        MetricPriorityOptions options;
        options.path = temp.filePath("priority.table");
        options.mutexName = "FloraGPA_Priority_" + QUuid::createUuid().toString();
        SharedMetricPriorityMutex lock("oa", options);
        lock.setPriority(7);
        QVERIFY(lock.acquire());
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, SharedMetricPriorityMutex("oa", options));
        std::exception_ptr error;
        std::thread other([&] {
            try {
                lock.release();
            } catch (...) {
                error = std::current_exception();
            }
        });
        other.join();
        QVERIFY(error != nullptr);
        QVERIFY(lock.audit()["depth"] == 1);
        lock.close();
        QVERIFY(!QFile::exists(*options.path));
        QVERIFY(lock.audit()["closed"] == true);
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, lock.release());
        QFile bad(*options.path);
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write("bad");
        bad.close();
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, SharedMetricPriorityMutex("oa", options));
        QVERIFY(bad.open(QIODevice::ReadOnly));
        QCOMPARE(bad.readAll(), QByteArray("bad"));
    }
    void acquisitionAuditFile() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        bool closed{};
        MetricPriorityClient client{
            {{"vendor", 0x8086}, {"luid", {1, 0}}}, [&] { closed = true; }, [&] { return closed; }};
        MetricPriorityOptions options;
        options.path = temp.filePath("priority.table");
        options.mutexName = "FloraGPA_Priority_" + QUuid::createUuid().toString();
        MetricAcquisitionPriority a(client, temp.path(),
                                    [&](const Json &catalog) { return metricDeviceMutex(catalog, options); });
        a.run([](MetricAcquisitionPriority &p) { p.replay(0, "A", 0, [] {}); });
        const Json profile = {
            {"arbitration", "gpa_priority_v2"},
            {"priority_audit", "priority-audit.json"},
            {"adapter_luid", {1, 0}},
            {"validation",
             {{"passes", Json::array({{{"pass_index", 0}, {"set", "A"}, {"sample_index", 0}}})}}}};
        QVERIFY(validateMetricPriorityResult(temp.path(), profile, true) == a.report());
        QVERIFY(!closed);
    }
    void failedDeleteIsAudited() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        MetricPriorityOptions options;
        options.path = temp.filePath("priority.table");
        options.mutexName = "FloraGPA_Priority_" + QUuid::createUuid().toString();
        SharedMetricPriorityMutex lock("oa", options);
        const auto path = QDir::toNativeSeparators(*options.path);
        const auto observer = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                          FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(observer != INVALID_HANDLE_VALUE);
        lock.close();
        CloseHandle(observer);
        QVERIFY(lock.audit()["closed"] == true);
        QVERIFY(lock.audit()["deleted_empty_storage"] == false);
        QVERIFY(lock.audit()["delete_error"].is_string());
        QVERIFY(QFile::exists(path));
        SharedMetricPriorityMutex retry("oa", options);
        retry.close();
        QVERIFY(!QFile::exists(path));
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 5 && args[1] == "--worker") {
        try {
            return worker(args);
        } catch (...) {
            std::cout << Json({{"error", metricPriorityFailure(std::current_exception())}}).dump()
                      << std::endl;
            return 2;
        }
    }
    if (args.size() == 4 && args[1] == "--probe") {
        QFile input(args[2]), output(args[3]);
        if (!input.open(QIODevice::ReadOnly))
            return 2;
        Json result = Json::array();
        for (const auto &j : Json::parse(input.readAll().toStdString()))
            try {
                result.push_back(run(j));
            } catch (const std::exception &e) {
                result.push_back({{"error", e.what()}});
            }
        if (!output.open(QIODevice::WriteOnly))
            return 3;
        output.write(QByteArray::fromStdString(result.dump(2)));
        return 0;
    }
    MetricPriorityTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "MetricPriorityTests.moc"
