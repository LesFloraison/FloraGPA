#include "application/Experiment.h"
#include "application/ReplaySession.h"
#include "dx12/NativeReplay.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QtTest>
#include <algorithm>
#include <array>
#include <cstring>
#include <set>

using namespace flora;
using Json = nlohmann::json;
namespace {
void require(bool condition, const char *reason) {
    if (!condition) throw std::runtime_error(reason);
}
QByteArray read(const QString &path) {
    QFile file(path); require(file.open(QIODevice::ReadOnly), "Cannot read visibility evidence");
    return file.readAll();
}
void save(const QString &path, const QByteArray &data) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(data) == data.size(), "Cannot save visibility evidence");
}
uint32_t word(const QByteArray &bytes, size_t offset) {
    require(offset <= size_t(bytes.size()) && 4 <= size_t(bytes.size()) - offset, "Visibility word is outside readback");
    uint32_t value{}; memcpy(&value, bytes.constData() + offset, 4); return value;
}
std::string digest(const QByteArray &bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString();
}
}
// Opt-in investigation of the real visibility-buffer mismatch. GPU work is
// confined to two serial prefixes, with all related reads on each same context.
class Dx12VisibilityTests : public QObject {
    Q_OBJECT
  private slots:
    void coherentUndoIdentity() {
        const auto captures = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto output = qEnvironmentVariable("FLORA_DX12_VISIBILITY_PAIR_OUTPUT");
        if (captures.isEmpty() || output.isEmpty()) QSKIP("Set captures and a new visibility-pair output directory");
        QVERIFY(!QFileInfo::exists(output)); QVERIFY(QDir().mkpath(output));
        const auto capture = std::filesystem::path(
            (captures + "/Hellblade2-Win64-Shipping_2026_10_17__03_44_05.gpa_frame").toStdWString());
        auto session = openReplaySession(capture);
        Experiment experiment(*session);
        dx12::StreamDirectory directory(capture);
        uint64_t argumentsCreation{};
        for (const auto id : session->resources())
            if (id <= 578355 && session->resource(id).capturedObject == 2144426640464ULL)
                argumentsCreation = std::max(argumentsCreation, id);
        QVERIFY(argumentsCreation != 0);
        std::array<QByteArray, 2> pixels, canonical;
        Json results = Json::array();
        for (size_t run = 0; run < 2; ++run) {
            const auto name = run == 0 ? QString("baseline") : QString("undo");
            const auto folder = output + '/' + name; QVERIFY(QDir().mkpath(folder));
            if (run) {
                experiment.setEnabled(*session, 10267, false);
                QVERIFY(experiment.undo());
            }
            experiment.save(folder + "/experiment.json");
            const auto edits = experiment.dx12Edits(*session);
            QVERIFY(edits.disabled.empty()); QVERIFY(edits.resources.empty()); QVERIFY(edits.shaders.empty());
            dx12::NativeDevice device(directory, true);
            dx12::NativeReplay replay(device, directory, 12884901888ULL);
            replay.setDisabledExecutions(edits.disabled);
            replay.setUavBarriersBefore(edits.uavBarriersBefore);
            replay.setShaderReplacements(edits.shaders);
            replay.setResourcePatches(edits.resources);
            replay.prepareExecution(10267, true);
            replay.initializeTransfers();
            replay.replayTo(10267, true);
            const auto completed = replay.information();
            Json report{{"case", name.toStdString()}, {"execution", "10267"}, {"reads", Json::object()},
                        {"experiment", experiment.document()},
                        {"scope", "Multiple readbacks from one completed native replay context; no original commands replayed between reads"}};
            const auto publish = [&] {
                save(folder + "/report.json", QByteArray::fromStdString(report.dump(2)));
            };
            report["replay"] = completed;
            report["device"] = device.information();
            publish();
            const auto texture = replay.readTexture(2152470441440ULL, 0,
                std::filesystem::path((folder + "/visibility.bin").toStdWString()), 574650);
            report["texture"] = texture;
            publish();
            pixels[run] = read(folder + "/visibility.bin");
            QCOMPARE(pixels[run].size(), qsizetype(524288));
            QCOMPARE(texture.at("format"), Json(17));
            const auto buffer = [&](const char *label, uint64_t creation, uint64_t offset, uint64_t length) {
                const auto path = folder + '/' + label + ".bin";
                report["reads"][label] = replay.inspectBuffer(creation, offset, length,
                    std::filesystem::path(path.toStdWString()));
                publish();
                return read(path);
            };
            const auto arguments = buffer("arguments", argumentsCreation, 16, 12);
            QCOMPARE(word(arguments, 0), 795U);
            QCOMPARE(word(arguments, 4), 1U); QCOMPARE(word(arguments, 8), 1U);
            // These bindings/offsets are from the selected PSO's recorded MS
            // pipeline inspection, cross-checked below against its GPU work.
            const auto globals = buffer("globals", 537415, 2338816, 28);
            const auto bin = word(globals, 16);
            QVERIFY(bin < 65520 / 24);
            const auto metadata = buffer("bin_metadata", 521198, uint64_t(bin) * 24, 24);
            const auto software = word(metadata, 0), hardware = word(metadata, 4), base = word(metadata, 8);
            QCOMPARE(hardware, word(arguments, 0));
            const auto binData = buffer("bin_data", 562546, (uint64_t(base) + software) * 8, uint64_t(hardware) * 8);
            std::set<uint32_t> dispatched;
            for (size_t i = 0; i < size_t(binData.size()); i += 8) dispatched.insert(word(binData, i));
            std::set<uint32_t> visible;
            for (size_t i = 0; i < size_t(pixels[run].size()); i += 8) {
                const auto low = word(pixels[run], i), high = word(pixels[run], i + 4);
                if (!low && !high) continue;
                QVERIFY(low >> 7);
                const auto index = (low >> 7) - 1;
                QVERIFY(dispatched.contains(index));
                visible.insert(index);
            }
            QVERIFY(!visible.empty());
            const uint64_t first = *visible.begin(), last = *visible.rbegin();
            QVERIFY(last - first < 1024 * 1024); // At most 8 MiB for this focused window.
            const auto entries = buffer("visible_entries", 562379, first * 8, (last - first + 1) * 8);
            canonical[run].resize(pixels[run].size() / 8 * 20);
            canonical[run].fill('\0');
            size_t nonzero{};
            for (size_t i = 0; i < size_t(pixels[run].size()); i += 8) {
                const auto low = word(pixels[run], i), high = word(pixels[run], i + 4);
                if (!low && !high) continue;
                ++nonzero;
                const uint64_t index = (low >> 7) - 1;
                const uint32_t identity[]{1, word(entries, size_t((index - first) * 8)),
                    word(entries, size_t((index - first) * 8 + 4)), low & 127, high};
                memcpy(canonical[run].data() + (i / 8) * 20, identity, sizeof(identity));
            }
            save(folder + "/entry_triangle_depth.bin", canonical[run]);
            const auto afterReads = replay.information();
            QCOMPARE(afterReads.at("last_record"), completed.at("last_record"));
            QCOMPARE(afterReads.at("executed_commands"), completed.at("executed_commands"));
            QCOMPARE(device.information().at("validation_error_count"), Json("0"));
            report["replay"] = afterReads; report["device"] = device.information();
            report["visible_indices"] = {{"first", std::to_string(first)}, {"last", std::to_string(last)},
                {"unique", visible.size()}, {"written_texels", nonzero}, {"all_in_dispatched_bin", true}};
            report["comparison_projection"] = {{"sha256", digest(canonical[run])},
                {"layout", "Five little-endian uint32 words per pixel: present, visible-entry words 0/1, primitive low7, depth bits"},
                {"scope", "Compares captured-resource entry identifiers and local primitive/depth. Does not validate all scene data, other outputs, original GPA fidelity or raw-byte equivalence."}};
            save(folder + "/report.json", QByteArray::fromStdString(report.dump(2)));
            results.push_back({{"case", name.toStdString()}, {"raw_sha256", digest(pixels[run])},
                {"entry_triangle_depth_sha256", digest(canonical[run])}, {"visible_indices", report.at("visible_indices")}});
        }
        Json comparison{{"observations", results}, {"raw_equal", pixels[0] == pixels[1]},
                        {"entry_triangle_depth_equal", canonical[0] == canonical[1]}};
        save(output + "/comparison.json", QByteArray::fromStdString(comparison.dump(2)));
        QCOMPARE(canonical[0], canonical[1]);
    }
};
QTEST_GUILESS_MAIN(Dx12VisibilityTests)
#include "Dx12VisibilityTests.moc"
