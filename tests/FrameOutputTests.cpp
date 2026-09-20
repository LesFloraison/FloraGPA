#include "MsaaCapture.h"
#include "SyntheticCapture.h"
#include "application/Experiment.h"
#include "application/FrameOutput.h"
#include "application/SessionUi.h"
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
            Json value;
            const auto action = op.at("action").get<std::string>();
            if (action == "display") {
                auto raw = QByteArray::fromHex(QByteArray::fromStdString(op.at("bytes").get<std::string>()));
                FrameDisplayOptions options;
                options.channel = op.value("channel", "rgba");
                options.low = op.value("low", 0.);
                options.high = op.value("high", 1.);
                auto bytes =
                    displayOutput(Bytes(reinterpret_cast<const uint8_t *>(raw.data()), size_t(raw.size())),
                                  op.at("format").get<uint32_t>(), options, op.value("aspect", ""));
                value = QByteArray(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size()))
                            .toHex()
                            .toStdString();
            } else if (action == "subresource") {
                Resource resource;
                resource.type = op.at("type").get<uint16_t>();
                resource.desc = op.at("desc").get<std::vector<uint32_t>>();
                auto words = op.at("view").get<std::vector<uint32_t>>();
                std::optional<uint32_t> layer;
                if (op.contains("layer") && !op["layer"].is_null())
                    layer = op["layer"].get<uint32_t>();
                value = outputSubresource(
                    resource, Bytes(reinterpret_cast<const uint8_t *>(words.data()), words.size() * 4), layer,
                    op.value("depth", false));
            } else if (action == "inventory") {
                Frame frame(QString::fromStdString(op.at("path").get<std::string>()).toStdWString());
                value = presentationInventory(frame);
            } else if (action == "target")
                value = parseOutputTarget(op.at("value").get<std::string>());
            else
                throw std::runtime_error("Unknown oracle operation");
            results.push_back({{"status", "ok"}, {"value", value}});
        } catch (const std::exception &e) {
            results.push_back({{"status", "error"}, {"error", e.what()}});
        }
    }
    std::cout << results.dump();
}
} // namespace
class FrameOutputTests : public QObject {
    Q_OBJECT
  private slots:
    void mappedWriteNavigation() {
        QTemporaryDir dir;
        Capture capture;
        capture.add(1, 5, 0x127, std::vector<uint8_t>(24));
        capture.add(2, 5, 0x83, statePack(Id(0), Id(0), 16u, 2u, 1u, 0x10000u, 0u, 0u, Id(0)));
        capture.add(5, 9, 1, statePack(16u, 11u, 22u, 33u, 44u));
        capture.add(100, 7, 0x246, statePack(Id(0), Id(1), int32_t(0), Id(2), 0u, 4u, 0u, Id(5)));
        capture.add(110, 7, 0x246, statePack(Id(0), Id(1), int32_t(-1), Id(2), 0u, 4u, 0u, Id(5)));
        capture.add(120, 7, 0x246, statePack(Id(0), Id(1), int32_t(0), Id(2), 0u, 1u, 0u, Id(0)));
        const auto path = dir.path() + "/mapped.gpa_frame";
        capture.save(path);
        Frame frame(path.toStdWString());
        ReplayOptions options;
        options.warp = true;
        Replay replay(frame, options);
        replay.run();
        QCOMPARE(replay.lastEvent(), Id(120));
        QCOMPARE(replay.lastWorkEvent(), Id(100));
        QCOMPARE(replay.counts.at("Map"), uint64_t(1));
        QCOMPARE(replay.readBuffer(2), statePack(11u, 22u, 33u, 44u));
        QCOMPARE(selectFrameOutput(replay)["navigation_event"], Json(100));
    }
    void beforeDrawTraversal() {
        QTemporaryDir dir;
        auto capture = msaaOutputCapture(false);
        const auto path = dir.path() + "/boundary.gpa_frame";
        capture.save(path);
        Frame frame(path.toStdWString());
        ReplayOptions options;
        options.warp = true;
        options.before = true;
        options.prepareBeforeDraw = false;
        options.until = 100;
        Replay first(frame, options);
        first.run();
        QCOMPARE(first.lastEvent(), Id(0));
        QVERIFY(selectFrameOutput(first)["resource"].is_null());
        options.until = 110;
        Replay ordinary(frame, options);
        ordinary.run();
        QCOMPARE(ordinary.lastEvent(), Id(100));
        QCOMPARE(ordinary.lastWorkEvent(), Id(100));
        QCOMPARE(selectFrameOutput(ordinary, "rt0")["view"], Json(22));
        options.disabled.insert(110);
        Replay observed(frame, options);
        int boundaries = 0;
        observed.run({}, [&](Id id, bool after, auto *, const auto &) {
            if (id == 110) {
                QVERIFY(!after);
                ++boundaries;
            }
        });
        QCOMPARE(boundaries, 1);
        QCOMPARE(observed.lastEvent(), Id(110));
        QCOMPARE(observed.lastWorkEvent(), Id(100));
        QCOMPARE(selectFrameOutput(observed, "rt0")["view"], Json(23));
        // Reusing the same replay must clear the observer and restore ordinary traversal.
        observed.run();
        QCOMPARE(observed.lastEvent(), Id(100));
        QCOMPARE(selectFrameOutput(observed, "rt0")["view"], Json(22));
    }
    void beforeDrawRecoversMissingBindings() {
        QTemporaryDir dir;
        auto capture = msaaOutputCapture(false);
        capture.add(105, 7, 0x34ff, statePack(Id(0), Id(1), 1u, uint8_t(1), Id(999999), Id(0)));
        const auto path = dir.path() + "/gap.gpa_frame";
        capture.save(path);
        Frame frame(path.toStdWString());
        ReplayOptions options;
        options.warp = true;
        options.before = true;
        options.prepareBeforeDraw = false;
        options.until = 110;
        Replay replay(frame, options);
        replay.run();
        QCOMPARE(replay.lastEvent(), Id(105));
        QCOMPARE(replay.lastWorkEvent(), Id(100));
        QCOMPARE(selectFrameOutput(replay, "rt0")["view"], Json(23));
        options.before = false;
        options.until = 105;
        Replay unresolved(frame, options);
        QVERIFY_THROWS_EXCEPTION(std::exception, unresolved.run());
    }
    void replayUiRoundTrip() {
        QTemporaryDir dir;
        auto capture = computeCapture();
        for (auto &entry : capture.entries)
            if (entry.id == 111)
                entry.id = UINT64_MAX;
        capture.save(dir.path() + "/ui.gpa_frame");
        Frame frame((dir.path() + "/ui.gpa_frame").toStdWString());
        Json original{{"shader_documents", {{"900", "unsaved shader"}}},
                      {"frame_display", {{"future_setting", "keep"}}},
                      {"future_id", UINT64_MAX}};
        ReplayUiState state;
        state.target = "rt3";
        state.channel = "b";
        state.low = "-0.00";
        state.high = "4e0";
        state.layer = 2;
        state.sample = 3;
        state.warp = true;
        state.event = UINT64_MAX;
        state.boundary = 1;
        auto ui = replayUiDocument(frame, state, original);
        auto restored = replayUiState(frame, ui);
        QCOMPARE(restored.event, UINT64_MAX);
        QCOMPARE(restored.low, std::string("-0.00"));
        QCOMPARE(restored.high, std::string("4e0"));
        QCOMPARE(restored.layer, std::optional<uint32_t>(2));
        QCOMPARE(restored.sample, std::optional<uint32_t>(3));
        QCOMPARE(restored.boundary, 1);
        QVERIFY(restored.warp);
        QCOMPARE(ui["frame_display"]["future_setting"], Json("keep"));
        QCOMPARE(ui["shader_documents"], original["shader_documents"]);
        Experiment experiment(frame);
        experiment.setEnabled(frame, UINT64_MAX, false);
        const auto before = experiment.document();
        const auto path = dir.path() + "/experiment.json";
        experiment.save(path, ui);
        QCOMPARE(experiment.document(), before);
        Experiment loaded(frame);
        loaded.load(path, frame);
        QCOMPARE(loaded.document()["ui"], ui);
        QCOMPARE(loaded.document()["history"], before["history"]);
        QVERIFY(!loaded.enabled(UINT64_MAX));
        QVERIFY_THROWS_EXCEPTION(std::exception, experiment.save(path, Json::array()));
        loaded.load(path, frame);
        QCOMPARE(loaded.document()["ui"], ui);
        for (auto bad :
             {Json{{"driver", "invalid"}}, Json{{"frame_display", {{"sample", "-1"}}}},
              Json{{"frame_display", {{"sample", "32"}}}}, Json{{"frame_display", {{"channel", "wat"}}}},
              Json{{"frame_display", {{"low", "1"}, {"high", "1"}}}},
              Json{{"frame_display", {{"low", "nan"}}}}, Json{{"frame_display", {{"layer", true}}}},
              Json{{"flora_output_boundary", 3}}})
            QVERIFY_THROWS_EXCEPTION(std::exception, replayUiState(frame, bad));
        QCOMPARE(replayUiState(frame, Json{{"frame_display", {{"target", "swap:123"}}}}).target,
                 std::string("auto"));
        QCOMPARE(replayUiState(frame, Json::object()).boundary, 0);
        QCOMPARE(replayUiState(frame, Json{{"event", 999}}, UINT64_MAX).event, UINT64_MAX);
        QCOMPARE(replayUiState(frame, Json{{"frame_display", {{"layer", " +2 "}, {"sample", "0"}}}}).layer,
                 std::optional<uint32_t>(2));
    }
    void displayValues() {
        std::vector<uint8_t> rgba{10, 20, 30, 40};
        QCOMPARE(displayOutput(rgba, 29), rgba);
        QCOMPARE(displayOutput(rgba, 91), std::vector<uint8_t>({30, 20, 10, 40}));
        QCOMPARE(displayOutput(rgba, 93), std::vector<uint8_t>({30, 20, 10, 255}));
        FrameDisplayOptions options;
        options.channel = "a";
        QCOMPARE(displayOutput(rgba, 28, options), std::vector<uint8_t>({40, 40, 40, 255}));
        options.channel = "rgba";
        options.high = 2;
        QCOMPARE(displayOutput(rgba, 28, options), std::vector<uint8_t>({5, 10, 15, 20}));
        options.low = options.high;
        QVERIFY_THROWS_EXCEPTION(std::exception, displayOutput({}, 28, options));
        options = {};
        options.low = std::numeric_limits<double>::infinity();
        QVERIFY_THROWS_EXCEPTION(std::exception, displayOutput({}, 28, options));
        QVERIFY_THROWS_EXCEPTION(std::exception, displayOutput({}, 40));
        QVERIFY_THROWS_EXCEPTION(std::exception, displayOutput({}, 28, {}, "depth"));
        QVERIFY_THROWS_EXCEPTION(std::exception, displayOutput({rgba.data(), 3}, 28));
    }
    void targetValidation() {
        for (auto target : {"auto", "present", "rt0", "rt7", "depth", "stencil", "swap:18446744073709551615"})
            QCOMPARE(parseOutputTarget(target), std::string(target));
        for (auto target :
             {"rt8", "rt01", "swap:0", "swap:01", "swap:-1", "swap:18446744073709551616", "swap:1junk"})
            QVERIFY_THROWS_EXCEPTION(std::exception, parseOutputTarget(target));
    }
    void msaaSamplesPreserveState() {
        QTemporaryDir dir;
        msaaOutputCapture().save(dir.path() + "/msaa.gpa_frame");
        Frame frame((dir.path() + "/msaa.gpa_frame").toStdWString());
        ReplayOptions options;
        options.warp = true;
        Replay replay(frame, options);
        replay.run();
        auto initial = selectFrameOutput(replay, "rt0");
        auto references = [&] {
            ULONG result = 0;
            replay.inspectNativeState([&](auto *, const auto &objects) {
                auto object = objects.at(23).Get();
                object->AddRef();
                result = object->Release();
            });
            return result;
        };
        const auto refs = references();
        for (int repeat = 0; repeat < 2; ++repeat) {
            for (uint32_t sample = 0; sample < 4; ++sample) {
                FrameDisplayOptions display;
                display.sample = sample;
                auto result = readFrameOutput(replay, 20, display, 23);
                QCOMPARE(result.storage.size(), size_t(35 * 16));
                Reader values(result.storage);
                QCOMPARE(values.read<uint32_t>(), 24u + sample * 4);
                QCOMPARE(values.read<uint32_t>(), 16777217u + sample);
                QCOMPARE(result.msaa["storage_path"], Json("integer_bits"));
                QCOMPARE(selectFrameOutput(replay, "rt0"), initial);
                QCOMPARE(references(), refs);
            }
            auto mean = readFrameOutput(replay, 20, {}, 23);
            QCOMPARE(Reader(mean.storage).read<uint32_t>(), 30u);
            QCOMPARE(mean.msaa["mode"], Json("gpa_shader_mean"));
            QCOMPARE(selectFrameOutput(replay, "rt0"), initial);
            QCOMPARE(references(), refs);
        }
        FrameDisplayOptions invalid;
        invalid.sample = 4;
        QVERIFY_THROWS_EXCEPTION(std::exception, readFrameOutput(replay, 20, invalid, 23));
        invalid.sample = 0;
        invalid.layer = 0;
        QVERIFY_THROWS_EXCEPTION(std::exception, readFrameOutput(replay, 20, invalid, 23));
    }
    void noDrawOutput() {
        QTemporaryDir dir;
        computeCapture().save(dir.path() + "/compute.gpa_frame");
        Frame frame((dir.path() + "/compute.gpa_frame").toStdWString());
        ReplayOptions options;
        options.warp = true;
        Replay replay(frame, options);
        replay.run();
        auto selection = selectFrameOutput(replay);
        QVERIFY(selection["resource"].is_null());
        QCOMPARE(selection["kind"], Json("no_draw_color_target"));
        QCOMPARE(selection["event"], Json(111));
        QCOMPARE(selection["navigation_event"], Json(111));
        QCOMPARE(selectFrameOutput(replay, "rt7")["kind"], Json("unbound_output_slot"));
        QCOMPARE(selectFrameOutput(replay, "present")["kind"], Json("unavailable_presentation_target"));
        QCOMPARE(firstWord(replay, 7), 28u);
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
    FrameOutputTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "FrameOutputTests.moc"
