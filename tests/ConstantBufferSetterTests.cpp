#include "ConstantBufferCapture.h"
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
class ConstantBufferSetterTests : public QObject {
    Q_OBJECT
  private slots:
    void snapshotDoesNotEstablishWindow() {
        QTemporaryDir dir;
        auto capture = constantBufferCapture();
        std::erase_if(capture.entries, [](const Entry &e) { return e.id == 75 || e.id == 80; });
        State state{};
        state.stages[4].cb[2] = 60;
        capture.add(35, 3, 3, snapshot(state));
        capture.add(85, 7, 0x37, statePack(Id(35), Id(0), Id(1), 0u, 0u));
        auto path = dir.path() + "/unknown.gpa_frame";
        capture.save(path);
        Frame frame(path.toStdWString());
        ConstantBufferHistory prefix(frame);
        QVERIFY(!prefix.advance(90)[4][2]);
        auto original = readConstantBufferSetter(frame.entry(90).type, frame.payload(90));
        auto edited = original;
        edited.start = 3;
        edited.buffers = {62};
        edited.first = std::vector<uint32_t>{16};
        edited.counts = std::vector<uint32_t>{16};
        ConstantBufferBindings tracker;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 tracker.transition(original, &edited, prefix.advance(90)));
        QVERIFY(!tracker.active(4));
    }
    void persistence_data() {
        QTest::addColumn<unsigned>("stage");
        QTest::addColumn<unsigned>("encoding");
        QTest::addColumn<int>("lifetime");
        for (unsigned stage = 0; stage < 6; ++stage)
            for (unsigned encoding = 0; encoding < 3; ++encoding)
                for (int life = 0; life < 3; ++life)
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(stage).arg(encoding).arg(life)))
                        << stage << encoding << life;
    }
    void persistence() {
        QFETCH(unsigned, stage);
        QFETCH(unsigned, encoding);
        QFETCH(int, lifetime);
        QTemporaryDir dir;
        auto path = dir.path() + "/cb.gpa_frame";
        constantBufferCapture(stage, encoding, lifetime).save(path);
        Frame frame(path.toStdWString());
        Experiment experiment(frame);
        auto values = capturedSetter(frame, 90);
        values["buffers"] = {60, 62};
        if (encoding == 2) {
            values["first_constants"] = {16, 16};
            values["constant_counts"] = {16, 16};
        }
        experiment.setSetter(frame, 90, values);
        ReplayOptions options;
        experiment.apply(frame, options);
        options.warp = true;
        options.until = 200;
        auto state = effectiveBindings(frame, 200, frame.state(34), options);
        QCOMPARE(state.stages[stage].cb[2], Id(60));
        QCOMPARE(state.stages[stage].cb[3], Id(62));
        QCOMPARE(state.stages[stage].cbRanges.at(2)[0],
                 encoding == 2 ? std::optional<uint32_t>(16) : std::nullopt);
        Replay replay(frame, options);
        for (int repeat = 0; repeat < 2; ++repeat) {
            replay.run();
            replay.inspectNativeState([&](auto *ctx, const auto &objects) {
                Com<ID3D11DeviceContext1> extended;
                check(ctx->QueryInterface(IID_PPV_ARGS(&extended)), "CB1 getter context");
                using Getter = void (STDMETHODCALLTYPE ID3D11DeviceContext1::*)(UINT, UINT, ID3D11Buffer **,
                                                                                UINT *, UINT *);
                static constexpr Getter getters[]{&ID3D11DeviceContext1::VSGetConstantBuffers1,
                                                  &ID3D11DeviceContext1::HSGetConstantBuffers1,
                                                  &ID3D11DeviceContext1::DSGetConstantBuffers1,
                                                  &ID3D11DeviceContext1::GSGetConstantBuffers1,
                                                  &ID3D11DeviceContext1::PSGetConstantBuffers1,
                                                  &ID3D11DeviceContext1::CSGetConstantBuffers1};
                ID3D11Buffer *buffer{};
                UINT first{}, count{};
                (extended.Get()->*getters[stage])(2, 1, &buffer, &first, &count);
                Com<ID3D11Buffer> owner;
                owner.Attach(buffer);
                QCOMPARE(static_cast<IUnknown *>(buffer), objects.at(60).Get());
                QCOMPARE(first, encoding == 2 ? 16u : 0u);
                QCOMPARE(count, encoding == 2 ? 16u : 4096u);
            });
        }
        options.until = 300;
        Replay later(frame, options);
        later.run();
        auto after = effectiveBindings(frame, 300, frame.state(34), options);
        QCOMPARE(after.stages[stage].cb[2], Id(lifetime ? 0 : 60));
        experiment.save(dir.path() + "/project.json");
        Experiment loaded(frame);
        loaded.load(dir.path() + "/project.json", frame);
        QCOMPARE(loaded.setter(frame, 90), values);
        loaded.undo();
        loaded.redo();
        QCOMPARE(loaded.document(), experiment.document());
        auto before = experiment.document();
        auto invalid = values;
        invalid["start_slot"] = 14;
        QVERIFY_THROWS_EXCEPTION(std::exception, experiment.setSetter(frame, 90, invalid));
        QCOMPARE(experiment.document(), before);
        values["start_slot"] = 3;
        values["buffers"] = {62};
        if (encoding == 2) {
            values["first_constants"] = {16};
            values["constant_counts"] = {16};
        }
        experiment.setSetter(frame, 90, values);
        experiment.apply(frame, options);
        options.until = 200;
        auto moved = effectiveBindings(frame, 200, frame.state(34), options);
        QCOMPARE(moved.stages[stage].cb[2], Id(60));
        QCOMPARE(moved.stages[stage].cbRanges.at(2)[0], std::optional<uint32_t>(16));
        Replay displaced(frame, options);
        displaced.run();
        QCOMPARE(displaced.constantRange(stage, 2, 60).first, 16u);
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
                auto value =
                    op.contains("values")
                        ? constantBufferSetterValues(validateConstantBufferSetter(frame, id, op.at("values")))
                        : capturedSetter(frame, id);
                results.push_back({{"status", "ok"}, {"value", value}});
            } catch (const std::exception &e) {
                results.push_back({{"status", "error"}, {"error", e.what()}});
            }
        std::cout << results.dump();
        return 0;
    }
    ConstantBufferSetterTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "ConstantBufferSetterTests.moc"
