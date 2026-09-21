#include "application/NativeDebugConfig.h"
#include "application/ReplayDebugModel.h"
#include <QCoreApplication>
#include <QFile>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
Json read(const std::string &path) {
    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read debug model fixture");
    const auto bytes = file.readAll();
    return Json::parse(bytes.begin(), bytes.end());
}
Json operation(ReplayDebugModel &m, const Json &op) {
    const auto name = op.at("op").get<std::string>();
    const auto position = op.value("position", m.position());
    const auto point = [&] { return ReplayDebugModel::Point{op.at("file"), op.at("line")}; };
    if (name == "move") {
        m.move(size_t(position));
        return {{"position", m.position()},
                {"variables", m.variables()},
                {"values", m.sourceValues()},
                {"location", m.location(position)},
                {"stack", m.stack(position)}};
    }
    if (name == "rows")
        return m.registerRows(op.value("mode", "float"));
    if (name == "location")
        return m.location(position);
    if (name == "stack")
        return m.stack(position);
    if (name == "info")
        return m.instructionInfo(op.at("instruction"));
    if (name == "next")
        return m.nextSource(position, op.at("direction"));
    if (name == "function") {
        const auto [index, why] = m.functionStep(position, op.at("mode") == "out");
        return {index, why};
    }
    if (name == "toggle") {
        m.toggle(point());
        return m.breakpoints();
    }
    if (name == "condition") {
        m.setCondition(point(), op.at("text"));
        return m.breakpoints();
    }
    if (name == "hit-count") {
        m.setHitCount(point(), op.at("mode"), op.at("count"));
        return m.breakpoints();
    }
    if (name == "entry")
        return m.lineEntry(position, point());
    if (name == "count")
        return m.encounterCount(position, point());
    if (name == "breakpoint")
        return m.breakpointEntry(position);
    if (name == "seek")
        return m.seek(position);
    if (name == "watch") {
        m.addWatch(op.at("text"));
        return m.watchResults();
    }
    if (name == "unwatch") {
        m.removeWatch(op.at("index"));
        return m.watchResults();
    }
    if (name == "watches")
        return m.watchResults();
    if (name == "eval") {
        const auto value = m.evaluate(size_t(position), op.at("text"));
        Json values = Json::array();
        for (const auto &v : value.values)
            std::visit([&](const auto &n) { values.push_back(n); }, v);
        return {{"type", value.kind}, {"values", values}, {"text", value.text()}};
    }
    if (name == "config")
        return m.configuration();
    if (name == "import") {
        m.importConfiguration(op.at("config"));
        return m.configuration();
    }
    if (name == "report")
        return m.exportReport();
    throw std::runtime_error("Unknown debug model operation");
}
int probe(const std::string &path) {
    const auto jobs = read(path);
    Json results = Json::array();
    for (const auto &job : jobs) {
        Json r{{"name", job.at("name")}};
        try {
            auto result = job.contains("input") ? read(job.at("input")) : job.at("result");
            ReplayDebugModel model(std::move(result));
            Json actions = Json::array();
            for (const auto &op : job.at("ops")) {
                Json item;
                try {
                    item["value"] = operation(model, op);
                    item["success"] = true;
                } catch (const std::exception &e) {
                    item = {{"success", false}, {"error", e.what()}};
                }
                item["position"] = model.position();
                actions.push_back(std::move(item));
            }
            r["success"] = true;
            r["actions"] = std::move(actions);
        } catch (const std::exception &e) {
            r["success"] = false;
            r["error"] = e.what();
        }
        results.push_back(std::move(r));
    }
    QFile out(QString::fromStdString(path + ".results.json"));
    const auto bytes = results.dump(2);
    if (!out.open(QIODevice::WriteOnly) ||
        out.write(bytes.data(), qint64(bytes.size())) != qint64(bytes.size()))
        throw std::runtime_error("Cannot write model probe results");
    return 0;
}
Json fixture() {
    return Json::parse(
        R"({"action":"debug-pixel","disassembly":"shader code","source_debug":{"files":[{"filename":"main.hlsl","contents":"float a;\nreturn a;\n"}]},
      "trace":{"inputs":[],"instInfo":[{"instruction":0,"lineInfo":{"fileIndex":0,"lineStart":1,"lineEnd":1}},{"instruction":3,"lineInfo":{"fileIndex":0,"lineStart":2,"lineEnd":2}}]},
      "steps":[{"nextInstruction":0,"changes":[],"callstack":["main"]},{"nextInstruction":1,"changes":[],"callstack":["main"]},{"nextInstruction":3,"changes":[],"callstack":["main"]}]})");
}
} // namespace
class ReplayDebugModelTests : public QObject {
    Q_OBJECT
  private slots:
    void configurationTransaction() {
        ReplayDebugModel model(fixture());
        model.move(1);
        model.setCondition({0, 1}, "true");
        model.setHitCount({0, 1}, "multiple", 2);
        model.addWatch("2u+3u");
        const auto original = model.configuration();
        auto bad = original;
        bad["breakpoints"][0]["line"] = 200;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, model.importConfiguration(bad));
        QCOMPARE(model.configuration(), original);
        QCOMPARE(model.position(), int64_t(1));
        bad = original;
        bad["watches"].push_back("2u+3u");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, model.importConfiguration(bad));
        QCOMPARE(model.configuration(), original);
        ReplayDebugModel restored(fixture());
        restored.importConfiguration(original);
        QCOMPARE(restored.configuration(), original);
    }
    void sparseLocationAndFailurePosition() {
        ReplayDebugModel model(fixture());
        model.move(1);
        QCOMPARE(model.location(1), Json({0, 1, 1, 0, 0}));
        QCOMPARE(model.nextSource(0, 1), size_t(2));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, model.move(100));
        QCOMPARE(model.position(), int64_t(1));
        model.setCondition({0, 2}, "unknown / 0");
        QVERIFY_THROWS_EXCEPTION(ExpressionError, model.seek(0));
        QCOMPARE(model.position(), int64_t(1));
        model.setCondition({0, 2}, "true");
        QCOMPARE(model.seek(0), size_t(2));
        QCOMPARE(model.position(), int64_t(1));
    }
    void metadataValidation() {
        auto r = fixture();
        r["trace"]["instInfo"].push_back(r["trace"]["instInfo"][0]);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, ReplayDebugModel{r});
        r = fixture();
        r["trace"]["instInfo"][0]["instruction"] = true;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, ReplayDebugModel{r});
        r = fixture();
        r["trace"]["instInfo"][0]["lineInfo"]["fileIndex"] = 999;
        ReplayDebugModel model(r);
        QVERIFY(model.location(1).is_null());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, model.toggle({0, 1}));
    }
    void preservesNegativeZeroDisplay() {
        auto r = fixture();
        r["trace"]["inputs"] = Json::array(
            {{{"name", "input"}, {"type", 0}, {"rows", 1}, {"columns", 1}, {"value", {{"f32v", {-0.0}}}}}});
        ReplayDebugModel model(r);
        QCOMPARE(model.registerRows()[0][1].get<std::string>(), std::string("-0"));
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc == 3 && std::string(argv[1]) == "--probe") {
        try {
            return probe(argv[2]);
        } catch (const std::exception &e) {
            qCritical("%s", e.what());
            return 1;
        }
    }
    ReplayDebugModelTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "ReplayDebugModelTests.moc"
