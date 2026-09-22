#include "application/MetricPassController.h"
#include "application/MetricProbeRegistry.h"
#include <QCoreApplication>
#include <QFile>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
Json bits(Json value) {
    if (value.is_number_float()) {
        const auto d = value.get<double>();
        return {{"$f", QByteArray(reinterpret_cast<const char *>(&d), 8).toHex().toStdString()}};
    }
    if (value.is_structured())
        for (auto &v : value)
            v = bits(std::move(v));
    return value;
}
std::vector<uint8_t> bytes(const Json &v) {
    const auto data = QByteArray::fromHex(QByteArray::fromStdString(v.get<std::string>()));
    return {reinterpret_cast<const uint8_t *>(data.data()),
            reinterpret_cast<const uint8_t *>(data.data()) + data.size()};
}
void receive(MetricConsumer &consumer, const Json &item) {
    const auto payload = item.value("payload", Json());
    const auto data = payload.is_null() ? std::vector<uint8_t>() : bytes(payload);
    consumer.receive(item.at("handle").get<uint64_t>(), item.value("type", uint64_t(0)),
                     payload.is_null() ? std::nullopt : std::optional<Bytes>(data),
                     item.value("present", true));
}
Json decode(Json value) {
    if (value.is_object() && value.size() == 1 && value.contains("$f")) {
        const auto data = bytes(value["$f"]);
        if (data.size() != 8)
            throw std::invalid_argument("Invalid float fixture");
        double d{};
        std::memcpy(&d, data.data(), 8);
        return d;
    }
    if (value.is_structured())
        for (auto &v : value)
            v = decode(std::move(v));
    return value;
}
struct Trace {
    Json items = Json::array(), behavior = Json::object();
    void fail(const std::string &name) {
        if (behavior.value("throw", "") == name)
            throw std::runtime_error("injected " + name);
    }
};
struct Lock : MetricPriorityLock {
    Trace &trace;
    std::string name;
    Lock(Trace &t, std::string n) : trace(t), name(std::move(n)) {}
    void setPriority(uint32_t v) override {
        trace.items.push_back({name, "priority", v});
        trace.fail(name + ".priority");
    }
    bool acquire() override {
        trace.items.push_back({name, "acquire"});
        trace.fail(name + ".acquire");
        return trace.behavior.value("acquire", true);
    }
    void release() override {
        trace.items.push_back({name, "release"});
        trace.fail(name + ".release");
    }
    void close() override {}
    bool closed() const override { return false; }
    Json audit() const override { return Json::object(); }
};
struct Backend : MetricProbeBackend {
    Trace &trace;
    explicit Backend(Trace &t) : trace(t) {}
    int32_t beginProbe(uint64_t h, uint64_t k0, uint64_t k1, uint32_t tag, uint64_t ctx) override {
        trace.items.push_back({"begin", h, k0, k1, tag, ctx});
        trace.fail("begin");
        return trace.behavior.value("failed_probe", uint64_t(0)) == h ? 9 : 0;
    }
    int32_t endProbe(uint64_t h, uint64_t key, uint32_t tag, uint64_t ctx) override {
        trace.items.push_back({"end", h, key, tag, ctx});
        trace.fail("end");
        return trace.behavior.value("failed_probe", uint64_t(0)) == h ? 9 : 0;
    }
};
struct Publisher : MetricPassPublisher {
    Trace &trace;
    Backend backend;
    MetricProbeFanout fanout;
    bool oraclePool{}, poolMethod{};
    Publisher(Trace &t, const Json &j)
        : trace(t), backend(t), fanout(j.value("probes", Json::array({9, 0, 2})), backend),
          oraclePool(j.value("oracle_pool", false)), poolMethod(j.value("pool_method", true)) {}
    void setPool(uint32_t size) override {
        if (oraclePool) {
            trace.items.push_back({"lookup", "methods\\QueryPoolSize"});
            if (poolMethod)
                trace.items.push_back({"pool", 0xbeef, 0x18400800, size});
        } else
            trace.items.push_back({"pool", size});
        trace.fail("pool");
    }
    int32_t subscribe(uint64_t c, uint64_t h) override {
        trace.items.push_back({"subscribe", c, h});
        trace.fail("subscribe");
        return trace.behavior.value("failed_subscription", uint64_t(0)) == h ? 7 : 0;
    }
    int32_t unsubscribe(uint64_t c, uint64_t h) override {
        trace.items.push_back({"unsubscribe", c, h});
        trace.fail("unsubscribe");
        return 17;
    }
    bool begin(uint64_t k0, uint64_t k1, uint32_t tag, uint64_t ctx) override {
        return fanout.begin(k0, k1, tag, ctx);
    }
    bool end(uint64_t key, uint32_t tag, uint64_t ctx) override { return fanout.end(key, tag, ctx); }
    bool configure(uint64_t key, Bytes data) override {
        trace.items.push_back({"configure", key,
                               QByteArray(reinterpret_cast<const char *>(data.data()), qsizetype(data.size()))
                                   .toHex()
                                   .toStdString()});
        trace.fail("configure");
        return trace.behavior.value("configured", true);
    }
    void flush(uint64_t c) override {
        trace.items.push_back({"flush", c});
        trace.fail("flush");
    }
};
struct RegistryBackend : MetricProbeRegistryBackend {
    Trace &trace;
    std::map<uint64_t, uint64_t> attempts, owners;
    uint64_t serial{};
    bool releaseFailure{true};
    explicit RegistryBackend(Trace &value) : trace(value) {}
    MetricProbeResult lookup(uint64_t parent, const std::string &path) override {
        trace.items.push_back({"lookup", parent, path});
        trace.fail("lookup");
        if (!parent)
            return {trace.behavior.value("lookup_policy", "") == "first_fail" ? 7 : 0,
                    trace.behavior.value("parent", Json(555))};
        const auto kind = trace.behavior.contains("kind")
                              ? trace.behavior["kind"]
                              : Json(std::stoull(path.substr(path.rfind('/') + 1)));
        return {trace.behavior.value("lookup_policy", "") == "second_fail_with_value" ? 8 : 0, kind};
    }
    MetricProbeResult createProbe(uint64_t kind, Bytes data) override {
        ++attempts[kind];
        ++serial;
        int32_t status{};
        Json value = 0x1000 + serial;
        const auto policy = trace.behavior.value("creation_policy", "");
        if (kind == trace.behavior.value("first_kind", uint64_t(9)) && attempts[kind] == 1) {
            if (policy == "zero_success" || policy == "zero_failure")
                value = 0;
            if (policy == "zero_failure")
                status = 9;
            if (policy == "value_failure")
                status = -1;
        }
        if (trace.behavior.contains("created_value"))
            value = trace.behavior["created_value"];
        if (value.is_number_integer() && value != 0)
            owners[value.get<uint64_t>()] = kind;
        trace.items.push_back({"create", kind,
                               QByteArray(reinterpret_cast<const char *>(data.data()), qsizetype(data.size()))
                                   .toHex()
                                   .toStdString(),
                               status, value});
        trace.fail("create");
        return {status, value};
    }
    int32_t destroyProbe(uint64_t value) override {
        const int32_t status =
            releaseFailure && owners[value] == trace.behavior.value("last_kind", uint64_t(9)) ? 9 : 0;
        if (status)
            releaseFailure = false;
        trace.items.push_back({"destroy", value, status});
        trace.fail("destroy");
        return status;
    }
    int32_t beginProbe(uint64_t h, uint64_t a, uint64_t b, uint32_t tag, uint64_t ctx) override {
        trace.items.push_back({"begin", h, a, b, tag, ctx});
        trace.fail("begin");
        return trace.behavior.value("failed_probe", uint64_t(0)) == h ? 9 : 0;
    }
    int32_t endProbe(uint64_t h, uint64_t key, uint32_t tag, uint64_t ctx) override {
        trace.items.push_back({"end", h, key, tag, ctx});
        trace.fail("end");
        return trace.behavior.value("failed_probe", uint64_t(0)) == h ? 9 : 0;
    }
};
Json run(const Json &j) {
    Json output = Json::array();
    if (j.at("kind") == "configuration" || j.at("kind") == "device_key") {
        const auto data = j.at("kind") == "configuration"
                              ? dx11MetricProbeConfiguration(j.at("pointer"))
                              : dx11MetricConfigurationForKey(j.at("devices"), j.at("key"));
        return QByteArray(reinterpret_cast<const char *>(data.data()), qsizetype(data.size()))
            .toHex()
            .toStdString();
    }
    if (j.at("kind") == "result")
        return receiveDx11MetricResult(j.at("ids"), j.at("tokens"), j.at("rows"),
                                       j.value("timings", Json::array()), j.value("initial", Json()));
    if (j.at("kind") == "registry") {
        Trace trace;
        trace.behavior = j.value("behavior", Json::object());
        RegistryBackend backend(trace);
        MetricProbeRegistry registry(backend);
        for (const auto &step : j.at("steps")) {
            trace.items = Json::array();
            trace.behavior = step.value("behavior", trace.behavior);
            Json row = {{"result", nullptr}};
            try {
                const auto op = step.at("op");
                if (op == "register")
                    row["result"] = registry.registerType(step.at("path"));
                else if (op == "configure")
                    row["result"] =
                        registry.configure(step.at("group"), bytes(step.value("configuration", Json(""))));
                else if (op == "begin")
                    row["result"] = registry.begin(100, 200, 6, 900);
                else if (op == "end")
                    row["result"] = registry.end(200, 6, 900);
                else if (op == "release")
                    registry.release();
                else
                    throw std::invalid_argument("Unknown registry action");
            } catch (const std::exception &e) {
                row = {{"error", e.what()}};
            }
            row["state"] = registry.snapshot();
            row["trace"] = trace.items;
            output.push_back(std::move(row));
        }
        return output;
    }
    if (j.at("kind") == "consumer") {
        MetricConsumer consumer(j.at("handles"), j.value("timing", Json(0)));
        for (const auto &step : j.at("steps")) {
            Json row;
            try {
                if (step.at("op") == "reset")
                    consumer.reset(step.at("handles"));
                else
                    for (const auto &item : step.at("items"))
                        receive(consumer, item);
                row = {{"result", nullptr}};
            } catch (const std::exception &e) {
                row = {{"error", e.what()}};
            }
            row["state"] = consumer.snapshot();
            output.push_back(std::move(row));
        }
        return output;
    }
    Trace trace;
    trace.behavior = j.value("behavior", Json::object());
    if (j.at("kind") == "fanout") {
        Backend backend(trace);
        MetricProbeFanout fanout(j.at("handles"), backend);
        for (const auto &step : j.at("steps")) {
            trace.items = Json::array();
            trace.behavior = step.value("behavior", trace.behavior);
            Json row;
            try {
                if (step.at("op") == "begin")
                    row["result"] = fanout.begin(0xfedcba9876543210, 9, 0x87654321, 0x12345678);
                else
                    row["result"] = fanout.end(0xfedcba9876543210, 0x87654321, 0x12345678);
            } catch (const std::exception &e) {
                row = {{"error", e.what()}};
            }
            row["trace"] = trace.items;
            output.push_back(row);
        }
        return output;
    }
    Publisher publisher(trace, j);
    Lock primary(trace, "primary"), secondary(trace, "secondary");
    MetricPassController controller(
        j.at("handles"), j.value("requests", Json::array()), j.value("passes", Json::array()), publisher,
        primary, j.value("secondary", true) ? &secondary : nullptr, j.value("options", Json::object()));
    controller.currentPass = j.value("current", UINT32_MAX);
    controller.completedProbes = j.value("count", 0u);
    for (const auto &step : j.at("steps")) {
        trace.items = Json::array();
        trace.behavior = step.value("behavior", trace.behavior);
        Json row = {{"result", nullptr}};
        try {
            const auto op = step.at("op");
            if (op == "select")
                controller.select(step.at("value"));
            else if (op == "begin")
                controller.begin(step.at("value"));
            else if (op == "end")
                controller.end();
            else if (op == "flush")
                controller.flush();
            else if (op == "finish")
                controller.finish(step.value("value", Json(true)));
            else if (op == "seed_count")
                controller.completedProbes = step.at("value").get<uint32_t>();
            else if (op == "key")
                controller.setProbeDeviceKey(step.at("value"));
            else if (op == "feed")
                receive(controller.consumer(), step.at("item"));
            else if (op == "configure")
                row["result"] = controller.configure(
                    step.at("value"), bytes(step.value("configuration", Json("030000000100000000000000"))));
            else if (op == "prepare") {
                const auto requests =
                    step.value("alias", false) ? controller.requests() : step.at("requests");
                row["result"] = controller.prepare(
                    requests, step.at("compatibility"), step.at("value"), [&](uint32_t key) {
                        trace.items.push_back({"configuration", key});
                        trace.fail("configuration");
                        return bytes(step.value("configuration", Json("030000000100000000000000")));
                    });
            } else
                throw std::invalid_argument("Unknown action");
        } catch (const std::exception &e) {
            row = {{"error", e.what()}};
        }
        row["trace"] = trace.items;
        row["state"] = controller.snapshot();
        output.push_back(std::move(row));
    }
    return output;
}
} // namespace
class MetricPassControllerTests : public QObject {
    Q_OBJECT
  private slots:
    void absentCallbackAndFailedReset() {
        MetricConsumer c({11});
        c.receive(999, UINT64_MAX, Bytes(), false);
        QVERIFY(c.rows().empty());
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, c.reset({11, 11}));
        QCOMPARE(c.handles(), std::vector<uint64_t>({11}));
    }
    void retainedPassAndKey() {
        Trace trace;
        Publisher publisher(trace, Json::object());
        Lock lock(trace, "primary");
        MetricPassController c({11}, {0}, {{0}}, publisher, lock);
        c.select(0);
        c.finish(false);
        trace.items.clear();
        c.select(0);
        QVERIFY(trace.items.empty());
        trace.behavior = {{"configured", false}};
        QVERIFY(!c.configure(17, {}));
        QCOMPARE(c.probeDeviceKey(), 0u);
    }
    void cachedFailedProbeAndRelease() {
        Trace trace;
        trace.behavior = {{"creation_policy", "value_failure"}, {"first_kind", 9}, {"last_kind", 9}};
        RegistryBackend backend(trace);
        MetricProbeRegistry registry(backend);
        QVERIFY(registry.registerType("probe_types/9"));
        QVERIFY(!registry.configure(0, {}));
        QVERIFY(registry.configure(0, {}));
        QCOMPARE(backend.serial, uint64_t(1));
        registry.release();
        QCOMPARE(registry.handles(), std::vector<uint64_t>({4097}));
        registry.release();
        QCOMPARE(registry.handles(), std::vector<uint64_t>({0}));
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 4 && args[1] == "--probe") {
        QFile in(args[2]), out(args[3]);
        if (!in.open(QIODevice::ReadOnly))
            return 2;
        Json output = Json::array();
        for (const auto &j : Json::parse(in.readAll().toStdString())) {
            try {
                output.push_back(bits(run(decode(j))));
            } catch (const std::exception &e) {
                output.push_back({{"error", e.what()}});
            }
        }
        if (!out.open(QIODevice::WriteOnly))
            return 3;
        out.write(QByteArray::fromStdString(output.dump(2)));
        return 0;
    }
    MetricPassControllerTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "MetricPassControllerTests.moc"
