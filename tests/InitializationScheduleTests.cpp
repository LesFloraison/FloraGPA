#include "core/InitializationSchedule.h"
#include <QFile>
#include <QtTest>
#include <nlohmann/json.hpp>
using namespace flora;
using Json = nlohmann::json;
class InitializationScheduleTests : public QObject {
    Q_OBJECT
  private slots:
    void nativeSchedules() {
        QFile file(QFINDTESTDATA("fixtures/initialization-schedules.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto report = Json::parse(file.readAll().toStdString());
        QVERIFY(report.at("gpu_acceptance") == false);
        QCOMPARE(report.at("cases").size(), size_t(28));
        size_t checks = 0;
        for (const auto &fixture : report.at("cases")) {
            const auto ready = fixture.at("initially_ready").get<std::set<uint32_t>>();
            std::vector<InitializationScheduleNode> nodes;
            for (const auto &node : fixture.at("nodes")) {
                auto id = node.at("id").get<uint32_t>();
                nodes.push_back({id, node.at("kind").get<uint32_t>(),
                                 node.at("dependencies").get<std::set<uint32_t>>(), ready.contains(id)});
            }
            const auto actual = modelInitializationSchedule(nodes);
            for (const auto &version : fixture.at("versions")) {
                QVERIFY2(actual.order == version.at("original_order").get<std::vector<uint32_t>>(),
                         fixture.at("name").get<std::string>().c_str());
                ++checks;
            }
            for (const auto &node : nodes)
                for (auto parent : node.dependencies)
                    QVERIFY(actual.dependents.at(parent).contains(node.id));
        }
        QCOMPARE(checks, size_t(56));
    }
    void repeatedReadyCallbackIsNotRecursive() {
        std::vector<InitializationScheduleNode> nodes{{10, 3, {20}}, {20, 3, {}, true}, {30, 3, {10}}};
        auto result = modelInitializationSchedule(nodes);
        QVERIFY(result.order == std::vector<uint32_t>({10, 20, 10, 30}));
        QVERIFY(result.previousStatuses == std::vector<uint32_t>({1, 2, 2, 1}));
        QVERIFY(modelInitializationSchedule({}).order.empty());
    }
    void rejectsInvalidGraphs() {
        for (const auto &nodes :
             std::vector<std::vector<InitializationScheduleNode>>{{{1, 0, {}}},
                                                                  {{1, 5, {}}},
                                                                  {{1, 1, {}}, {1, 2, {}}},
                                                                  {{1, 3, {2}}},
                                                                  {{1, 3, {1}}},
                                                                  {{1, 3, {2}}, {2, 2, {1}}},
                                                                  {{1, 3, {2}, true}, {2, 2, {1}, true}}})
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, modelInitializationSchedule(nodes));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, modelInitializationSchedule({{UINT32_MAX, 4, {}}}, 0));
    }
    void deepGraphsAreIterativeAndBounded() {
        std::vector<InitializationScheduleNode> nodes;
        for (uint32_t id = 1; id <= 1000; ++id)
            nodes.push_back({id, 3, id == 1000 ? std::set<uint32_t>{} : std::set<uint32_t>{id + 1}});
        const auto actual = modelInitializationSchedule(nodes);
        QCOMPARE(actual.order.size(), nodes.size());
        QCOMPARE(actual.order.front(), 1000u);
        QCOMPARE(actual.order.back(), 1u);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, modelInitializationSchedule(nodes, 100));
    }
};
QTEST_GUILESS_MAIN(InitializationScheduleTests)
#include "InitializationScheduleTests.moc"
