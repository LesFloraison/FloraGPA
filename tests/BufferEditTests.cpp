#include "SyntheticCapture.h"
#include "application/Constants.h"
#include "application/Experiment.h"
#include "core/BufferBindings.h"
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <d3dcompiler.h>
using namespace flora;
using namespace flora::testing;

class BufferEditTests final : public QObject {
    Q_OBJECT
  private slots:
    void typedConstantTransaction() {
        QTemporaryDir dir;
        auto capture = computeCapture();
        capture.save(dir.path() + "/compute.gpa_frame");
        Frame frame((dir.path() + "/compute.gpa_frame").toStdWString());
        Experiment project(frame);
        ReplayOptions options;
        options.warp = true;
        options.until = 100;
        options.before = true;
        Replay replay(frame, options);
        replay.run();
        auto constants = inspectConstants(frame, replay, options, 100, 2, replay.readBuffer(2));
        QCOMPARE(constants.size(), size_t(1));
        QCOMPARE(constants[0]["stage"], nlohmann::json("cs"));
        auto field = constants[0]["variables"][0]["fields"][0];
        QCOMPARE(field["value"], nlohmann::json({2, 0, 0, 0}));
        auto patches = constantPatches(field, nlohmann::json({20, 0, 0, 9}));
        QCOMPARE(patches.size(), size_t(2));
        project.setBufferPatches(frame, 100, 2, patches, "Constant c");
        QCOMPARE(project.revision(), size_t(1));
        auto saved = project.document();
        QVERIFY_THROWS_EXCEPTION(
            std::runtime_error,
            project.setBufferPatches(frame, 100, 2, {{0, word(30)}, {16, word(40)}}, "Invalid transaction"));
        QCOMPARE(project.document(), saved);
        project.setBufferPatches(frame, 100, 2, {}, "No change");
        QCOMPARE(project.document(), saved);
        project.save(dir.path() + "/constants.json");
        Experiment loaded(frame);
        loaded.load(dir.path() + "/constants.json", frame);
        loaded.apply(frame, options);
        Replay edited(frame, options);
        edited.run();
        edited.inspectEventInputs(100, [&] {
            auto value = inspectConstants(frame, edited, options, 100, 2, edited.readBuffer(2));
            QCOMPARE(value[0]["variables"][0]["fields"][0]["value"], nlohmann::json({20, 0, 0, 9}));
        });
        QCOMPARE(firstWord(edited, 2), 2u);
        QVERIFY(loaded.undo());
        loaded.apply(frame, options);
        QVERIFY(options.buffers.empty());
        QVERIFY(loaded.redo());
        loaded.apply(frame, options);
        options.until = 0;
        options.before = false;
        Replay submitted(frame, options);
        submitted.run();
        QCOMPARE(firstWord(submitted, 7), 46u);
        QCOMPARE(firstWord(submitted, 2), 2u);
    }
    void scopesAndCounters() {
        QTemporaryDir dir;
        auto capture = computeCapture();
        capture.save(dir.path() + "/compute.gpa_frame");
        Frame frame((dir.path() + "/compute.gpa_frame").toStdWString());
        Experiment project(frame);
        project.setBuffer(frame, 100, 2, 0, word(20));
        project.setBuffer(frame, 100, 4, 0, word(70));
        project.setBuffer(frame, 100, 7, 0, word(100));
        project.setBuffer(frame, 110, 10, 12, word(999));
        ReplayOptions options;
        options.warp = true;
        project.apply(frame, options);
        Replay replay(frame, options);
        replay.run();
        QCOMPARE(firstWord(replay, 2), 2u);
        QCOMPARE(firstWord(replay, 4), 7u);
        QCOMPARE(firstWord(replay, 7), 199u);
        QCOMPARE(firstWord(replay, 13), 2u);
        auto appended = replay.readBuffer(10);
        Reader r(appended);
        QCOMPARE(r.read<uint32_t>(), 70u);
        QCOMPARE(r.read<uint32_t>(), 7u);
        QCOMPARE(r.read<uint32_t>(), 0u);
        QCOMPARE(r.read<uint32_t>(), 999u);
        // A second run on the same device must not inherit clones, patches or counters.
        replay.run();
        QCOMPARE(firstWord(replay, 7), 199u);
        QCOMPARE(firstWord(replay, 13), 2u);
        project.setEnabled(frame, 100, false);
        project.apply(frame, options);
        Replay disabled(frame, options);
        disabled.run();
        QCOMPARE(firstWord(disabled, 7), 19u);
        QCOMPARE(firstWord(disabled, 13), 1u);
        QVERIFY(project.undo());
        while (project.undo()) {
        }
        project.apply(frame, options);
        Replay original(frame, options);
        original.run();
        QCOMPARE(firstWord(original, 7), 28u);
        QCOMPARE(firstWord(original, 13), 2u);
        QVERIFY(options.buffers.empty());
    }
    void previewsRollbackAndValidation() {
        QTemporaryDir dir;
        auto capture = computeCapture();
        capture.save(dir.path() + "/compute.gpa_frame");
        Frame frame((dir.path() + "/compute.gpa_frame").toStdWString());
        Experiment project(frame);
        project.setBuffer(frame, 100, 2, 0, word(20));
        project.setBuffer(frame, 100, 4, 0, word(70));
        project.setBuffer(frame, 100, 7, 0, word(100));
        ReplayOptions options;
        options.warp = true;
        options.until = 100;
        options.before = true;
        project.apply(frame, options);
        Replay replay(frame, options);
        replay.run();
        QCOMPARE(firstWord(replay, 7), 10u);
        replay.inspectEventInputs(100, [&] {
            QCOMPARE(firstWord(replay, 2), 20u);
            QCOMPARE(firstWord(replay, 4), 70u);
            QCOMPARE(firstWord(replay, 7), 100u);
        });
        QCOMPARE(firstWord(replay, 2), 2u);
        QCOMPARE(firstWord(replay, 4), 7u);
        QCOMPARE(firstWord(replay, 7), 10u);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.inspectEventInputs(100, [&] {
            throw std::runtime_error("abort preview");
        }));
        QCOMPARE(firstWord(replay, 7), 10u);
        auto saved = project.document();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 project.setBuffer(frame, 100, 13, 0, word(1))); // Unbound buffer.
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, project.setBuffer(frame, 90, 2, 0, word(1))); // Setter.
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, project.setBuffer(frame, 100, 2, UINT64_MAX, word(1)));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, project.setBuffer(frame, 100, 2, 15, word(1)));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, project.setBuffer(frame, 100, 2, 0, Bytes{}));
        QCOMPARE(project.document(), saved);
        project.setBuffer(frame, 100, 2, 0, word(21)); // Later overlapping edits win.
        project.save(dir.path() + "/edit.json");
        Experiment loaded(frame);
        loaded.load(dir.path() + "/edit.json", frame);
        loaded.apply(frame, options);
        Replay later(frame, options);
        later.run();
        later.inspectEventInputs(100, [&] { QCOMPARE(firstWord(later, 2), 21u); });
        QVERIFY(loaded.undo());
        loaded.apply(frame, options);
        Replay previous(frame, options);
        previous.run();
        previous.inspectEventInputs(100, [&] { QCOMPARE(firstWord(previous, 2), 20u); });
        QVERIFY(loaded.redo());
        QCOMPARE(loaded.document(), project.document());
    }
};
QTEST_GUILESS_MAIN(BufferEditTests)
#include "BufferEditTests.moc"
