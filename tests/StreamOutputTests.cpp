#include "StreamCapture.h"
#include "application/Experiment.h"
#include "application/Geometry.h"
#include "application/ShaderInspector.h"
#include "application/StreamOutputInspector.h"
#include "core/Dxbc.h"
#include "core/InspectionRecords.h"
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>
using namespace flora;
using namespace flora::testing;
class StreamOutputTests final : public QObject {
    Q_OBJECT
  private slots:
    void replay_data() {
        QTest::addColumn<bool>("warp");
        QTest::addColumn<bool>("signature");
        QTest::addColumn<bool>("dirty");
        for (bool warp : {false, true})
            for (bool signature : {false, true})
                for (bool dirty : {false, true})
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(warp).arg(signature).arg(dirty)))
                        << warp << signature << dirty;
    }
    void replay() {
        QFETCH(bool, warp);
        QFETCH(bool, signature);
        QFETCH(bool, dirty);
        auto c = streamCapture(signature, dirty);
        QTemporaryDir dir;
        c.save(dir.path() + "/so.gpa_frame");
        Frame frame((dir.path() + "/so.gpa_frame").toStdWString());
        auto decl = readStreamOutputDeclaration(frame, 62);
        QCOMPARE(decl.entries.size(), size_t(1));
        QCOMPARE(*decl.entries[0].semantic, std::string("SV_Position"));
        auto info = inspectResourceShader(frame, 60, frame.shader(61));
        QCOMPARE(info["passthrough"], nlohmann::json(signature));
        ReplayOptions options;
        options.warp = warp;
        options.until = 200;
        Replay replay(frame, options);
        for (int repeat = 0; repeat < 2; ++repeat) {
            replay.run();
            QCOMPARE(replay.drawAutoParameters(200).vertexCount, dirty ? 3u : 6u);
            QVERIFY(replay.drawAutoParameters(200).verified);
            QCOMPARE(replay.streamOutputHistory.size(), size_t(2));
            QCOMPARE(replay.streamOutputHistory[0].written, uint64_t(1));
            auto bytes = replay.readBuffer(70);
            for (size_t i = 0; i < 16; ++i)
                QCOMPARE(bytes[i], uint8_t(0xcd));
            Reader values(Bytes(bytes).subspan(16));
            for (unsigned i = 0; i < (dirty ? 1u : 2u); ++i)
                for (auto v :
                     std::array<std::array<float, 4>, 3>{{{-1, -1, 0, 1}, {-1, 3, 0, 1}, {3, -1, 0, 1}}})
                    QCOMPARE((values.array<float, 4>()), v);
            QCOMPARE(replay.output(20).rgba, (std::vector<uint8_t>{255, 0, 0, 255}));
        }
        auto artifacts = qEnvironmentVariable("FLORA_SO_ARTIFACT_DIR");
        if (!artifacts.isEmpty()) {
            QDir().mkpath(artifacts);
            c.save(artifacts + QString("/so-%1-%2.gpa_frame").arg(signature).arg(dirty));
        }
        options.before = true;
        Replay before(frame, options);
        before.run();
        auto geometry = inspectGeometry(frame, before, 200);
        QCOMPARE(geometry["vertex_references"], nlohmann::json(dirty ? 3 : 6));
        QCOMPARE(geometry["draw_auto"]["history_verified"], nlohmann::json(true));
    }
    void clonedInputAndDisabled() {
        auto c = streamCapture();
        QTemporaryDir dir;
        c.save(dir.path() + "/so.gpa_frame");
        Frame frame((dir.path() + "/so.gpa_frame").toStdWString());
        ReplayOptions options;
        options.warp = true;
        options.until = 200;
        options.disabled.insert(150);
        options.buffers[200][70].push_back({16, {0, 0, 128, 191}});
        Replay replay(frame, options);
        replay.run();
        QCOMPARE(replay.drawAutoParameters(200).vertexCount, 3u);
        QCOMPARE(replay.streamOutputHistory.size(), size_t(1));
        options.before = true;
        Replay before(frame, options);
        before.run();
        before.inspectEventInputs(200, [&] { QCOMPARE(before.drawAutoParameters(200).vertexCount, 3u); });
    }
    void topologyAndTimings_data() {
        QTest::addColumn<bool>("warp");
        QTest::addColumn<bool>("lines");
        for (bool warp : {false, true})
            for (bool lines : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(warp).arg(lines))) << warp << lines;
    }
    void topologyAndTimings() {
        QFETCH(bool, warp);
        QFETCH(bool, lines);
        auto source = QString::fromStdString(streamProducer);
        source.replace("TriangleStream", lines ? "LineStream" : "PointStream");
        if (lines)
            source.remove("v.p=float4(3,-1,0,1);dst.Append(v);");
        auto c = streamCapture(false, false, source.toStdString());
        QTemporaryDir dir;
        c.save(dir.path() + "/topology.gpa_frame");
        Frame frame((dir.path() + "/topology.gpa_frame").toStdWString());
        ReplayOptions options;
        options.warp = warp;
        options.timings = true;
        options.until = 200;
        Replay replay(frame, options);
        replay.run();
        QCOMPARE(replay.streamOutputHistory.size(), size_t(2));
        for (const auto &query : replay.streamOutputHistory) {
            QCOMPARE(query.factor, lines ? 2u : 1u);
            QCOMPARE(query.written, lines ? uint64_t(1) : uint64_t(3));
            QCOMPARE(query.needed, query.written);
        }
        QCOMPARE(replay.drawAutoParameters(200).vertexCount, lines ? 4u : 6u);
        QVERIFY(replay.drawAutoParameters(200).verified);
        QCOMPARE(replay.timings.size(), size_t(3));
        for (size_t i = 0; i < replay.timings.size(); ++i) {
            QCOMPARE(replay.timings[i].event, (std::array<Id, 3>{100, 150, 200}[i]));
            QVERIFY(std::isfinite(replay.timings[i].microseconds));
            QVERIFY(replay.timings[i].microseconds >= 0);
        }
        auto bytes = replay.readBuffer(70);
        Reader values(Bytes(bytes).subspan(16));
        const std::array<std::array<float, 4>, 3> positions{{{-1, -1, 0, 1}, {-1, 3, 0, 1}, {3, -1, 0, 1}}};
        for (unsigned draw = 0; draw < 2; ++draw)
            for (unsigned vertex = 0; vertex < (lines ? 2u : 3u); ++vertex)
                QCOMPARE((values.array<float, 4>()), positions[vertex]);
    }
    void unknownHistoryExperiment() {
        auto c = streamCapture();
        c.entries.erase(std::remove_if(c.entries.begin(), c.entries.end(),
                                       [](const auto &e) { return e.id == 100 || e.id == 150; }),
                        c.entries.end());
        QTemporaryDir dir;
        c.save(dir.path() + "/unknown.gpa_frame");
        Frame frame((dir.path() + "/unknown.gpa_frame").toStdWString());
        ReplayOptions options;
        options.warp = true;
        options.until = 200;
        Replay original(frame, options);
        original.run();
        QCOMPARE(original.drawAutoParameters(200).vertexCount, 99u);
        QVERIFY(!original.drawAutoParameters(200).verified);
        Experiment project(frame);
        project.setEnabled(frame, 200, true);
        project.apply(frame, options);
        QVERIFY(options.disabled.empty());
        QVERIFY(options.editedEvents);
        Replay edited(frame, options);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, edited.run());
        QVERIFY(project.undo());
        project.apply(frame, options);
        QVERIFY(!options.editedEvents);
        Replay restored(frame, options);
        restored.run();
        QCOMPARE(restored.drawAutoParameters(200).vertexCount, 99u);
    }
    void malformedRecords() {
        auto original = streamCapture();
        auto reject = [&](Capture c) {
            QTemporaryDir dir;
            c.save(dir.path() + "/bad.gpa_frame");
            Frame frame((dir.path() + "/bad.gpa_frame").toStdWString());
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, readStreamOutputDeclaration(frame, 62));
        };
        for (uint32_t size = 0; size < 40; ++size) {
            auto c = original;
            for (auto &e : c.entries)
                if (e.id == 62)
                    e.size = size;
            reject(c);
        }
        for (auto [offset, value] : {std::pair<size_t, uint32_t>{0, 513}, {4, 4}, {32, 2052}, {36, 4}}) {
            auto c = original;
            for (const auto &e : c.entries)
                if (e.id == 62)
                    put(c.bytes, e.offset + offset, value);
            reject(c);
        }
        for (auto raw : {statePack(Id(0), Id(1), 5u, uint8_t(0), uint8_t(0)),
                         statePack(Id(0), Id(1), 1u, uint8_t(2), uint8_t(0))})
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, readStreamOutputTargets(raw));
    }
    void capturedQueryMetadata() {
        auto capture = streamCapture();
        std::vector<std::pair<uint16_t, std::vector<uint8_t>>> records;
        auto add = [&](uint16_t type, const std::vector<uint8_t> &tail) {
            auto raw = statePack(Id(0), Id(1));
            raw.insert(raw.end(), tail.begin(), tail.end());
            records.emplace_back(type, raw);
            capture.add(300 + records.size(), 7, type, raw);
        };
        for (auto t : {0x3074, 0x3235, 0x33a8, 0x3471, 0x34b2, 0x358d}) {
            add(uint16_t(t), statePack(0, uint8_t(1), 0u, 0u, Id(0)));
            add(uint16_t(t), statePack(int32_t(0x80004005), uint8_t(0), Id(0)));
            QVERIFY_THROWS_EXCEPTION(
                std::runtime_error,
                acceptQueryMetadata(uint16_t(t), statePack(Id(0), Id(1), 0, uint8_t(1), 0u, 0u, Id(123))));
            QVERIFY_THROWS_EXCEPTION(
                std::runtime_error,
                acceptQueryMetadata(uint16_t(t), statePack(Id(0), Id(1), 0, uint8_t(2), Id(0))));
        }
        for (auto t : {0x3495, 0x34d6, 0x35b1})
            add(uint16_t(t), statePack(0));
        for (auto t : {0x30b4, 0x31b4, 0x331d, 0x33e3, 0x34fb}) {
            add(uint16_t(t), statePack(0, Id(UINT64_MAX), uint8_t(1), 0xdeadbeefu, 88u, 0u));
            add(uint16_t(t), statePack(1, Id(123), uint8_t(0), 0u, 1u));
        }
        add(0x3151, statePack(88u));
        add(0x3152, statePack(uint8_t(1), 4u, 0u));
        add(0x3152, statePack(uint8_t(0)));
        for (const auto &[type, raw] : records) {
            QVERIFY(acceptQueryMetadata(type, raw));
            for (size_t length = 0; length < raw.size(); ++length)
                QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                         acceptQueryMetadata(type, Bytes(raw).first(length)));
            auto trailing = raw;
            trailing.push_back(0);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, acceptQueryMetadata(type, trailing));
        }
        QVERIFY(!acceptQueryMetadata(0xffff, {}));
        QTemporaryDir dir;
        capture.save(dir.filePath("queries.gpa_frame"));
        Frame frame(dir.filePath("queries.gpa_frame").toStdWString());
        ReplayOptions options;
        options.warp = true;
        Replay replay(frame, options);
        replay.run();
        QCOMPARE(replay.counts["query_metadata_records"], uint64_t(records.size()));
        QCOMPARE(replay.drawAutoParameters(200).vertexCount, 6u);
    }
    void trailingInspectionRecords() {
        auto c = streamCapture();
        std::vector<std::pair<uint16_t, std::vector<uint8_t>>> records;
        auto add = [&](uint16_t type, std::vector<uint8_t> tail) {
            auto raw = statePack(Id(0), Id(0xfedcba9876543210));
            raw.insert(raw.end(), tail.begin(), tail.end());
            records.emplace_back(type, raw);
            c.add(300 + records.size(), 7, type, raw);
        };
        for (auto t : {0x304c, 0x304d, 0x4029, 0x402a})
            add(uint16_t(t), statePack(3u));
        add(0x304b, statePack(0u, Id(0), Id(0), Id(0xdeadbeef)));
        add(0x359d, statePack(Id(0)));
        add(0x359d, statePack(Id(UINT64_MAX)));
        for (auto t : {0x30ea, 0x31ea, 0x3353, 0x3419, 0x3531, 0x313b, 0x300e}) {
            add(uint16_t(t), statePack(uint8_t(1), 4u));
            add(uint16_t(t), statePack(uint8_t(0)));
        }
        add(0x3528, statePack(Id(999), uint8_t(1), 2u, uint8_t(1), Id(1), Id(2)));
        add(0x3528, statePack(Id(0), uint8_t(0), uint8_t(0)));
        add(0x3537, statePack(1u, uint8_t(1), Id(777), Id(888)));
        add(0x3537, statePack(0u, uint8_t(0), Id(0)));
        add(0x353d, statePack(uint8_t(1), 1u, uint8_t(1), 0.f, 0.f, 1.f, 1.f, 0.f, 1.f));
        add(0x353d, statePack(uint8_t(0), uint8_t(0)));
        for (auto &[type, raw] : records) {
            QVERIFY(acceptInspectionRecord(type, raw));
            for (size_t length = 0; length < raw.size(); ++length)
                QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                         acceptInspectionRecord(type, Bytes(raw).first(length)));
            auto extra = raw;
            extra.push_back(0);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, acceptInspectionRecord(type, extra));
        }
        QVERIFY(!acceptInspectionRecord(0xffff, {}));
        for (auto bad : {statePack(Id(0), Id(1), uint8_t(2)), statePack(Id(0), Id(1), uint8_t(0), uint8_t(1)),
                         statePack(Id(0), Id(1), uint8_t(1), 65537u, uint8_t(1))})
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, acceptInspectionRecord(0x353d, bad));
        QTemporaryDir dir;
        c.save(dir.path() + "/inspection.gpa_frame");
        Frame frame((dir.path() + "/inspection.gpa_frame").toStdWString());
        ReplayOptions options;
        options.warp = true;
        Replay replay(frame, options);
        replay.run();
        QCOMPARE(replay.counts["inspection_records"], uint64_t(records.size()));
        QCOMPARE(replay.drawAutoParameters(200).vertexCount, 6u);
        QCOMPARE(replay.output(20).rgba, (std::vector<uint8_t>{255, 0, 0, 255}));
    }
};
QTEST_GUILESS_MAIN(StreamOutputTests)
#include "StreamOutputTests.moc"
