#include "IaSetterCapture.h"
#include "application/Experiment.h"
#include "application/SetterEdits.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QtTest>
#include <fstream>
#include <iostream>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
class IaSetterTests final : public QObject {
    Q_OBJECT
  private slots:
    void lifetime_data() {
        QTest::addColumn<int>("lifetime");
        QTest::newRow("persist") << 0;
        QTest::newRow("restore") << 1;
        QTest::newRow("clear") << 2;
    }
    void lifetime() {
        QFETCH(int, lifetime);
        QTemporaryDir dir;
        auto path = dir.path() + "/ia.gpa_frame";
        iaSetterCapture(lifetime).save(path);
        Frame frame(path.toStdWString());
        Experiment project(frame);
        Json values{{"start_slot", 1}, {"buffers", {62}}, {"strides", {4}}, {"offsets", {4}}};
        project.setSetter(frame, 90, values);
        project.setSetter(frame, 91, {{"ib", 66}, {"ib_format", 42}, {"ib_offset", 4}});
        project.setSetter(frame, 92, {{"input_layout", 0}});
        ReplayOptions options;
        options.warp = true;
        options.until = 200;
        project.apply(frame, options);
        auto state = effectiveBindings(frame, 200, frame.state(34), options);
        QCOMPARE(state.vb[0], Id(60));
        QCOMPARE(state.vb[1], Id(62));
        QCOMPARE(state.ibFormat, 42u);
        Replay replay(frame, options);
        for (int repeat = 0; repeat < 2; ++repeat) {
            replay.run();
            replay.inspectNativeState([&](auto *ctx, const auto &objects) {
                ID3D11Buffer *buffers[2]{};
                UINT strides[2]{}, offsets[2]{};
                ctx->IAGetVertexBuffers(0, 2, buffers, strides, offsets);
                Com<ID3D11Buffer> first, second;
                first.Attach(buffers[0]);
                second.Attach(buffers[1]);
                QCOMPARE(static_cast<IUnknown *>(first.Get()), objects.at(60).Get());
                QCOMPARE(static_cast<IUnknown *>(second.Get()), objects.at(62).Get());
                QCOMPARE(strides[1], 4u);
                QCOMPARE(offsets[1], 4u);
            });
        }
        auto later = effectiveBindings(frame, 300, frame.state(34), options);
        QCOMPARE(later.vb[0], Id(lifetime ? 0 : 60));
        QCOMPARE(later.vb[1], Id(lifetime ? 0 : 62));
        options.until = 300;
        Replay last(frame, options);
        last.run();
        last.inspectNativeState([&](auto *ctx, const auto &objects) {
            ID3D11Buffer *buffer{};
            UINT stride{}, offset{};
            ctx->IAGetVertexBuffers(1, 1, &buffer, &stride, &offset);
            Com<ID3D11Buffer> owner;
            owner.Attach(buffer);
            QCOMPARE(static_cast<IUnknown *>(buffer), lifetime ? nullptr : objects.at(62).Get());
        });
        project.save(dir.path() + "/project.json");
        Experiment loaded(frame);
        loaded.load(dir.path() + "/project.json", frame);
        QCOMPARE(loaded.setter(frame, 90), values);
        loaded.undo();
        loaded.redo();
        QCOMPARE(loaded.document(), project.document());
        auto original = project.document();
        QVERIFY_THROWS_EXCEPTION(
            std::exception,
            project.setSetter(frame, 90,
                              {{"start_slot", 0}, {"buffers", {62}}, {"strides", {2049}}, {"offsets", {0}}}));
        QCOMPARE(project.document(), original);
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc == 3 && std::string(argv[1]) == "--oracle") {
        std::ifstream input(argv[2]);
        Json request;
        input >> request;
        Json results = Json::array();
        for (const auto &op : request.at("cases"))
            try {
                Frame frame(QString::fromStdString(op.at("path").get<std::string>()).toStdWString());
                auto id = op.at("event").get<Id>();
                auto value = op.contains("values")
                                 ? iaSetterValues(validateIaSetter(frame, id, op.at("values")))
                                 : capturedSetter(frame, id);
                results.push_back({{"status", "ok"}, {"value", value}});
            } catch (const std::exception &e) {
                results.push_back({{"status", "error"}, {"error", e.what()}});
            }
        std::cout << results.dump();
        return 0;
    }
    IaSetterTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "IaSetterTests.moc"
