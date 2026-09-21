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
