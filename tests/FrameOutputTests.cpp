#include "MsaaCapture.h"
#include "SyntheticCapture.h"
#include "application/FrameOutput.h"
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
