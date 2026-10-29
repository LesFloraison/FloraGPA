#include "TextureOperationOracle.h"
#include "application/DxbcCheckpointModel.h"
#include "application/DxbcGpuOperations.h"
#include <QDir>
#include <QFile>
#include <bit>
using namespace flora;
using namespace flora::dxbc_exec;
using namespace flora::testing;
using Json = nlohmann::json;
namespace {
Com<ID3D11Device> device(uint32_t vendor) {
    Com<IDXGIAdapter1> selected;
    if (vendor) {
        Com<IDXGIFactory1> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create explicit sampling adapter factory");
        for (UINT i = 0;; ++i) {
            Com<IDXGIAdapter1> adapter;
            const auto hr = factory->EnumAdapters1(i, &adapter);
            if (hr == DXGI_ERROR_NOT_FOUND)
                break;
            check(hr, "Enumerate explicit sampling adapters");
            DXGI_ADAPTER_DESC1 desc{};
            check(adapter->GetDesc1(&desc), "Read explicit sampling adapter");
            if (desc.VendorId == vendor && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                selected = adapter;
                break;
            }
        }
        if (!selected)
            throw std::runtime_error("Requested explicit sampling adapter is absent");
    }
    Com<ID3D11Device> result;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
    check(D3D11CreateDevice(selected.Get(), vendor ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP, nullptr,
                            D3D11_CREATE_DEVICE_DEBUG, &level, 1, D3D11_SDK_VERSION, &result, nullptr,
                            nullptr),
          "Create explicit sampling device");
    return result;
}
void save(const QString &path, Bytes bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size()) != qint64(bytes.size()))
        throw std::runtime_error("Cannot save explicit sampling evidence");
}
void save(const QString &path, const Json &value) {
    const auto text = value.dump(2);
    save(path, Bytes(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
}
uint32_t bits(float value) { return std::bit_cast<uint32_t>(value); }
} // namespace
class DxbcExplicitSamplingPrecisionTests final : public QObject {
    Q_OBJECT
  private slots:
    void execution_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("stage");
        QTest::addColumn<bool>("comparison");
        QTest::addColumn<bool>("feedback");
        QTest::addColumn<unsigned>("precision");
        QTest::addColumn<unsigned>("scenario");
        for (const uint32_t vendor : {0u, 4318u, 32902u})
            for (const uint32_t stage : {0u, 1u, 5u})
                for (const bool comparison : {false, true})
                    for (const bool feedback : {false, true})
                        for (const unsigned precision : {1u, 2u})
                            for (const unsigned scenario : {1u, 2u, 3u, 4u})
                                QTest::newRow(qPrintable(QString("%1-%2-%3-%4-%5-%6")
                                                             .arg(vendor)
                                                             .arg(stage)
                                                             .arg(comparison)
                                                             .arg(feedback)
                                                             .arg(precision)
                                                             .arg(scenario)))
                                    << vendor << stage << comparison << feedback << precision << scenario;
    }
    void execution() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, stage);
        QFETCH(bool, comparison);
        QFETCH(bool, feedback);
        QFETCH(unsigned, precision);
        QFETCH(unsigned, scenario);
        try {
            const std::string reduced = precision == 1 ? "min16float" : "min10float";
            const auto dataType = scenario == 1 || scenario == 4 ? reduced : "float";
            const auto coordinateType = scenario == 2 || scenario == 4 ? reduced : "float";
            const auto parameterType = scenario == 3 || scenario == 4 ? reduced : "float";
            std::string source = "Texture2D<" + dataType + "4> tex:register(t3);";
            source += comparison ? "SamplerComparisonState s:register(s4);" : "SamplerState s:register(s4);";
            source += "cbuffer C:register(b2){uint4 parameters;}";
            source +=
                stage == 5
                    ? "RWStructuredBuffer<uint4> outputData:register(u3);[numthreads(1,1,1)]void main(){"
                : stage == 0 ? "uint4 main():SV_Target{"
                             : "uint4 main():TEXCOORD0{";
            source += coordinateType + "2 uv=" + coordinateType + "2(asfloat(parameters.yz))*" +
                      coordinateType + "2(1.0007,0.9991);";
            source += parameterType + " p=" + parameterType + "(asfloat(parameters.x))*" + parameterType +
                      "(1.0007);";
            source += "uint status;" + dataType + "4 sampled=tex.";
            source += comparison ? "SampleCmpLevelZero(s,uv,p,int2(-1,1)" : "SampleLevel(s,uv,p,int2(-1,1)";
            source += feedback ? ",status).xxxx;" : ").xxxx;";
            source += "float4 widened=sampled*" + dataType + "(1.0007);uint4 result=asuint(widened);";
            if (feedback)
                source += "result.w^=CheckAccessFullyMapped(status)?0:0xffffffff;";
            source += stage == 5 ? "outputData[0]=result;}" : "return result;}";
            const auto code = compileHlsl(source, stage == 5   ? "cs_5_0"
                                                  : stage == 0 ? "ps_5_0"
                                                               : "vs_5_0")
                                  .bytecode;
            const unsigned opcode = feedback ? (comparison ? 229u : 228u) : (comparison ? 71u : 72u);
            // FXC leaves CmpLz itself at full precision when only its result is
            // reduced; the downstream arithmetic still carries minimum precision.
            const std::array<uint32_t, 3> expectedPrecision{
                (scenario == 1 && !comparison) || scenario == 4 ? precision : 0,
                scenario == 2 || scenario == 4 ? precision : 0,
                scenario == 3 || scenario == 4 ? precision : 0};
            unsigned instructions = 0;
            for (const auto &[tag, bytes] : readDxbcParts(code)) {
                if (tag != 0x58454853 && tag != 0x52444853)
                    continue;
                for (const auto &row : readDxbcProgram(bytes).instructions) {
                    if ((row[0] & 2047) != opcode)
                        continue;
                    ++instructions;
                    const auto operands = checkpoint::instructionOperands(row);
                    const auto precisionAt = [&](size_t index) {
                        const auto first = operands.ranges.at(index).first;
                        return row[first] >> 31 ? (row.at(first + 1) >> 14) & 7 : 0u;
                    };
                    QCOMPARE((std::array<uint32_t, 3>{precisionAt(0), precisionAt(1 + feedback),
                                                      precisionAt(4 + feedback)}),
                             expectedPrecision);
                }
            }
            QCOMPARE(instructions, 1u);
            auto native = device(vendor);
            auto gpu = std::make_shared<GpuOperations>(native.Get(), !vendor);
            auto textures = std::make_shared<TextureInputs>();
            TextureInput input;
            input.resource.id = 1;
            input.resource.type = 0x85;
            input.resource.desc = {4, 4, 3, 1, DXGI_FORMAT_R32_FLOAT, 1, 0, 0, D3D11_BIND_SHADER_RESOURCE,
                                   0, 0};
            input.subresources = textureSubresources(input.resource);
            std::vector<uint8_t> storage(84);
            for (const auto &sub : input.subresources)
                for (size_t at = sub.offset; at < sub.offset + sub.size; at += 4) {
                    const auto value = bits(.250123f + .17321f * sub.mip + .00437f * float(at / 4));
                    std::memcpy(storage.data() + at, &value, 4);
                }
            input.storage = Buffer::known(storage);
            textures->resources.emplace(1, input);
            TextureInputView view;
            view.resource = 1;
            view.format = DXGI_FORMAT_R32_FLOAT;
            view.dimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            view.mipCount = 3;
            view.nonTiled = true;
            D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.Format = DXGI_FORMAT_R32_FLOAT;
            desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            desc.Texture2D.MipLevels = 3;
            view.nativeDescriptor.resize(sizeof desc / 4);
            std::memcpy(view.nativeDescriptor.data(), &desc, sizeof desc);
            textures->views.emplace(Register{7, {3}}, view);
            D3D11_SAMPLER_DESC sd{};
            sd.Filter =
                comparison ? D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
            sd.MaxAnisotropy = 1;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            SamplerInput sampler;
            sampler.descriptor.emplace();
            std::memcpy(sampler.descriptor->data(), &sd, sizeof sd);
            textures->samplers.emplace(Register{6, {4}}, sampler);
            std::array<std::array<uint32_t, 4>, 4> parameters, originals;
            for (unsigned i = 0; i < 4; ++i) {
                parameters[i] = {bits(comparison ? .25f + i * .15f : i * .75f), bits(.49981f), bits(.37513f),
                                 0};
                originals[i] =
                    directTexture(native.Get(), code, stage, input, view, {parameters[i]}, 0, &sampler);
            }
            const auto evidence = qEnvironmentVariable("FLORA_EXPLICIT_PRECISION_EVIDENCE");
            QDir folder(QDir(evidence).filePath(QString::fromLatin1(QTest::currentDataTag())));
            if (!evidence.isEmpty()) {
                QVERIFY(folder.mkpath("."));
                save(folder.filePath("shader.dxbc"), Bytes(code));
                save(folder.filePath("original.json"), Json(originals));
            }
            const Program program(code);
            const auto execute = [&](Value parameter) {
                Inputs inputs;
                inputs.gpu = gpu;
                inputs.textures = textures;
                inputs.constantBuffers[2] = {parameter};
                inputs.memory = std::make_shared<Memory>(
                    std::map<uint64_t, Buffer>{{9, Buffer::known(std::vector<uint8_t>(16))}},
                    std::map<Register, BufferView>{{{30, {3}}, {9, 0, 16, 16}}});
                Lane lane(program, inputs);
                while (!lane.finished())
                    lane.step();
                if (stage != 5)
                    return lane.value({2, {0}});
                const auto &buffer = inputs.memory->buffers().at(9);
                Value result;
                std::memcpy(result.bits.data(), buffer.bytes.data(), 16);
                for (unsigned c = 0; c < 4; ++c)
                    if (std::all_of(buffer.valid.begin() + c * 4, buffer.valid.begin() + c * 4 + 4,
                                    [](auto v) { return v == 1; }))
                        result.valid |= uint8_t(1u << c);
                return result;
            };
            Json actual = Json::array(), reversed = Json::array(), unknown = Json::array();
            for (unsigned i = 0; i < 4; ++i) {
                const auto value = execute(Value::known(parameters[i]));
                QCOMPARE(value.valid, uint8_t(15));
                QCOMPARE(value.bits, originals[i]);
                actual.push_back(value.bits);
            }
            for (unsigned i = 0; i < 4; ++i) {
                const auto value = execute(Value::known(parameters[3 - i]));
                QCOMPARE(value.valid, uint8_t(15));
                QCOMPARE(value.bits, originals[3 - i]);
                reversed.push_back(value.bits);
            }
            for (const uint8_t invalid : {1u, 2u}) {
                auto parameter = Value::known(parameters[0]);
                parameter.valid &= ~invalid;
                const auto value = execute(parameter);
                QCOMPARE(value.valid, uint8_t(0));
                unknown.push_back(value.valid);
            }
            QCOMPARE(textures->resources.at(1).storage.bytes, storage);
            if (!evidence.isEmpty())
                save(folder.filePath("report.json"), Json{{"adapter", gpu->identity()},
                                                          {"stage", stage},
                                                          {"comparison", comparison},
                                                          {"feedback", feedback},
                                                          {"precision", expectedPrecision},
                                                          {"opcode", opcode},
                                                          {"parameters", parameters},
                                                          {"original", originals},
                                                          {"native", actual},
                                                          {"reversed", reversed},
                                                          {"unknown_validity", unknown}});
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
};
QTEST_GUILESS_MAIN(DxbcExplicitSamplingPrecisionTests)
#include "DxbcExplicitSamplingPrecisionTests.moc"
