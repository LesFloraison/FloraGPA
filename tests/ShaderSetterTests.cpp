#include "ClassCapture.h"
#include "application/Constants.h"
#include "application/Experiment.h"
#include "application/SetterEdits.h"
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
class ShaderSetterTests final : public QObject {
    Q_OBJECT
  private slots:
    void lifetime_data() {
        QTest::addColumn<int>("lifetime");
        QTest::newRow("persistent") << 0;
        QTest::newRow("original-reset") << 1;
        QTest::newRow("clear-state") << 2;
    }
    void lifetime() {
        QFETCH(int, lifetime);
        auto capture = classCapture();
        // A second instance of A refers to the first array member (constant value 10).
        capture.add(63, 5, 0x98, statePack(Id(0), Id(60), 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, Id(64)));
        auto raw = statePack(Id(0), Id(1), Id(15), 1u, uint8_t(1), Id(62));
        capture.add(90, 7, 0x3523, raw);
        if (lifetime == 1)
            capture.add(105, 7, 0x3523, raw);
        if (lifetime == 2)
            capture.add(105, 7, 0x242, statePack(Id(0), Id(1)));
        QTemporaryDir dir;
        auto path = dir.path() + "/shader.gpa_frame";
        capture.save(path);
        Frame frame(path.toStdWString());
        Experiment project(frame);
        const Json values{{"shader", 15}, {"class_instances", Json::array({63})}};
        project.setSetter(frame, 90, values);
        ReplayOptions options;
        options.warp = true;
        options.until = 100;
        project.apply(frame, options);
        auto state = effectiveBindings(frame, 100, frame.state(80), options);
        QCOMPARE(state.stages[5].classes[0], Id(63));
        QCOMPARE(state.stages[5].cb[0], Id(2));
        auto later = effectiveBindings(frame, 110, frame.state(80), options);
        QCOMPARE(later.stages[5].classes[0], Id(lifetime ? 62 : 63));
        Replay replay(frame, options);
        for (int repeat = 0; repeat < 2; ++repeat) {
            replay.run();
            auto bytes = replay.readBuffer(7);
            Reader reader(bytes);
            for (uint32_t i = 0; i < 4; ++i)
                QCOMPARE(reader.read<uint32_t>(), 10u + i);
        }
        options.until = 110;
        Replay second(frame, options);
        second.run();
        auto bytes = second.readBuffer(7);
        Reader reader(bytes);
        for (uint32_t i = 0; i < 4; ++i)
            QCOMPARE(reader.read<uint32_t>(), (lifetime ? 20u : 10u) + i);
        project.save(dir.path() + "/project.json");
        Experiment loaded(frame);
        loaded.load(dir.path() + "/project.json", frame);
        QCOMPARE(loaded.setter(frame, 90), values);
        loaded.undo();
        QCOMPARE(loaded.setter(frame, 90), capturedSetter(frame, 90));
        loaded.redo();
        QCOMPARE(loaded.setter(frame, 90), values);
        auto original = project.document();
        auto incompatible = compileClassProgram(
            "interface I{uint apply(uint x);};class A:I{uint value;uint apply(uint x){return x+value;}};"
            "class B:I{uint value;uint apply(uint x){return x+value*2;}};"
            "cbuffer Classes:register(b0){A first[2];B second[2];} I selected[2];"
            "RWStructuredBuffer<uint> dst:register(u0);"
            "[numthreads(4,1,1)]void main(uint3 "
            "id:SV_DispatchThreadID){dst[id.x]=selected[0].apply(id.x)+selected[1].apply(id.x);}",
            "cs_5_0");
        QVERIFY_THROWS_EXCEPTION(std::exception, project.setShader(frame, 15, incompatible, "", "main"));
        QCOMPARE(project.document(), original);
        for (const Json &invalid : {Json{{"shader", 15}, {"class_instances", Json::array()}},
                                    Json{{"shader", 0}, {"class_instances", Json::array({63})}},
                                    Json{{"shader", 15}, {"class_instances", Json::array({60})}}}) {
            QVERIFY_THROWS_EXCEPTION(std::exception, project.setSetter(frame, 90, invalid));
            QCOMPARE(project.document(), original);
        }
    }
};
QTEST_GUILESS_MAIN(ShaderSetterTests)
#include "ShaderSetterTests.moc"
