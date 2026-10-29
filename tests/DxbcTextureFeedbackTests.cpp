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
Com<ID3D11Device> device(uint32_t vendor) {
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
                            D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &result, nullptr,
                            nullptr),
          "Create feedback device");
    return result;
}
std::vector<uint8_t> shader(uint32_t stage, unsigned mode, bool feedback) {
    const std::string offset = mode == 4 || mode == 6 ? "int2(control.xy)" : "int2(0,0)";
    const std::string operation = mode == 0   ? "inputData.Load(int3(1,2,0)," + offset
                                  : mode == 1 ? "inputData.SampleLevel(s,float2(0.4,0.6),0," + offset
                                  : mode == 2 ? "inputData.SampleCmpLevelZero(s,float2(0.4,0.6),0.5," + offset
                                  : mode == 3 || mode == 4
                                      ? "inputData.GatherRed(s,float2(0.4,0.6)," + offset
                                      : "inputData.GatherCmpRed(s,float2(0.4,0.6),0.5," + offset;
    const bool comparison = mode == 2 || mode >= 5;
    std::string source = "Texture2D<float> inputData:register(t3);cbuffer C:register(b2){uint4 control;}";
    source += comparison ? "SamplerComparisonState s:register(s4);" : "SamplerState s:register(s4);";
    if (stage == 5)
        source += "RWStructuredBuffer<uint4> outputData:register(u3);[numthreads(1,1,1)]void main(){";
    else
        source += stage == 0 ? "uint4 main():SV_Target{" : "uint4 main():TEXCOORD0{";
    source += "uint status;float4 value=" + operation + (feedback ? ",status);" : ");");
    source += "uint4 result=asuint(value);";
    if (feedback)
        source += "result.w^=CheckAccessFullyMapped(status)?0:0xffffffff;";
    source += stage == 5 ? "outputData[0]=result;}" : "return result;}";
    return compileHlsl(source, stage == 5 ? "cs_5_0" : stage == 0 ? "ps_5_0" : "vs_5_0").bytecode;
}
void save(const QString &name, Bytes bytes) {
    QFile file(name);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size()) != qint64(bytes.size()))
        throw std::runtime_error("Cannot save feedback evidence");
}
} // namespace
class DxbcTextureFeedbackTests final : public QObject {
    Q_OBJECT
  private slots:
    void padding_data() {
        QTest::addColumn<uint32_t>("stage");
        QTest::addColumn<unsigned>("mode");
        for (uint32_t stage : {0u, 1u, 5u})
            for (unsigned mode : {1u, 2u})
                QTest::newRow(qPrintable(QString("%1-%2").arg(stage).arg(mode))) << stage << mode;
    }
    void padding() {
        QFETCH(uint32_t, stage);
        QFETCH(unsigned, mode);
        const auto compiled = shader(stage, mode, true);
        bool found = false;
        for (const auto &[tag, bytes] : readDxbcParts(compiled)) {
            if (tag != 0x58454853 && tag != 0x52444853)
                continue;
            auto code = readDxbcProgram(bytes);
            for (auto &row : code.instructions) {
                if ((row[0] & 2047) != (mode == 1 ? 228u : 229u))
                    continue;
                QVERIFY(!found);
                found = true;
                const auto original = row;
                QVERIFY(dxbc_detail::compilerInstructionPadding(row, row.size() - 1));
                auto encoded = [&] {
                    row[0] = (row[0] & ~(127u << 24)) | (uint32_t(row.size()) << 24);
                    const auto rewritten = writeDxbcProgram(code);
                    auto parts = readDxbcParts(compiled);
                    for (auto &part : parts)
                        if (part.first == tag)
                            part.second = rewritten;
                    return makeDxbc(parts);
                };
                row.back() = 1;
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, static_cast<void>(Program(encoded())));
                row = original;
                row.push_back(0);
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, static_cast<void>(Program(encoded())));
                row = original;
                row.pop_back();
                const Program unpadded(encoded());
                QCOMPARE(unpadded.instructions().size(), Program(compiled).instructions().size());
                row = original;
                std::set<unsigned> unused;
                size_t position = 1;
                for (auto extended = row[0] >> 31; extended;)
                    extended = row.at(position++) >> 31;
                for (unsigned i = 0; i < 3; ++i)
                    position = dxbc_detail::operand(row, position, unused, 0, false);
                row[position] = (row[position] & ~(255u << 12)) | (6u << 12);
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, static_cast<void>(Program(encoded())));
                row = original;
            }
        }
        QVERIFY(found);
    }
    void texture_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("stage");
        QTest::addColumn<unsigned>("mode");
        QTest::addColumn<bool>("known");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t stage : {0u, 1u, 5u})
                for (unsigned mode = 0; mode < 7; ++mode)
                    for (bool known : {false, true})
                        QTest::newRow(
                            qPrintable(QString("%1-%2-%3-%4").arg(vendor).arg(stage).arg(mode).arg(known)))
                            << vendor << stage << mode << known;
    }
    void texture() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, stage);
        QFETCH(unsigned, mode);
        QFETCH(bool, known);
        try {
            auto native = device(vendor);
            auto gpu = std::make_shared<GpuOperations>(native.Get(), !vendor);
            auto textures = std::make_shared<TextureInputs>();
            TextureInput input;
            input.resource.id = 1;
            input.resource.type = 0x85;
            input.resource.desc = {4, 4, 1, 1, DXGI_FORMAT_R32_FLOAT, 1, 0, 0, D3D11_BIND_SHADER_RESOURCE,
                                   0, 0};
            std::vector<uint8_t> bytes(64);
            for (unsigned i = 0; i < 16; ++i) {
                const auto value = float(i + 1) / 16.f;
                std::memcpy(bytes.data() + 4 * i, &value, 4);
            }
            input.storage = Buffer::known(bytes);
            input.subresources = textureSubresources(input.resource);
            textures->resources.emplace(1, input);
            TextureInputView view;
            view.resource = 1;
            view.format = DXGI_FORMAT_R32_FLOAT;
            view.dimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            view.nonTiled = known;
            D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.Format = DXGI_FORMAT_R32_FLOAT;
            desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            desc.Texture2D.MipLevels = 1;
            view.nativeDescriptor.resize(sizeof(desc) / 4);
            std::memcpy(view.nativeDescriptor.data(), &desc, sizeof(desc));
            textures->views.emplace(Register{7, {3}}, view);
            const bool comparison = mode == 2 || mode >= 5;
            D3D11_SAMPLER_DESC sd{};
            sd.Filter =
                comparison ? D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_POINT;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
            sd.MaxAnisotropy = 1;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            SamplerInput sampler;
            sampler.descriptor.emplace();
            std::memcpy(sampler.descriptor->data(), &sd, sizeof(sd));
            textures->samplers.emplace(Register{6, {4}}, sampler);
            Inputs inputs;
            inputs.textures = textures;
            inputs.gpu = gpu;
            inputs.constantBuffers[2] = {Value::known({1, 0, 0, 0})};
            inputs.memory = std::make_shared<Memory>(
                std::map<uint64_t, Buffer>{{9, Buffer::known(std::vector<uint8_t>(16))}},
                std::map<Register, BufferView>{{{30, {3}}, {9, 0, 16, 16}}});
            const auto feedback = shader(stage, mode, true), control = shader(stage, mode, false);
            const auto evidenceRoot = qEnvironmentVariable("FLORA_TEXTURE_FEEDBACK_EVIDENCE");
            if (!evidenceRoot.isEmpty()) {
                QDir directory(QDir(evidenceRoot).filePath(QString::fromLatin1(QTest::currentDataTag())));
                QVERIFY(directory.mkpath("."));
                save(directory.filePath("feedback.dxbc"), feedback);
                save(directory.filePath("control.dxbc"), control);
                Com<ID3DBlob> assembly;
                check(D3DDisassemble(feedback.data(), feedback.size(), 0, nullptr, &assembly),
                      "Disassemble feedback fixture");
                save(directory.filePath("feedback.txt"),
                     Bytes(static_cast<const uint8_t *>(assembly->GetBufferPointer()),
                           assembly->GetBufferSize()));
            }
            const Program program(feedback);
            static constexpr unsigned opcodes[]{223, 228, 229, 219, 221, 220, 222};
            QCOMPARE(std::count_if(program.instructions().begin(), program.instructions().end(),
                                   [&](const auto &i) { return i.opcode == opcodes[mode]; }),
                     1);
            Lane lane(program, inputs);
            unsigned opaque = 0;
            while (!lane.finished()) {
                const auto step = lane.step();
                for (const auto &change : step.changes)
                    for (unsigned c = 0; c < 4; ++c)
                        if (change.after.mapping[c] != MappingStatus::NotStatus) {
                            QCOMPARE(change.after.mapping[c],
                                     known ? MappingStatus::Mapped : MappingStatus::Unknown);
                            QVERIFY(change.after.wellFormed());
                            ++opaque;
                        }
            }
            QVERIFY(opaque > 0);
            Value result;
            if (stage == 5) {
                const auto &buffer = inputs.memory->buffers().at(9);
                std::memcpy(result.bits.data(), buffer.bytes.data(), 16);
                for (unsigned c = 0; c < 4; ++c)
                    if (std::all_of(buffer.valid.begin() + 4 * c, buffer.valid.begin() + 4 * c + 4,
                                    [](auto v) { return v == 1; }))
                        result.valid |= uint8_t(1u << c);
            } else
                result = lane.value({2, {0}});
            const auto ordinary =
                directTexture(native.Get(), control, stage, input, view, {{1, 0, 0, 0}}, 0, &sampler);
            const auto original =
                directTexture(native.Get(), feedback, stage, input, view, {{1, 0, 0, 0}}, 0, &sampler);
            const auto root = qEnvironmentVariable("FLORA_TEXTURE_FEEDBACK_EVIDENCE");
            if (!root.isEmpty()) {
                QDir directory(QDir(root).filePath(QString::fromLatin1(QTest::currentDataTag())));
                QVERIFY(directory.mkpath("."));
                save(directory.filePath("feedback.dxbc"), feedback);
                save(directory.filePath("control.dxbc"), control);
                const auto report = Json({{"adapter", gpu->identity()},
                                          {"stage", stage},
                                          {"mode", mode},
                                          {"known_non_tiled", known},
                                          {"native", result.bits},
                                          {"valid", result.valid},
                                          {"original", original},
                                          {"control", ordinary},
                                          {"opaque_changes", opaque}})
                                        .dump(2);
                save(directory.filePath("report.json"),
                     Bytes(reinterpret_cast<const uint8_t *>(report.data()), report.size()));
            }
            QCOMPARE(result.valid, uint8_t(known ? 15 : 7));
            for (unsigned c = 0; c < 4; ++c)
                if (result.valid & (1u << c))
                    QCOMPARE(result.bits[c], ordinary[c]);
            QCOMPARE(original, ordinary);
            QCOMPARE(textures->resources.at(1).storage.bytes, bytes);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
};
QTEST_GUILESS_MAIN(DxbcTextureFeedbackTests)
#include "DxbcTextureFeedbackTests.moc"
