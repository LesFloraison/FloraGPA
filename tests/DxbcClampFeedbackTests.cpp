#include "TextureOperationOracle.h"
#include "application/DxbcGpuOperations.h"
#include "application/DxbcInspection.h"
#include <QDir>
#include <QFile>
#include <QtTest>
#include <bit>
#include <d3dcompiler.h>
using namespace flora;
using namespace flora::dxbc_exec;
using namespace flora::testing;
using Json = nlohmann::json;
namespace {
Com<ID3D11Device> device(uint32_t vendor, D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1) {
    Com<IDXGIAdapter1> selected;
    if (vendor) {
        Com<IDXGIFactory1> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create feedback adapter factory");
        for (UINT i = 0;; ++i) {
            Com<IDXGIAdapter1> adapter;
            const auto result = factory->EnumAdapters1(i, &adapter);
            if (result == DXGI_ERROR_NOT_FOUND)
                break;
            check(result, "Enumerate feedback adapters");
            DXGI_ADAPTER_DESC1 desc{};
            check(adapter->GetDesc1(&desc), "Read feedback adapter identity");
            if (desc.VendorId == vendor && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                selected = adapter;
                break;
            }
        }
        if (!selected)
            throw std::runtime_error("Requested feedback adapter is absent");
    }
    Com<ID3D11Device> result;
    check(D3D11CreateDevice(selected.Get(), vendor ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP, nullptr,
                            D3D11_CREATE_DEVICE_DEBUG, &level, 1, D3D11_SDK_VERSION, &result, nullptr,
                            nullptr),
          "Create feedback device");
    return result;
}

uint32_t bits(float x) { return std::bit_cast<uint32_t>(x); }
void save(const QString &path, Bytes bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size()) != qint64(bytes.size()))
        throw std::runtime_error("Cannot save clamp evidence");
}
std::vector<uint8_t> shader(uint32_t stage, unsigned mode, bool clamped, bool dynamicBias = false) {
    std::string source = "Texture2D<float> tex:register(t3);cbuffer C:register(b2){uint4 parameters[4];}";
    source += mode == 3 ? "SamplerComparisonState samp:register(s4);" : "SamplerState samp:register(s4);";
    if (stage == 5)
        source += "RWStructuredBuffer<uint4> outputData:register(u3);[numthreads(4,1,1)]void main(uint3 "
                  "t:SV_GroupThreadID){uint i=t.x;";
    else
        source += stage == 0 ? "uint4 main():SV_Target{uint i=0;" : "uint4 main():TEXCOORD0{uint i=0;";
    source += "uint status;float value=tex.";
    source += mode == 0   ? "Sample(samp,float2(0.4,0.6)"
              : mode == 1 ? "SampleBias(samp,float2(0.4,0.6),0"
              : mode == 2 ? "SampleGrad(samp,float2(0.4,0.6),float2(0,0),float2(0,0)"
                          : "SampleCmp(samp,float2(0.4,0.6),0.75";
    if (mode == 1 && dynamicBias) {
        const auto pos = source.rfind(",0");
        source.replace(pos, 2, ",asfloat(parameters[i].y)");
    }
    source += ",int2(0,0)";
    source += clamped ? ",asfloat(parameters[i].x),status);" : ");";
    source += "uint4 result=asuint(value).xxxx;";
    if (clamped)
        source += "result.w^=CheckAccessFullyMapped(status)?0:0xffffffff;";
    source += stage == 5 ? "outputData[i]=result;}" : "return result;}";
    return compileHlsl(source, stage == 5 ? "cs_5_0" : stage == 0 ? "ps_5_0" : "vs_5_0").bytecode;
}
std::vector<uint8_t> unpadded(Bytes original) {
    auto parts = readDxbcParts(original);
    std::vector<uint8_t> replaced;
    for (auto &[tag, bytes] : parts) {
        if (tag != 0x58454853 && tag != 0x52444853)
            continue;
        auto code = readDxbcProgram(bytes);
        for (auto &row : code.instructions) {
            const auto op = row[0] & 2047;
            if (op < 230 || op > 233)
                continue;
            if (dxbc_detail::compilerInstructionPadding(row, row.size() - 1)) {
                row.pop_back();
                row[0] = (row[0] & ~(127u << 24)) | (uint32_t(row.size()) << 24);
            }
        }
        replaced = writeDxbcProgram(code);
        bytes = replaced;
    }
    return makeDxbc(parts);
}
} // namespace
class DxbcClampFeedbackTests final : public QObject {
    Q_OBJECT
  private slots:
    void framing_data() {
        QTest::addColumn<uint32_t>("stage");
        QTest::addColumn<unsigned>("mode");
        for (uint32_t stage : {0u, 1u, 5u})
            for (unsigned mode = 0; mode < 4; ++mode)
                if (stage == 0 || mode == 2)
                    QTest::newRow(qPrintable(QString("%1-%2").arg(stage).arg(mode))) << stage << mode;
    }
    void framing() {
        QFETCH(uint32_t, stage);
        QFETCH(unsigned, mode);
        const auto compiled = shader(stage, mode, true);
        bool found = false;
        for (const auto &[tag, bytes] : readDxbcParts(compiled)) {
            if (tag != 0x58454853 && tag != 0x52444853)
                continue;
            auto code = readDxbcProgram(bytes);
            for (auto &row : code.instructions) {
                if ((row[0] & 2047) != 230 + mode)
                    continue;
                QVERIFY(!found);
                found = true;
                const auto original = row;
                QVERIFY(dxbc_detail::compilerInstructionPadding(row, row.size() - 1));
                auto encoded = [&] {
                    row[0] = (row[0] & ~(127u << 24)) | (uint32_t(row.size()) << 24);
                    const auto replacement = writeDxbcProgram(code);
                    auto parts = readDxbcParts(compiled);
                    for (auto &part : parts)
                        if (part.first == tag)
                            part.second = replacement;
                    return makeDxbc(parts);
                };
                row.back() = 1;
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, static_cast<void>(Program(encoded())));
                row = original;
                row.push_back(0);
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, static_cast<void>(Program(encoded())));
                row = original;
                row.pop_back();
                const Program canonical(encoded());
                QCOMPARE(canonical.instructions().size(), Program(compiled).instructions().size());
                row = original;
            }
        }
        QVERIFY(found);
    }
    void execution_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("stage");
        QTest::addColumn<unsigned>("mode");
        QTest::addColumn<unsigned>("scenario");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t stage : {0u, 1u, 5u})
                for (unsigned mode = 0; mode < 4; ++mode) {
                    if (stage != 0 && mode != 2)
                        continue;
                    for (unsigned scenario = 0; scenario < 9; ++scenario)
                        QTest::newRow(
                            qPrintable(QString("%1-%2-%3-%4").arg(vendor).arg(stage).arg(mode).arg(scenario)))
                            << vendor << stage << mode << scenario;
                }
    }
    void execution() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, stage);
        QFETCH(unsigned, mode);
        QFETCH(unsigned, scenario);
        try {
            auto native = device(vendor, scenario == 8 ? D3D_FEATURE_LEVEL_11_0 : D3D_FEATURE_LEVEL_11_1);
            auto gpu = std::make_shared<GpuOperations>(native.Get(), !vendor);
            auto textures = std::make_shared<TextureInputs>();
            TextureInput input;
            input.resource.id = 1;
            input.resource.type = 0x85;
            input.resource.desc = {4,
                                   4,
                                   3,
                                   1,
                                   DXGI_FORMAT_R32_FLOAT,
                                   1,
                                   0,
                                   0,
                                   D3D11_BIND_SHADER_RESOURCE,
                                   0,
                                   D3D11_RESOURCE_MISC_RESOURCE_CLAMP};
            input.minLod = scenario == 3 ? 1.5f : 0.f;
            input.subresources = textureSubresources(input.resource);
            std::vector<uint8_t> bytes(84);
            for (const auto &sub : input.subresources)
                for (size_t p = sub.offset; p < sub.offset + sub.size; p += 4) {
                    const auto value = bits(sub.mip == 0 ? 0.25f : sub.mip == 1 ? 0.5f : 1.f);
                    std::memcpy(bytes.data() + p, &value, 4);
                }
            input.storage = Buffer::known(bytes);
            textures->resources.emplace(1, input);
            TextureInputView view;
            view.resource = 1;
            view.format = DXGI_FORMAT_R32_FLOAT;
            view.dimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            view.firstMip = scenario == 1 ? 1u : 0u;
            view.mipCount = 3 - view.firstMip;
            view.nonTiled = scenario != 7;
            D3D11_SHADER_RESOURCE_VIEW_DESC descriptor{};
            descriptor.Format = DXGI_FORMAT_R32_FLOAT;
            descriptor.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            descriptor.Texture2D = {view.firstMip, view.mipCount};
            view.nativeDescriptor.resize(sizeof descriptor / 4);
            std::memcpy(view.nativeDescriptor.data(), &descriptor, sizeof descriptor);
            textures->views.emplace(Register{7, {3}}, view);
            D3D11_SAMPLER_DESC sd{};
            sd.Filter = mode == 3       ? D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT
                        : scenario == 4 ? D3D11_FILTER_MIN_MAG_MIP_LINEAR
                                        : D3D11_FILTER_MIN_MAG_MIP_POINT;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
            sd.MaxAnisotropy = 1;
            sd.MaxLOD = scenario == 2 ? 1.f : D3D11_FLOAT32_MAX;
            SamplerInput sampler;
            sampler.descriptor.emplace();
            std::memcpy(sampler.descriptor->data(), &sd, sizeof sd);
            textures->samplers.emplace(Register{6, {4}}, sampler);
            std::vector<std::array<uint32_t, 4>> values;
            for (unsigned i = 0; i < 4; ++i)
                values.push_back({bits(scenario == 4 ? 0.25f + 0.5f * i : float(i)), 0, 0, 0});
            if (scenario == 6) {
                values[0][0] = 0x7fc01234;
                values[1][0] = 0x7f800000;
                values[2][0] = 0xff800000;
                values[3][0] = bits(0);
            }
            Inputs inputs;
            inputs.gpu = gpu;
            inputs.textures = textures;
            for (const auto &value : values)
                inputs.constantBuffers[2].push_back(Value::known(value));
            if (scenario == 5)
                inputs.constantBuffers[2][1].valid = 14;
            inputs.memory = std::make_shared<Memory>(
                std::map<uint64_t, Buffer>{{9, Buffer::known(std::vector<uint8_t>(64))}},
                std::map<Register, BufferView>{{{30, {3}}, {9, 0, 64, 16}}});
            const auto code = shader(stage, mode, true), ordinary = shader(stage, mode, false);
            const auto canonical = unpadded(code), dynamic = shader(stage, mode, true, true);
            const auto evidence = qEnvironmentVariable("FLORA_CLAMP_FEEDBACK_EVIDENCE");
            QDir folder(QDir(evidence).filePath(QString::fromLatin1(QTest::currentDataTag())));
            if (!evidence.isEmpty()) {
                QVERIFY(folder.mkpath("."));
                save(folder.filePath("feedback.dxbc"), code);
                save(folder.filePath("ordinary.dxbc"), ordinary);
                save(folder.filePath("unpadded.dxbc"), canonical);
                save(folder.filePath("dynamic.dxbc"), dynamic);
            }
            Program program(code);
            std::array<Value, 4> results;
            uint64_t batches = 0;
            auto execute = [&] {
                if (stage == 5) {
                    Group group(program, inputs);
                    while (!group.finished())
                        group.step();
                    batches = group.samplingBatches();
                    const auto &buffer = inputs.memory->buffers().at(9);
                    for (unsigned i = 0; i < 4; ++i) {
                        std::memcpy(results[i].bits.data(), buffer.bytes.data() + i * 16, 16);
                        for (unsigned c = 0; c < 4; ++c)
                            if (std::all_of(buffer.valid.begin() + i * 16 + c * 4,
                                            buffer.valid.begin() + i * 16 + c * 4 + 4,
                                            [](auto v) { return v == 1; }))
                                results[i].valid |= uint8_t(1u << c);
                    }
                } else if (stage == 0) {
                    std::array<Inputs, 4> lanes{inputs, inputs, inputs, inputs};
                    for (unsigned i = 0; i < 4; ++i)
                        lanes[i].constantBuffers[2][0] = inputs.constantBuffers[2][i];
                    PixelQuad quad(program, lanes);
                    while (!quad.finished())
                        quad.step();
                    batches = quad.samplingBatches();
                    for (unsigned i = 0; i < 4; ++i)
                        results[i] = quad.lane(i).value({2, {0}});
                } else {
                    for (unsigned i = 0; i < 4; ++i) {
                        auto selected = inputs;
                        selected.constantBuffers[2][0] = inputs.constantBuffers[2][i];
                        Lane lane(program, selected);
                        while (!lane.finished())
                            lane.step();
                        results[i] = lane.value({2, {0}});
                    }
                }
            };
            if (scenario == 8) {
                std::string failure;
                try {
                    execute();
                } catch (const std::runtime_error &error) {
                    failure = error.what();
                }
                QCOMPARE(failure, std::string("Per-sample LOD clamp requires feature level 11.1 and tier 2 "
                                              "on the selected device"));
                QCOMPARE(gpu->operations(), uint64_t(0));
                QCOMPARE(gpu->identity().at("feature_level"), Json(uint32_t(D3D_FEATURE_LEVEL_11_0)));
                if (!evidence.isEmpty()) {
                    const auto report = Json{{"adapter", gpu->identity()},
                                             {"stage", stage},
                                             {"mode", mode},
                                             {"scenario", scenario},
                                             {"rejected", failure},
                                             {"gpu_operations", gpu->operations()}}
                                            .dump(2);
                    save(folder.filePath("report.json"),
                         Bytes(reinterpret_cast<const uint8_t *>(report.data()), report.size()));
                }
                return;
            }
            execute();
            std::array<Value, 4> nativeDynamic{};
            if (stage == 0 && mode == 1) {
                const Program dynamicProgram(dynamic);
                std::array<Inputs, 4> lanes{inputs, inputs, inputs, inputs};
                for (unsigned i = 0; i < 4; ++i)
                    lanes[i].constantBuffers[2][0] = inputs.constantBuffers[2][i];
                PixelQuad quad(dynamicProgram, lanes);
                while (!quad.finished())
                    quad.step();
                for (unsigned i = 0; i < 4; ++i)
                    nativeDynamic[i] = quad.lane(i).value({2, {0}});
            }
            Json original = Json::array(), baseline = Json::array(), actual = Json::array();
            Json unpaddedResults = Json::array(), dynamicResults = Json::array();
            bool differs = false;
            for (unsigned i = 0; i < 4; ++i) {
                auto parameters = values;
                if (stage != 5)
                    parameters[0] = values[i];
                const auto expected = directTexture(native.Get(), code, stage, input, view, parameters, 0,
                                                    &sampler, stage == 5 ? 4u : 1u, stage == 5 ? i : 0u);
                const auto unclamped = directTexture(native.Get(), ordinary, stage, input, view, parameters,
                                                     0, &sampler, stage == 5 ? 4u : 1u, stage == 5 ? i : 0u);
                unpaddedResults.push_back(directTexture(native.Get(), canonical, stage, input, view,
                                                        parameters, 0, &sampler, stage == 5 ? 4u : 1u,
                                                        stage == 5 ? i : 0u));
                dynamicResults.push_back(directTexture(native.Get(), dynamic, stage, input, view, parameters,
                                                       0, &sampler, stage == 5 ? 4u : 1u,
                                                       stage == 5 ? i : 0u));
                original.push_back(expected);
                baseline.push_back(unclamped);
                actual.push_back({{"bits", results[i].bits}, {"valid", results[i].valid}});
                differs |= expected != unclamped;
            }
            Json dynamicExecution = Json::array();
            if (stage == 0 && mode == 1)
                for (const auto &v : nativeDynamic)
                    dynamicExecution.push_back({{"bits", v.bits}, {"valid", v.valid}});
            if (!evidence.isEmpty()) {
                auto report = Json{
                    {"adapter", gpu->identity()},
                    {"stage", stage},
                    {"mode", mode},
                    {"scenario", scenario},
                    {"clamps", values},
                    {"native", actual},
                    {"native_dynamic", dynamicExecution},
                    {"original", original},
                    {"unpadded", unpaddedResults},
                    {"dynamic", dynamicResults},
                    {"unclamped", baseline},
                    {"batches",
                     batches}}.dump(2);
                save(folder.filePath("report.json"),
                     Bytes(reinterpret_cast<const uint8_t *>(report.data()), report.size()));
            }
            for (unsigned i = 0; i < 4; ++i) {
                QCOMPARE(results[i].valid, uint8_t(scenario == 5 && i == 1 ? 0 : scenario == 7 ? 7 : 15));
                const auto expected = original[i].get<std::array<uint32_t, 4>>();
                for (unsigned c = 0; c < 4; ++c)
                    if (results[i].valid & (1u << c))
                        QCOMPARE(results[i].bits[c], expected[c]);
            }
            if (stage == 0 && mode == 1)
                for (unsigned i = 0; i < 4; ++i) {
                    QCOMPARE(nativeDynamic[i].valid, results[i].valid);
                    const auto expected = dynamicResults[i].get<std::array<uint32_t, 4>>();
                    for (unsigned c = 0; c < 4; ++c)
                        if (nativeDynamic[i].valid & (1u << c))
                            QCOMPARE(nativeDynamic[i].bits[c], expected[c]);
                }
            QCOMPARE(unpaddedResults, original);
            if (scenario == 0)
                QVERIFY2(differs, "The original feedback shader did not apply the per-sample clamp");
            if (stage == 5 && scenario != 5)
                QCOMPARE(batches, uint64_t(1));
            if (stage == 0 && mode != 2)
                QCOMPARE(batches, uint64_t(1));
            QCOMPARE(textures->resources.at(1).storage.bytes, bytes);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
};
QTEST_GUILESS_MAIN(DxbcClampFeedbackTests)
#include "DxbcClampFeedbackTests.moc"
