#include "application/CommandEdits.h"
#include "application/Experiment.h"
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
template <class T> void append(std::vector<uint8_t> &out, T value) {
    auto p = reinterpret_cast<const uint8_t *>(&value);
    out.insert(out.end(), p, p + sizeof value);
}
template <class T> void put(std::vector<uint8_t> &out, size_t offset, T value) {
    std::memcpy(out.data() + offset, &value, sizeof value);
}
struct Capture {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x128);
    std::vector<Entry> entries;
    void add(Id id, uint8_t category, uint16_t type, const std::vector<uint8_t> &payload) {
        entries.push_back({id, bytes.size(), uint32_t(payload.size()), 0, category, type});
        bytes.insert(bytes.end(), payload.begin(), payload.end());
    }
    void save(const QString &path) {
        put(bytes, 0, 0x41504749u);
        put(bytes, 4, 0x128u);
        put(bytes, 8, 3u);
        put(bytes, 12, uint32_t(entries.size()));
        put(bytes, 0xf4, uint64_t(bytes.size()));
        std::memcpy(bytes.data() + 0x44, "DX11", 4);
        for (auto &e : entries) {
            append(bytes, e.id);
            append(bytes, e.offset);
            append(bytes, e.size);
            append(bytes, e.flags);
            append(bytes, e.category);
            append(bytes, e.type);
        }
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) ||
            file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size()) != qint64(bytes.size()))
            throw std::runtime_error("Cannot write synthetic fixture");
    }
};
std::vector<uint8_t> command(size_t size) {
    std::vector<uint8_t> raw(size);
    put(raw, 8, Id(1));
    return raw;
}
Capture updateCapture(bool badBox = false) {
    Capture c;
    c.add(1, 5, 0x127, std::vector<uint8_t>(24));
    std::vector<uint8_t> resource(16);
    append(resource, D3D11_BUFFER_DESC{16, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0});
    append(resource, Id(3));
    c.add(2, 5, 0x83, resource);
    std::vector<uint8_t> initial(20);
    put(initial, 0, 16u);
    c.add(3, 9, 1, initial);
    std::vector<uint8_t> data(8, 0x44);
    put(data, 0, 4u);
    c.add(4, 9, 1, data);
    auto raw = command(69);
    put(raw, 16, Id(2));
    raw[28] = 1;
    put(raw, 29, D3D11_BOX{4, 0, 0, badBox ? 24u : 8u, 1, 1});
    put(raw, 53, Id(4));
    put(raw, 61, 4096u);
    put(raw, 65, 8192u); // Deliberately padded captured pitches.
    c.add(100, 7, 0x247, raw);
    return c;
}
} // namespace
class CommandTests final : public QObject {
    Q_OBJECT
  private slots:
    void clearValidation() {
        for (uint16_t type : {0x31, 0x32, 0x33, 0x34}) {
            auto raw = command(type == 0x31 ? 33 : 41);
            if (type == 0x31) {
                put(raw, 24, 3u);
                put(raw, 28, 1.f);
            } else
                raw[24] = 1;
            Json values = type == 0x31   ? Json{{"flags", 2}, {"depth", .25}, {"stencil", 255}}
                          : type == 0x33 ? Json{{"values", {0u, 1u, 0x80000000u, UINT32_MAX}}}
                                         : Json{{"values", {-.5, .25, 2., 1.}}};
            auto patched = patchClear(type, raw, values);
            QCOMPARE(patched.size(), raw.size());
            QVERIFY(std::equal(raw.begin(), raw.begin() + 24, patched.begin()));
            QCOMPARE(clearValues(type, patched), values);
            auto invalid = values;
            invalid["unexpected"] = 0;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, patchClear(type, raw, invalid));
            raw.push_back(0);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, patchClear(type, raw, values));
        }
        auto depth = command(33);
        Json values{{"flags", 1}, {"depth", 1.0}, {"stencil", 0}};
        for (const auto &bad : {Json(0), Json(4), Json(-1), Json(true), Json(1.5), Json(UINT64_MAX)}) {
            auto copy = values;
            copy["flags"] = bad;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, patchClear(0x31, depth, copy));
        }
        for (const auto &bad : {Json(-.1), Json(1.1), Json(1.e100), Json(true), Json("0.5")}) {
            auto copy = values;
            copy["depth"] = bad;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, patchClear(0x31, depth, copy));
        }
        auto color = command(41);
        color[24] = 1;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 patchClear(0x33, color, Json{{"values", {0, 1, 2, -1}}}));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, patchClear(0x32, color, Json{{"values", {0, 1, 2}}}));
        color[24] = 0;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, clearValues(0x32, color));
    }
    void writableLayouts() {
        Capture c;
        c.add(1, 5, 0x127, std::vector<uint8_t>(24));
        std::map<uint16_t, size_t> sizes{{0x31, 33},  {0x32, 41},  {0x33, 41}, {0x34, 41},
                                         {0x3e, 32},  {0x3f, 36},  {0x40, 53}, {0x42, 44},
                                         {0x245, 24}, {0x246, 48}, {0x247, 45}};
        for (auto [type, size] : sizes) {
            auto raw = command(size);
            if (type >= 0x32 && type <= 0x34)
                raw[24] = 1;
            c.add(type, 7, type, raw);
            raw.push_back(0);
            c.add(type + 0x1000, 7, type, raw);
        }
        QTemporaryDir dir;
        c.save(dir.path() + "/layouts.gpa_frame");
        Frame frame((dir.path() + "/layouts.gpa_frame").toStdWString());
        for (auto [type, size] : sizes) {
            validateWritableCommand(frame, type);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateWritableCommand(frame, type + 0x1000));
        }
    }
    void updateHistoryAndGpu() {
        QTemporaryDir dir;
        auto path = dir.path() + "/update.gpa_frame";
        auto capture = updateCapture();
        capture.save(path);
        Frame frame(path.toStdWString());
        auto layout = updateSourceLayout(frame, 100);
        QCOMPARE(layout.size, 4ull);
        QCOMPARE(layout.rowPitch, 4u);
        QCOMPARE(layout.slicePitch, 4u);
        Experiment experiment(frame);
        std::vector<uint8_t> edit{1, 2, 3, 4};
        experiment.setUpdateSource(frame, 100, edit);
        auto projectPath = dir.path() + "/experiment.json";
        experiment.save(projectPath);
        Experiment loaded(frame);
        loaded.load(projectPath, frame);
        QCOMPARE(loaded.document(), experiment.document());
        auto read = [&](Experiment &project, bool before = false) {
            ReplayOptions options;
            options.warp = true;
            options.until = 100;
            options.before = before;
            project.apply(frame, options);
            Replay replay(frame, options);
            replay.run();
            return replay.readBuffer(2);
        };
        auto bytes = read(loaded);
        QCOMPARE(bytes, std::vector<uint8_t>({0, 0, 0, 0, 1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 0, 0}));
        QCOMPARE(read(loaded, true), std::vector<uint8_t>(16));
        QVERIFY(loaded.undo());
        QCOMPARE(read(loaded),
                 std::vector<uint8_t>({0, 0, 0, 0, 0x44, 0x44, 0x44, 0x44, 0, 0, 0, 0, 0, 0, 0, 0}));
        QVERIFY(loaded.redo());
        QCOMPARE(read(loaded), bytes);
        loaded.setEnabled(frame, 100, false);
        QCOMPARE(read(loaded), std::vector<uint8_t>(16));
        QVERIFY(loaded.undo());
        QCOMPARE(read(loaded), bytes);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 loaded.setUpdateSource(frame, 100, std::vector<uint8_t>(5)));
        auto bad = updateCapture(true);
        bad.save(dir.path() + "/bad.gpa_frame");
        Frame invalid((dir.path() + "/bad.gpa_frame").toStdWString());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, updateSourceLayout(invalid, 100));
    }
    void uintClearGpu() {
        Capture capture;
        capture.add(1, 5, 0x127, std::vector<uint8_t>(24));
        std::vector<uint8_t> buffer(16);
        append(buffer, D3D11_BUFFER_DESC{16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                                         D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0});
        append(buffer, Id(0));
        capture.add(2, 5, 0x83, buffer);
        std::vector<uint8_t> view(16);
        append(view, Id(2));
        D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
        desc.Format = DXGI_FORMAT_R32_TYPELESS;
        desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        desc.Buffer.NumElements = 4;
        desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        append(view, desc);
        capture.add(3, 5, 0x8f, view);
        auto raw = command(41);
        put(raw, 16, Id(3));
        raw[24] = 1;
        capture.add(100, 7, 0x33, raw);
        QTemporaryDir dir;
        capture.save(dir.path() + "/uint.gpa_frame");
        Frame frame((dir.path() + "/uint.gpa_frame").toStdWString());
        Experiment experiment(frame);
        experiment.setClear(frame, 100, Json{{"values", {UINT32_MAX, 0u, 0x80000000u, 1u}}});
        ReplayOptions options;
        options.warp = true;
        experiment.apply(frame, options);
        Replay replay(frame, options);
        replay.run();
        QCOMPARE(replay.readBuffer(2), std::vector<uint8_t>(16, 255));
    }
    void textureUpdateLayouts() {
        QTemporaryDir dir;
        int index = 0;
        auto layout = [&](uint16_t type, std::vector<uint32_t> desc, uint32_t subresource, D3D11_BOX box) {
            Capture capture;
            capture.add(1, 5, 0x127, std::vector<uint8_t>(24));
            std::vector<uint8_t> resource(16);
            for (auto word : desc)
                append(resource, word);
            append(resource, Id(0));
            capture.add(2, 5, type, resource);
            auto raw = command(69);
            put(raw, 16, Id(2));
            put(raw, 24, subresource);
            raw[28] = 1;
            put(raw, 29, box);
            capture.add(100, 7, 0x247, raw);
            auto path = dir.path() + '/' + QString::number(index++) + ".gpa_frame";
            capture.save(path);
            Frame frame(path.toStdWString());
            return updateSourceLayout(frame, 100);
        };
        auto mip = layout(0x85, {8, 8, 2, 2, 28, 1, 0, 0, 8, 0, 0}, 3, {1, 1, 0, 3, 3, 1});
        QCOMPARE(mip.size, 16ull);
        QCOMPARE(mip.rowPitch, 8u);
        QCOMPARE(mip.depth, 1u);
        auto volume = layout(0x86, {8, 8, 4, 2, 28, 0, 8, 0, 0}, 1, {1, 1, 0, 3, 3, 2});
        QCOMPARE(volume.size, 32ull);
        QCOMPARE(volume.slicePitch, 16u);
        QCOMPARE(volume.depth, 2u);
        auto bc = layout(0x85, {7, 7, 1, 1, 71, 1, 0, 0, 8, 0, 0}, 0, {4, 4, 0, 7, 7, 1});
        QCOMPARE(bc.size, 8ull);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 layout(0x85, {7, 7, 1, 1, 71, 1, 0, 0, 8, 0, 0}, 0, {1, 4, 0, 7, 7, 1}));
        auto planar = layout(0x85, {8, 8, 1, 1, 103, 1, 0, 0, 8, 0, 0}, 0, {0, 2, 0, 4, 6, 1});
        QCOMPARE(planar.size, 24ull);
        QCOMPARE(planar.rowPitch, 4u);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 layout(0x85, {8, 8, 1, 1, 103, 1, 0, 0, 8, 0, 0}, 0, {1, 2, 0, 5, 6, 1}));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 layout(0x85, {8, 8, 1, 1, 28, 4, 0, 0, 8, 0, 0}, 0, {0, 0, 0, 8, 8, 1}));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 layout(0x85, {8, 8, 2, 2, 28, 1, 0, 0, 8, 0, 0}, 4, {0, 0, 0, 4, 4, 1}));
    }
    void rejectDeferredAndCorruptProjects() {
        QTemporaryDir dir;
        auto capture = updateCapture();
        put(capture.bytes, size_t(capture.entries[0].offset) + 16, 1u);
        capture.save(dir.path() + "/deferred.gpa_frame");
        Frame deferred((dir.path() + "/deferred.gpa_frame").toStdWString());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateWritableCommand(deferred, 100));
        capture = updateCapture();
        capture.save(dir.path() + "/valid.gpa_frame");
        Frame frame((dir.path() + "/valid.gpa_frame").toStdWString());
        Experiment project(frame);
        project.setUpdateSource(frame, 100, std::vector<uint8_t>(4, 42));
        auto doc = project.document();
        doc["history"][0]["operations"][0]["asset"]["sha256"] = std::string(64, '0');
        QFile file(dir.path() + "/bad.json");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray::fromStdString(doc.dump()));
        file.close();
        auto saved = project.document();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, project.load(file.fileName(), frame));
        QCOMPARE(project.document(), saved);
    }
    void capturedClearAndGpu() {
        auto directory = qEnvironmentVariable("FLORA_TEST_CAPTURE_DIR");
        if (directory.isEmpty())
            QSKIP("External capture fixtures are not configured");
        Frame frame((directory + "/GF2_Exilium_2026_03_03__00_19_35.gpa_frame").toStdWString());
        Id clear = 0;
        for (auto &[id, e] : frame.entries())
            if (e.category == 7 && e.type == 0x32) {
                clear = id;
                break;
            }
        QVERIFY(clear);
        Reader r(frame.payload(clear));
        r.skip(16);
        Reader view(frame.payload(r.read<Id>()));
        view.skip(16);
        auto texture = view.read<Id>();
        Experiment project(frame);
        auto original = project.clear(frame, clear);
        auto read = [&] {
            ReplayOptions options;
            options.until = clear;
            project.apply(frame, options);
            Replay replay(frame, options);
            replay.run();
            return replay.output(texture).rgba;
        };
        auto baseline = read();
        project.setClear(frame, clear, Json{{"values", {1., 0., 1., 1.}}});
        auto pixels = read();
        QVERIFY(pixels != baseline);
        for (size_t i = 0; i < pixels.size(); i += 4) {
            QCOMPARE(pixels[i], uint8_t(255));
            QCOMPARE(pixels[i + 1], uint8_t(0));
            QCOMPARE(pixels[i + 2], uint8_t(255));
            QCOMPARE(pixels[i + 3], uint8_t(255));
        }
        QVERIFY(project.undo());
        QCOMPARE(project.clear(frame, clear), original);
        QCOMPARE(read(), baseline);
        QVERIFY(project.redo());
        QCOMPARE(read(), pixels);
    }
};
QTEST_GUILESS_MAIN(CommandTests)
#include "CommandTests.moc"
