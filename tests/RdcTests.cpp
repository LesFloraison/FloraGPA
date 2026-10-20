#include "application/RdcEvents.h"
#include "application/ReplayRequest.h"
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
    void frameEndCaptureBoundary() {
        const Json boundary{{"schema", 1}, {"scope", "frame_stream"},
            {"last_record", "18446744073709551615"}, {"last_execution", "9007199254740993"},
            {"missing_query_policy", "isolate_readback_outputs"}};
        const auto marker = node(42, rdcFrameEndMarker(boundary), 0x20);
        const auto parsed = readRdcFrameEnd(Json::array({marker}));
        QCOMPARE(parsed.at("boundary"), boundary);
        QCOMPARE(parsed.at("event_id"), Json(42));
        const Json job{{"action", "history"}, {"frame_end", true},
            {"frame_end_record", boundary.at("last_record")}, {"execution", boundary.at("last_execution")},
            {"isolate_missing_queries", true}};
        requireRdcFrameEnd(parsed, job);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, requireRdcFrameEnd(nullptr, job));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readRdcFrameEnd(Json::array({marker, marker})));
        QVERIFY(readRdcFrameEnd(Json::array()).is_null());
        for (const auto &key : {"last_record", "last_execution"}) {
            auto invalid = boundary; invalid[key] = 9007199254740993ULL;
            QVERIFY_THROWS_EXCEPTION(std::exception, rdcFrameEndMarker(invalid));
        }
        auto wrong = job; wrong["frame_end_record"] = "9007199254740993";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, requireRdcFrameEnd(parsed, wrong));
        wrong = job; wrong["execution"] = "9007199254740992";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, requireRdcFrameEnd(parsed, wrong));
        wrong = job; wrong["isolate_missing_queries"] = false;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, requireRdcFrameEnd(parsed, wrong));
        wrong = job; wrong["action"] = "debug-pixel";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, requireRdcFrameEnd(parsed, wrong));
    }
    void isolatedQueryResolveExpansion() {
        Json command{{"status","mapped"},{"name","ID3D12GraphicsCommandListResolveQueryData"},
            {"execution","9007199254740993"},{"source_record","8"},{"submission","9"},{"queue","10"},
            {"event_ids",Json::array({20,21})},{"marker_eids",Json::array({19})}};
        Json entry{{"execution","9007199254740993"},{"source_record","8"},{"submission","9"},{"queue","10"},
            {"ranges",Json::array({
                {{"offset","0"},{"bytes","8"},{"query_start",0},{"query_count",1},{"available_from_replayed_queries",false}},
                {{"offset","8"},{"bytes","8"},{"query_start",1},{"query_count",1},{"available_from_replayed_queries",true}},
                {{"offset","16"},{"bytes","8"},{"query_start",2},{"query_count",1},{"available_from_replayed_queries",false}},
                {{"offset","24"},{"bytes","16"},{"query_start",3},{"query_count",2},{"available_from_replayed_queries",true}}})}};
        Json first{{"eventId",20},{"resource_id","9007199254740995"},{"kind","buffer_range"},{"role","destination"},
            {"offset","8"},{"byte_length","8"},{"query_start",1},{"query_count",1},{"query_heap","12"},
            {"query_type",0},{"query_result_stride",8}};
        auto second=first; second.update({{"eventId",21},{"offset","24"},{"byte_length","16"},{"query_start",3},{"query_count",2}});
        const auto operations=Json::array({first,second});
        verifyRdcQueryResolveExpansion(command,entry,operations,"9007199254740995");
        for (const auto key : {"execution","source_record","submission","queue","name","status"}) {
            auto changed=command; changed[key]="different";
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,verifyRdcQueryResolveExpansion(changed,entry,operations,"9007199254740995"));
        }
        for (const auto key : {"offset","byte_length","resource_id","query_heap"}) {
            auto changed=operations; changed[1][key]="0";
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,verifyRdcQueryResolveExpansion(command,entry,changed,"9007199254740995"));
        }
        for (const auto key : {"eventId","query_start","query_count","query_type","query_result_stride"}) {
            auto changed=operations; changed[1][key]=99;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,verifyRdcQueryResolveExpansion(command,entry,changed,"9007199254740995"));
        }
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,verifyRdcQueryResolveExpansion(command,entry,Json::array({first}),"9007199254740995"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,selectDx12RdcExecution({{"execution_commands",{{"9007199254740993",command}}}},
            "9007199254740993"));
    }
    void isolatedQueryCapturePolicy() {
        const auto high = std::to_string((1ULL << 53) + 8);
        Json policy{{"schema", 1}, {"policy", "isolate_readback_outputs"}, {"frame_complete", false},
            {"selected_execution", "18446744073709551615"}, {"after", true},
            {"resources", Json::array({{{"creation", high}, {"object", "123"}, {"width", "18446744073709551615"}}})},
            {"resolves", Json::array({{{"execution", "18446744073709551615"}, {"source_record", high}, {"submission", "8"},
                {"queue", "9"}, {"creation", high}, {"offset", high}, {"bytes", "16"},
                {"ranges", Json::array({{{"offset", high}, {"bytes", "8"}, {"available_from_replayed_queries", false}},
                    {{"offset", std::to_string((1ULL << 53) + 16)}, {"bytes", "8"}, {"available_from_replayed_queries", true}}})}}})}};
        const auto marker = node(1, rdcQueryIsolationMarker(policy), 0x20);
        QCOMPARE(readRdcQueryIsolation(Json::array({marker})), policy);
        QVERIFY(readRdcQueryIsolation(Json::array()).is_null());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readRdcQueryIsolation(Json::array({marker, marker})));
        auto invalid = policy; invalid["resources"][0]["creation"] = 9007199254740993ULL;
        QVERIFY_THROWS_EXCEPTION(std::exception, rdcQueryIsolationMarker(invalid));
        invalid = policy; invalid["resources"][0]["width"] = high;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, rdcQueryIsolationMarker(invalid));
        invalid = policy; invalid["resolves"][0]["ranges"][1]["offset"] = high;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, rdcQueryIsolationMarker(invalid));
        invalid = policy; invalid["after"] = false;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, rdcQueryIsolationMarker(invalid));
        Json job{{"action", "inventory"}, {"isolate_missing_queries", true}};
        requireRdcQueryIsolation(policy, job);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, requireRdcQueryIsolation(nullptr, job));
        job["isolate_missing_queries"] = false;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, requireRdcQueryIsolation(policy, job));
        job["isolate_missing_queries"] = true; job["action"] = "debug-pixel";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, requireRdcQueryIsolation(policy, job));
        job["action"] = "texture"; job["resource"] = "123"; job["resource_creation"] = high;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, requireRdcQueryIsolation(policy, job));
        job["resource"] = "124"; requireRdcQueryIsolation(policy, job);
        job["action"]="resource-history"; job["resource"]="123";
        job["execution"]="18446744073709551615";
        requireRdcQueryIsolation(policy,job); // Index-only history also permits isolated query resources.
        auto pixel = job; pixel["action"] = "history";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, requireRdcQueryIsolation(policy, pixel));
        pixel["resource"] = "124"; requireRdcQueryIsolation(policy, pixel);
        for (const auto *key : {"execution", "resource", "resource_creation"}) {
            auto invalidPixel = pixel; invalidPixel[key] = "0";
            QVERIFY_THROWS_EXCEPTION(std::exception, requireRdcQueryIsolation(policy, invalidPixel));
            invalidPixel.erase(key);
            QVERIFY_THROWS_EXCEPTION(std::exception, requireRdcQueryIsolation(policy, invalidPixel));
        }
        auto missing=job; missing.erase("resource_creation");
        QVERIFY_THROWS_EXCEPTION(std::exception,requireRdcQueryIsolation(policy,missing));
        job["action"]="pipeline";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,requireRdcQueryIsolation(policy,job)); // No query-byte selector.
        job.erase("resource"); job.erase("resource_creation");
        requireRdcQueryIsolation(policy,job);
        missing=job; missing.erase("execution");
        QVERIFY_THROWS_EXCEPTION(std::exception,requireRdcQueryIsolation(policy,missing));
        missing=job; missing["execution"]="0";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,requireRdcQueryIsolation(policy,missing));
        for (const auto *action : {"debug-vertex","debug-pixel","debug-thread","postmesh"}) {
            auto debug=job; debug["action"]=action;
            requireRdcQueryIsolation(policy,debug);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,requireRdcQueryIsolation(nullptr,debug));
            debug["resource"]="123"; debug["resource_creation"]=high;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,requireRdcQueryIsolation(policy,debug));
            debug.erase("resource"); debug.erase("resource_creation"); debug.erase("execution");
            QVERIFY_THROWS_EXCEPTION(std::exception,requireRdcQueryIsolation(policy,debug));
        }
        const ReplayRequestIdentity identity{"isolated", ReplayBackend::Dx12, UINT64_MAX, UINT64_MAX, false};
        job["request_identity"] = replayIdentityJson(identity);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, requireRdcQueryIsolation(policy, job));
    }
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
        auto provenance = rdcProvenance(11, result["reverse"]); provenance["eventId"] = 11;
        QVERIFY(provenance.at("selectable").get<bool>());
        QCOMPARE(provenance.at("indirect_action_kind"), Json("draw"));
        QCOMPARE(indirectActionOrdinal(provenance), int64_t(1));
        QCOMPARE(indirectActionOrdinal(Json::object()), int64_t(-1));
        auto invalid = provenance; invalid["eventId"] = 10;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, indirectActionOrdinal(invalid));
        invalid = provenance; invalid.erase("indirect_action_count");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, indirectActionOrdinal(invalid));
        invalid = provenance; invalid["indirect_action"] = 2;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, indirectActionOrdinal(invalid));
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
