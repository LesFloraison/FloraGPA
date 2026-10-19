#include "application/RdcEvents.h"
#include <QCoreApplication>
#include <QFile>
#include <QtTest>
#include <cstdio>
using namespace flora;
using Json = nlohmann::json;
namespace {
Json node(unsigned id, std::string name, unsigned flags, Json children = Json::array(),
          Json events = Json::array()) {
    // A single braced Json argument can select its copy constructor. Normalize
    // that fixture spelling to a one-element sequence, never a production input.
    if (children.is_object())
        children = Json::array({children});
    if (events.is_object())
        events = Json::array({events});
    return {{"eventId", id},        {"customName", name},
            {"flags", flags},       {"flags_text", std::to_string(flags)},
            {"numIndices", 3},      {"numInstances", 1},
            {"children", children}, {"events", events}};
}
} // namespace
class RdcTests : public QObject {
    Q_OBJECT
  private slots:
    void dx12ExecutionProvenance() {
        const auto marker = [](const char *execution, const char *record, const char *submission,
                               const char *method) {
            return std::string("Flora DX12 execution ") + execution + " record " + record + " submission " +
                   submission +
                   " queue 18446744073709551615 list 9007199254740993 generation 2 ordinal 0: "
                   "ID3D12GraphicsCommandList" +
                   method;
        };
        const auto roots = Json::array(
            {node(1, marker("9007199254740993", "19", "30", "DrawIndexedInstanced"), 0x40, {node(2, "", 2)}),
             node(3, marker("9007199254740994", "19", "31", "DrawIndexedInstanced"), 0x40, {node(4, "", 2)}),
             node(5, marker("9007199254740995", "20", "31", "IASetPrimitiveTopology"), 0x40,
                  {node(7, "", 0x80, Json::array(), {{{"eventId", 6}}})}),
             node(8, "Flora DX12 helper", 0x40, {node(9, "", 0x400)}),
             node(10, marker("9007199254740996", "21", "32", "DrawInstanced"), 0x40, {node(11, "", 2)}),
             node(12, marker("9007199254740996", "21", "32", "DrawInstanced"), 0x40, {node(13, "", 2)}),
             node(14, "Flora DX12 disabled execution 9007199254740997 record 22 submission 33 queue 1 list 2 generation 1 ordinal 0: ID3D12GraphicsCommandListDrawInstanced", 0x40),
             node(15, "Flora DX12 experiment UAV barrier before execution 9007199254740993", 0x40,
                  {node(17, "", 0x80, Json::array(), {{{"eventId", 16}}})})});
        const auto result = indexDx12RdcEvents(
            roots, {{"6", {{"name", "ID3D12GraphicsCommandList::IASetPrimitiveTopology"}}},
                    {"16", {{"name", "ID3D12GraphicsCommandList::ResourceBarrier"}}}});
        QCOMPARE(result["execution_map"].size(), size_t(3));
        QVERIFY(result["execution_map"]["9007199254740993"] == 2);
        QVERIFY(result["execution_map"]["9007199254740994"] == 4);
        QVERIFY(result["execution_map"]["9007199254740995"] == 6);
        QVERIFY(result["record_executions"]["19"] == Json::array({"9007199254740993", "9007199254740994"}));
        const auto provenance = rdcProvenance(4, result["reverse"]);
        QCOMPARE(provenance["execution"].get<std::string>(), std::string("9007199254740994"));
        QCOMPARE(provenance["submission"].get<std::string>(), std::string("31"));
        QCOMPARE(provenance["queue"].get<std::string>(), std::string("18446744073709551615"));
        QVERIFY(!result["execution_map"].contains("9007199254740996"));
        QVERIFY(!result["reverse"].contains("9"));
        QVERIFY(!result["reverse"].contains("11"));
        QVERIFY(!result["execution_map"].contains("9007199254740997"));
        QVERIFY(!result["reverse"].contains("14"));
        QVERIFY(!result["reverse"].contains("16"));
        QVERIFY_THROWS_EXCEPTION(
            std::runtime_error,
            indexDx12RdcEvents({node(1, marker("18446744073709551616", "19", "30", "DrawInstanced"), 0x40)},
                               Json::object()));
    }
    void dx12IndirectActions() {
        const auto marker = [](unsigned execution) {
            return "Flora DX12 execution " + std::to_string(execution) +
                   " record 265648 submission 364657 queue 861708976 list 10653671952 generation 1 ordinal "
                   "59: ID3D12GraphicsCommandListExecuteIndirect";
        };
        Json native = Json::object();
        auto wrapper = [&](unsigned id, unsigned flags, Json children = Json::array()) {
            native[std::to_string(id)] = {{"name", "ID3D12GraphicsCommandList::ExecuteIndirect"}};
            return node(id, "ExecuteIndirect", flags, children, {{{"eventId", id}}});
        };
        // The real BF1 single-Dispatch expansion and two boundary cases: no
        // generated work, and multiple Draws that must not collapse to one EID.
        const auto roots = Json::array(
            {node(1, marker(35808), 0x40, {wrapper(2, 0x40, {node(3, "Dispatch", 4)}), wrapper(4, 0x80)}),
             node(5, marker(35814), 0x40, {wrapper(6, 0x40), wrapper(7, 0x80)}),
             node(8, marker(35834), 0x40,
                  {wrapper(9, 0x40, {node(10, "Draw", 2), node(11, "Draw", 2)}), wrapper(12, 0x80)}),
             node(13, marker(10267), 0x40,
                  {wrapper(14, 0x40, {node(15, "DispatchMesh", 8)}), wrapper(16, 0x80)})});
        const auto result = indexDx12RdcEvents(roots, native);
        QCOMPARE(result["execution_map"], Json({{"35808", 3}, {"10267", 15}}));
        QCOMPARE(rdcProvenance(15, result["reverse"])["execution"], Json("10267"));
        QCOMPARE(result["actions"].back()["eid"], Json(15));
        QCOMPARE(result["execution_commands"]["35808"]["api_event_ids"], Json::array({2, 4}));
        QCOMPARE(result["execution_commands"]["35814"]["status"], Json("unrepresented"));
        QVERIFY(!result["execution_commands"]["35834"]["selectable"].get<bool>());
        QCOMPARE(rdcProvenance(11, result["reverse"])["execution"], Json("35834"));
        QCOMPARE(rdcProvenance(10, result["reverse"])["indirect_action"], Json(0));
        QCOMPARE(rdcProvenance(11, result["reverse"])["indirect_action"], Json(1));
        QCOMPARE(rdcProvenance(11, result["reverse"])["indirect_action_count"], Json(2));
        QCOMPARE(selectDx12RdcExecution(result, "35808").at("eid"), Json(3));
        QCOMPARE(selectDx12RdcExecution(result, "35834", 0).at("eid"), Json(10));
        QCOMPARE(selectDx12RdcExecution(result, "35834", 1).at("eid"), Json(11));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, selectDx12RdcExecution(result, "35834"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, selectDx12RdcExecution(result, "35834", 2));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, selectDx12RdcExecution(result, "35834", -1));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, selectDx12RdcExecution(result, "35834", 0.0));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, selectDx12RdcExecution(result, "35814", 0));
        auto duplicate = result; duplicate["execution_commands"]["35834"]["status"] = "duplicate_markers";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, selectDx12RdcExecution(duplicate, "35834", 0));
        auto direct = result; direct["execution_commands"]["35808"]["name"] = "ID3D12GraphicsCommandListDispatch";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, selectDx12RdcExecution(direct, "35808", 0));
        for (const auto id : {2u, 4u, 6u, 7u, 9u, 12u, 14u, 16u})
            QVERIFY(!result["reverse"].contains(std::to_string(id)));
    }
    void commandProvenance() {
        auto roots = Json::array(
            {node(1, "GPA 20: Draw", 0x40, {node(2, "", 2)}), node(3, "helper", 0x400),
             node(4, "GPA API 30: CopyResource", 0x40, {node(5, "", 0x400)}),
             node(6, "GPA API 40: ClearRenderTargetView", 0x40, {node(7, "", 1)}),
             node(8, "GPA API 40: ClearRenderTargetView", 0x40, {node(9, "", 1)}),
             node(10, "GPA API 50: CopyResource", 0x40, {node(11, "", 0x400), node(12, "", 0x400)}),
             node(13, "GPA API 60: CopyResource", 0x40,
                  {node(14, "GPA broken", 0x40, {node(15, "", 0x400)})}),
             node(16, "GPA 70: Draw", 2)});
        const auto r = indexRdcEvents(roots, Json::object());
        QVERIFY(r.at("gpa_event_map") == Json({{"20", 2}, {"30", 5}}));
        QCOMPARE(r.at("gpa_command_map").at("40").at("status").get<std::string>(),
                 std::string("duplicate_markers"));
        QVERIFY(!r.at("gpa_command_map").at("50").at("selectable").get<bool>());
        QVERIFY(rdcProvenance(11, r.at("reverse")).at("gpa_event") == 50);
        for (auto id : {3u, 7u, 9u, 15u, 16u})
            QVERIFY(rdcProvenance(id, r.at("reverse")).at("gpa_event").is_null());
    }
    void cpuBoundaries() {
        auto roots = Json::array(
            {node(1, "GPA API 100: MapCapturedWrites", 0x40,
                  {node(5, "", 0, Json::array(), {{{"eventId", 2}}, {{"eventId", 4}}, {{"eventId", 5}}})}),
             node(6, "GPA API 101: UpdateSubresource", 0x40,
                  {node(8, "", 0x400, Json::array(), {{{"eventId", 8}}})})});
        const auto r = indexRdcEvents(roots, {{"2", {{"name", "ID3D11DeviceContext::Map"}}},
                                              {"4", {{"name", "ID3D11DeviceContext::Unmap"}}},
                                              {"5", {{"name", "helper"}}},
                                              {"8", {{"name", "ID3D11DeviceContext::UpdateSubresource"}}}});
        QVERIFY(r.at("gpa_event_map") == Json({{"100", 4}, {"101", 8}}));
        QVERIFY(r.at("gpa_command_map").at("101").at("event_ids").size() == 1);
        QVERIFY(rdcProvenance(2, r.at("reverse")).at("gpa_event").is_null());
    }
    void traversalBounds() {
        auto roots = Json::array({node(1, "", 0)});
        for (unsigned i = 0; i < 130; ++i)
            roots = Json::array({node(i + 2, "nested", 0, roots)});
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, indexRdcEvents(roots, Json::object()));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, indexRdcEvents(Json::object(), Json::object()));
        auto invalid = node(1, "", 0);
        invalid["eventId"] = -1;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, indexRdcEvents({invalid}, Json::object()));
        QVERIFY_THROWS_EXCEPTION(
            std::runtime_error,
            indexRdcEvents({node(1, "GPA 18446744073709551616: Draw", 0x40)}, Json::object()));
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() == 4 && app.arguments()[1] == "--index") {
        try {
            QFile input(app.arguments()[2]);
            if (!input.open(QIODevice::ReadOnly) || input.size() > 64 * 1024 * 1024)
                throw std::runtime_error("Cannot read bounded event probe input");
            const auto bytes = input.readAll();
            const auto cases = Json::parse(bytes.begin(), bytes.end());
            Json outputs = Json::array();
            for (const auto &item : cases) {
                try {
                    auto result = indexRdcEvents(item.at("roots"), item.value("native", Json::object()));
                    Json provenance = Json::object();
                    for (const auto &id : item.value("lookup", Json::array()))
                        provenance[std::to_string(id.get<uint32_t>())] =
                            rdcProvenance(id.get<uint32_t>(), result.at("reverse"));
                    result["provenance"] = provenance;
                    outputs.push_back({{"ok", true}, {"result", result}});
                } catch (const std::exception &e) {
                    outputs.push_back({{"ok", false}, {"error", e.what()}});
                }
            }
            QFile output(app.arguments()[3]);
            const auto raw = outputs.dump(2);
            if (!output.open(QIODevice::WriteOnly) ||
                output.write(raw.data(), qsizetype(raw.size())) != qsizetype(raw.size()))
                throw std::runtime_error("Cannot write event probe output");
            return 0;
        } catch (const std::exception &e) {
            qCritical("%s", e.what());
            return 1;
        }
    }
    RdcTests tests;
    try {
        return QTest::qExec(&tests, argc, argv);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
#include "RdcTests.moc"
