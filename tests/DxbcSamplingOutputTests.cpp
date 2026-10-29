#include "TextureOperationOracle.h"
#include "application/DxbcGpuOperations.h"
#include "core/Dxbc.h"
#include <QDir>
#include <QFile>
#include <bit>
using namespace flora;
using namespace flora::dxbc_exec;
using namespace flora::testing;
using Json = nlohmann::json;
namespace {
using Words = std::vector<uint32_t>;
uint32_t bits(float value) { return std::bit_cast<uint32_t>(value); }
Words row(uint32_t opcode, std::initializer_list<Words> args = {}) {
    Words result{opcode};
    for (const auto &arg : args)
        result.insert(result.end(), arg.begin(), arg.end());
    result[0] |= uint32_t(result.size()) << 24;
    return result;
}
Words annotated(Words operand, uint32_t precision) {
    if (precision) {
        operand[0] |= 0x80000000;
        operand.insert(operand.begin() + 1, 1u | precision << 14);
    }
    return operand;
}
std::vector<uint8_t> shader(uint32_t stage, bool comparison, bool feedback, uint32_t precision,
                            uint32_t sourcePrecision, uint32_t slot, uint32_t mask) {
    const auto output = [&](uint32_t selected) {
        return annotated({0x102002u | selected << 4, slot}, precision);
    };
    std::vector<Words> rows{
        row(106 | (1u << 16)),
        row(89, {{0x208e46, 2, 2}}),
        row(88 | (3u << 11), {{0x1070f2, 3}, {0x5555}}),
        row(90 | (comparison ? 0x800 : 0), {{0x106000, 4}}),
        row(101, {output(15)}),
        row(104, {{1}}),
        row(54, {output(15), {0x4002, bits(.125123f), bits(.375321f), bits(.625432f), bits(.875543f)}})};
    const auto opcode = feedback ? (comparison ? 229u : 228u) : (comparison ? 71u : 72u);
    Words operation{opcode | 0x80000000u, 1u | (15u << 9) | (1u << 13)};
    const auto append = [&](const Words &arg) { operation.insert(operation.end(), arg.begin(), arg.end()); };
    append(output(mask));
    if (feedback)
        append({0x100012, 0});
    append(annotated({0x208e46, 2, 0}, sourcePrecision));
    append({0x107006u | ((comparison ? 0u : 0x1bu) << 4), 3});
    append({0x106000, 4});
    append(annotated({0x20800a, 2, 1}, sourcePrecision));
    operation[0] |= uint32_t(operation.size()) << 24;
    rows.push_back(operation);
    rows.push_back(row(62));
    const std::string type = precision == 1 ? "min16float" : precision == 2 ? "min10float" : "float";
    const auto signature =
        compileHlsl(type + "4 main():" + (stage == 0 ? "SV_Target" + std::to_string(slot) : "TEXCOORD0") +
                        "{return 0;}",
                    stage == 0 ? "ps_5_0" : "vs_5_0")
            .bytecode;
    auto parts = readDxbcParts(signature);
    std::erase_if(parts, [](const auto &part) {
        return part.first == 0x58454853 || part.first == 0x52444853 || part.first == 0x54415453;
    });
    const auto bytes = writeDxbcProgram({{stage << 16 | 0x50, 0}, rows});
    parts.emplace_back(0x58454853, bytes);
    return makeDxbc(parts);
}
Com<ID3D11Device> device(uint32_t vendor) {
    Com<IDXGIAdapter1> selected;
    if (vendor) {
        Com<IDXGIFactory1> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create sampling output factory");
        for (UINT i = 0;; ++i) {
            Com<IDXGIAdapter1> adapter;
            const auto hr = factory->EnumAdapters1(i, &adapter);
            if (hr == DXGI_ERROR_NOT_FOUND)
                break;
            check(hr, "Enumerate sampling output adapters");
            DXGI_ADAPTER_DESC1 desc{};
            check(adapter->GetDesc1(&desc), "Read sampling output adapter");
            if (desc.VendorId == vendor && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                selected = adapter;
                break;
            }
        }
        if (!selected)
            throw std::runtime_error("Requested sampling output adapter is absent");
    }
    Com<ID3D11Device> result;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
    check(D3D11CreateDevice(selected.Get(), vendor ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP, nullptr,
                            D3D11_CREATE_DEVICE_DEBUG, &level, 1, D3D11_SDK_VERSION, &result, nullptr,
                            nullptr),
          "Create sampling output device");
    return result;
}
void save(const QString &path, Bytes bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size()) != qint64(bytes.size()))
        throw std::runtime_error("Cannot save sampling output evidence");
}
void save(const QString &path, const Json &value) {
    const auto text = value.dump(2);
    save(path, Bytes(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
}
} // namespace
class DxbcSamplingOutputTests final : public QObject {
    Q_OBJECT
  private slots:
    void execution_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("stage");
        QTest::addColumn<bool>("comparison");
        QTest::addColumn<uint32_t>("precision");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t stage : {0u, 1u})
                for (bool comparison : {false, true})
                    for (uint32_t precision : {0u, 1u, 2u})
                        QTest::newRow(qPrintable(
                            QString("%1-%2-%3-%4").arg(vendor).arg(stage).arg(comparison).arg(precision)))
                            << vendor << stage << comparison << precision;
    }
    void execution() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, stage);
        QFETCH(bool, comparison);
        QFETCH(uint32_t, precision);
        try {
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
            const auto evidence = qEnvironmentVariable("FLORA_SAMPLING_OUTPUT_EVIDENCE");
            QDir folder(QDir(evidence).filePath(QString::fromLatin1(QTest::currentDataTag())));
            if (!evidence.isEmpty())
                QVERIFY(folder.mkpath("."));
            Json original = Json::array();
            std::map<std::string, std::vector<uint8_t>> programs;
            for (bool feedback : {false, true})
                for (uint32_t slot : stage == 0 ? std::vector<uint32_t>{0, 3, 7} : std::vector<uint32_t>{0})
                    for (uint32_t mask : {15u, 9u, 6u})
                        for (uint32_t sourcePrecision :
                             precision ? std::array{0u, precision} : std::array{1u, 2u}) {
                            const auto name = std::to_string(feedback) + "-" + std::to_string(slot) + "-" +
                                              std::to_string(mask) + "-" + std::to_string(sourcePrecision) +
                                              ".dxbc";
                            auto &code = programs[name];
                            code =
                                shader(stage, comparison, feedback, precision, sourcePrecision, slot, mask);
                            Json parameters = Json::array(), outputs = Json::array();
                            for (unsigned i = 0; i < 4; ++i) {
                                const std::vector<std::array<uint32_t, 4>> constants{
                                    {bits(.49981f), bits(.37513f), 0, 0},
                                    {bits(comparison ? .25f + i * .15f : i * .75f), 0, 0, 0}};
                                outputs.push_back(directTexture(native.Get(), code, stage, input, view,
                                                                constants, precision ? precision : 1,
                                                                &sampler, 1, 0, slot));
                                parameters.push_back(constants);
                            }
                            original.push_back({{"shader", name},
                                                {"feedback", feedback},
                                                {"slot", slot},
                                                {"mask", mask},
                                                {"source_precision", sourcePrecision},
                                                {"parameters", parameters},
                                                {"outputs", outputs}});
                            if (!evidence.isEmpty())
                                save(folder.filePath(QString::fromStdString(name)), Bytes(code));
                        }
            if (!evidence.isEmpty())
                save(folder.filePath("original.json"), original);
            Json results = Json::array();
            for (const auto &record : original) {
                const Program program(programs.at(record["shader"].get<std::string>()));
                const uint32_t slot = record["slot"], mask = record["mask"];
                const auto execute = [&](unsigned index, unsigned unknown) {
                    Inputs inputs;
                    inputs.gpu = gpu;
                    inputs.textures = textures;
                    for (const auto &value : record["parameters"][index])
                        inputs.constantBuffers[2].push_back(
                            Value::known(value.get<std::array<uint32_t, 4>>()));
                    if (unknown)
                        inputs.constantBuffers[2][unknown - 1].valid &= uint8_t(~1u);
                    Lane lane(program, inputs);
                    while (!lane.finished())
                        lane.step();
                    return lane.value({2, {slot}});
                };
                Json outputs = Json::array(), unknown = Json::array();
                for (unsigned i = 0; i < 4; ++i) {
                    const auto value = execute(i, 0);
                    QCOMPARE(value.valid, uint8_t(15));
                    QCOMPARE(value.bits, (record["outputs"][i].get<std::array<uint32_t, 4>>()));
                    outputs.push_back(value.bits);
                }
                for (unsigned selected : {1u, 2u}) {
                    const auto value = execute(0, selected);
                    QCOMPARE(value.valid, uint8_t(15 ^ mask));
                    for (unsigned c = 0; c < 4; ++c)
                        if (value.valid & (1u << c))
                            QCOMPARE(value.bits[c], record["outputs"][0][c].get<uint32_t>());
                    unknown.push_back({{"bits", value.bits}, {"valid", value.valid}});
                }
                results.push_back({{"shader", record["shader"]}, {"native", outputs}, {"unknown", unknown}});
            }
            QCOMPARE(textures->resources.at(1).storage.bytes, storage);
            if (!evidence.isEmpty())
                save(folder.filePath("report.json"), Json{{"adapter", gpu->identity()},
                                                          {"stage", stage},
                                                          {"comparison", comparison},
                                                          {"precision", precision},
                                                          {"results", results}});
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
};
QTEST_GUILESS_MAIN(DxbcSamplingOutputTests)
#include "DxbcSamplingOutputTests.moc"
