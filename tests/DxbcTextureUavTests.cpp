#include "StateCapture.h"
#include "application/DxbcGpuOperations.h"
#include "application/DxbcInputSnapshot.h"
#include "application/DxbcTypedBuffer.h"
#include "application/HlslCompilation.h"
#include "core/Dxbc.h"
#include <QTemporaryDir>
#include <QtTest>
#include <d3d11sdklayers.h>
using namespace flora;
using namespace flora::dxbc_exec;
namespace {
using Words = std::vector<uint32_t>;
Words op(uint32_t opcode, std::initializer_list<Words> args = {}) {
    Words result{opcode};
    for (const auto &arg : args)
        result.insert(result.end(), arg.begin(), arg.end());
    result[0] |= uint32_t(result.size()) << 24;
    return result;
}
std::vector<uint8_t> shader(uint32_t dimension, uint32_t format, bool extended = false, bool alias = false,
                            bool feedback = false) {
    const auto returns = (format == 24 || format == 28 ? 1u : typedBufferFormat(format)->returnType) * 0x1111;
    std::vector<Words> rows{op(89, {{0x208e46, 0, 2}}), op(104, {{feedback ? 2u : 1u}}), op(155, {{1, 1, 1}}),
                            op(156 | (dimension << 11), {{0x11e0f2, 0}, {returns}}),
                            op(158, {{0x11e0f2, 1}, {16}})};
    if (alias)
        rows.push_back(op(156 | (dimension << 11), {{0x11e0f2, 2}, {returns}}));
    auto store = op(164, {{0x11e0f2, 0}, {0x208e46, 0, 0}, {0x208e46, 0, 1}});
    auto load = op(163, {{0x1000f2, 0}, {0x208e46, 0, 0}, {0x11e1b6, alias ? 2u : 0u}});
    if (feedback) {
        load.insert(load.begin() + 3, {0x100012, 1});
        load[0] = 225u | (uint32_t(load.size()) << 24);
    }
    if (extended)
        for (auto *instruction : {&store, &load}) {
            instruction->insert(instruction->begin() + 1,
                                {0x80000002u | (dimension << 6), 3u | (returns << 6)});
            instruction->at(0) =
                (instruction->at(0) & 0x00ffffffu) | 0x80000000u | (uint32_t(instruction->size()) << 24);
        }
    rows.push_back(store);
    rows.push_back(load);
    rows.push_back(op(168, {{0x11e0f2, 1}, {0x4001, 0}, {0x4001, 0}, {0x100e46, 0}}));
    rows.push_back(op(62));
    const auto signature = compileHlsl("[numthreads(1,1,1)]void main(){}", "cs_5_0").bytecode;
    auto parts = readDxbcParts(signature);
    std::erase_if(parts, [](const auto &part) {
        return part.first == 0x58454853 || part.first == 0x52444853 || part.first == 0x54415453;
    });
    const auto executable = writeDxbcProgram({{0x50050, 0}, rows});
    parts.emplace_back(0x58454853, executable);
    return makeDxbc(parts);
}
testing::Capture fixture(uint32_t dimension, uint32_t format, unsigned mode, bool cooperative = false) {
    using namespace testing;
    Capture c;
    c.add(1, 5, 0x127, std::vector<uint8_t>(24));
    Resource resource{};
    const auto layers = dimension == 7 || dimension == 8 ? 3u : 1u;
    resource.type = dimension == 2 || dimension == 7 ? 0x84 : dimension == 5 ? 0x86 : 0x85;
    if (resource.type == 0x84)
        resource.desc = {8, 3, layers, format, 0, 136, 0, 0x80};
    else if (resource.type == 0x86)
        resource.desc = {8, 8, 8, 3, format, 0, 136, 0, 0x80};
    else
        resource.desc = {8, 8, 3, layers, format == 16 ? 15u : format, 1, 0, 0, 136, 0, 0x80};
    auto raw = statePack(Id(0), Id(0));
    for (auto word : resource.desc)
        append(raw, word);
    append(raw, Id(21));
    c.add(20, 5, resource.type, raw);
    const auto layout = textureSubresources(resource);
    std::vector<uint8_t> data(size_t(layout.back().offset + layout.back().size),
                              mode == 5 || mode == 7 ? 0 : 0x11);
    auto blob = word(uint32_t(data.size()));
    blob.insert(blob.end(), data.begin(), data.end());
    c.add(21, 9, 1, blob);
    D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
    view.Format = DXGI_FORMAT(format);
    view.ViewDimension = D3D11_UAV_DIMENSION(dimension == 2   ? 2
                                             : dimension == 3 ? 4
                                             : dimension == 5 ? 8
                                             : dimension == 7 ? 3
                                                              : 5);
    view.Texture2DArray = {1, 1, 2};
    c.add(22, 5, 0x8f, statePack(Id(0), Id(0), Id(20), view));
    std::array<uint32_t, 4> address{1, 1, 1, 0xdeadbeef};
    if (mode == 1)
        address[0] = UINT32_MAX;
    if (mode == 2 || mode == 7) {
        if (dimension == 7)
            address[1] = 2;
        else if (dimension == 5 || dimension == 8)
            address[2] = 2;
        else
            address[0] = 4;
    }
    const auto type = typedBufferFormat(format)->returnType;
    const std::array<uint32_t, 4> value =
        mode == 5 || mode == 7 ? std::array<uint32_t, 4>{}
        : type == 5            ? std::array<uint32_t, 4>{0x3e800000, 0xbf000000, 0x3fa00000, 0x3f400000}
                               : std::array<uint32_t, 4>{0x12345678, 0xfffffffb, 0x89abcdef, 17};
    c.add(2, 5, 0x83, statePack(Id(0), Id(0), D3D11_BUFFER_DESC{32, D3D11_USAGE_DEFAULT, 4, 0, 0, 0}, Id(3)));
    c.add(3, 9, 1, statePack(32u, address, value));
    const auto outputBytes = cooperative ? 32u : 16u;
    c.add(
        10, 5, 0x83,
        statePack(Id(0), Id(0), D3D11_BUFFER_DESC{outputBytes, D3D11_USAGE_DEFAULT, 128, 0, 64, 16}, Id(11)));
    auto outputData = word(outputBytes);
    outputData.resize(4 + outputBytes);
    c.add(11, 9, 1, outputData);
    D3D11_UNORDERED_ACCESS_VIEW_DESC output{};
    output.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    output.Buffer.NumElements = outputBytes / 16;
    c.add(12, 5, 0x8f, statePack(Id(0), Id(0), Id(10), output));
    auto object = std::vector<uint8_t>(48);
    append(object, Id(16));
    c.add(15, 5, 0x93, object);
    const auto binary = cooperative ? compileHlsl(R"(
RWTexture2D<uint> tex : register(u0);
RWStructuredBuffer<uint4> result : register(u1);
[numthreads(2,1,1)] void main(uint3 id : SV_GroupThreadID) {
    tex[uint2(id.x, 1)] = 100 + id.x;
    DeviceMemoryBarrierWithGroupSync();
    result[id.x] = tex[uint2(1 - id.x, 1)];
})",
                                                  "cs_5_0")
                                          .bytecode
                                    : shader(dimension, format, mode == 6);
    auto code = statePack(uint64_t(binary.size()));
    code.insert(code.end(), binary.begin(), binary.end());
    append(code, Id(0));
    c.add(16, 9, 0x81, code);
    State state{};
    state.stages[5].shader = 15;
    state.stages[5].cb[0] = 2;
    if (mode != 4)
        state.csUav[0] = 22;
    state.csUav[1] = 12;
    state.csCount = 2;
    c.add(17, 3, 3, snapshot(state));
    c.add(90, 7, 0x3515, statePack(Id(0), Id(1), Id(20), mode == 3 ? 2.2f : 0.f));
    c.add(100, 7, 0x35, statePack(Id(17), Id(0), Id(1), 1u, 1u, 1u));
    return c;
}
ReplayOptions options(uint32_t vendor, bool before = true) {
    ReplayOptions o;
    o.warp = !vendor;
    if (vendor)
        o.vendor = vendor;
    o.until = 100;
    o.before = before;
    o.debug = true;
    return o;
}
void validate(Replay &replay) {
    Com<ID3D11InfoQueue> q;
    check(replay.nativeDevice()->QueryInterface(IID_PPV_ARGS(&q)), "Read texture UAV diagnostics");
    for (UINT64 i = 0; i < q->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0;
        check(q->GetMessage(i, nullptr, &size), "Read texture UAV diagnostic size");
        std::vector<uint8_t> bytes(size);
        auto *m = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
        check(q->GetMessage(i, m, &size), "Read texture UAV diagnostic");
        if (m->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
            throw std::runtime_error(m->pDescription);
    }
}
} // namespace
class DxbcTextureUavTests final : public QObject {
    Q_OBJECT
  private slots:
    void validityAndTransactions_data() {
        QTest::addColumn<uint32_t>("format");
        QTest::addColumn<unsigned>("inputMask");
        QTest::addColumn<unsigned>("outputMask");
        QTest::newRow("packed-shared-bytes") << 24u << 13u << 1u;
        QTest::newRow("missing-default-components") << 42u << 1u << 15u;
        QTest::newRow("independent-rg-channels") << 16u << 1u << 11u;
    }
    void validityAndTransactions() {
        QFETCH(uint32_t, format);
        QFETCH(unsigned, inputMask);
        QFETCH(unsigned, outputMask);
        QTemporaryDir dir;
        const auto path = dir.filePath("validity.gpa_frame");
        fixture(3, format, 0).save(path);
        Frame frame(path.toStdWString());
        Replay replay(frame, options(0));
        auto snapshot = snapshotResourceInputs(replay, 5);
        auto &textures = *snapshot.inputs.textures;
        const auto before = textures.resources.at(20).storage;
        snapshot.inputs.constantBuffers.at(0).at(1).valid = uint8_t(inputMask);
        const Program program(snapshot.bytecode);
        Lane lane(program, snapshot.inputs);
        const auto step = lane.step();
        QCOMPARE(step.textureChanges.size(), size_t(1));
        QVERIFY(step.memoryChanges.empty());
        QVERIFY(step.memoryGpuAssisted);
        const auto after = textures.resources.at(20).storage;
        lane.step();
        QCOMPARE(lane.value({0, {0}}).valid, uint8_t(outputMask));
        auto invalid = step.textureChanges;
        invalid[0].after[0] ^= 1;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, textures.apply(invalid, false));
        QCOMPARE(textures.resources.at(20).storage.bytes, after.bytes);
        auto duplicate = step.textureChanges;
        duplicate.push_back(duplicate[0]);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, textures.apply(duplicate, false));
        QCOMPARE(textures.resources.at(20).storage.bytes, after.bytes);
        textures.apply(step.textureChanges, false);
        QCOMPARE(textures.resources.at(20).storage.bytes, before.bytes);
        QCOMPARE(textures.resources.at(20).storage.valid, before.valid);
        textures.apply(step.textureChanges);
        QCOMPARE(textures.resources.at(20).storage.bytes, after.bytes);
        QCOMPARE(textures.resources.at(20).storage.valid, after.valid);
        snapshot.inputs.constantBuffers.at(0).at(0).valid = 14;
        Lane unknown(program, snapshot.inputs);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, unknown.step());
        QCOMPARE(textures.resources.at(20).storage.bytes, after.bytes);
        snapshot.inputs.constantBuffers.at(0).at(0).valid = 15;
        textures.views.at({30, {0}}).firstMip = 2;
        Lane inconsistent(program, snapshot.inputs);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, inconsistent.step());
        QCOMPARE(replay.readTexture(20), before.bytes);
        validate(replay);
    }
    void cancellationAndRecovery() {
        QTemporaryDir dir;
        const auto path = dir.filePath("cancel.gpa_frame");
        fixture(3, 42, 0).save(path);
        Frame frame(path.toStdWString());
        Replay replay(frame, options(0));
        auto snapshot = snapshotResourceInputs(replay, 5);
        const auto before = snapshot.inputs.textures->resources.at(20).storage.bytes;
        const Program program(snapshot.bytecode);
        bool cancelled = false;
        snapshot.inputs.gpu =
            std::make_shared<GpuOperations>(replay.nativeDevice(), true, [&] { return cancelled; });
        cancelled = true;
        Lane early(program, snapshot.inputs);
        QVERIFY_THROWS_EXCEPTION(OperationCancelled, early.step());
        QCOMPARE(snapshot.inputs.textures->resources.at(20).storage.bytes, before);
        cancelled = false;
        const auto first = early.step();
        QCOMPARE(first.textureChanges.size(), size_t(1));
        snapshot.inputs.textures->apply(first.textureChanges, false);
        unsigned checks = 0;
        snapshot.inputs.gpu =
            std::make_shared<GpuOperations>(replay.nativeDevice(), true, [&] { return ++checks == 3; });
        Lane submitted(program, snapshot.inputs);
        QVERIFY_THROWS_EXCEPTION(OperationCancelled, submitted.step());
        QCOMPARE(snapshot.inputs.textures->resources.at(20).storage.bytes, before);
        QCOMPARE(snapshot.inputs.gpu->operations(), uint64_t(0));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, submitted.step());
        snapshot.inputs.gpu = std::make_shared<GpuOperations>(replay.nativeDevice(), true);
        Lane recovered(program, snapshot.inputs);
        QCOMPARE(recovered.step().textureChanges.size(), size_t(1));
        QCOMPARE(replay.readTexture(20), before);
        validate(replay);
    }
    void cooperativeAndRollback_data() {
        QTest::addColumn<uint32_t>("vendor");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            QTest::newRow(qPrintable(QString::number(vendor))) << vendor;
    }
    void cooperativeAndRollback() {
        QFETCH(uint32_t, vendor);
        try {
            QTemporaryDir dir;
            const auto path = dir.filePath("cooperative.gpa_frame");
            fixture(3, 42, 0, true).save(path);
            Frame frame(path.toStdWString());
            Replay replay(frame, options(vendor)), reference(frame, options(vendor, false));
            const auto snapshot = snapshotResourceInputs(replay, 5);
            const auto before = replay.readTexture(20);
            reference.run();
            const Program program(snapshot.bytecode);
            Group group(program, snapshot.inputs);
            std::vector<std::vector<MemoryChange>> changes;
            while (!group.finished()) {
                const auto step = group.step();
                if (!step.step.textureChanges.empty())
                    changes.push_back(step.step.textureChanges);
            }
            QCOMPARE(changes.size(), size_t(2));
            QCOMPARE(snapshot.inputs.memory->buffers().at(10).bytes, reference.readBuffer(10));
            auto &textures = *snapshot.inputs.textures;
            QCOMPARE(textures.resources.at(20).storage.bytes, reference.readTexture(20));
            bool assisted = false;
            auto read = [&] {
                return textures.loadUav({30, {0}}, 3, 0x4444, Value::known({0, 1, 0, 0}), {0, 1, 2, 3}, 15,
                                        snapshot.inputs.gpu.get(), assisted);
            };
            QCOMPARE(read().bits[0], 100u);
            for (auto it = changes.rbegin(); it != changes.rend(); ++it)
                textures.apply(*it, false);
            QCOMPARE(textures.resources.at(20).storage.bytes, before);
            QCOMPARE(read().bits[0], 0x11111111u);
            for (const auto &step : changes)
                textures.apply(step);
            QCOMPARE(read().bits[0], 100u);
            QCOMPARE(replay.readTexture(20), before);
            validate(replay);
            validate(reference);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void hardware_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("dimension");
        QTest::addColumn<uint32_t>("format");
        QTest::addColumn<unsigned>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t dimension : {2u, 3u, 5u, 7u, 8u})
                for (uint32_t format : {41u, 42u, 43u, 16u, 28u, 10u, 24u})
                    for (unsigned mode = 0; mode < (dimension == 5 ? 8u : 7u); ++mode)
                        QTest::newRow(qPrintable(
                            QString("%1-%2-%3-%4").arg(vendor).arg(dimension).arg(format).arg(mode)))
                            << vendor << dimension << format << mode;
    }
    void hardware() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, dimension);
        QFETCH(uint32_t, format);
        QFETCH(unsigned, mode);
        try {
            QTemporaryDir dir;
            const auto path = dir.filePath("texture-uav.gpa_frame");
            fixture(dimension, format, mode).save(path);
            Frame frame(path.toStdWString());
            Replay replay(frame, options(vendor)), reference(frame, options(vendor, false));
            const auto snapshot = snapshotResourceInputs(replay, 5);
            const auto before = replay.readTexture(20);
            reference.run();
            const auto expected = reference.readBuffer(10), expectedTexture = reference.readTexture(20);
            snapshot.inputs.textures->views[{30, {2}}] = snapshot.inputs.textures->views.at({30, {0}});
            const Program program(shader(dimension, format, mode == 6, true));
            Group group(program, snapshot.inputs);
            std::vector<MemoryChange> changes;
            while (!group.finished()) {
                const auto step = group.step();
                changes.insert(changes.end(), step.step.textureChanges.begin(),
                               step.step.textureChanges.end());
            }
            QCOMPARE(snapshot.inputs.memory->buffers().at(10).bytes, expected);
            QVERIFY(std::all_of(snapshot.inputs.memory->buffers().at(10).valid.begin(),
                                snapshot.inputs.memory->buffers().at(10).valid.end(),
                                [](uint8_t bit) { return bit == 1; }));
            auto expectedChanges = mode == 4                ? 0u
                                   : mode == 1 || mode == 2 ? unsigned(expectedTexture != before)
                                                            : 1u;
            if (mode == 7) {
                const auto markerPath = dir.filePath("marker.gpa_frame");
                fixture(dimension, format, 2).save(markerPath);
                Frame markerFrame(markerPath.toStdWString());
                Replay markerBefore(markerFrame, options(vendor)),
                    markerAfter(markerFrame, options(vendor, false));
                markerBefore.run();
                markerAfter.run();
                expectedChanges = markerBefore.readTexture(20) != markerAfter.readTexture(20);
                validate(markerBefore);
                validate(markerAfter);
            }
            QCOMPARE(changes.size(), size_t(expectedChanges));
            if (mode != 4) {
                QCOMPARE(snapshot.inputs.textures->resources.size(), size_t(1));
                auto &storage = snapshot.inputs.textures->resources.at(20).storage;
                QCOMPARE(storage.bytes, expectedTexture);
                snapshot.inputs.textures->apply(changes, false);
                QCOMPARE(storage.bytes, before);
                snapshot.inputs.textures->apply(changes);
                QCOMPARE(storage.bytes, expectedTexture);
                if (mode == 5 || (mode == 7 && expectedChanges)) {
                    QCOMPARE(changes.at(0).before, changes.at(0).after);
                    QVERIFY(!changes.at(0).before.empty());
                }
            }
            QCOMPARE(replay.readTexture(20), before);
            auto feedbackInputs = snapshotResourceInputs(replay, 5).inputs;
            feedbackInputs.textures->views[{30, {2}}] = feedbackInputs.textures->views.at({30, {0}});
            const Program feedbackProgram(shader(dimension, format, mode == 6, true, true));
            Group feedbackGroup(feedbackProgram, feedbackInputs);
            while (!feedbackGroup.finished())
                feedbackGroup.step();
            QCOMPARE(feedbackInputs.memory->buffers().at(10).bytes, expected);
            const auto status = feedbackGroup.lane(0).value({0, {1}});
            QCOMPARE(status.mapping[0], mode == 4 ? MappingStatus::Unknown : MappingStatus::Mapped);
            QCOMPARE(status.valid & 1, 0);
            QCOMPARE(replay.readTexture(20), before);
            validate(replay);
            validate(reference);
            qInfo().noquote() << QString::fromStdString(nlohmann::json{
                {"probe", "dxbc-texture-uav"},
                {"vendor", vendor},
                {"dimension", dimension},
                {"format", format},
                {"mode", mode},
                {"frame_sha256", snapshot.frameSha256},
                {"changes", changes.size()},
                {"helper", snapshot.inputs.gpu ? snapshot.inputs.gpu->identity() : nlohmann::json(nullptr)},
                {"passed", true}}.dump());
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
};
QTEST_GUILESS_MAIN(DxbcTextureUavTests)
#include "DxbcTextureUavTests.moc"
