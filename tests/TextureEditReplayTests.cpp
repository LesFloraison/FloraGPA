#include "TextureEditCapture.h"
#include "application/Experiment.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QtTest>
#include <fstream>
#include <iostream>
using namespace flora;
using namespace flora::testing;
class TextureEditReplayTests : public QObject {
    Q_OBJECT
  private slots:
    void lifetime_data() {
        QTest::addColumn<unsigned>("format");
        QTest::addColumn<unsigned>("samples");
        QTest::addColumn<bool>("output");
        QTest::addColumn<bool>("warp");
        QTest::addColumn<unsigned>("kind");
        for (bool warp : {false, true}) {
            for (unsigned kind : {0x84u, 0x85u, 0x86u})
                for (bool output : {false, true})
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(warp).arg(kind).arg(output)))
                        << 28u << 1u << output << warp << kind;
            for (unsigned format : {28u, 41u, 20u, 45u, 26u, 88u})
                for (bool output : {false, true}) {
                    if (!output && (format == 20 || format == 45))
                        continue;
                    QTest::newRow(qPrintable(QString("msaa-%1-%2-%3").arg(warp).arg(format).arg(output)))
                        << format << 4u << output << warp << 0x85u;
                }
        }
    }
    void lifetime() {
        QFETCH(unsigned, format);
        QFETCH(unsigned, samples);
        QFETCH(bool, output);
        QFETCH(bool, warp);
        QFETCH(unsigned, kind);
        try {
            QTemporaryDir dir;
            auto path = dir.path() + "/texture.gpa_frame";
            auto capture =
                textureEditCapture(format, samples, output, format == 20 || format == 45, uint16_t(kind));
            capture.save(path);
            Frame frame(path.toStdWString());
            auto resource = frame.resource(20);
            auto subs = textureSubresources(resource);
            TexturePatch patch;
            patch.mip = samples > 1 ? 0 : 1;
            patch.layer = kind == 0x86 ? 0 : 1;
            if (samples > 1)
                patch.sample = 2;
            auto sub = subs.at(patch.layer * textureInfo(resource).mips + patch.mip);
            patch.bytes.assign(size_t(sub.size), 0);
            for (size_t pos = 0; pos < patch.bytes.size(); pos += 4)
                put(patch.bytes, pos, 0x00335577u);
            if (format == 20)
                for (size_t pos = 0; pos < patch.bytes.size(); pos += 8) {
                    put(patch.bytes, pos, .375f);
                    put(patch.bytes, pos + 4, 0xa5u);
                }
            if (format == 41)
                for (size_t pos = 0; pos < patch.bytes.size(); pos += 4)
                    put(patch.bytes, pos, pos % 8 ? 0x80000000u : 0x7fc01234u);
            if (format == 45)
                for (size_t pos = 0; pos < patch.bytes.size(); pos += 4)
                    put(patch.bytes, pos, 0xa5355779u + uint32_t(pos));
            if (format == 26)
                for (size_t pos = 0; pos < patch.bytes.size(); pos += 4)
                    put(patch.bytes, pos, 0x781e03c0u);
            auto expected = std::vector<uint8_t>(size_t(subs.back().offset + subs.back().size));
            std::copy(patch.bytes.begin(), patch.bytes.end(), expected.begin() + size_t(sub.offset));
            const auto blank = std::vector<uint8_t>(expected.size());
            auto read = [&](Replay &replay, unsigned sample = 2) {
                return samples > 1 ? replay.readMsaa(20, sample, format).bytes : replay.readTexture(20);
            };
            Experiment experiment(frame);
            experiment.setTexturePatch(frame, 100, 20, patch, output);
            auto unchanged = experiment.document();
            auto invalid = patch;
            invalid.bytes.pop_back();
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                     experiment.setTexturePatch(frame, 100, 20, invalid, output));
            QCOMPARE(experiment.document(), unchanged);
            experiment.save(dir.path() + "/project.json");
            Experiment loaded(frame);
            loaded.load(dir.path() + "/project.json", frame);
            QCOMPARE(loaded.document(), experiment.document());
            ReplayOptions options;
            loaded.apply(frame, options);
            options.warp = warp;
            options.until = 100;
            options.before = true;
            Replay preview(frame, options);
            preview.run();
            QCOMPARE(read(preview), blank);
            preview.inspectEventInputs(100, [&] {
                QCOMPARE(read(preview), expected);
                if (samples > 1)
                    for (unsigned sample : {0u, 1u, 3u})
                        QCOMPARE(read(preview, sample), blank);
                if (!output)
                    preview.inspectNativeState([&](auto *, const auto &objects) {
                        Com<ID3D11ShaderResourceView> active, inactive;
                        check(objects.at(22).As(&active), "Active edit SRV");
                        check(objects.at(23).As(&inactive), "Inactive edit SRV");
                        Com<ID3D11Resource> activeResource, inactiveResource;
                        active->GetResource(&activeResource);
                        inactive->GetResource(&inactiveResource);
                        QVERIFY(activeResource.Get() != inactiveResource.Get());
                    });
            });
            QCOMPARE(read(preview), blank);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, preview.inspectEventInputs(100, [] {
                throw std::runtime_error("inspection failed");
            }));
            QCOMPARE(read(preview), blank);
            options.before = false;
            options.until = 200;
            Replay submitted(frame, options);
            submitted.run();
            QCOMPARE(read(submitted), output ? expected : blank);
            options.disabled.insert(100);
            Replay disabled(frame, options);
            disabled.run();
            QCOMPARE(read(disabled), blank);
            options.disabled.clear();
            options.until = 100;
            Replay failedBefore(frame, options);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                     failedBefore.run({}, [](Id, bool after, auto *, const auto &) {
                                         if (!after)
                                             throw std::runtime_error("before submission failure");
                                     }));
            QCOMPARE(read(failedBefore), blank);
            Replay failedAfter(frame, options);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                     failedAfter.run({}, [](Id, bool after, auto *, const auto &) {
                                         if (after)
                                             throw std::runtime_error("after submission failure");
                                     }));
            QCOMPARE(read(failedAfter), output ? expected : blank);
            loaded.undo();
            loaded.apply(frame, options);
            QVERIFY(options.textureInputs.empty());
            QVERIFY(options.textureOutputs.empty());
            loaded.redo();
            loaded.apply(frame, options);
            QVERIFY(output ? options.textureOutputs.contains(100) : options.textureInputs.contains(100));
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc == 3 && std::string(argv[1]) == "--oracle") {
        using Json = nlohmann::json;
        std::ifstream input(argv[2]);
        Json request;
        input >> request;
        Json results = Json::array();
        for (const auto &item : request.at("cases"))
            try {
                Frame frame(QString::fromStdString(item.at("path").get<std::string>()).toStdWString());
                Experiment experiment(frame);
                if (item.contains("project"))
                    experiment.load(QString::fromStdString(item.at("project").get<std::string>()), frame);
                ReplayOptions options;
                experiment.apply(frame, options);
                options.warp = item.value("warp", false);
                options.until = item.value("event", Id(100));
                options.before = item.value("before", false);
                Replay replay(frame, options);
                replay.run();
                const auto id = item.value("resource", Id(20));
                std::vector<uint8_t> bytes;
                auto read = [&] {
                    bytes = textureInfo(frame.resource(id)).samples > 1
                                ? replay
                                      .readMsaa(id, item.at("sample").get<uint32_t>(),
                                                item.at("format").get<uint32_t>())
                                      .bytes
                                : replay.readTexture(id);
                };
                if (options.before)
                    replay.inspectEventInputs(options.until, read);
                else
                    read();
                results.push_back({{"status", "ok"},
                                   {"bytes", QByteArray(reinterpret_cast<const char *>(bytes.data()),
                                                        qsizetype(bytes.size()))
                                                 .toHex()
                                                 .toStdString()}});
            } catch (const std::exception &e) {
                results.push_back({{"status", "error"}, {"error", e.what()}});
            }
        std::cout << results.dump();
        return 0;
    }
    TextureEditReplayTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "TextureEditReplayTests.moc"
