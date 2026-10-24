#include "StateCapture.h"
#include "application/DxbcGpuOperations.h"
#include "application/DxbcInputSnapshot.h"
#include "application/HlslCompilation.h"
#include "core/Dxbc.h"
#include <QTemporaryDir>
#include <QtTest>
#include <d3d11sdklayers.h>
using namespace flora;
using namespace flora::dxbc_exec;
namespace {
using Words = std::vector<uint32_t>;
Words op(uint32_t code, std::initializer_list<Words> args = {}) {
    Words out{code};
    for (const auto &arg : args)
        out.insert(out.end(), arg.begin(), arg.end());
    out[0] |= uint32_t(out.size()) << 24;
    return out;
}
std::vector<uint8_t> shader(uint32_t dimension, bool uav, uint32_t mode, uint32_t stage = 5,
                            uint32_t mask = 15, bool extended = false) {
    const auto resource = 0x100006u | ((uav ? 30u : 7u) << 12) | (0x1b << 4);
    const auto target = stage == 5 ? 0x1000f2u : 0x1020f2u;
    std::vector<Words> rows{
        op(89, {{0x208e46, 0, 1}}),
        op((uav ? 156u : 88u) | (dimension << 11) | ((dimension == 4 || dimension == 9) ? (4u << 16) : 0),
           {{0x1000f2u | ((uav ? 30u : 7u) << 12), 0}, {0x4444}})};
    if (stage == 5) {
        rows.push_back(op(104, {{1}}));
        rows.push_back(op(155, {{1, 1, 1}}));
        rows.push_back(op(158, {{0x11e0f2, 1}, {16}}));
    } else
        rows.push_back(op(101, {{target, 0}}));
    if (mask != 15)
        rows.push_back(op(54, {{target, 0}, {0x4002, 7, 8, 9, 10}}));
    auto query =
        op(61 | (mode << 11), {{(target & ~0xf0u) | (mask << 4), 0}, {0x20800a, 0, 0}, {resource, 0}});
    if (extended) {
        query.insert(query.begin() + 1, {0x80000002u | (dimension << 6), 3u | (0x4444u << 6)});
        query[0] = (query[0] & 0x00ffffffu) | 0x80000000u | (uint32_t(query.size()) << 24);
    }
    rows.push_back(std::move(query));
    if (stage == 5)
        rows.push_back(op(168, {{0x11e0f2, 1}, {0x4001, 0}, {0x4001, 0}, {0x100e46, 0}}));
    rows.push_back(op(62));
    const auto code = writeDxbcProgram({{(stage << 16) | 0x50, 0}, rows});
    const std::map<uint32_t, std::string> names{
        {2, "Texture1D"},      {3, "Texture2D"},        {4, "Texture2DMS"},
        {5, "Texture3D"},      {6, "TextureCube"},      {7, "Texture1DArray"},
        {8, "Texture2DArray"}, {9, "Texture2DMSArray"}, {10, "TextureCubeArray"}};
    const std::string arguments = uav                                                  ? (dimension == 2                     ? "w"
                                                                                          : dimension == 3 || dimension == 7 ? "w,h"
                                                                                                                             : "w,h,d")
                                  : dimension == 4                                     ? "w,h,n"
                                  : dimension == 9                                     ? "w,h,d,n"
                                  : dimension == 2                                     ? "m,w,n"
                                  : dimension == 3 || dimension == 6 || dimension == 7 ? "m,w,h,n"
                                                                                       : "m,w,h,d,n";
    const auto signature =
        compileHlsl("cbuffer Params:register(b0){uint m;}" + std::string(uav ? "RW" : "") +
                        names.at(dimension) + "<uint4> src:register(" + (uav ? "u0" : "t0") +
                        ");RWStructuredBuffer<uint4> output:register(u1);[numthreads(1,1,1)]void main(){"
                        "uint w=0,h=0,d=0,n=1;src.GetDimensions(" +
                        arguments + ");output[0]=uint4(w,h,d,n+m);}",
                    "cs_5_0")
            .bytecode;
    auto parts = readDxbcParts(signature);
    std::erase_if(parts, [](const auto &part) {
        return part.first == 0x58454853 || part.first == 0x52444853 || part.first == 0x54415453;
    });
    parts.emplace_back(0x58454853, code);
    return makeDxbc(parts);
}
Resource resource(uint32_t dimension) {
    Resource r{};
    r.type = dimension == 2 || dimension == 7 ? 0x84 : dimension == 5 ? 0x86 : 0x85;
    const bool ms = dimension == 4 || dimension == 9, cube = dimension == 6 || dimension == 10;
    const auto layers = dimension == 7 || dimension == 8 || dimension == 9 ? 3u
                        : dimension == 6                                   ? 6u
                        : dimension == 10                                  ? 18u
                                                                           : 1u;
    if (r.type == 0x84)
        r.desc = {30, 4, layers, 42, 0, 136, 0, 0x80};
    else if (r.type == 0x86)
        r.desc = {30, 18, 10, 4, 42, 0, 136, 0, 0x80};
    else
        r.desc = {cube ? 18u : 30u,
                  18,
                  ms ? 1u : 4u,
                  layers,
                  42,
                  ms ? 4u : 1u,
                  0,
                  0,
                  ms || cube ? 8u : 136u,
                  0,
                  (cube ? 4u : 0u) | (ms ? 0u : 0x80u)};
    return r;
}
testing::Capture fixture(uint32_t dimension, bool uav, uint32_t mode, unsigned scenario,
                         bool extended = false) {
    using namespace testing;
    Capture c;
    c.add(1, 5, 0x127, std::vector<uint8_t>(24));
    c.buffer(2, 3, 4, 0, {scenario == 1 ? 1u : scenario == 2 ? 99u : 0u, 0, 0, 0});
    const auto r = resource(dimension);
    auto raw = statePack(Id(0), Id(0));
    for (auto v : r.desc)
        append(raw, v);
    append(raw, Id(0));
    c.add(20, 5, r.type, raw);
    if (uav) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format = DXGI_FORMAT_R32_UINT;
        d.ViewDimension = D3D11_UAV_DIMENSION(dimension == 2   ? 2
                                              : dimension == 3 ? 4
                                              : dimension == 5 ? 8
                                              : dimension == 7 ? 3
                                                               : 5);
        d.Texture2DArray = {1, 1, dimension == 5 ? 3u : 2u};
        c.add(22, 5, 0x8f, statePack(Id(0), Id(0), Id(20), d));
    } else {
        D3D11_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = DXGI_FORMAT_R32_UINT;
        const std::map<uint32_t, uint32_t> native{{2, 2}, {3, 4}, {4, 6}, {5, 8},  {6, 9},
                                                  {7, 3}, {8, 5}, {9, 7}, {10, 10}};
        d.ViewDimension = D3D11_SRV_DIMENSION(native.at(dimension));
        d.Texture2DArray = {1, 2, 1, 2};
        if (dimension == 4 || dimension == 9)
            d.Texture2DMSArray = {1, 2};
        if (dimension == 10)
            d.TextureCubeArray = {1, 2, 6, 2};
        c.add(22, 5, 0x8c, statePack(Id(0), Id(0), Id(20), d));
    }
    c.buffer(10, 11, 128, 64, {0, 0, 0, 0});
    D3D11_UNORDERED_ACCESS_VIEW_DESC out{};
    out.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    out.Buffer.NumElements = 1;
    c.add(12, 5, 0x8f, statePack(Id(0), Id(0), Id(10), out));
    // Capture::buffer has a four-byte stride; the result needs one uint4 element.
    std::erase_if(c.entries, [](const auto &entry) { return entry.id == 10; });
    c.add(10, 5, 0x83,
          statePack(Id(0), Id(0), D3D11_BUFFER_DESC{16, D3D11_USAGE_DEFAULT, 128, 0, 64, 16}, Id(11)));
    auto object = std::vector<uint8_t>(48);
    append(object, Id(16));
    c.add(15, 5, 0x93, object);
    const auto binary = shader(dimension, uav, mode, 5, 15, extended);
    auto code = statePack(uint64_t(binary.size()));
    code.insert(code.end(), binary.begin(), binary.end());
    append(code, Id(0));
    c.add(16, 9, 0x81, code);
    State state{};
    state.stages[5].shader = 15;
    state.stages[5].cb[0] = 2;
    if (scenario != 3)
        (uav ? state.csUav[0] : state.stages[5].srv[0]) = 22;
    state.csUav[1] = 12;
    state.csCount = 2;
    c.add(17, 3, 3, snapshot(state));
    if (dimension != 4 && dimension != 9)
        c.add(90, 7, 0x3515, statePack(Id(0), Id(1), Id(20), scenario == 4 ? 2.2f : 0.f));
    c.add(100, 7, 0x35, statePack(Id(17), Id(0), Id(1), 1u, 1u, 1u));
    return c;
}
ReplayOptions options(uint32_t vendor, bool before) {
    ReplayOptions o;
    o.warp = !vendor;
    if (vendor)
        o.vendor = vendor;
    o.before = before;
    o.until = 100;
    o.debug = true;
    return o;
}
void validate(Replay &r) {
    Com<ID3D11InfoQueue> q;
    check(r.nativeDevice()->QueryInterface(IID_PPV_ARGS(&q)), "Read resource query diagnostics");
    for (UINT64 i = 0; i < q->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0;
        q->GetMessage(i, nullptr, &size);
        std::vector<uint8_t> bytes(size);
        auto *m = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
        check(q->GetMessage(i, m, &size), "Read resource query message");
        if (m->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
            throw std::runtime_error(m->pDescription);
    }
}
} // namespace
class DxbcResourceInfoTests final : public QObject {
    Q_OBJECT
  private slots:
    void extendedDeclarations_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<bool>("uav");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (bool uav : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(uav))) << vendor << uav;
    }
    void extendedDeclarations() {
        QFETCH(uint32_t, vendor);
        QFETCH(bool, uav);
        QTemporaryDir dir;
        const auto path = dir.filePath("extended.gpa_frame");
        fixture(3, uav, 2, 0, true).save(path);
        Frame frame(path.toStdWString());
        Replay replay(frame, options(vendor, true)), reference(frame, options(vendor, false));
        const auto snapshot = snapshotResourceInputs(replay, 5);
        reference.run();
        const auto bytes = reference.readBuffer(10);
        std::array<uint32_t, 4> expected;
        std::memcpy(expected.data(), bytes.data(), 16);
        for (uint32_t stage : {0u, 1u, 5u}) {
            if (uav && stage != 5)
                continue;
            const auto binary = shader(3, uav, 2, stage, 15, true);
            const Program program(binary);
            Lane lane(program, snapshot.inputs);
            lane.step();
            QCOMPARE(lane.value({stage == 5 ? 0u : 2u, {0}}), Value::known(expected));
            for (unsigned extension : {1u, 2u}) {
                auto parts = readDxbcParts(binary);
                auto executable = std::find_if(parts.begin(), parts.end(),
                                               [](const auto &part) { return part.first == 0x58454853; });
                auto code = readDxbcProgram(executable->second);
                for (auto &row : code.instructions)
                    if ((row[0] & 0x7ff) == 61)
                        row.at(extension) ^= 1u << 6;
                const auto changed = writeDxbcProgram(code);
                executable->second = changed;
                const Program corrupt(makeDxbc(parts));
                Lane rejected(corrupt, snapshot.inputs);
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, rejected.step());
            }
        }
        validate(replay);
        validate(reference);
    }
    void nativeHlslReference_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<bool>("volume");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (bool volume : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(volume))) << vendor << volume;
    }
    void nativeHlslReference() {
        QFETCH(uint32_t, vendor);
        QFETCH(bool, volume);
        QTemporaryDir dir;
        const auto assembledPath = dir.filePath("assembled.gpa_frame");
        const auto compiledPath = dir.filePath("compiled.gpa_frame");
        auto capture = fixture(volume ? 5 : 3, volume, 2, volume ? 0 : 2);
        capture.save(assembledPath);
        const auto bytecode =
            compileHlsl(
                std::string("cbuffer Params:register(b0){uint m;}") +
                    (volume ? "RWTexture3D<uint4> src:register(u0);" : "Texture2D<uint4> src:register(t0);") +
                    "RWStructuredBuffer<uint4> output:register(u1);[numthreads(1,1,1)]void main(){"
                    "uint w=0,h=0,d=0,n=1;" +
                    (volume ? "src.GetDimensions(w,h,d);" : "src.GetDimensions(m,w,h,n);") +
                    "output[0]=uint4(n,d,h,w);}",
                "cs_5_0")
                .bytecode;
        std::erase_if(capture.entries, [](const auto &entry) { return entry.id == 16; });
        auto code = testing::statePack(uint64_t(bytecode.size()));
        code.insert(code.end(), bytecode.begin(), bytecode.end());
        testing::append(code, Id(0));
        capture.add(16, 9, 0x81, code);
        capture.save(compiledPath);
        Frame assembledFrame(assembledPath.toStdWString()), compiledFrame(compiledPath.toStdWString());
        Replay assembled(assembledFrame, options(vendor, false)),
            compiled(compiledFrame, options(vendor, false));
        assembled.run();
        compiled.run();
        const auto expected = compiled.readBuffer(10);
        QCOMPARE(assembled.readBuffer(10), expected);
        validate(assembled);
        validate(compiled);
        std::array<uint32_t, 4> words;
        std::memcpy(words.data(), expected.data(), 16);
        qInfo().noquote() << QString::fromStdString(nlohmann::json{
            {"probe", "native-hlsl-resource-query"},
            {"vendor", vendor},
            {"volume", volume},
            {"words", words},
            {"passed", true}}.dump());
    }
    void validityAndSelection() {
        Inputs inputs;
        inputs.textures = std::make_shared<TextureInputs>();
        inputs.constantBuffers[0] = {Value::known({})};
        TextureInputView view;
        view.resource = 20;
        view.format = 42;
        view.dimension = 4;
        view.firstMip = 1;
        view.mipCount = 2;
        view.nativeDescriptor = {42, 4, 1, 2, 0, 0};
        inputs.textures->views[{7, {0}}] = view;
        inputs.textures->metadata[20] = {resource(3), 0};
        const Program program(shader(3, false, 2));
        Lane valid(program, inputs);
        const auto step = valid.step();
        QCOMPARE(valid.value({0, {0}}), Value::known({2, 0, 9, 15}));
        QCOMPARE(step.changes.at(0).before.valid, uint8_t(0));
        QCOMPARE(step.changes.at(0).after, valid.value({0, {0}}));
        inputs.constantBuffers[0][0].valid = 14;
        Lane unknown(program, inputs);
        unknown.step();
        QCOMPARE(unknown.value({0, {0}}).valid, uint8_t(3));
        QCOMPARE(unknown.value({0, {0}}).bits[0], 2u);
        inputs.constantBuffers[0][0] = Value::known({});
        const Program masked(shader(3, false, 2, 5, 9));
        Lane partial(masked, inputs);
        partial.step();
        partial.step();
        QCOMPARE(partial.value({0, {0}}), Value::known({2, 8, 9, 15}));
        inputs.textures->metadata.clear();
        Lane missing(program, inputs);
        missing.step();
        QCOMPARE(missing.value({0, {0}}).valid, uint8_t(0));
        inputs.textures->views[{7, {0}}] = {};
        Lane unbound(program, inputs);
        unbound.step();
        QCOMPARE(unbound.value({0, {0}}), Value::known({}));
        inputs.textures->views.clear();
        Lane unobserved(program, inputs);
        unobserved.step();
        QCOMPARE(unobserved.value({0, {0}}).valid, uint8_t(0));
        inputs.textures->views[{7, {0}}] = view;
        inputs.textures->views[{7, {0}}].firstMip = 2;
        inputs.textures->metadata[20] = {resource(3), 0};
        Lane corrupt(program, inputs);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, corrupt.step());
    }
    void missingBindingsValidateOperands() {
        Inputs inputs;
        inputs.constantBuffers[0] = {Value::known({})};
        const Program reserved(shader(3, false, 3));
        Lane invalid(reserved, inputs);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, invalid.step());
        const Program valid(shader(3, false, 2));
        Lane missing(valid, inputs);
        missing.step();
        QCOMPARE(missing.value({0, {0}}).valid, uint8_t(0));
    }
    void gpuQueryCacheAndCancellation() {
        QTemporaryDir dir;
        const auto path = dir.filePath("query-cancel.gpa_frame");
        fixture(5, true, 2, 0).save(path);
        Frame frame(path.toStdWString());
        Replay replay(frame, options(0, true));
        auto snapshot = snapshotResourceInputs(replay, 5);
        const Program program(snapshot.bytecode);
        bool cancelled = false;
        snapshot.inputs.gpu =
            std::make_shared<GpuOperations>(replay.nativeDevice(), true, [&] { return cancelled; });
        Lane first(program, snapshot.inputs);
        QVERIFY(first.step().memoryGpuAssisted);
        QCOMPARE(snapshot.inputs.gpu->operations(), uint64_t(1));
        Lane cached(program, snapshot.inputs);
        QVERIFY(cached.step().memoryGpuAssisted);
        QCOMPARE(cached.value({0, {0}}), first.value({0, {0}}));
        QCOMPARE(snapshot.inputs.gpu->operations(), uint64_t(1));
        cancelled = true;
        Lane stopped(program, snapshot.inputs);
        QVERIFY_THROWS_EXCEPTION(OperationCancelled, stopped.step());
        QCOMPARE(stopped.value({0, {0}}).valid, uint8_t(0));
        cancelled = false;
        QVERIFY(stopped.step().memoryGpuAssisted);
        unsigned checks = 0;
        snapshot.inputs.gpu =
            std::make_shared<GpuOperations>(replay.nativeDevice(), true, [&] { return ++checks == 3; });
        Lane submitted(program, snapshot.inputs);
        QVERIFY_THROWS_EXCEPTION(OperationCancelled, submitted.step());
        QCOMPARE(submitted.value({0, {0}}).valid, uint8_t(0));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, submitted.step());
        snapshot.inputs.gpu = std::make_shared<GpuOperations>(replay.nativeDevice(), true);
        Lane recovered(program, snapshot.inputs);
        QVERIFY(recovered.step().memoryGpuAssisted);
        QCOMPARE(recovered.value({0, {0}}), first.value({0, {0}}));
        validate(replay);
    }
    void hardware_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("dimension");
        QTest::addColumn<bool>("uav");
        QTest::addColumn<uint32_t>("mode");
        QTest::addColumn<unsigned>("scenario");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (bool uav : {false, true})
                for (uint32_t dimension = 2; dimension <= 10; ++dimension) {
                    if (uav && (dimension == 4 || dimension == 6 || dimension >= 9))
                        continue;
                    for (uint32_t mode = 0; mode < 3; ++mode)
                        for (unsigned scenario = 0; scenario < 5; ++scenario) {
                            if (scenario == 4 && (dimension == 4 || dimension == 9))
                                continue;
                            QTest::newRow(qPrintable(QString("%1-%2-%3-%4-%5")
                                                         .arg(vendor)
                                                         .arg(dimension)
                                                         .arg(uav)
                                                         .arg(mode)
                                                         .arg(scenario)))
                                << vendor << dimension << uav << mode << scenario;
                        }
                }
    }
    void hardware() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, dimension);
        QFETCH(bool, uav);
        QFETCH(uint32_t, mode);
        QFETCH(unsigned, scenario);
        try {
            QTemporaryDir dir;
            const auto path = dir.filePath("resource-query.gpa_frame");
            fixture(dimension, uav, mode, scenario).save(path);
            Frame frame(path.toStdWString());
            Replay replay(frame, options(vendor, true));
            const auto snapshot = snapshotResourceInputs(replay, 5);
            Replay reference(frame, options(vendor, false));
            reference.run();
            const auto expectedBytes = reference.readBuffer(10);
            std::array<uint32_t, 4> expected;
            std::memcpy(expected.data(), expectedBytes.data(), 16);
            uint8_t known = 15;
            if (scenario == 4 && !uav)
                known = dimension == 2 ? 7 : dimension == 5 || dimension >= 8 ? 1 : 3;
            if (dimension == 10 && scenario != 3)
                known &= ~2u;
            for (uint32_t stage : {0u, 1u, 5u}) {
                if (uav && stage != 5)
                    continue;
                const Program program(shader(dimension, uav, mode, stage));
                Lane lane(program, snapshot.inputs);
                const auto step = lane.step();
                const bool assisted =
                    scenario != 3 &&
                    (mode == 1 || scenario == 2 ||
                     (scenario == 1 && (uav || dimension == 4 || dimension == 9)) || (uav && dimension == 5));
                QCOMPARE(step.memoryGpuAssisted, assisted);
                const auto value = lane.value({stage == 5 ? 0u : 2u, {0}});
                QCOMPARE(value.valid, known);
                for (unsigned c = 0; c < 4; ++c)
                    if (known & (1u << c))
                        QCOMPARE(value.bits[c], expected[c]);
            }
            const Program program(snapshot.bytecode);
            Group group(program, snapshot.inputs);
            while (!group.finished())
                group.step();
            const auto &output = snapshot.inputs.memory->buffers().at(10);
            for (size_t i = 0; i < 16; ++i) {
                QCOMPARE(output.valid[i] != 0, bool(known & (1u << (i / 4))));
                if (output.valid[i])
                    QCOMPARE(output.bytes[i], expectedBytes[i]);
            }
            if (scenario != 3) {
                QVERIFY(snapshot.inputs.textures->metadata.contains(20));
                const auto native = replay.observeTextureMetadata(20);
                QCOMPARE(native.resource.desc, snapshot.inputs.textures->metadata.at(20).resource.desc);
                QCOMPARE(native.minLod, snapshot.inputs.textures->metadata.at(20).minLod);
                if (dimension == 4 || dimension == 9)
                    QVERIFY(snapshot.inputs.textures->resources.empty());
                else
                    QCOMPARE(replay.readTexture(20),
                             snapshot.inputs.textures->resources.at(20).storage.bytes);
            }
            validate(replay);
            validate(reference);
            qInfo().noquote() << QString::fromStdString(nlohmann::json{
                {"probe", "dxbc-resource-info"},
                {"vendor", vendor},
                {"dimension", dimension},
                {"uav", uav},
                {"mode", mode},
                {"scenario", scenario},
                {"expected", expected},
                {"valid_mask", known},
                {"frame_sha256", snapshot.frameSha256},
                {"adapter_vendor", snapshot.adapterVendor},
                {"adapter_device", snapshot.adapterDevice},
                {"adapter_luid", snapshot.adapterLuid},
                {"passed", true}}.dump());
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
};
QTEST_GUILESS_MAIN(DxbcResourceInfoTests)
#include "DxbcResourceInfoTests.moc"
