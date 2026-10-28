#include "StateCapture.h"
#include "application/DxbcExecutionTraceReader.h"
#include "application/DxbcTypedBuffer.h"
#include "application/HlslCompilation.h"
#include "application/NativePixelDrawHistory.h"
#include "application/NativePixelGeometryReplay.h"
#include "application/NativePixelOutputs.h"
#include <QDir>
#include <QTemporaryDir>
#include <QtTest>
#include <bit>
using namespace flora;
using namespace flora::testing;
using namespace flora::dxbc_exec;
using Json = nlohmann::json;
namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
void save(const Json &report) {
    const auto root = qEnvironmentVariable("FLORA_PIXEL_CHECKPOINT_INPUTS_EVIDENCE");
    if (root.isEmpty())
        return;
    require(QDir().mkpath(root), "Cannot create checkpoint input evidence directory");
    QFile file(QDir(root).filePath(QString::fromLatin1(QTest::currentTestFunction()) + "-" +
                                   QString::fromLatin1(QTest::currentDataTag()) + ".json"));
    const auto text = report.dump(2);
    require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
                file.write(text.data(), qint64(text.size())) == qint64(text.size()),
            "Cannot save checkpoint input evidence");
}
void count(Capture &c, uint32_t value) {
    for (const auto &e : c.entries)
        if (e.id == 200)
            put(c.bytes, size_t(e.offset) + 24, value * 3);
}
Capture fixture(const QString &mode) {
    auto c = graphicsCounterCapture(mode != "append");
    const bool texture = mode == "texture";
    std::erase_if(c.entries, [](const auto &e) {
        return e.id == 20 || e.id == 21 || e.id == 10 || e.id == 11 || e.id == 12;
    });
    auto addTexture = [&](Id id, Id data, DXGI_FORMAT format, uint32_t bind) {
        std::vector<uint8_t> resource(16);
        append(resource, D3D11_TEXTURE2D_DESC{1, 1, 1, 1, format, {1, 0}, D3D11_USAGE_DEFAULT, bind, 0, 0});
        append(resource, data);
        c.add(id, 5, 0x85, resource);
        const auto size = format == DXGI_FORMAT_R32_UINT ? 4u : 16u;
        auto bytes = word(size);
        bytes.resize(size + 4);
        c.add(data, 9, 1, bytes);
    };
    addTexture(20, 400, DXGI_FORMAT_R32G32B32A32_UINT, D3D11_BIND_RENDER_TARGET);
    std::vector<uint8_t> view(16);
    append(view, Id(20));
    D3D11_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = DXGI_FORMAT_R32G32B32A32_UINT;
    rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    append(view, rtv);
    c.add(21, 5, 0x8d, view);
    if (texture)
        addTexture(10, 11, DXGI_FORMAT_R32_UINT, D3D11_BIND_UNORDERED_ACCESS);
    else {
        c.buffer(10, 11, D3D11_BIND_UNORDERED_ACCESS,
                 mode == "raw"     ? D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS
                 : mode == "typed" ? 0
                                   : D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                 {});
        if (mode == "raw")
            for (const auto &e : c.entries)
                if (e.id == 10)
                    put(c.bytes, size_t(e.offset) + 36, 0u);
    }
    view.assign(16, 0);
    append(view, Id(10));
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = mode == "raw"                ? DXGI_FORMAT_R32_TYPELESS
                 : texture || mode == "typed" ? DXGI_FORMAT_R32_UINT
                                              : DXGI_FORMAT_UNKNOWN;
    uav.ViewDimension = texture ? D3D11_UAV_DIMENSION_TEXTURE2D : D3D11_UAV_DIMENSION_BUFFER;
    if (!texture) {
        uav.Buffer.NumElements = 4;
        uav.Buffer.Flags = mode == "counter"  ? D3D11_BUFFER_UAV_FLAG_COUNTER
                           : mode == "append" ? D3D11_BUFFER_UAV_FLAG_APPEND
                           : mode == "raw"    ? D3D11_BUFFER_UAV_FLAG_RAW
                                              : 0;
    }
    append(view, uav);
    c.add(12, 5, 0x8f, view);
    count(c, 3);
    return c;
}
ReplayOptions options(uint32_t vendor, const QString &mode) {
    ReplayOptions o;
    o.until = 200;
    o.warp = !vendor;
    if (vendor)
        o.vendor = vendor;
    o.debug = true;
    o.shaders[30] = compileHlsl("float4 main(uint id:SV_VertexID):SV_Position{uint i=id%3;"
                                "return float4(i==2?3:-1,i==1?3:-1,0,1);}",
                                "vs_5_0", "main")
                        .bytecode;
    const auto declaration = mode == "counter"  ? "RWStructuredBuffer<uint>"
                             : mode == "append" ? "AppendStructuredBuffer<uint>"
                             : mode == "raw"    ? "RWByteAddressBuffer"
                             : mode == "typed"  ? "RWBuffer<uint>"
                                                : "RWTexture2D<uint>";
    const auto body = mode == "counter"  ? "uint i=target.IncrementCounter();target[i]=i+11;"
                      : mode == "append" ? "uint i=7;target.Append(i);"
                      : mode == "raw"    ? "uint i;target.InterlockedAdd(0,1,i);"
                      : mode == "typed"  ? "uint i;InterlockedAdd(target[0],1,i);"
                                         : "uint i;InterlockedAdd(target[uint2(0,0)],1,i);";
    o.shaders[32] = compileHlsl(std::string(declaration) +
                                    " target:register(u1);"
                                    "uint4 main():SV_Target{" +
                                    body + "return uint4(i,11,17,23);}",
                                "ps_5_0", "main")
                        .bytecode;
    return o;
}
std::vector<uint8_t> storage(Replay &r, const QString &mode) {
    return mode == "texture" ? r.readTexture(10) : r.readBuffer(10);
}
std::vector<uint8_t> snapshotStorage(const InputSnapshot &s, const QString &mode) {
    return mode == "texture" ? s.inputs.textures->resources.at(10).storage.bytes
                             : s.inputs.memory->buffers().at(10).bytes;
}
std::vector<uintptr_t> bindings(Replay &r) {
    std::vector<uintptr_t> result;
    r.inspectNativeState([&](auto *context, const auto &) {
        Com<ID3D11PixelShader> ps;
        Com<ID3D11UnorderedAccessView> uav;
        Com<ID3D11RenderTargetView> rtv;
        Com<ID3D11Predicate> predicate;
        BOOL value = FALSE;
        context->PSGetShader(&ps, nullptr, nullptr);
        context->OMGetRenderTargetsAndUnorderedAccessViews(1, rtv.GetAddressOf(), nullptr, 1, 1,
                                                           uav.GetAddressOf());
        context->GetPredication(&predicate, &value);
        result = {uintptr_t(ps.Get()), uintptr_t(uav.Get()), uintptr_t(rtv.Get()), uintptr_t(predicate.Get()),
                  uintptr_t(value)};
    });
    return result;
}
void devices() {
    QTest::addColumn<uint32_t>("vendor");
    for (uint32_t vendor : {0u, 4318u, 32902u})
        QTest::newRow(qPrintable(QString::number(vendor))) << vendor;
}
Capture textureRegion(uint32_t dimension, uint32_t format) {
    auto c = fixture("texture");
    count(c, 1);
    std::erase_if(c.entries, [](const auto &e) { return e.id == 10 || e.id == 11 || e.id == 12; });
    Resource resource{};
    const auto layers = dimension == 7 || dimension == 8 ? 3u : 1u;
    resource.type = dimension == 2 || dimension == 7 ? 0x84 : dimension == 5 ? 0x86 : 0x85;
    if (resource.type == 0x84)
        resource.desc = {8, 3, layers, format, 0, 128, 0, 0};
    else if (resource.type == 0x86)
        resource.desc = {8, 8, 8, 3, format, 0, 128, 0, 0};
    else
        resource.desc = {8, 8, 3, layers, format, 1, 0, 0, 128, 0, 0};
    auto raw = statePack(Id(0), Id(0));
    for (const auto word : resource.desc)
        append(raw, word);
    append(raw, Id(11));
    c.add(10, 5, resource.type, raw);
    const auto layout = textureSubresources(resource);
    const auto bytes = uint32_t(layout.back().offset + layout.back().size);
    auto data = word(bytes);
    data.resize(bytes + 4, 0x11);
    c.add(11, 9, 1, data);
    D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
    view.Format = DXGI_FORMAT(format);
    view.ViewDimension = D3D11_UAV_DIMENSION(dimension == 2   ? 2
                                             : dimension == 3 ? 4
                                             : dimension == 5 ? 8
                                             : dimension == 7 ? 3
                                                              : 5);
    view.Texture2DArray = {1, 1, 2};
    c.add(12, 5, 0x8f, statePack(Id(0), Id(0), Id(10), view));
    return c;
}
} // namespace
class NativePixelCheckpointInputsTests final : public QObject {
    Q_OBJECT
  private slots:
    void typedPixelReadWrite_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("dimension");
        QTest::addColumn<uint32_t>("format");
        QTest::addColumn<uint32_t>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t dimension : {1u, 2u, 3u, 5u, 7u, 8u})
                for (uint32_t format : {42u, 43u, 41u, 28u, 31u, 10u})
                    for (uint32_t mode : {0u, 1u, 2u})
                        QTest::newRow(qPrintable(
                            QString("%1-%2-%3-%4").arg(vendor).arg(dimension).arg(format).arg(mode)))
                            << vendor << dimension << format << mode;
    }
    void typedPixelReadWrite() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, dimension);
        QFETCH(uint32_t, format);
        QFETCH(uint32_t, mode);
        try {
            const QString kind = dimension == 1 ? "typed" : "texture";
            auto c = dimension == 1 ? fixture("typed") : textureRegion(dimension, format);
            count(c, 1);
            const auto layout = typedBufferFormat(format).value();
            if (dimension == 1) {
                std::erase_if(c.entries,
                              [](const auto &e) { return e.id == 10 || e.id == 11 || e.id == 12; });
                c.add(10, 5, 0x83,
                      statePack(
                          Id(0), Id(0),
                          D3D11_BUFFER_DESC{64, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, 0, 0},
                          Id(11)));
                auto data = word(64);
                data.resize(68, 0x11);
                c.add(11, 9, 1, data);
                D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
                view.Format = DXGI_FORMAT(format);
                view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
                view.Buffer = {1, 2, 0};
                c.add(12, 5, 0x8f, statePack(Id(0), Id(0), Id(10), view));
            }
            std::array<uint32_t, 3> address{dimension == 1 ? 0u : 1u, 1, 1};
            if (mode == 1)
                address[0] = UINT32_MAX;
            if (mode == 2) {
                if (dimension == 7)
                    address[1] = 2;
                else if (dimension == 5 || dimension == 8)
                    address[2] = 2;
                else
                    address[0] = dimension == 1 ? 2 : 4;
            }
            const auto components = dimension == 1 || dimension == 2   ? 1u
                                    : dimension == 3 || dimension == 7 ? 2u
                                                                       : 3u;
            std::string coordinate =
                "uint" + (components == 1 ? std::string() : std::to_string(components)) + "(";
            for (uint32_t i = 0; i < components; ++i)
                coordinate += (i ? "," : "") + std::to_string(address[i]) + "u";
            coordinate += ")";
            const auto shape = dimension == 1   ? "RWBuffer"
                               : dimension == 2 ? "RWTexture1D"
                               : dimension == 3 ? "RWTexture2D"
                               : dimension == 5 ? "RWTexture3D"
                               : dimension == 7 ? "RWTexture1DArray"
                                                : "RWTexture2DArray";
            const auto type = format == 28             ? "unorm float4"
                              : format == 31           ? "snorm float4"
                              : layout.returnType == 4 ? "uint4"
                              : layout.returnType == 3 ? "int4"
                                                       : "float4";
            const auto value = layout.returnType == 4   ? "uint4(11,17,23,29)"
                               : layout.returnType == 3 ? "int4(-11,17,-23,29)"
                                                        : "float4(.25,.5,.75,1)";
            auto o = options(vendor, kind);
            o.shaders[32] =
                compileHlsl(std::string(shape) + "<" + type +
                                "> target:register(u1);uint4 main():SV_Target{target[" + coordinate + "]=" +
                                value + ";DeviceMemoryBarrier();return asuint(target[" + coordinate + "]);}",
                            "ps_5_0")
                    .bytecode;
            QTemporaryDir dir;
            const auto path = dir.filePath("typed.gpa_frame");
            c.save(path);
            Frame f(path.toStdWString());
            Replay reference(f, o);
            reference.run();
            const auto native = storage(reference, kind), color = reference.readTexture(20);
            o.before = o.prepareBeforeDraw = true;
            Replay r(f, o);
            r.run();
            const auto initial = storage(r, kind);
            const auto bound = bindings(r);
            auto snap = snapshotBoundResourceInputs(r, 4);
            Program program(snap.bytecode);
            std::array<Inputs, 4> inputs;
            inputs.fill(snap.inputs);
            PixelQuad quad(program, inputs, 1);
            Json changes = Json::array();
            uint32_t loads = 0, stores = 0;
            while (!quad.finished()) {
                const auto step = quad.step();
                const auto opcode = program.instructions()[step.step.instruction].opcode;
                if (opcode == 163 || opcode == 164) {
                    if (step.helper) {
                        QVERIFY(!step.step.memoryGpuAssisted);
                        QVERIFY(step.step.memoryChanges.empty());
                        QVERIFY(step.step.textureChanges.empty());
                    } else {
                        QVERIFY(step.step.memoryGpuAssisted);
                        loads += opcode == 163;
                        stores += opcode == 164;
                    }
                    changes.push_back(
                        executionTraceStep(GroupStep{step.sequence, 0, step.lane, step.step, false}));
                }
            }
            QCOMPARE(loads, 1u);
            QCOMPARE(stores, 1u);
            const auto actual = quad.lane(0).value({2, {0}});
            QCOMPARE(actual.valid, uint8_t(15));
            std::array<uint32_t, 4> expected{};
            QCOMPARE(color.size(), size_t(16));
            std::memcpy(expected.data(), color.data(), 16);
            QCOMPARE(actual.bits, expected);
            QCOMPARE(snapshotStorage(snap, kind), native);
            for (unsigned lane = 1; lane < 4; ++lane)
                QCOMPARE(quad.lane(lane).value({2, {0}}).valid, uint8_t(0));
            QCOMPARE(storage(r, kind), initial);
            QCOMPARE(bindings(r), bound);
            // Export and traverse the same instruction/resource journal in both directions.
            PixelTraceOptions trace;
            trace.activeMask = 1;
            trace.selectedLane = 0;
            trace.inputProvenance = {{"source", "explicit-one-covered-lane"}};
            const auto journal = dir.filePath("trace.jsonl");
            const auto fresh = snapshotBoundResourceInputs(r, 4);
            exportPixelExecutionTrace(journal, fresh, trace);
            QFile file(journal);
            require(file.open(QIODevice::ReadOnly), "Cannot read typed pixel journal");
            const auto bytes = file.readAll();
            file.close();
            ExecutionTraceReader reader(journal, sha256(Bytes(reinterpret_cast<const uint8_t *>(bytes.data()),
                                                              size_t(bytes.size()))));
            reader.moveTo(reader.size());
            QCOMPARE(reader.registerValue(0, {2, {0}}), actual);
            reader.moveTo(0);
            reader.moveTo(reader.size());
            QCOMPARE(reader.registerValue(0, {2, {0}}), actual);
            save({{"schema", 1},
                  {"status", "ok"},
                  {"vendor", vendor},
                  {"dimension", dimension},
                  {"format", format},
                  {"mode", mode},
                  {"address", address},
                  {"gpu_storage", native},
                  {"gpu_color", color},
                  {"raw_bits", actual.bits},
                  {"valid_mask", actual.valid},
                  {"transactions", changes},
                  {"helper_accesses_suppressed", true},
                  {"journal_roundtrip", true},
                  {"original_storage_restored", true}});
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }

    void shaderHistory_data() { prefixes_data(); }
    void shaderHistory() {
        QFETCH(uint32_t, vendor);
        QFETCH(QString, mode);
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.gpa_frame");
        fixture(mode).save(path);
        Frame f(path.toStdWString());
        auto o = options(vendor, mode);
        o.before = o.prepareBeforeDraw = true;
        Replay r(f, o);
        r.run();
        const auto original = storage(r, mode), color = r.readTexture(20);
        const auto bound = bindings(r);
        const bool counted = mode == "counter" || mode == "append";
        const auto counter = counted ? r.readCounter(12) : 0;
        NativeHistorySelection selection;
        selection.resource = 20;
        NativePixelDrawHistoryLimits limits;
        limits.evaluateShaderOutputs = true;
        limits.evaluateOutputMerger = true;
        auto report = collectNativePixelDrawHistory(r, 200, selection, limits);
        QCOMPARE(report.at("merger_evaluation").at("invocations"), Json(3));
        QCOMPARE(report.at("records").size(), size_t(3));
        for (uint32_t i = 0; i < 3; ++i) {
            const auto &row = report.at("records")[i];
            QCOMPARE(row.at("private_output_mergers").size(), size_t(1));
            QCOMPARE(row.at("private_output_mergers")[0].at("observed_value_matches_checkpoint"), Json(true));
            QCOMPARE(row.at("private_output_mergers")[0].at("exact_storage_matches_checkpoint"), Json(true));
            QCOMPARE(row.at("private_shader_outputs").size(), size_t(1));
            const auto &out = row.at("private_shader_outputs")[0];
            QCOMPARE(out.at("completed_primitives"), Json(i));
            QCOMPARE(out.at("resource_observation"), Json("current_native_bindings"));
            const auto &value = out.at("lanes")[out.at("selected_lane").get<size_t>()].at("outputs")[0];
            const std::array<uint32_t, 4> expected{mode == "append" ? 7u : i, 11, 17, 23};
            QCOMPARE(value.at("valid_mask"), Json(15));
            QCOMPARE((value.at("raw_bits").get<std::array<uint32_t, 4>>()), expected);
            const auto bytes = row.at("after").at("bytes").get<std::vector<uint8_t>>();
            std::array<uint32_t, 4> native{};
            QCOMPARE(bytes.size(), size_t(16));
            std::memcpy(native.data(), bytes.data(), 16);
            QCOMPARE(native, expected);
            QVERIFY(out.at("input_provenance").at("coverage_only").get<bool>());
        }
        // A single draw does not serialize atomic allocation by primitive order.
        auto words = [&](const std::vector<uint8_t> &bytes) {
            require(bytes.size() == 16, "Original atomic output size");
            std::array<uint32_t, 4> result{};
            std::memcpy(result.data(), bytes.data(), 16);
            require((mode == "append" ? result[0] == 7 : result[0] < 3) && result[1] == 11 &&
                        result[2] == 17 && result[3] == 23,
                    "Original atomic output constraints");
            return result;
        };
        const auto observed =
            words(report.at("original_draw_observation").at("after").at("bytes").get<std::vector<uint8_t>>());
        o.before = o.prepareBeforeDraw = false;
        o.measurement = ReplayOptions::Measurement{200, 200, true};
        Replay reference(f, o);
        reference.run();
        const auto referenceWords = words(reference.readTexture(20));
        const std::array<uint32_t, 4> expectedWords =
            mode == "counter"  ? std::array<uint32_t, 4>{11, 12, 13, 0}
            : mode == "append" ? std::array<uint32_t, 4>{7, 7, 7, 0}
                               : std::array<uint32_t, 4>{3, 0, 0, 0};
        std::vector<uint8_t> expectedStorage(original.size());
        QVERIFY(expectedStorage.size() <= sizeof expectedWords);
        std::memcpy(expectedStorage.data(), expectedWords.data(), expectedStorage.size());
        QCOMPARE(storage(reference, mode), expectedStorage);
        QCOMPARE(reference.measurementResult()->occlusion, uint64_t(3));
        std::vector<uint8_t> observedStorage;
        std::array<uint32_t, 4> repeatWords{};
        uint32_t observedCounter = 0;
        auto repeat = replayNativeOriginalDraw(r, 200, [&](Replay &p, bool after) {
            if (after) {
                observedStorage = storage(p, mode);
                repeatWords = words(p.readTexture(20));
                if (counted)
                    observedCounter = p.readCounter(12);
            }
        });
        QCOMPARE(observedStorage, expectedStorage);
        QCOMPARE(repeat.at("passing_samples"), Json(3));
        if (counted) {
            QCOMPARE(observedCounter, 3u);
            QCOMPARE(reference.readCounter(12), 3u);
        }
        report["original_draw_atomic_reference"] = {
            {"schema", 1},
            {"allocation_order_assumed", false},
            {"observed_color", observed},
            {"reference_color", referenceWords},
            {"repeat_color", repeatWords},
            {"reference_storage", storage(reference, mode)},
            {"repeat_storage", observedStorage},
            {"reference_passing_samples", reference.measurementResult()->occlusion},
            {"counter", counted ? Json(observedCounter) : Json(nullptr)},
            {"repeat", repeat}};
        QCOMPARE(storage(r, mode), original);
        QCOMPARE(r.readTexture(20), color);
        QCOMPARE(bindings(r), bound);
        if (counted)
            QCOMPARE(r.readCounter(12), counter);
        report["mode"] = mode.toStdString();
        report["status"] = "ok";
        save(report);
    }

    void computeGroup_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (const auto *mode : {"typed", "texture"})
                QTest::newRow(qPrintable(QString::number(vendor) + "-" + mode)) << vendor << QString(mode);
    }
    void computeGroup() {
        QFETCH(uint32_t, vendor);
        QFETCH(QString, mode);
        auto c = computeCapture(true);
        std::erase_if(c.entries, [](const auto &e) { return e.id == 10 || e.id == 11 || e.id == 12; });
        const auto graphics = fixture(mode);
        for (const auto &e : graphics.entries)
            if (e.id == 10 || e.id == 11 || e.id == 12)
                c.add(e.id, e.category, e.type,
                      {graphics.bytes.begin() + e.offset, graphics.bytes.begin() + e.offset + e.size});
        ReplayOptions o;
        o.until = 100;
        o.warp = !vendor;
        if (vendor)
            o.vendor = vendor;
        o.debug = true;
        o.shaders[15] =
            compileHlsl(
                std::string(mode == "typed" ? "RWBuffer<uint>" : "RWTexture2D<uint>") +
                    " target:register(u1);RWStructuredBuffer<uint> result:register(u0);"
                    "[numthreads(4,1,1)]void main(uint index:SV_GroupIndex){uint old;InterlockedAdd(target[" +
                    (mode == "typed" ? "0" : "uint2(0,0)") + "],1,old);result[index]=old;}",
                "cs_5_0")
                .bytecode;
        QTemporaryDir dir;
        const auto path = dir.filePath("group.gpa_frame");
        c.save(path);
        Frame f(path.toStdWString());
        Replay gpu(f, o);
        gpu.run();
        const auto native = storage(gpu, mode), nativeResults = gpu.readBuffer(7);
        o.before = o.prepareBeforeDraw = true;
        Replay r(f, o);
        r.run();
        const auto original = storage(r, mode), originalResults = r.readBuffer(7);
        const auto generation = r.generation();
        auto s = snapshotBoundResourceInputs(r, 5);
        const Program program(s.bytecode);
        Group group(program, s.inputs);
        Json transactions = Json::array();
        while (!group.finished()) {
            const auto step = group.step();
            if (step.step.atomic)
                transactions.push_back(executionTraceStep(step));
        }
        const auto actual = s.inputs.memory->buffers().at(7).bytes;
        auto values = [](const auto &bytes) {
            Reader reader(bytes);
            std::array<uint32_t, 4> result{};
            for (auto &value : result)
                value = reader.read<uint32_t>();
            std::sort(result.begin(), result.end());
            return result;
        };
        const std::array<uint32_t, 4> expected{0, 1, 2, 3};
        QCOMPARE(values(actual), expected);
        QCOMPARE(values(nativeResults), expected);
        QCOMPARE(Reader(native).read<uint32_t>(), 4u);
        QCOMPARE(snapshotStorage(s, mode), native);
        QCOMPARE(transactions.size(), size_t(4));
        QCOMPARE(storage(r, mode), original);
        QCOMPARE(r.readBuffer(7), originalResults);
        QCOMPARE(r.generation(), generation);
        save({{"schema", 1},
              {"status", "ok"},
              {"vendor", vendor},
              {"mode", mode.toStdString()},
              {"gpu_storage", native},
              {"gpu_results", nativeResults},
              {"results", actual},
              {"transactions", transactions},
              {"original_storage_restored", true},
              {"resource_observation", s.resourceObservation},
              {"original_gpu_schedule", false}});
    }
    void textureRegions_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("dimension");
        QTest::addColumn<uint32_t>("format");
        QTest::addColumn<uint32_t>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t dimension : {2u, 3u, 5u, 7u, 8u})
                for (uint32_t format : {42u, 43u})
                    for (uint32_t mode = 0; mode < 4; ++mode)
                        QTest::newRow(qPrintable(
                            QString("%1-%2-%3-%4").arg(vendor).arg(dimension).arg(format).arg(mode)))
                            << vendor << dimension << format << mode;
    }
    void textureRegions() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, dimension);
        QFETCH(uint32_t, format);
        QFETCH(uint32_t, mode);
        try {
            QTemporaryDir dir;
            const auto path = dir.filePath("region.gpa_frame");
            textureRegion(dimension, format).save(path);
            Frame f(path.toStdWString());
            std::array<uint32_t, 3> address{1, 1, 1};
            if (mode == 1)
                address[0] = UINT32_MAX;
            if (mode >= 2) {
                if (dimension == 7)
                    address[1] = 2;
                else if (dimension == 5 || dimension == 8)
                    address[2] = 2;
                else
                    address[0] = 4;
            }
            const auto kind = dimension == 2   ? "1D"
                              : dimension == 3 ? "2D"
                              : dimension == 5 ? "3D"
                              : dimension == 7 ? "1DArray"
                                               : "2DArray";
            const auto components = dimension == 2 ? 1u : dimension == 3 || dimension == 7 ? 2u : 3u;
            std::string coordinate =
                "uint" + (components == 1 ? std::string() : std::to_string(components)) + "(";
            for (uint32_t i = 0; i < components; ++i)
                coordinate += (i ? "," : "") + std::to_string(address[i]) + "u";
            coordinate += ")";
            const auto shader = [&](uint32_t add) {
                const auto type = format == 42 ? "uint" : "int";
                return compileHlsl(std::string("RWTexture") + kind + "<" + type +
                                       "> target:register(u1);"
                                       "uint4 main():SV_Target{" +
                                       type + " old;InterlockedAdd(target[" + coordinate + "]," +
                                       std::to_string(add) + ",old);return uint4(asuint(old),11,17,23);}",
                                   "ps_5_0")
                    .bytecode;
            };
            auto o = options(vendor, "texture");
            o.shaders[32] = shader(mode == 3 ? 0 : 5);
            Replay gpu(f, o);
            gpu.run();
            const auto native = gpu.readTexture(10), color = gpu.readTexture(20);
            o.before = o.prepareBeforeDraw = true;
            Replay r(f, o);
            r.run();
            auto s = snapshotBoundResourceInputs(r, 4);
            const auto before = r.readTexture(10);
            auto marker = native;
            if (mode == 3) {
                auto probeOptions = o;
                probeOptions.before = false;
                probeOptions.shaders[32] = shader(5);
                Replay probe(f, probeOptions);
                probe.run();
                marker = probe.readTexture(10);
            }
            std::optional<size_t> offset;
            for (size_t i = 0; i < before.size(); ++i)
                if (before[i] != marker[i]) {
                    const auto word = i / 4 * 4;
                    require(!offset || *offset == word, "Original atomic changed multiple words");
                    offset = word;
                }
            const Program program(s.bytecode);
            std::array<Inputs, 4> inputs;
            inputs.fill(s.inputs);
            PixelQuad quad(program, inputs, 1);
            std::vector<MemoryChange> changes;
            bool assisted = false;
            while (!quad.finished()) {
                const auto step = quad.step();
                assisted |= step.step.memoryGpuAssisted;
                changes.insert(changes.end(), step.step.textureChanges.begin(),
                               step.step.textureChanges.end());
            }
            const auto output = quad.lane(0).value({2, {0}});
            QCOMPARE(output.valid, uint8_t(offset ? 15 : 14));
            QCOMPARE(s.inputs.textures->resources.at(10).storage.bytes, native);
            QCOMPARE(changes.size(), offset ? size_t(1) : size_t(0));
            if (offset) {
                QCOMPARE(changes[0].offset, uint64_t(*offset));
                QCOMPARE(output.bits[0], Reader(color).read<uint32_t>());
                QCOMPARE(output.bits[0], 0x11111111u);
                s.inputs.textures->apply(changes, false);
                QCOMPARE(s.inputs.textures->resources.at(10).storage.bytes, before);
                s.inputs.textures->apply(changes);
                QCOMPARE(s.inputs.textures->resources.at(10).storage.bytes, native);
            }
            QCOMPARE(r.readTexture(10), before);
            save({{"schema", 1},
                  {"status", "ok"},
                  {"vendor", vendor},
                  {"dimension", dimension},
                  {"format", format},
                  {"mode", mode},
                  {"address", address},
                  {"gpu_assisted", assisted},
                  {"selected_offset", offset ? Json(*offset) : Json(nullptr)},
                  {"raw_bits", output.bits},
                  {"valid_mask", output.valid},
                  {"gpu_color", color},
                  {"storage_sha256", sha256(native)},
                  {"marker_sha256", sha256(marker)},
                  {"before_sha256", sha256(before)},
                  {"changes", changes.size()},
                  {"original_storage_restored", true}});
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void typedBounds_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (const auto *mode : {"typed", "texture"})
                QTest::newRow(qPrintable(QString::number(vendor) + "-" + mode)) << vendor << QString(mode);
    }
    void typedBounds() {
        QFETCH(uint32_t, vendor);
        QFETCH(QString, mode);
        auto c = fixture(mode);
        count(c, 1);
        if (mode == "typed")
            for (const auto &e : c.entries)
                if (e.id == 12) {
                    put(c.bytes, size_t(e.offset) + 32, 1u);
                    put(c.bytes, size_t(e.offset) + 36, 2u);
                }
        QTemporaryDir dir;
        const auto path = dir.filePath("bounds.gpa_frame");
        c.save(path);
        Frame f(path.toStdWString());
        Json rows = Json::array();
        for (uint32_t address : {0u, 1u, 2u, UINT32_MAX}) {
            auto o = options(vendor, mode);
            const auto coordinate = std::to_string(address) + "u";
            o.shaders[32] =
                compileHlsl(std::string(mode == "typed" ? "RWBuffer<uint>" : "RWTexture2D<uint>") +
                                " target:register(u1);uint4 main():SV_Target{uint i;InterlockedAdd(target[" +
                                (mode == "typed" ? coordinate : "uint2(" + coordinate + ",0)") +
                                "],5,i);return uint4(i,11,17,23);}",
                            "ps_5_0")
                    .bytecode;
            Replay gpu(f, o);
            gpu.run();
            const auto native = storage(gpu, mode);
            const bool inBounds = address < (mode == "typed" ? 2u : 1u);
            o.before = o.prepareBeforeDraw = true;
            Replay r(f, o);
            r.run();
            auto s = snapshotBoundResourceInputs(r, 4);
            const auto original = storage(r, mode);
            const Program program(s.bytecode);
            auto execute = [&](InputSnapshot &source) {
                std::array<Inputs, 4> inputs;
                inputs.fill(source.inputs);
                PixelQuad quad(program, inputs, 1);
                while (!quad.finished())
                    quad.step();
                return quad.lane(0).value({2, {0}});
            };
            const auto output = execute(s);
            QCOMPARE(snapshotStorage(s, mode), native);
            QCOMPARE(output.valid, uint8_t(inBounds ? 15 : 14));
            if (inBounds)
                QCOMPARE(output.bits[0], 0u);
            else
                QCOMPARE(native, original);
            QCOMPARE(storage(r, mode), original);
            rows.push_back({{"address", address},
                            {"in_bounds", inBounds},
                            {"storage", native},
                            {"raw_bits", output.bits},
                            {"valid_mask", output.valid}});
            auto unbound = snapshotBoundResourceInputs(r, 4);
            if (mode == "typed")
                unbound.inputs.memory = std::make_shared<Memory>(
                    std::map<uint64_t, Buffer>{},
                    std::map<Register, BufferView>{{{30, {1}}, {std::nullopt, 0, 0, 0, {}, 42}}});
            else
                unbound.inputs.textures->views.at({30, {1}}).resource.reset();
            QCOMPARE(execute(unbound).valid, uint8_t(14));
            auto invalid = snapshotBoundResourceInputs(r, 4);
            if (mode == "typed")
                invalid.inputs.memory = std::make_shared<Memory>(
                    invalid.inputs.memory->buffers(),
                    std::map<Register, BufferView>{{{30, {1}}, {10, 4, 8, 0, {}, 41}}});
            else
                invalid.inputs.textures->views.at({30, {1}}).format = 41;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, execute(invalid));
            QCOMPARE(storage(r, mode), original);
        }
        save({{"schema", 1},
              {"status", "ok"},
              {"vendor", vendor},
              {"mode", mode.toStdString()},
              {"cases", rows},
              {"unbound_result_unknown", true},
              {"invalid_format_rejected", true},
              {"original_storage_restored", true}});
    }
    void typedInstructions_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("signedValue");
        QTest::addColumn<QString>("operation");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (const auto *mode : {"typed", "texture"})
                for (bool sign : {false, true})
                    for (const auto *operation :
                         {"add", "and", "or", "xor", "min", "max", "exchange", "compare_equal",
                          "compare_different", "store_equal", "store_different", "add_no_result"})
                        QTest::newRow(
                            qPrintable(QString("%1-%2-%3-%4").arg(vendor).arg(mode).arg(sign).arg(operation)))
                            << vendor << QString(mode) << sign << QString(operation);
    }
    void typedInstructions() {
        QFETCH(uint32_t, vendor);
        QFETCH(QString, mode);
        QFETCH(bool, signedValue);
        QFETCH(QString, operation);
        try {
            auto c = fixture(mode);
            count(c, 1);
            const uint32_t initial = signedValue ? std::bit_cast<uint32_t>(int32_t(-9)) : 0x80000007u;
            for (const auto &e : c.entries) {
                if (e.id == 11)
                    put(c.bytes, size_t(e.offset) + 4, initial);
                if (signedValue && e.id == 12)
                    put(c.bytes, size_t(e.offset) + 24, 43u);
                if (signedValue && mode == "texture" && e.id == 10)
                    put(c.bytes, size_t(e.offset) + 32, 43u);
            }
            const auto target = mode == "typed" ? "target[0]" : "target[uint2(0,0)]";
            const auto type = signedValue ? "int" : "uint";
            const uint32_t operand =
                (operation == "min" || operation == "max") ? std::bit_cast<uint32_t>(int32_t(-3)) : 5u;
            const auto literal = [&](uint32_t bits) {
                return std::string(signedValue ? "asint(" : "uint(") + std::to_string(bits) + "u)";
            };
            std::string call;
            uint32_t expected = initial, returned = initial;
            if (operation.startsWith("compare") || operation.startsWith("store")) {
                const bool equal = operation.endsWith("equal");
                const bool store = operation.startsWith("store");
                call = std::string(store ? "InterlockedCompareStore(" : "InterlockedCompareExchange(") +
                       target + "," + literal(equal ? initial : initial + 1) + "," + literal(operand) +
                       (store ? ");" : ",i);");
                if (equal)
                    expected = operand;
                if (store)
                    returned = 123;
            } else {
                const auto name = operation.startsWith("add") ? "Add"
                                  : operation == "and"        ? "And"
                                  : operation == "or"         ? "Or"
                                  : operation == "xor"        ? "Xor"
                                  : operation == "min"        ? "Min"
                                  : operation == "max"        ? "Max"
                                                              : "Exchange";
                call = std::string("Interlocked") + name + "(" + target + "," + literal(operand) +
                       (operation == "add_no_result" ? ");" : ",i);");
                if (operation.startsWith("add"))
                    expected = initial + operand;
                else if (operation == "and")
                    expected = initial & operand;
                else if (operation == "or")
                    expected = initial | operand;
                else if (operation == "xor")
                    expected = initial ^ operand;
                else if (operation == "exchange")
                    expected = operand;
                else {
                    const bool less = signedValue
                                          ? std::bit_cast<int32_t>(initial) < std::bit_cast<int32_t>(operand)
                                          : initial < operand;
                    expected = (operation == "min" ? less : !less) ? initial : operand;
                }
                if (operation == "add_no_result")
                    returned = 123;
            }
            auto o = options(vendor, mode);
            o.shaders[32] = compileHlsl(std::string(mode == "typed" ? "RWBuffer<" : "RWTexture2D<") + type +
                                            "> target:register(u1);uint4 main():SV_Target{" + type +
                                            " i=123;" + call + "return uint4(asuint(i),11,17,23);}",
                                        "ps_5_0")
                                .bytecode;
            QTemporaryDir dir;
            const auto path = dir.filePath("typed.gpa_frame");
            c.save(path);
            Frame f(path.toStdWString());
            Replay gpu(f, o);
            gpu.run();
            const auto nativeStorage = storage(gpu, mode), nativeColor = gpu.readTexture(20);
            QCOMPARE(Reader(nativeStorage).read<uint32_t>(), expected);
            QCOMPARE(Reader(nativeColor).read<uint32_t>(), returned);
            o.before = o.prepareBeforeDraw = true;
            Replay r(f, o);
            r.run();
            auto snapshot = snapshotBoundResourceInputs(r, 4);
            const auto before = snapshotStorage(snapshot, mode);
            Program program(snapshot.bytecode);
            std::array<Inputs, 4> inputs;
            inputs.fill(snapshot.inputs);
            PixelQuad quad(program, std::move(inputs), 1, 10000);
            Json transactions = Json::array();
            while (!quad.finished()) {
                const auto step = quad.step();
                if (step.step.atomic)
                    transactions.push_back(
                        executionTraceStep(GroupStep{step.sequence, 0, step.lane, step.step, false}));
            }
            const auto output = quad.lane(0).value({2, {0}});
            QCOMPARE(output.valid, uint8_t(15));
            QCOMPARE(output.bits[0], returned);
            QCOMPARE(snapshotStorage(snapshot, mode), nativeStorage);
            QCOMPARE(storage(r, mode), before);
            save({{"schema", 1},
                  {"status", "ok"},
                  {"vendor", vendor},
                  {"mode", mode.toStdString()},
                  {"signed", signedValue},
                  {"operation", operation.toStdString()},
                  {"initial", initial},
                  {"operand", operand},
                  {"expected", expected},
                  {"returned", returned},
                  {"gpu_storage", nativeStorage},
                  {"gpu_color", nativeColor},
                  {"raw_bits", output.bits},
                  {"valid_mask", output.valid},
                  {"transactions", transactions},
                  {"original_storage_restored", true}});
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void prefixes_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (const auto *mode : {"counter", "append", "raw", "typed", "texture"})
                QTest::newRow(qPrintable(QString::number(vendor) + "-" + mode)) << vendor << QString(mode);
    }
    void prefixes() {
        QFETCH(uint32_t, vendor);
        QFETCH(QString, mode);
        try {
            auto capture = fixture(mode);
            QTemporaryDir dir;
            const auto path = dir.filePath("source.gpa_frame");
            capture.save(path);
            Frame f(path.toStdWString());
            auto o = options(vendor, mode);
            std::array<std::vector<uint8_t>, 4> references;
            Json concurrent = Json::array();
            for (uint32_t i = 1; i <= 4; ++i) {
                count(capture, i);
                const auto referencePath = dir.filePath(QString::number(i) + ".gpa_frame");
                capture.save(referencePath);
                Frame source(referencePath.toStdWString());
                Replay reference(source, o);
                reference.run();
                const auto color = reference.readTexture(20);
                const auto atomic = storage(reference, mode);
                const auto value = Reader(color).read<uint32_t>();
                require(mode == "append" ? value == 7 : value < i,
                        "Unordered draw returned an impossible atomic value");
                if (mode == "counter" || mode == "append")
                    require(reference.readCounter(12) == i, "Unordered draw counter constraint failed");
                else
                    require(Reader(atomic).read<uint32_t>() == i, "Unordered draw atomic constraint failed");
                concurrent.push_back({{"primitives", i}, {"color", color}, {"storage", atomic}});
            }
            // A multi-primitive Draw does not prescribe atomic invocation order.
            // Compare checkpoint execution with independently serialized original
            // draws, while retaining the unordered draw and its constraints.
            count(capture, 1);
            const auto serialPath = dir.filePath("serial.gpa_frame");
            capture.save(serialPath);
            Frame serialFrame(serialPath.toStdWString());
            Replay serial(serialFrame, o);
            serial.run();
            for (uint32_t i = 0; i < 4; ++i) {
                if (i)
                    serial.inspectNativeState([](auto *context, const auto &) { context->Draw(3, 0); });
                references[i] = serial.readTexture(20);
                require(Reader(references[i]).read<uint32_t>() == (mode == "append" ? 7u : i),
                        "Serialized original draw violates the known atomic prefix");
            }
            o.before = o.prepareBeforeDraw = true;
            Replay r(f, o);
            r.run();
            const auto original = storage(r, mode), originalColor = r.readTexture(20);
            const auto originalBindings = bindings(r);
            const auto generation = r.generation();
            const bool counted = mode == "counter" || mode == "append";
            const auto originalCounter = counted ? r.readCounter(12) : 0;
            Json rows = Json::array();
            const auto geometry =
                replayNativePixelGeometry(r, 200, {0, 3, {1, 2, 3}}, [&](Replay &p, uint64_t completed) {
                    const auto native = storage(p, mode);
                    const auto bound = bindings(p);
                    const auto s = snapshotBoundResourceInputs(p, 4);
                    require(s.event == 200 && s.shader == 32 && s.stage == 4 && s.frameSha256 == f.sha256() &&
                                s.replayGeneration == generation && s.bytecode == o.shaders.at(32) &&
                                s.resourceObservation == "current_native_bindings",
                            "Checkpoint input context differs");
                    require(s.bindings.at({30, {1}}).resource == 10 && s.bindings.at({30, {1}}).view == 12,
                            "Checkpoint input identity differs");
                    require(snapshotStorage(s, mode) == native, "Snapshot is not current private storage");
                    if (counted)
                        require(s.inputs.memory->counters().at(12).value == completed &&
                                    p.readCounter(12) == completed,
                                "Snapshot is not the current private counter");
                    else
                        require(Reader(native).read<uint32_t>() == completed,
                                "Private atomic prefix differs");
                    PixelTraceOptions selection;
                    selection.activeMask = 1;
                    selection.inputProvenance = {{"source", "owned-input-free-single-pixel-fixture"},
                                                 {"completed_primitives", completed}};
                    const auto outputs = evaluateNativePixelOutputs(s, selection);
                    const auto bits =
                        outputs.at("lanes")[0].at("outputs")[0].at("raw_bits").get<std::array<uint32_t, 4>>();
                    std::vector<uint8_t> bytes;
                    append(bytes, bits);
                    if (bytes != references.at(size_t(completed)))
                        save({{"status", "mismatch"},
                              {"completed_primitives", completed},
                              {"private_storage", native},
                              {"outputs", outputs},
                              {"normal_next_prefix", references.at(size_t(completed))}});
                    require(bytes == references.at(size_t(completed)),
                            "Checkpoint interpreter output differs from independent next GPU prefix");
                    require(outputs.at("lanes")[0].at("outputs")[0].at("valid_mask") == 15,
                            "Checkpoint shader output is invalid");
                    require(outputs.at("resource_observation") == "current_native_bindings" &&
                                outputs.at("replay_generation") == generation,
                            "Checkpoint output lost resource acquisition scope");
                    require(snapshotStorage(s, mode) == native && storage(p, mode) == native &&
                                bindings(p) == bound && p.generation() == generation,
                            "Checkpoint observation or evaluation changed replay state");
                    if (counted)
                        require(p.readCounter(12) == completed &&
                                    s.inputs.memory->counters().at(12).value == completed,
                                "Checkpoint evaluation changed observed hidden counter");
                    rows.push_back({{"completed_primitives", completed},
                                    {"storage", native},
                                    {"native_counter", counted ? Json(p.readCounter(12)) : Json(nullptr)},
                                    {"outputs", outputs},
                                    {"normal_next_prefix", references.at(size_t(completed))}});
                });
            QCOMPARE(rows.size(), size_t(4));
            QCOMPARE(storage(r, mode), original);
            QCOMPARE(r.readTexture(20), originalColor);
            QCOMPARE(bindings(r), originalBindings);
            QCOMPARE(r.generation(), generation);
            if (counted)
                QCOMPARE(r.readCounter(12), originalCounter);
            save({{"schema", 1},
                  {"mode", mode.toStdString()},
                  {"geometry", geometry},
                  {"checkpoints", rows},
                  {"unordered_original_draws", concurrent},
                  {"reference_schedule", "separate_original_draws_with_readback"},
                  {"original_storage_restored", true},
                  {"status", "ok"}});
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void recovery_data() { devices(); }
    void recovery() {
        QFETCH(uint32_t, vendor);
        QTemporaryDir dir;
        const auto path = dir.filePath("source.gpa_frame");
        fixture("counter").save(path);
        Frame f(path.toStdWString());
        auto o = options(vendor, "counter");
        o.before = o.prepareBeforeDraw = true;
        Replay r(f, o);
        QVERIFY_EXCEPTION_THROWN(snapshotBoundResourceInputs(r, 4), std::runtime_error);
        r.run();
        const auto native = r.readBuffer(10);
        const auto generation = r.generation();
        QVERIFY_EXCEPTION_THROWN(snapshotBoundResourceInputs(r, 5), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(snapshotBoundResourceInputs(r, 6), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(snapshotBoundResourceInputs(r, 4, 0), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(snapshotBoundResourceInputs(r, 4, 15), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(snapshotBoundResourceInputs(r, 4, 1024, [] { return true; }),
                                 OperationCancelled);
        QVERIFY_EXCEPTION_THROWN(replayNativePixelGeometry(r, 200, {0, 3, {1, 2, 3}},
                                                           [&](Replay &p, uint64_t completed) {
                                                               if (completed == 1)
                                                                   snapshotBoundResourceInputs(p, 4, 15);
                                                           }),
                                 std::runtime_error);
        QCOMPARE(r.readBuffer(10), native);
        QCOMPARE(r.readCounter(12), 0u);
        QCOMPARE(r.generation(), generation);
        const auto s = snapshotBoundResourceInputs(r, 4, 16);
        QCOMPARE(snapshotStorage(s, "counter"), native);
        QCOMPARE(s.inputs.memory->counters().at(12).value, std::optional<uint32_t>(0));
        save({{"schema", 1},
              {"status", "ok"},
              {"vendor", vendor},
              {"original_storage_restored", true},
              {"replay_generation", generation}});
    }
};
QTEST_GUILESS_MAIN(NativePixelCheckpointInputsTests)
#include "NativePixelCheckpointInputsTests.moc"
