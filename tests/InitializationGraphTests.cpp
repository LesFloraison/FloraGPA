#include "SyntheticCapture.h"
#include "application/InitializationGraph.h"
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
namespace {
using Raw = std::vector<uint8_t>;
template <class... T> Raw pack(T... values) {
    Raw raw;
    (append(raw, values), ...);
    return raw;
}
Json node(uint8_t category, uint16_t type, const Raw &raw) {
    QTemporaryDir dir;
    Capture capture;
    capture.add(1, category, type, raw);
    capture.save(dir.filePath("node.gpa_frame"));
    Frame frame(dir.filePath("node.gpa_frame").toStdWString());
    return inspectInitializationNode(frame, 1);
}
Json graph(Capture capture) {
    QTemporaryDir dir;
    capture.save(dir.filePath("graph.gpa_frame"));
    Frame frame(dir.filePath("graph.gpa_frame").toStdWString());
    return inspectInitializationGraph(frame);
}
} // namespace
class InitializationGraphTests : public QObject {
    Q_OBJECT
  private slots:
    void resourceReferenceBoundaries() {
        for (auto [type, size, mandatory] :
             std::vector<std::tuple<uint16_t, size_t, bool>>{{0x88, 68, false},
                                                             {0x8b, 68, true},
                                                             {0x10d, 344, true},
                                                             {0x10f, 64, true},
                                                             {0x127, 24, true},
                                                             {0x9a, 16, true}}) {
            for (Id owner : {Id(0), Id(1) << 32, Id(0xfedcba9800000007)}) {
                Raw raw(size);
                put(raw, 8, owner);
                auto actual = node(5, type, raw);
                QVERIFY(actual["status"] == "recovered");
                const auto expected =
                    mandatory || uint32_t(owner) ? Json::array({uint32_t(owner)}) : Json::array();
                QVERIFY(actual["collector_sequence"] == expected);
            }
        }
        for (uint16_t type : {0x8c, 0x8d, 0x8e, 0x8f}) {
            Raw raw(type == 0x8c || type == 0x8e ? 48 : 44);
            put(raw, 8, Id(0x100000000));
            put(raw, 16, Id(0x200000000));
            QVERIFY(node(5, type, raw)["collector_sequence"] ==
                    (type == 0x8d || type == 0x8e ? Json::array({0, 0}) : Json::array({0})));
        }
        for (uint16_t type : {0x90, 0x92, 0x93, 0x94, 0x95}) {
            Raw raw(56);
            put(raw, 8, Id(7));
            put(raw, 32, Id(9));
            put(raw, 40, Id(999));
            put(raw, 48, Id(7));
            const auto result = node(5, type, raw);
            QVERIFY(result["collector_sequence"] == Json::array({7, 7, 9}));
            QVERIFY(result["dependency_set"] == Json::array({7, 9}));
        }
        for (auto [type, size] : std::vector<std::pair<uint16_t, size_t>>{{0x38, 88}, {0x81, 28}}) {
            Raw raw(size);
            put(raw, 8, Id(42));
            QVERIFY(node(5, type, raw)["collector_sequence"].empty());
        }
        Raw layout(24);
        put(layout, 16, Id(0x100000000));
        QVERIFY(node(5, 0x82, layout)["status"] == "invalid_record");
        put(layout, 16, Id(0x100000007));
        QVERIFY(node(5, 0x82, layout)["collector_sequence"] == Json::array({7}));
    }
    void textureAliasBranch() {
        Raw raw(68);
        put(raw, 0, Id(0x100000000));
        put(raw, 8, Id(7));
        put(raw, 60, Id(9));
        put(raw, 52, 0x80000000u);
        QVERIFY(node(5, 0x85, raw)["collector_sequence"] == Json::array({7, 9, 0}));
        put(raw, 44, 1u);
        QVERIFY(node(5, 0x85, raw)["collector_sequence"] == Json::array({7, 9}));
        put(raw, 44, 0u);
        put(raw, 52, 0u);
        QVERIFY(node(5, 0x85, raw)["collector_sequence"] == Json::array({7, 9}));
    }
    void dataLengthsAndReferences() {
        // Prefix and trailing-byte counterexamples use the checked C++ reader,
        // never the original player's unchecked private ABI.
        std::vector<std::pair<uint16_t, Raw>> records{
            {1, pack(1u, uint8_t(7))},
            {0x102, pack(0u)},
            {0x100, pack(1u, 2u, uint8_t(1), uint16_t(2))},
            {0x101, pack(1u, uint8_t(1), 2u, uint16_t(2))},
            {0x81, pack(1u, 2u, uint8_t(1), uint16_t(2), 1u, uint8_t(3), 1u, Id(99))},
            {0x103, pack(Id(99), Id(1), Id(2), 1u, uint8_t(3))},
            {0x104, pack(Id(0x100000000), 4u)},
            {0x86, pack(1u, Id(0), Id(0))},
            {0x87, pack(1u, Id(0), Id(0), Id(0))},
            {0x84, pack(2u, Id(0x100000000), Id(0), Id(0), Id(0), Id(0x200000007), Id(0), Id(0), Id(0), 1u,
                        uint8_t(4))}};
        for (const auto &[type, raw] : records) {
            auto result = node(9, type, raw);
            QVERIFY(result["status"] == "recovered");
            const auto expected = type == 0x84    ? Json::array({0, 7})
                                  : type == 0x104 ? Json::array({0})
                                                  : Json::array();
            QVERIFY(result["collector_sequence"] == expected);
            for (size_t length = 0; length < raw.size(); ++length) {
                auto invalid = node(9, type, Raw(raw.begin(), raw.begin() + length));
                QVERIFY(invalid["status"] == "invalid_record");
                QVERIFY(!invalid.contains("collector_sequence"));
            }
            auto trailing = raw;
            trailing.push_back(0);
            QVERIFY(node(9, type, trailing)["status"] == "invalid_record");
        }
        for (uint16_t type : {1, 0x81, 0x84, 0x86, 0x87, 0x100, 0x101, 0x102})
            QVERIFY(node(9, type, pack(UINT32_MAX, UINT32_MAX))["status"] == "invalid_record");
    }
    void resourceAndStateLengthRejections() {
        for (auto [type, size] : std::vector<std::pair<uint16_t, size_t>>{
                 {0x38, 88}, {0x81, 28},   {0x82, 24},  {0x83, 48},  {0x84, 56}, {0x85, 68},
                 {0x86, 60}, {0x87, 68},   {0x88, 68},  {0x8b, 68},  {0x8c, 48}, {0x8d, 44},
                 {0x8e, 48}, {0x8f, 44},   {0x90, 56},  {0x92, 56},  {0x93, 56}, {0x94, 56},
                 {0x95, 56}, {0x10d, 344}, {0x10f, 64}, {0x127, 24}, {0x9a, 16}}) {
            for (size_t n = 0; n <= size + 1; ++n) {
                if (n == size)
                    continue;
                QVERIFY(node(5, type, Raw(n))["status"] == "invalid_record");
            }
        }
        for (size_t size : {0, 127, 128, 137, 12000, 22319, 22321})
            QVERIFY(node(3, 3, Raw(size))["status"] == "invalid_record");
    }
    void stateOrderAndOmissions() {
        State s{};
        s.ib = 100;
        s.layout = 2;
        s.vb[0] = 3;
        s.vb[31] = 3;
        for (size_t i = 0; i < 6; ++i) {
            s.stages[i].cb[0] = 10 + i;
            s.stages[i].cb[1] = 20 + i;
            s.stages[i].classes[0] = 30 + i;
            s.stages[i].classCount = 1;
        }
        s.csUav[7] = 40;
        s.csExtended[55] = 41;
        s.so[3] = 42;
        s.scissors = 43;
        s.rasterizer = 44;
        s.viewports = 45;
        s.blend = 46;
        s.depthState = 47;
        s.dsv = 48;
        s.rtv[7] = 49;
        s.omExtended[55] = 50;
        s.predicate = 51;
        const auto expected = Json::array({2,  3,  3,  10, 13, 14, 11, 12, 15, 20, 23, 24, 21, 22, 25, 30,
                                           33, 34, 31, 32, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51});
        QVERIFY(node(3, 3, snapshot(s))["collector_sequence"] == expected);
        s.mask.fill(UINT32_MAX);
        QVERIFY(node(3, 3, snapshot(s))["collector_sequence"] == expected);
        s.stages[5].classCount = UINT32_MAX; // Omitted CS array is never indexed.
        QVERIFY(node(3, 3, snapshot(s))["collector_sequence"] == expected);
        s.stages[0].classCount = 257;
        auto bad = node(3, 3, snapshot(s));
        QVERIFY(bad["status"] == "invalid_record" && !bad.contains("collector_sequence"));
        s = {};
        s.layout = Id(1) << 32;
        s.vb[0] = 0xfedcba9800000007;
        QVERIFY(node(3, 3, snapshot(s))["collector_sequence"] == Json::array({7}));
    }
    void unsupportedAndGraphBoundaries() {
        QVERIFY(node(5, 0xffff, Raw(16))["status"] == "unrecovered");
        QVERIFY(node(9, 0x9999, Raw(4))["status"] == "unrecovered");
        QVERIFY(node(3, 4, Raw(22320))["status"] == "unrecovered");
        Capture capture;
        capture.add(1, 9, 1, pack(0u));
        auto valid = graph(capture);
        QVERIFY(valid["dependency_graph_complete"] == true);
        QVERIFY(valid["initialization_schedule_available"] == false && valid["execution_supported"] == false);
        Raw resource(48);
        put(resource, 40, Id(2));
        capture.add(3, 5, 0x83, resource);
        auto missing = graph(capture);
        QVERIFY(missing["dependency_graph_complete"] == false);
        QVERIFY(missing["issues"][0]["id"] == 3 && missing["issues"][0]["dependency"] == 2);
        capture.add(2, 9, 1, pack(0u));
        QVERIFY(graph(capture)["dependency_graph_complete"] == true);
        capture.add(4, 5, 0xffff, Raw(16));
        QVERIFY(graph(capture)["dependency_graph_complete"] == false);
        capture.entries[0].flags = 1;
        auto unsupported = graph(capture);
        QVERIFY(unsupported["dependency_graph_complete"] == false && unsupported["nodes"].empty());
    }
    void listDependenciesDoNotResolveExecution() {
        for (Id owner : {Id(0), Id(1) << 32, Id(7), Id(0xfedcba9800000009)}) {
            for (Id operand : {Id(0), Id(1) << 32, Id(7), Id(0xabcdef1200000009)}) {
                for (uint32_t restore : {0u, 1u, 2u, UINT32_MAX}) {
                    Capture capture;
                    capture.add(100, 7, 0x41, pack(Id(99), owner, operand, restore));
                    capture.add(101, 5, 0x9a, pack(operand, owner));
                    auto result = graph(capture);
                    QVERIFY(result["execution_supported"] == false);
                    QVERIFY(result["initialization_schedule_available"] == false);
                    QVERIFY(result["dependency_graph_complete"] == false);
                    QCOMPARE(result["nodes"].size(), size_t(2));
                    for (const auto &n : result["nodes"]) {
                        QVERIFY(n["status"] == "recovered");
                        QVERIFY(n["collector_sequence"] == Json::array({uint32_t(owner)}));
                        QVERIFY(n["dependency_set"] == Json::array({uint32_t(owner)}));
                        QVERIFY(n["references"].size() == 1);
                        QVERIFY(n["references"][0]["captured_id"] == owner);
                    }
                    QVERIFY(result["nodes"][0]["collector_rva"] == "0x5bad0");
                    QVERIFY(result["nodes"][1]["collector_rva"] == "0x23be0");
                }
            }
        }
        const auto raw = pack(Id(0), Id(7), Id(8), 1u);
        for (size_t size = 0; size <= raw.size() + 1; ++size) {
            if (size == raw.size())
                continue;
            auto bad = raw;
            bad.resize(size);
            Capture capture;
            capture.add(100, 7, 0x41, bad);
            const auto result = graph(capture);
            QVERIFY(result["nodes"][0]["status"] == "invalid_record");
            QVERIFY(!result["nodes"][0].contains("collector_sequence"));
        }
        // Even a fully resolved initial dependency graph is not execution proof.
        Capture linked;
        linked.add(1, 5, 0x81, Raw(28));
        Raw context(344);
        put(context, 8, Id(1));
        linked.add(7, 5, 0x10d, context);
        linked.add(101, 5, 0x9a, pack(Id(0x1234), Id(7)));
        linked.add(100, 7, 0x41, pack(Id(0), Id(7), Id(101), 1u));
        auto complete = graph(linked);
        QVERIFY(complete["dependency_graph_complete"] == true);
        QVERIFY(complete["issues"].empty());
        QVERIFY(complete["execution_supported"] == false);
        QVERIFY(complete["initialization_schedule_available"] == false);
        // Raw API 0x30d1 is not an ERG descriptor, even though it has the same layout.
        Capture capture;
        capture.add(100, 7, 0x30d1, raw);
        QVERIFY(graph(capture)["nodes"].empty());
    }
    void editedVersionsAreNotInitialGraphs() {
        QTemporaryDir dir;
        Capture capture;
        Raw view(48);
        capture.add(1, 5, 0x8c, view);
        capture.save(dir.filePath("view.gpa_frame"));
        Frame original(dir.filePath("view.gpa_frame").toStdWString());
        put(view, 24, 28u);
        Frame edited(original, {{1, view}});
        auto result = inspectInitializationGraph(edited);
        QVERIFY(result["dependency_graph_complete"] == false && result["nodes"].empty());
        QVERIFY(inspectInitializationNode(edited, 1)["status"] == "unrecovered");
    }
};
QTEST_GUILESS_MAIN(InitializationGraphTests)
#include "InitializationGraphTests.moc"
