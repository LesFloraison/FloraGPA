#include "StateCapture.h"
#include "application/Experiment.h"
#include "application/ReplayPipeline.h"
#include "application/ViewEdits.h"
#include "core/Contexts.h"
#include "core/OutputBindings.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QtTest>
#include <fstream>
#include <iostream>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
namespace {
void oracle(const QString &path) {
    std::ifstream input(path.toStdWString());
    Json request;
    input >> request;
    Json results = Json::array();
    for (const auto &op : request.at("cases")) {
        try {
            const auto kind = op.at("kind").get<std::string>();
            const auto action = op.at("action").get<std::string>();
            Json value;
            if (action == "normalize")
                value = normalizeView(kind, op.at("value"));
            else if (action == "keys")
                value = viewKeys(kind, op.at("dimension").get<unsigned>());
            else if (action == "merge")
                value = mergeView(kind, op.at("previous"), op.at("value"));
            else if (action == "unpack") {
                auto raw = QByteArray::fromHex(QByteArray::fromStdString(op.at("bytes").get<std::string>()));
                value = unpackView(kind,
                                   Bytes(reinterpret_cast<const uint8_t *>(raw.data()), size_t(raw.size())));
            } else if (action == "pack") {
                auto bytes = packView(kind, op.at("value"));
                value = QByteArray(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size()))
                            .toHex()
                            .toStdString();
            } else
                throw std::runtime_error("Unknown test operation");
            results.push_back({{"status", "ok"}, {"value", value}});
        } catch (const std::exception &e) {
            results.push_back({{"status", "error"}, {"error", e.what()}});
        }
    }
    std::cout << results.dump();
}
} // namespace
class ViewEditTests final : public QObject {
    Q_OBJECT
  private slots:
    void descriptors() {
        for (auto kind : {"srv", "rtv", "dsv", "uav"})
            for (unsigned dimension = 1; dimension <= 11; ++dimension) {
                std::vector<std::string> keys;
                try {
                    keys = viewKeys(kind, dimension);
                } catch (const std::exception &) {
                    continue;
                }
                Json value = Json::object();
                for (auto &key : keys)
                    value[key] = key == "dimension" ? dimension
                                 : key == "format"  ? 28u
                                 : key == "flags"   ? 0u
                                                    : 1u;
                auto bytes = packView(kind, value);
                QCOMPARE(unpackView(kind, bytes), value);
                QCOMPARE(mergeView(kind, value, {{"format", 41}}).at("format"), Json(41));
                auto invalid = value;
                invalid["format"] = true;
                QVERIFY_THROWS_EXCEPTION(std::exception, packView(kind, invalid));
                invalid = value;
                invalid["extra"] = 0;
                QVERIFY_THROWS_EXCEPTION(std::exception, packView(kind, invalid));
                bytes.pop_back();
                QVERIFY_THROWS_EXCEPTION(std::exception, unpackView(kind, bytes));
            }
        auto original = Json{{"format", 28}, {"dimension", 4}, {"mip_slice", 0}};
        QVERIFY_THROWS_EXCEPTION(std::exception, mergeView("rtv", original, {{"dimension", 5}}));
        QCOMPARE(mergeView("rtv", original, {{"dimension", 6}}), Json({{"format", 28}, {"dimension", 6}}));
        QVERIFY_THROWS_EXCEPTION(std::exception, mergeView("rtv", original, {{"flags", 0}}));
        QVERIFY_THROWS_EXCEPTION(std::exception, normalizeView("uav", {{"flags", 3}}));
        QVERIFY_THROWS_EXCEPTION(std::exception, normalizeView("dsv", {{"flags", 4}}));
    }
    void projectHistory() {
        QTemporaryDir dir;
        stateCapture().save(dir.path() + "/frame.gpa_frame");
        Frame frame((dir.path() + "/frame.gpa_frame").toStdWString());
        Experiment project(frame);
        const auto original = project.view(frame, 8);
        project.setView(frame, 8, {{"mip_slice", 1}});
        project.setView(frame, 8, {{"first_array_slice", 1}});
        QCOMPARE(project.view(frame, 8)["descriptor"]["mip_slice"], Json(1));
        QCOMPARE(project.view(frame, 8)["descriptor"]["first_array_slice"], Json(1));
        const auto valid = project.document();
        QVERIFY_THROWS_EXCEPTION(std::exception, project.setView(frame, 8, {{"dimension", 8}}));
        QCOMPARE(project.document(), valid);
        QVERIFY_THROWS_EXCEPTION(std::exception, project.setView(frame, 20, {{"format", 28}}));
        QCOMPARE(project.document(), valid);
        ReplayOptions options;
        project.apply(frame, options);
        auto edited = options.viewFrame;
        QVERIFY(edited);
        QVERIFY(project.undo());
        project.apply(*edited, options);
        QCOMPARE(describeView(effectiveFrame(frame, options), 8)["descriptor"]["first_array_slice"], Json(0));
        QVERIFY(project.undo());
        project.apply(*edited, options);
        QCOMPARE(describeView(effectiveFrame(frame, options), 8), original);
        QVERIFY(project.redo());
        project.setView(frame, 8, {{"format", 29}});
        QVERIFY(!project.canRedo());
        project.save(dir.path() + "/project.json");
        Experiment loaded(frame);
        loaded.load(dir.path() + "/project.json", frame);
        QCOMPARE(loaded.document(), project.document());
        QCOMPARE(loaded.view(frame, 8), project.view(frame, 8));
        QCOMPARE(describeView(frame, 8), original);
    }
    void bufferRanges_data() {
        QTest::addColumn<bool>("warp");
        QTest::addColumn<bool>("counter");
        for (bool warp : {true, false})
            for (bool counter : {true, false})
                QTest::newRow(qPrintable(
                    QString("%1-%2").arg(warp ? "warp" : "hardware").arg(counter ? "counter" : "append")))
                    << warp << counter;
    }
    void bufferRanges() {
        QFETCH(bool, warp);
        QFETCH(bool, counter);
        QTemporaryDir dir;
        computeCapture(counter).save(dir.path() + "/frame.gpa_frame");
        Frame frame((dir.path() + "/frame.gpa_frame").toStdWString());
        Experiment project(frame);
        project.setView(frame, 6, {{"first_element", 1}, {"num_elements", 3}});
        project.setView(frame, 9, {{"first_element", 2}, {"num_elements", 2}});
        project.setView(frame, 12, {{"first_element", 1}, {"num_elements", 3}});
        ReplayOptions options;
        options.warp = warp;
        project.apply(frame, options);
        Replay replay(frame, options);
        replay.run();
        auto expected = statePack(10u, 0u, 20u, 0u);
        QCOMPARE(replay.readBuffer(7), expected);
        QCOMPARE(replay.readBuffer(10), statePack(0u, 8u, 8u, 0u));
        QCOMPARE(replay.readCounter(12), 2u);
        QCOMPARE(firstWord(replay, 13), 2u);
        // Event SRVs inherit the edited global range; unchanged later events
        // restore that global range rather than the capture's original range.
        project.setSrv(frame, 100, "cs", 0, {{"first_element", 2}, {"num_elements", 2}});
        project.apply(frame, options);
        Replay scoped(frame, options);
        scoped.run();
        QCOMPARE(scoped.readBuffer(7), statePack(10u, 0u, 21u, 0u));
        QCOMPARE(scoped.readBuffer(10), statePack(0u, 9u, 8u, 0u));
        Experiment reverse(frame);
        reverse.setSrv(frame, 100, "cs", 0, {{"first_element", 2}, {"num_elements", 2}});
        reverse.setView(frame, 6, {{"first_element", 1}, {"num_elements", 3}});
        reverse.setView(frame, 9, {{"first_element", 2}, {"num_elements", 2}});
        reverse.setView(frame, 12, {{"first_element", 1}, {"num_elements", 3}});
        reverse.apply(frame, options);
        Replay reversed(frame, options);
        reversed.run();
        QCOMPARE(reversed.readBuffer(7), scoped.readBuffer(7));
        QCOMPARE(reversed.readBuffer(10), scoped.readBuffer(10));
        while (project.undo()) {
        }
        project.apply(frame, options);
        QVERIFY(!options.viewFrame);
        Replay original(frame, options);
        original.run();
        QCOMPARE(original.readBuffer(7), statePack(28u, 0u, 0u, 0u));
        QCOMPARE(original.readBuffer(10), statePack(7u, 7u, 0u, 0u));
    }
    void textureRanges_data() { bufferRanges_data(); }
    void textureRanges() {
        QFETCH(bool, warp);
        QFETCH(bool, counter);
        QTemporaryDir dir;
        auto capture = computeCapture(counter);
        capture.add(20, 5, 0x85,
                    statePack(Id(0), Id(0), 4u, 4u, 2u, 2u, 27u, 1u, 0u, 0u, 40u, 0u, 0u, Id(21)));
        auto data = statePack(160u);
        data.resize(164, 0);
        capture.add(21, 9, 1, data);
        capture.add(30, 5, 0x8d, statePack(Id(0), Id(0), Id(20), 28u, 5u, 0u, 1u, 1u));
        capture.add(120, 7, 0x34ff, statePack(Id(0), Id(1), 3u, uint8_t(1), Id(0), Id(0), Id(30), Id(0)));
        capture.add(130, 7, 0x32, statePack(Id(0), Id(1), Id(30), uint8_t(1), .5f, 0.f, 0.f, 1.f));
        capture.save(dir.path() + "/frame.gpa_frame");
        Frame frame((dir.path() + "/frame.gpa_frame").toStdWString());
        Experiment project(frame);
        project.setView(frame, 30, {{"format", 29}, {"mip_slice", 1}, {"first_array_slice", 0}});
        ReplayOptions options;
        options.warp = warp;
        options.until = 130;
        project.apply(frame, options);
        Replay replay(frame, options);
        auto pipeline = inspectReplayPipeline(frame, replay, true);
        bool found = false;
        for (const auto &row : pipeline["fields"])
            if (row["field"] == "rtv.2") {
                QCOMPARE(row["object"]["descriptor_source"], Json("experiment_view_resource"));
                QCOMPARE(row["object"]["descriptor"], project.view(frame, 30)["descriptor"]);
                found = true;
            }
        QVERIFY(found);
        auto bindings = [&] {
            std::array<uintptr_t, 9> values{};
            replay.inspectNativeState([&](ID3D11DeviceContext *context, const auto &) {
                std::array<ID3D11RenderTargetView *, 8> targets{};
                Com<ID3D11DepthStencilView> depth;
                context->OMGetRenderTargets(8, targets.data(), &depth);
                for (size_t i = 0; i < targets.size(); ++i) {
                    values[i] = reinterpret_cast<uintptr_t>(targets[i]);
                    if (targets[i])
                        targets[i]->Release();
                }
                values[8] = reinterpret_cast<uintptr_t>(depth.Get());
            });
            return values;
        };
        auto originalBindings = bindings();
        QVERIFY(originalBindings[2]);
        std::vector<uint8_t> expected(160, 0);
        for (size_t i = 64; i < 80; i += 4) {
            expected[i] = 188;
            expected[i + 3] = 255;
        }
        QCOMPARE(replay.readTexture(20), expected);
        QCOMPARE(bindings(), originalBindings);
        QCOMPARE(replay.readCounter(12), 2u);
        QCOMPARE(replay.readTexture(20), expected);
        QCOMPARE(replay.readCounter(12), 2u);
        QCOMPARE(replay.readBuffer(10), statePack(7u, 7u, 0u, 0u));
        QVERIFY(project.undo());
        project.apply(frame, options);
        Replay original(frame, options);
        original.run();
        const auto originalBytes = original.readTexture(20);
        // UNORM half-value conversion differs by one unit between adapters.
        // The Python oracle separately checks exact bytes on the same driver.
        QVERIFY(originalBytes.at(80) == 127 || originalBytes.at(80) == 128);
        expected.assign(160, 0);
        for (size_t i = 80; i < 144; i += 4) {
            expected[i] = originalBytes.at(80);
            expected[i + 3] = 255;
        }
        QCOMPARE(originalBytes, expected);
        project.setView(frame, 30, {{"mip_slice", 2}});
        project.apply(frame, options);
        Replay invalid(frame, options);
        QVERIFY_THROWS_EXCEPTION(std::exception, invalid.run());
    }
    void immutableOverlayLifetime() {
        QTemporaryDir dir;
        stateCapture().save(dir.path() + "/frame.gpa_frame");
        auto capture = std::make_unique<Frame>((dir.path() + "/frame.gpa_frame").toStdWString());
        const auto hash = capture->sha256();
        const auto original = describeView(*capture, 8);
        const auto originalState = capture->state(90);
        auto descriptor = mergeView("rtv", original.at("descriptor"), {{"mip_slice", 1}});
        auto bytes = editedViewPayload(*capture, 8, descriptor);
        Frame overlay(*capture, {{8, bytes}});
        QCOMPARE(describeView(overlay, 8).at("descriptor"), descriptor);
        QCOMPARE(describeView(*capture, 8), original);
        QCOMPARE(overlay.capturedPayload(8).data(), capture->payload(8).data());
        QVERIFY(overlay.payload(8).data() != capture->payload(8).data());
        QCOMPARE(overlay.payload(5).data(), capture->payload(5).data());
        QCOMPARE(overlay.sha256(), hash);
        QCOMPARE(overlay.state(90).stages[5].shader, originalState.stages[5].shader);
        QVERIFY(&overlay.contextRecovery() != &capture->contextRecovery());
        QVERIFY(overlay.isViewEdited(8));
        QVERIFY(!capture->isViewEdited(8));
        auto invalid = bytes;
        invalid[16] ^= 1;
        QVERIFY_THROWS_EXCEPTION(std::exception, Frame(*capture, {{8, invalid}}));
        invalid = bytes;
        invalid.pop_back();
        QVERIFY_THROWS_EXCEPTION(std::exception, Frame(*capture, {{8, invalid}}));
        QVERIFY_THROWS_EXCEPTION(std::exception, Frame(*capture, {{20, bytes}}));
        QVERIFY_THROWS_EXCEPTION(std::exception, overlay.payload(8, 5, 0x8c));
        // A view keeps the shared read-only mapping alive independently of its owner.
        capture.reset();
        QCOMPARE(describeView(overlay, 8).at("descriptor"), descriptor);
        QCOMPARE(overlay.sha256(), hash);
        QCOMPARE(overlay.state(90).stages[5].shader, originalState.stages[5].shader);
        Frame second(overlay, {});
        QCOMPARE(describeView(second, 8), original);
        QVERIFY(!second.isViewEdited(8));
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() == 3 && app.arguments()[1] == "--oracle") {
        try {
            oracle(app.arguments()[2]);
            return 0;
        } catch (const std::exception &e) {
            std::cerr << e.what();
            return 1;
        }
    }
    ViewEditTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "ViewEditTests.moc"
