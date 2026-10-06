#include "SyntheticCapture.h"
#include "application/Experiment.h"
#include "application/UavCounterInspector.h"
#include "application/FrameValidation.h"
#include "core/UavCounters.h"
#include <QDir>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
class UavCounterTests final : public QObject {
    Q_OBJECT
  private slots:
    void missingInitialValue_data() { scopeAndWrap_data(); }
    void missingInitialValue() {
        QFETCH(bool, counter);
        QFETCH(bool, warp);
        QTemporaryDir dir;
        auto capture = computeCapture(counter);
        capture.entries.erase(std::remove_if(capture.entries.begin(), capture.entries.end(),
                                            [](const auto &e) { return e.id == 90; }), capture.entries.end());
        const auto path = dir.filePath("missing-initial.gpa_frame");
        capture.save(path);
        Frame frame(path.toStdWString());
        const auto report = validateFrame(path.toStdWString());
        bool located = false;
        for (const auto &finding : report["findings"])
            if (finding["kind"] == "counter_initial_value_missing") {
                QVERIFY(finding["resource_id"] == 12);
                QVERIFY(finding["storage_id"] == 10);
                located = true;
            }
        QVERIFY(located);
        ReplayOptions options;
        options.warp = warp;
        Replay unknown(frame, options);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, unknown.readCounter(12));
        const auto unavailable = inspectUavCounters(frame, unknown, 100, 10);
        QVERIFY(unavailable.at(0)["value"].is_null());
        QCOMPARE(unavailable.at(0)["status"], Json("counter_value_unavailable"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, unknown.run());
        QVERIFY(!unknown.counts.contains("Dispatch"));
        options.initialUavCounters[12] = 1;
        Replay seeded(frame, options);
        for (int repeat = 0; repeat < 2; ++repeat) {
            seeded.run();
            QCOMPARE(seeded.readCounter(12), 3u);
            QCOMPARE(firstWord(seeded, 13), 3u);
        }
    }
    void graphics_data() { scopeAndWrap_data(); }
    void graphics() {
        QFETCH(bool, counter);
        QFETCH(bool, warp);
        QTemporaryDir dir;
        auto capture = graphicsCounterCapture(counter);
        capture.save(dir.path() + "/graphics.gpa_frame");
        Frame frame((dir.path() + "/graphics.gpa_frame").toStdWString());
        Experiment project(frame);
        project.setUavCounter(frame, 12, 11);
        project.setUavCounter(frame, 12, 3, 200);
        ReplayOptions options;
        options.warp = warp;
        options.until = 200;
        options.before = true;
        project.apply(frame, options);
        Replay before(frame, options);
        before.run();
        QCOMPARE(before.readCounter(12), 0u);
        before.inspectEventInputs(200, [&] {
            QCOMPARE(before.readCounter(12), 3u);
            auto metadata = inspectUavCounters(frame, before, 200, 10);
            QCOMPARE(metadata[0]["bindings"], Json::array({{{"stage", "om"}, {"slot", 1}}}));
        });
        QCOMPARE(before.readCounter(12), 0u);
        auto setter = referencedCounters(frame, 190, 10);
        QCOMPARE(setter[0].references, std::vector<std::string>({"uavs[0]"}));
        options.before = false;
        Replay after(frame, options);
        after.run();
        QCOMPARE(after.readCounter(12), 4u);
        auto data = after.readBuffer(10);
        QCOMPARE(Reader(Bytes(data).subspan(12)).read<uint32_t>(), 7u);
        QCOMPARE(after.output().rgba, std::vector<uint8_t>({255, 0, 0, 255}));
        project.setUavCounter(frame, 12, UINT32_MAX, 200);
        project.apply(frame, options);
        Replay wrap(frame, options);
        wrap.run();
        QCOMPARE(wrap.readCounter(12), 0u);
        QCOMPARE(wrap.output().rgba, std::vector<uint8_t>({255, 0, 0, 255}));
        auto evidence = qEnvironmentVariable("FLORA_COUNTER_EVIDENCE_DIR");
        if (!evidence.isEmpty() && warp) {
            QVERIFY(QDir().mkpath(evidence));
            capture.save(evidence + (counter ? "/om-counter.gpa_frame" : "/om-append.gpa_frame"));
            project.save(evidence + (counter ? "/om-counter-project.json" : "/om-append-project.json"));
        }
    }
    void scopeAndWrap_data() {
        QTest::addColumn<bool>("counter");
        QTest::addColumn<bool>("warp");
        QTest::newRow("append-warp") << false << true;
        QTest::newRow("counter-warp") << true << true;
        QTest::newRow("append-hardware") << false << false;
        QTest::newRow("counter-hardware") << true << false;
    }
    void scopeAndWrap() {
        QFETCH(bool, counter);
        QFETCH(bool, warp);
        QTemporaryDir dir;
        auto capture = computeCapture(counter);
        capture.uav(18, 10, counter ? 4 : 2); // Different view of the same storage has its own counter.
        capture.save(dir.path() + "/counter.gpa_frame");
        Frame frame((dir.path() + "/counter.gpa_frame").toStdWString());
        Experiment project(frame);
        QVERIFY(project.setUavCounter(frame, 12, UINT32_MAX));
        QVERIFY(project.setUavCounter(frame, 18, 19));
        QVERIFY(!project.setUavCounter(frame, 18, 19));
        ReplayOptions options;
        options.warp = warp;
        options.until = 90;
        options.before = true;
        project.apply(frame, options);
        Replay initial(frame, options);
        initial.run();
        QCOMPARE(initial.readCounter(12), UINT32_MAX);
        QCOMPARE(initial.readCounter(18), 19u);
        QCOMPARE(initial.readBuffer(10), std::vector<uint8_t>(16));
        // Captured reset wins over the frame-initial seed.
        options.until = 100;
        Replay reset(frame, options);
        reset.run();
        QCOMPARE(reset.readCounter(12), 0u);
        QVERIFY(project.setUavCounter(frame, 12, UINT32_MAX, 100));
        project.apply(frame, options);
        Replay preview(frame, options);
        preview.run();
        QCOMPARE(preview.readCounter(12), 0u);
        preview.inspectEventInputs(100, [&] {
            QCOMPARE(preview.readCounter(12), UINT32_MAX);
            auto counters = inspectUavCounters(frame, preview, 100, 10);
            QCOMPARE(counters[0]["value"], Json(UINT32_MAX));
        });
        QCOMPARE(preview.readCounter(12), 0u);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, preview.inspectEventInputs(100, [&] {
            throw std::runtime_error("Abort inspection");
        }));
        QCOMPARE(preview.readCounter(12), 0u);
        options.until = 0;
        options.before = false;
        Replay submitted(frame, options);
        submitted.run();
        QCOMPARE(submitted.readCounter(12), 1u); // UINT_MAX + two submissions wraps to one.
        QCOMPARE(firstWord(submitted, 13), 1u);  // CopyStructureCount sees the retained value.
        QCOMPARE(firstWord(submitted, 10), 7u);
        QCOMPARE(firstWord(submitted, 7), 28u); // Helper restored CB, SRV and compute shader bindings.
        QCOMPARE(submitted.readCounter(18), 19u);
        submitted.run();
        QCOMPARE(submitted.readCounter(12), 1u);
        QCOMPARE(submitted.readCounter(18), 19u);
        project.setEnabled(frame, 100, false);
        project.apply(frame, options);
        Replay disabled(frame, options);
        disabled.run();
        QCOMPARE(disabled.readCounter(12), 1u);
        QCOMPARE(firstWord(disabled, 7), 19u);
        QVERIFY(project.undo());
        QVERIFY(project.undo());
        project.apply(frame, options);
        Replay undo(frame, options);
        undo.run();
        QCOMPARE(undo.readCounter(12), 2u);
        QVERIFY(project.redo());
        project.save(dir.path() + "/experiment.json");
        Experiment loaded(frame);
        loaded.load(dir.path() + "/experiment.json", frame);
        loaded.apply(frame, options);
        Replay redo(frame, options);
        redo.run();
        QCOMPARE(redo.readCounter(12), 1u);
        auto evidence = qEnvironmentVariable("FLORA_COUNTER_EVIDENCE_DIR");
        if (!evidence.isEmpty() && warp) {
            QVERIFY(QDir().mkpath(evidence));
            capture.save(evidence + (counter ? "/counter.gpa_frame" : "/append.gpa_frame"));
            project.save(evidence + (counter ? "/counter-project.json" : "/append-project.json"));
        }
    }
    void persistentSeedAndReferences() {
        QTemporaryDir dir;
        auto capture = computeCapture();
        capture.entries.erase(std::remove_if(capture.entries.begin(), capture.entries.end(),
                                             [](const auto &e) { return e.id == 90; }),
                              capture.entries.end());
        std::vector<uint8_t> clear(16);
        put(clear, 8, Id(1));
        append(clear, Id(12));
        append(clear, uint8_t(1));
        for (int i = 0; i < 4; ++i)
            append(clear, 123u);
        capture.add(108, 7, 0x33, clear);
        capture.save(dir.path() + "/seed.gpa_frame");
        Frame frame((dir.path() + "/seed.gpa_frame").toStdWString());
        Experiment project(frame);
        project.setUavCounter(frame, 12, 3);
        ReplayOptions options;
        options.warp = true;
        options.until = 108;
        project.apply(frame, options);
        Replay replay(frame, options);
        replay.run();
        QCOMPARE(replay.readCounter(12), 4u); // Clear bytes does not clear the hidden counter.
        auto references = inspectUavCounters(frame, replay, 108, 10);
        QCOMPARE(references[0]["scope"], Json("command_reference"));
        QCOMPARE(references[0]["reference_fields"], Json({"view"}));
        options.until = 0;
        Replay full(frame, options);
        full.run();
        QCOMPARE(full.readCounter(12), 5u);
        QCOMPARE(firstWord(full, 13), 5u);
        auto copy = inspectUavCounters(frame, full, 111, 10);
        QCOMPARE(copy[0]["reference_fields"], Json({"source_uav"}));
        QCOMPARE(copy[0]["bindings"], Json::array());
        QCOMPARE(copy[0]["value"], Json(5));
        auto original = computeCapture();
        original.save(dir.path() + "/original.gpa_frame");
        Frame f((dir.path() + "/original.gpa_frame").toStdWString());
        auto set = referencedCounters(f, 90, 10);
        QCOMPARE(set.size(), size_t(1));
        QCOMPARE(set[0].references, std::vector<std::string>({"uavs[1]"}));
        auto state = f.state(f.event(100).state);
        state.csUav[2] = 12;
        auto bound = boundCounters(f, f.event(100), state, 10);
        QCOMPARE(bound.size(), size_t(1));
        QCOMPARE(bound[0].bindings.size(), size_t(2));
        auto draw = f.event(100);
        draw.type = 0x37;
        QVERIFY(boundCounters(f, draw, state, 10).empty());
        state.omStart = 1;
        state.rtCount = 2;
        state.rtv[1] = 12;
        QCOMPARE(boundCounters(f, draw, state, 10)[0].bindings[0].stage, std::string("om"));
    }
    void validationAndCombinedEdits() {
        QTemporaryDir dir;
        auto capture = computeCapture();
        capture.uav(18, 10, 2);
        capture.save(dir.path() + "/counter.gpa_frame");
        Frame frame((dir.path() + "/counter.gpa_frame").toStdWString());
        Experiment project(frame);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, project.setUavCounter(frame, 9, 1));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, project.setUavCounter(frame, 18, 1, 100));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, project.setUavCounter(frame, 12, 1, 90));
        QCOMPARE(project.revision(), size_t(0));
        project.setBuffer(frame, 100, 10, 12, word(999));
        project.setUavCounter(frame, 12, 3, 100);
        ReplayOptions options;
        options.warp = true;
        options.until = 100;
        options.before = true;
        project.apply(frame, options);
        Replay preview(frame, options);
        preview.run();
        preview.inspectEventInputs(100, [&] {
            QCOMPARE(preview.readCounter(12), 3u);
            auto data = preview.readBuffer(10);
            QCOMPARE(Reader(Bytes(data).subspan(12)).read<uint32_t>(), 999u);
        });
        QCOMPARE(preview.readCounter(12), 0u);
        QCOMPARE(preview.readBuffer(10), std::vector<uint8_t>(16));
        options.before = false;
        options.until = 0;
        Replay submitted(frame, options);
        submitted.run();
        QCOMPARE(submitted.readCounter(12), 5u);
        auto bytes = submitted.readBuffer(10);
        QCOMPARE(Reader(Bytes(bytes).subspan(12)).read<uint32_t>(), 7u);
        for (const auto &bad : {Json(-1), Json(4294967296ULL), Json(true), Json(1.0)}) {
            auto document = project.document();
            document["history"][1]["operations"][0]["value"] = bad;
            QFile file(dir.path() + "/invalid.json");
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QByteArray::fromStdString(document.dump()));
            file.close();
            Experiment loaded(frame);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, loaded.load(file.fileName(), frame));
            QCOMPARE(loaded.revision(), size_t(0));
        }
    }
};
QTEST_GUILESS_MAIN(UavCounterTests)
#include "UavCounterTests.moc"
