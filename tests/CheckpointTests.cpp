#include "application/DxbcCheckpointModel.h"
#include "application/InvocationSelector.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::checkpoint;
namespace {
std::vector<uint8_t> read(const std::string &path) {
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read checkpoint test input");
    auto bytes = f.readAll();
    return {bytes.begin(), bytes.end()};
}
Json accessJson(const ArrayAccess &a) {
    return Json::array({a.array, a.offset, a.relative ? Json(*a.relative) : Json(nullptr)});
}
Json inputsJson(const DeclaredInputs &inputs, bool domain) {
    Json operands = Json::array();
    for (const auto &slot : inputs.inputSlots) {
        Json row = Json::array({inputOperand(slot)});
        for (unsigned c = 0; c < 4; ++c)
            row.push_back(inputOperand(slot, c));
        operands.push_back(row);
    }
    Json result{{"slots", inputs.inputSlots},
                {"known", inputs.known},
                {"identity", inputs.identity},
                {"operands", operands}};
    if (domain)
        result["domain"] = inputs.domain;
    return result;
}
Json evaluate(const Json &j) {
    const auto op = j.at("op").get<std::string>();
    if (op == "domain")
        return inputsJson(domainInputs(j.at("rows").get<Rows>()), true);
    if (op == "hull")
        return inputsJson(hullInputs(j.at("globals").get<Rows>(), j.at("rows").get<Rows>(), j.at("phase")),
                          false);
    if (op == "phases")
        return hullPhases(j.at("rows").get<Rows>());
    if (op == "array-operand")
        return indexableOperand(
            j.at("array"), j.at("index"), j.value("mask", 15u),
            j.contains("swizzle") ? std::optional<uint32_t>(j.at("swizzle")) : std::nullopt,
            j.contains("component") ? std::optional<uint32_t>(j.at("component")) : std::nullopt,
            j.contains("relative") ? std::optional<Words>(j.at("relative").get<Words>()) : std::nullopt);
    if (op == "program") {
        auto p = program(read(j.at("input")), j.at("stage"));
        return {{"profile", p.profile}, {"split", p.split}, {"catalog", p.catalog}};
    }
    if (op == "calls") {
        auto g = subroutines(j.at("rows").get<Rows>(), j.at("split"));
        Json owners = Json::object(), targets = Json::object(), labels = Json::object();
        for (const auto &[i, label] : g.owners)
            owners[std::to_string(i)] = label ? Json(*label) : Json(nullptr);
        for (const auto &[i, target] : g.targets)
            targets[std::to_string(i)] = target;
        for (const auto &[label, i] : g.labels)
            labels[std::to_string(label)] = i;
        return {{"owners", owners}, {"targets", targets}, {"labels", labels}, {"depth", g.depth}};
    }
    if (op == "instruction") {
        auto parsed = instructionOperands(j.at("words").get<Words>());
        return {{"prefix", parsed.prefix},
                {"ranges", parsed.ranges},
                {"dependent", dependentResult(j.at("words").get<Words>(),
                                              indexableDeclarations(j.at("declarations").get<Rows>()))}};
    }
    if (op == "arrays") {
        const auto arrays = indexableDeclarations(j.at("declarations").get<Rows>());
        Json decls = Json::array();
        for (const auto &a : arrays)
            decls.push_back({{"array", a.array},
                             {"elements", a.elements},
                             {"components", a.components},
                             {"mask", a.mask}});
        Json result{{"declarations", decls}};
        if (j.contains("words")) {
            const auto words = j.at("words").get<Words>();
            Json accesses = Json::array(), replacements = Json::array();
            for (const auto &a : indexableAccesses(words, arrays))
                accesses.push_back(accessJson(a));
            unsigned index = 0;
            auto rewritten = rewriteIndices(words, [&](const auto &a) {
                replacements.push_back(accessJson(a));
                return Words{0x10000a | ((index++ % 4) << 4), 91};
            });
            result.update({{"accesses", accesses}, {"rewritten", rewritten}, {"replacements", replacements}});
            if (j.value("destination", false)) {
                const auto d = indexableDestination(words, arrays);
                result["destination"] =
                    Json::array({d.access.array, d.mask, d.access.offset,
                                 d.access.relative ? Json(*d.access.relative) : Json(nullptr)});
            }
        }
        return result;
    }
    if (op == "changes")
        return changesIndex(j.at("destination").get<Words>(), j.at("relative").is_null()
                                                                  ? std::optional<Words>{}
                                                                  : j.at("relative").get<Words>());
    if (op == "selector")
        return validateSelector(j.at("selector"), j.at("shader").get<std::vector<uint8_t>>(), j.at("stage"),
                                j.at("slots"), j.at("known"), j.value("phase", Json(nullptr)));
    if (op == "snapshot")
        return selectorFromSnapshot(j.at("result"), j.at("registers"), j.at("row"),
                                    j.value("policy", std::string("unique")));
    if (op == "verify") {
        verifySelectorRecords(j.at("data").get<std::vector<uint8_t>>(), j.at("metadata"));
        return true;
    }
    throw std::runtime_error("Unknown checkpoint probe operation");
}
int probe(const std::string &path) {
    const auto jobs = Json::parse(read(path));
    Json results = Json::array();
    for (const auto &j : jobs) {
        Json result{{"name", j.at("name")}};
        try {
            result["value"] = evaluate(j);
            result["success"] = true;
        } catch (const std::exception &error) {
            result["success"] = false;
            result["error"] = error.what();
        }
        results.push_back(result);
    }
    QFile output(QString::fromStdString(path + ".results.json"));
    const auto text = results.dump(2);
    if (!output.open(QIODevice::WriteOnly) || output.write(text.data(), text.size()) != qint64(text.size()))
        throw std::runtime_error("Cannot write checkpoint probe results");
    return 0;
}
} // namespace
class CheckpointTests final : public QObject {
    Q_OBJECT
  private slots:
    void nestedAddressOrder() {
        const auto arrays = indexableDeclarations({{0x04000069, 7, 8, 4}, {0x04000069, 3, 4, 2}});
        const auto nested = indexableOperand(7, 0, 15, {}, 1, indexableOperand(3, 2, 3, {}, 0));
        const auto accesses = indexableAccesses(nested, arrays);
        QCOMPARE(accesses.size(), size_t(2));
        QCOMPARE(accesses[0].array, 3u);
        QCOMPARE(accesses[0].offset, 2u);
        QCOMPARE(accesses[1].array, 7u);
        std::vector<ArrayAccess> visited;
        const auto rewritten = rewriteIndices(nested, [&](const auto &access) {
            visited.push_back(access);
            return Words{0x10000a, uint32_t(100 + visited.size())};
        });
        QCOMPARE(visited.size(), size_t(2));
        QVERIFY(visited[1].relative == Words({0x0420300a, 3, 0x10000a, 101}));
        QVERIFY(rewritten == Words({0x0420301a, 7, 0x10000a, 102}));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, indexableAccesses(indexableOperand(3, 4), arrays));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, indexableDestination(indexableOperand(3, 1), arrays));
        auto depth = Words{0x10000a, 0};
        for (unsigned i = 0; i < 34; ++i) {
            Words next{0x420300a, 7};
            next.insert(next.end(), depth.begin(), depth.end());
            depth = next;
        }
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, indexableAccesses(depth, arrays));
    }
    void callGraph() {
        Rows code{{0x03000004, 0x10a000, 4},
                  {0x03000004, 0x10a000, 9},
                  {0x0100003e},
                  {0x0300002c, 0x10a000, 4},
                  {0x03000004, 0x10a000, 9},
                  {0x0100003e},
                  {0x0300002c, 0x10a000, 9},
                  {0x0100003e}};
        const auto graph = subroutines(code, 0);
        QCOMPARE(graph.depth, 2u);
        QVERIFY(!graph.owners.at(0));
        QCOMPARE(*graph.owners.at(4), 4u);
        code.insert(code.end() - 1, {0x03000004, 0x10a000, 4});
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, subroutines(code, 0));
        // A long static chain must be bounded by storage, not the host call stack.
        Rows deep{{0x03000004, 0x10a000, 0}, {0x0100003e}};
        for (unsigned i = 0; i < 4096; ++i) {
            deep.push_back({0x0300002c, 0x10a000, i});
            if (i != 4095)
                deep.push_back({0x03000004, 0x10a000, i + 1});
            deep.push_back({0x0100003e});
        }
        QCOMPARE(subroutines(deep, 0).depth, 4096u);
    }
    void selectorFiles() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto path = std::filesystem::path(directory.path().toStdWString()) / L"input-selector.json";
        const Json selector{
            {"format", "FloraGPA native input selector 1"}, {"name", "纹理"}, {"bits", 0xffffffffu}};
        writeSelector(path, selector);
        QVERIFY(readSelector(path) == selector);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, writeSelector(path, std::string(1024 * 1024, 'x')));
        QVERIFY(readSelector(path) == selector); // Failed serialization must preserve the previous file.
        QFile f(QString::fromStdWString(path.wstring()));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(1024 * 1024 + 1, ' '));
        f.close();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readSelector(path));
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QByteArray(150, '[') + QByteArray(150, ']'));
        f.close();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readSelector(path));
    }
    void selectorRecordBounds() {
        const Json meta = Json::parse(R"({"record_stride":64,"registers":1,"matched_invocations":1,
        "register_slots":[{"name":"v[0][0]"}],
        "input_selector":{"inputs":[{"name":"v[0][0]","component":0,"bits":2143363909}]}})");
        std::vector<uint8_t> bytes(64);
        auto put = [&](size_t offset, uint32_t value) { std::memcpy(bytes.data() + offset, &value, 4); };
        put(32, 0x7fc12345);
        put(48, 1);
        verifySelectorRecords(bytes, meta);
        put(32, 0x7fc00000); // NaN payloads remain distinct raw inputs.
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, verifySelectorRecords(bytes, meta));
        put(32, 0x7fc12345);
        put(48, 0);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, verifySelectorRecords(bytes, meta));
        put(48, 1);
        bytes.pop_back();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, verifySelectorRecords(bytes, meta));
        Json bad = meta;
        bad["record_stride"] = 0;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, verifySelectorRecords({}, bad));
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc == 3 && std::string(argv[1]) == "--probe") {
        try {
            return probe(argv[2]);
        } catch (const std::exception &error) {
            qCritical("%s", error.what());
            return 1;
        }
    }
    CheckpointTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "CheckpointTests.moc"
