#include "StateCapture.h"
#include "application/DxbcGpuOperations.h"
#include "application/DxbcInputSnapshot.h"
#include "application/DxbcTextureInputs.h"
#include "application/HlslCompilation.h"
#include "core/Dxbc.h"
#include "replay/Replay.h"
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <bit>
using namespace flora;
using namespace flora::dxbc_exec;
using Json = nlohmann::json;
namespace {
using Words = std::vector<uint32_t>;
#ifdef FLORA_SAMPLE_GRAD_TESTS
constexpr bool gradient = true;
#else
constexpr bool gradient = false;
#endif
constexpr uint32_t samplingOpcode = gradient ? 73 : 72;
const QString shaderPrefix = gradient ? "sample-grad" : "sample-level";
Words row(uint32_t opcode, std::initializer_list<Words> args = {}) {
    Words result{opcode};
    for (const auto &arg : args)
        result.insert(result.end(), arg.begin(), arg.end());
    result[0] |= uint32_t(result.size()) << 24;
    return result;
}
std::vector<uint8_t> program(uint32_t stage, unsigned offset, uint32_t mask = 15, bool saturate = false,
                             uint32_t dimension = 3, bool separateLod = false) {
    const uint32_t target = stage == 5 ? 0x100002 : 0x102002;
    std::vector<Words> rows{row(89, {{0x208e46, 0,
                                      gradient      ? 3u
                                      : separateLod ? 2u
                                                    : 1u}}),
                            row(88 | (dimension << 11), {{0x1070f2, 0}, {0x5555}}), row(90, {{0x106000, 3}})};
    if (stage == 5) {
        rows.push_back(row(104, {{1}}));
        rows.push_back(row(155, {{1, 1, 1}}));
    } else
        rows.push_back(row(101, {{target | 0xf0, 0}}));
    rows.push_back(row(54, {{target | 0xf0, 0}, {0x4002, 7, 8, 9, 10}}));
    const uint32_t control = offset ? 1 | (15 << 9) | (1 << 13) : 1;
    if (gradient)
        rows.push_back(row(73 | 0x80000000 | (saturate ? 0x2000u : 0u), {{control},
                                                                         {target | (mask << 4), 0},
                                                                         {0x208e46, 0, 0},
                                                                         {0x1071b6, 0},
                                                                         {0x106000, 3},
                                                                         {0x208e46, 0, 1},
                                                                         {0x208e46, 0, 2}}));
    else
        rows.push_back(row(72 | 0x80000000 | (saturate ? 0x2000u : 0u),
                           {{control},
                            {target | (mask << 4), 0},
                            {0x208e46, 0, 0},
                            {0x1071b6, 0},
                            {0x106000, 3},
                            separateLod ? Words{0x20800a, 0, 1} : Words{0x20802a, 0, 0}}));
    rows.push_back(row(62));
    return makeDxbc({{0x58454853, writeDxbcProgram({{(stage << 16) | 0x50, 0}, rows})}});
}
Com<ID3D11Device> device(uint32_t vendor) {
    Com<IDXGIAdapter1> selected;
    if (vendor) {
        Com<IDXGIFactory1> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create sampling test factory");
        for (UINT index = 0;; ++index) {
            Com<IDXGIAdapter1> candidate;
            const auto hr = factory->EnumAdapters1(index, &candidate);
            if (hr == DXGI_ERROR_NOT_FOUND)
                break;
            check(hr, "Enumerate sampling test adapter");
            DXGI_ADAPTER_DESC1 desc{};
            check(candidate->GetDesc1(&desc), "Read sampling test adapter");
            if (desc.VendorId == vendor && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                selected = candidate;
                break;
            }
        }
        if (!selected)
            throw std::runtime_error("Requested sampling test adapter unavailable");
    }
    Com<ID3D11Device> result;
    check(D3D11CreateDevice(selected.Get(), vendor ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP, nullptr,
                            D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &result, nullptr,
                            nullptr),
          "Create sampling test device");
    return result;
}
Inputs inputs(const Json &item, std::shared_ptr<GpuOperations> gpu) {
    Inputs result;
    result.gpu = std::move(gpu);
    result.textures = std::make_shared<TextureInputs>();
    auto &texture = result.textures->resources[1];
    texture.resource.id = 1;
    texture.resource.type = 0x85;
    texture.resource.desc = {
        8, 8, 4, 1, 2, 1, 0, 0, 8, 0, item["resource_min_lod"].get<float>() ? 0x80u : 0u};
    texture.minLod = item["resource_min_lod"].get<float>();
    texture.subresources = textureSubresources(texture.resource);
    std::vector<uint8_t> bytes;
    for (unsigned mip = 0; mip < 4; ++mip)
        for (unsigned y = 0; y < (8u >> mip); ++y)
            for (unsigned x = 0; x < (8u >> mip); ++x) {
                const std::array<float, 4> texel{float(mip) + .25f, float(x) / 8, float(y) / 8, 1};
                const auto *begin = reinterpret_cast<const uint8_t *>(texel.data());
                bytes.insert(bytes.end(), begin, begin + 16);
            }
    texture.storage = Buffer::known(std::move(bytes));
    auto &view = result.textures->views[{7, {0}}];
    if (item["resource_bound"].get<bool>()) {
        view.resource = 1;
        view.format = 2;
        view.dimension = 4;
        view.firstMip = item["first_mip"].get<uint32_t>();
        view.mipCount = item["mip_count"].get<uint32_t>();
        view.nativeDescriptor = {2, 4, view.firstMip, view.mipCount, 0, 0};
    }
    auto &sampler = result.textures->samplers[{6, {3}}];
    if (!item["observed_sampler_descriptor"].is_null()) {
        sampler.object = 4;
        sampler.descriptor = item["observed_sampler_descriptor"].get<std::array<uint32_t, 13>>();
    }
    return result;
}
std::vector<uint8_t> readBytes(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read sampling test evidence");
    const auto bytes = file.readAll();
    return {bytes.begin(), bytes.end()};
}
testing::Capture shapeCapture(uint32_t dimension, uint32_t format) {
    using namespace testing;
    Capture capture;
    capture.add(1, 5, 0x127, std::vector<uint8_t>(24));
    const bool cube = dimension == 6 || dimension == 10;
    const auto layers = dimension == 10                    ? 12u
                        : dimension == 6                   ? 6u
                        : dimension == 7 || dimension == 8 ? 3u
                                                           : 1u;
    Resource resource{};
    resource.type = dimension == 2 || dimension == 7 ? 0x84 : dimension == 5 ? 0x86 : 0x85;
    if (resource.type == 0x84)
        resource.desc = {8, 3, layers, format, 0, 8, 0, 0};
    else if (resource.type == 0x86)
        resource.desc = {8, 8, 4, 3, format, 0, 8, 0, 0};
    else
        resource.desc = {8, 8, 3, layers, format, 1, 0, 0, 8, 0, cube ? 4u : 0u};
    auto description = statePack(Id(0), Id(0));
    for (auto word : resource.desc)
        append(description, word);
    append(description, Id(5));
    capture.add(4, 5, resource.type, description);
    std::vector<uint8_t> texels;
    for (const auto &sub : textureSubresources(resource))
        for (unsigned z = 0; z < sub.depth; ++z)
            for (unsigned y = 0; y < sub.height; ++y)
                for (unsigned x = 0; x < sub.width; ++x) {
                    if (format == 2) {
                        append(texels, float(sub.mip) + .25f);
                        append(texels, float(sub.layer) / 16);
                        append(texels, float(x) / 8);
                        append(texels, float(y + z) / 8);
                    } else {
                        append(texels, uint8_t(sub.mip * 32 + 16));
                        append(texels, uint8_t(sub.layer * 16));
                        append(texels, uint8_t(x * 16));
                        append(texels, uint8_t((y + z) * 16));
                    }
                }
    auto raw = statePack(uint32_t(texels.size()));
    raw.insert(raw.end(), texels.begin(), texels.end());
    capture.add(5, 9, 1, raw);
    const uint32_t native = dimension == 2   ? 2
                            : dimension == 3 ? 4
                            : dimension == 5 ? 8
                            : dimension == 6 ? 9
                            : dimension == 7 ? 3
                            : dimension == 8 ? 5
                                             : 10;
    const uint32_t firstLayer = dimension == 10 ? 6 : dimension == 7 || dimension == 8 ? 1 : 0;
    const uint32_t count = dimension == 10 ? 1 : dimension == 7 || dimension == 8 ? 2 : 0;
    capture.add(6, 5, 0x8c, statePack(Id(0), Id(0), Id(4), format, native, 1u, 2u, firstLayer, count));
    std::vector<float> values{.3f, .4f, .6f, 0, .75f, 0, 0, 0};
    if (gradient)
        values = {.3f, .4f, .6f, 0, .25f, .125f, .0625f, 7, -.125f, .25f, .03125f, -9};
    if (dimension == 7)
        values[1] = 1;
    if (dimension == 8)
        values[2] = 1;
    if (cube) {
        values[0] = 1;
        values[1] = .2f;
        values[2] = -.3f;
    }
    capture.add(2, 5, 0x83,
                statePack(Id(0), Id(0),
                          D3D11_BUFFER_DESC{UINT(values.size() * 4), D3D11_USAGE_DEFAULT, 4, 0, 0, 0},
                          Id(3)));
    auto constants = statePack(uint32_t(values.size() * 4));
    for (float value : values)
        append(constants, value);
    capture.add(3, 9, 1, constants);
    capture.add(7, 5, 0x83,
                statePack(Id(0), Id(0),
                          D3D11_BUFFER_DESC{16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                                            D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16},
                          Id(8)));
    capture.add(8, 9, 1, statePack(16u, std::array<uint32_t, 4>{}));
    D3D11_UNORDERED_ACCESS_VIEW_DESC outputView{};
    outputView.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    outputView.Buffer.NumElements = 1;
    capture.add(9, 5, 0x8f, statePack(Id(0), Id(0), Id(7), outputView));
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler.MipLODBias = .125f;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    capture.add(60, 5, 0x88, statePack(Id(0), Id(0), sampler));
    const std::string shape = dimension == 2   ? "Texture1D"
                              : dimension == 3 ? "Texture2D"
                              : dimension == 5 ? "Texture3D"
                              : dimension == 6 ? "TextureCube"
                              : dimension == 7 ? "Texture1DArray"
                              : dimension == 8 ? "Texture2DArray"
                                               : "TextureCubeArray";
    const std::string coords = dimension == 2                     ? "p.x"
                               : dimension == 3 || dimension == 7 ? "p.xy"
                               : dimension == 10                  ? "p"
                                                                  : "p.xyz";
    const std::string components = dimension == 2 || dimension == 7   ? ".x"
                                   : dimension == 3 || dimension == 8 ? ".xy"
                                                                      : ".xyz";
    const std::string sampling = gradient ? ",lod" + components + ",dy" + components : ",lod.x";
    const auto shader =
        compileHlsl("cbuffer Params:register(b0){float4 p;float4 lod;float4 dy;}" + shape +
                        "<float4> src:register(t0);SamplerState samp:register(s3);"
                        "RWStructuredBuffer<uint4> dst:register(u0);[numthreads(1,1,1)]void main(){"
                        "dst[0]=asuint(src." +
                        std::string(gradient ? "SampleGrad" : "SampleLevel") + "(samp," + coords + sampling +
                        ").wzyx);}",
                    "cs_5_0")
            .bytecode;
    auto object = std::vector<uint8_t>(48);
    append(object, Id(16));
    capture.add(15, 5, 0x93, object);
    raw = statePack(uint64_t(shader.size()));
    raw.insert(raw.end(), shader.begin(), shader.end());
    append(raw, Id(0));
    capture.add(16, 9, 0x81, raw);
    State state{};
    state.stages[5].shader = 15;
    state.stages[5].srv[0] = 6;
    state.stages[5].cb[0] = 2;
    state.stages[5].samplers[3] = 60;
    state.csCount = 1;
    state.csUav[0] = 9;
    state.sampleMask = UINT32_MAX;
    capture.add(17, 3, 3, snapshot(state));
    capture.add(100, 7, 0x35, statePack(Id(17), Id(0), Id(1), 1u, 1u, 1u));
    return capture;
}
} // namespace
class DxbcSampleLevelTests final : public QObject {
    Q_OBJECT
    QTemporaryDir evidence;
    std::map<uint32_t, Json> references;
    std::map<std::pair<uint32_t, uint32_t>, Json> laneReferences;
  private slots:
    void initTestCase() {
        for (uint32_t vendor : {0u, 4318u, 32902u}) {
            const auto directory = evidence.filePath(QString::number(vendor));
            QProcess process;
            QStringList arguments{directory, QString::number(vendor)};
            if (gradient)
                arguments.push_back("--grad");
            process.start(QCoreApplication::applicationDirPath() + "/FloraSampleLevelProbe.exe", arguments);
            if (!process.waitForFinished(60000)) {
                process.kill();
                process.waitForFinished(5000);
                QFAIL("Independent sample-level probe timed out");
            }
            QVERIFY2(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
                     process.readAllStandardError().constData());
            const auto report = readBytes(directory + "/results.json");
            references[vendor] = Json::parse(report);
            QCOMPARE(references[vendor]["validation_errors"].get<unsigned>(), 0u);
            if (gradient)
                for (uint32_t stage : {0u, 1u, 5u}) {
                    const QString name = stage == 0 ? "ps" : stage == 1 ? "vs" : "cs";
                    const auto path = evidence.filePath(QString("%1-%2-single").arg(vendor).arg(name));
                    process.start(QCoreApplication::applicationDirPath() + "/FloraSampleLevelProbe.exe",
                                  {path, QString::number(vendor), "--grad", name + "-single"});
                    if (!process.waitForFinished(60000)) {
                        process.kill();
                        process.waitForFinished(5000);
                        QFAIL("Independent graphics sampling probe timed out");
                    }
                    QVERIFY2(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
                             process.readAllStandardError().constData());
                    auto &graphics = laneReferences[{vendor, stage}];
                    graphics = Json::parse(readBytes(path + "/results.json"));
                    QCOMPARE(graphics["validation_errors"].get<unsigned>(), 0u);
                    QCOMPARE(graphics["adapter_luid"], references[vendor]["adapter_luid"]);
                    QCOMPARE(graphics["stage"].get<std::string>(), name.toStdString());
                }
        }
    }
    void hardware_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<unsigned>("configuration");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (unsigned configuration = 0; configuration < 34; ++configuration)
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(configuration)))
                    << vendor << configuration;
    }
    void hardware() {
        QFETCH(uint32_t, vendor);
        QFETCH(unsigned, configuration);
        try {
            const auto &reference = references.at(vendor);
            const auto &item = reference.at("cases").at(configuration);
            QCOMPARE(item.at("status").get<std::string>(), std::string("observed"));
            auto native = device(vendor);
            auto gpu = std::make_shared<GpuOperations>(native.Get(), !vendor);
            const auto identity = gpu->identity();
            const auto luid = (uint64_t(uint32_t(identity.at("luid_high").get<int32_t>())) << 32) |
                              identity.at("luid_low").get<uint32_t>();
            QCOMPARE(luid, reference.at("adapter_luid").get<uint64_t>());
            auto input = inputs(item, gpu);
            const auto original = input.textures->resources.at(1).storage.bytes;
            uint64_t comparisons = 0;
            bool lanesMatch = true;
            for (uint32_t stage : {0u, 1u, 5u}) {
                const auto &stageItem =
                    gradient ? laneReferences.at({vendor, stage}).at("cases").at(configuration) : item;
                QCOMPARE(stageItem["observed_sampler_descriptor"], item["observed_sampler_descriptor"]);
                const Program code(program(stage, item["offset"].get<unsigned>()));
                for (const auto &observation : stageItem["records"]) {
                    auto parameters = observation["parameters"].get<std::array<float, 4>>();
                    input.constantBuffers[0] = {
                        Value::known(std::bit_cast<std::array<uint32_t, 4>>(parameters))};
                    if (gradient)
                        for (auto key : {"ddx", "ddy"})
                            input.constantBuffers[0].push_back(
                                Value::known(std::bit_cast<std::array<uint32_t, 4>>(
                                    observation[key].get<std::array<float, 4>>())));
                    Lane lane(code, input);
                    lane.step();
                    const auto step = lane.step();
                    QCOMPARE(step.memoryGpuAssisted, item["resource_bound"].get<bool>());
                    auto expected = observation["bits"].get<std::array<uint32_t, 4>>();
                    std::reverse(expected.begin(), expected.end());
                    const auto actual = lane.value({stage == 5 ? 0u : 2u, {0}});
                    if (actual != Value::known(expected))
                        qInfo().noquote() << QString::fromStdString(Json{
                            {"sampling_mismatch", true},
                            {"vendor", vendor},
                            {"configuration", configuration},
                            {"stage", stage},
                            {"input", observation},
                            {"expected", expected},
                            {"actual", actual.bits},
                            {"valid", actual.valid}}.dump());
                    lanesMatch = lanesMatch && actual == Value::known(expected);
                    QCOMPARE(step.changes.size(), size_t(1));
                    ++comparisons;
                }
            }
            // Execute the original compiler-generated CS, including its input
            // selection, branch, resource declarations and output writes.
            std::vector<uint8_t> parameters, expected;
            for (const auto &observation : item["records"]) {
                const auto value = observation["parameters"].get<std::array<float, 4>>();
                auto *first = reinterpret_cast<const uint8_t *>(value.data());
                parameters.insert(parameters.end(), first, first + 16);
                if (gradient)
                    for (auto key : {"ddx", "ddy"}) {
                        const auto derivative = observation[key].get<std::array<float, 4>>();
                        const auto *data = reinterpret_cast<const uint8_t *>(derivative.data());
                        parameters.insert(parameters.end(), data, data + 16);
                    }
                const auto bits = observation["bits"].get<std::array<uint32_t, 4>>();
                first = reinterpret_cast<const uint8_t *>(bits.data());
                expected.insert(expected.end(), first, first + 16);
            }
            input.memory = std::make_shared<Memory>(
                std::map<uint64_t, Buffer>{{2, Buffer::known(parameters)},
                                           {3, Buffer::known(std::vector<uint8_t>(expected.size()))}},
                std::map<Register, BufferView>{{{7, {1}}, {2, 0, parameters.size(), 16}},
                                               {{30, {0}}, {3, 0, expected.size(), 16}}});
            const auto binary = readBytes(evidence.filePath(
                QString("%1/%2-%3.dxbc").arg(vendor).arg(shaderPrefix).arg(item["offset"].get<unsigned>())));
            const Program code(binary);
            const auto beforeGroupOperations = gpu->operations();
            Group group(code, input);
            while (!group.finished())
                group.step();
            if (gradient) {
                QCOMPARE(group.samplingBatches(),
                         item["resource_bound"].get<bool>() ? uint64_t(1) : uint64_t(0));
                QCOMPARE(gpu->operations() - beforeGroupOperations, group.samplingBatches());
            }
            const bool groupMatches = input.memory->buffers().at(3).bytes == expected;
            if (!groupMatches) {
                Json differences = Json::array();
                const auto &actual = input.memory->buffers().at(3).bytes;
                for (size_t offset = 0; offset < expected.size(); offset += 4) {
                    uint32_t a, b;
                    std::memcpy(&a, actual.data() + offset, 4);
                    std::memcpy(&b, expected.data() + offset, 4);
                    if (a != b)
                        differences.push_back({{"byte_offset", offset}, {"actual", a}, {"expected", b}});
                }
                qInfo().noquote() << QString::fromStdString(Json{
                    {"group_sampling_mismatch", true},
                    {"vendor", vendor},
                    {"configuration", configuration},
                    {"differences", differences}}.dump());
            }
            QCOMPARE(input.textures->resources.at(1).storage.bytes, original);
            qInfo().noquote() << QString::fromStdString(Json{
                {"sample_level_verified", !gradient},
                {"sample_grad_verified", gradient},
                {"vendor", vendor},
                {"configuration", configuration},
                {"name", item["name"]},
                {"offset", item["offset"]},
                {"records", item["records"].size()},
                {"lane_comparisons", comparisons},
                {"group_output_verified", groupMatches},
                {"group_sampling_batches", group.samplingBatches()},
                {"group_execution_model", Group::executionModel},
                {"lane_reference_invocations", gradient ? "single" : "batch"},
                {"group_reference_invocations", "batch"},
                {"lane_output_verified", lanesMatch},
                {"reference_stages", gradient ? Json({"ps", "vs", "cs"}) : Json({"cs"})},
                {"original_storage_unchanged", true},
                {"helper", gpu->identity()}}.dump());
            QVERIFY2(lanesMatch, "Native per-stage sampling values differ");
            QVERIFY2(groupMatches, "Native complete CS group output differs");
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
#ifdef FLORA_SAMPLE_GRAD_TESTS
    void batchCancellation_data() {
        QTest::addColumn<uint32_t>("vendor");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            QTest::newRow(qPrintable(QString::number(vendor))) << vendor;
    }
    void batchCancellation() {
        QFETCH(uint32_t, vendor);
        auto native = device(vendor);
        unsigned checks = 0;
        auto gpu = std::make_shared<GpuOperations>(native.Get(), !vendor, [&] { return ++checks == 2; });
        checks = 0;
        auto input = inputs(references.at(vendor).at("cases").at(0), gpu);
        input.constantBuffers[0] = std::vector<Value>(3, Value::known({}));
        const auto binary = program(5, 0);
        auto instructions = readDxbcProgram(readDxbcParts(binary).front().second);
        for (auto &words : instructions.instructions)
            if ((words[0] & 2047) == 155)
                words[1] = 64;
        const Program code(makeDxbc({{ 0x58454853, writeDxbcProgram(instructions) }}));
        const auto original = input.textures->resources.at(1).storage.bytes;
        Group group(code, input);
        for (unsigned i = 0; i < 64; ++i)
            group.step();
        std::vector<std::map<Register, Value>> before;
        for (unsigned i = 0; i < 64; ++i)
            before.push_back(group.lane(i).registers());
        QVERIFY_THROWS_EXCEPTION(OperationCancelled, group.step());
        QCOMPARE(checks, 2u);
        QCOMPARE(group.samplingBatches(), uint64_t(0));
        QCOMPARE(gpu->operations(), uint64_t(0));
        for (unsigned i = 0; i < 64; ++i)
            QCOMPARE(group.lane(i).registers(), before[i]);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, group.step());
        input.gpu = std::make_shared<GpuOperations>(native.Get(), !vendor);
        Group retry(code, input);
        while (!retry.finished())
            retry.step();
        QCOMPARE(retry.samplingBatches(), uint64_t(1));
        QCOMPARE(input.gpu->operations(), uint64_t(1));
        for (unsigned i = 0; i < 64; ++i)
            QCOMPARE(retry.lane(i).value({0, {0}}),
                     Value::known({std::bit_cast<uint32_t>(1.f), 0, 0, std::bit_cast<uint32_t>(.25f)}));
        QCOMPARE(input.textures->resources.at(1).storage.bytes, original);
    }
    void batchValidation() {
        auto input = inputs(references.at(0).at("cases").at(0), {});
        std::vector<SamplingInvocation> invocations(64);
        invocations[0] = {Value::known({}), Value::known({}), Value::known({}), true};
        bool assisted = true;
        auto sample = [&](std::array<uint32_t, 3> size) {
            return input.textures->sampleGradBatch({7, {0}}, {6, {3}}, 3, 0x5555, size, invocations,
                                                   {0, 1, 2, 3}, 15, {}, nullptr, assisted);
        };
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, sample({0, 1, 1}));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, sample({1, 1, 65}));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, sample({32, 1, 1}));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, sample({64, 1, 1}));
        invocations[0].ddx.valid = 1;
        QVERIFY(!sample({64, 1, 1}).has_value());
        QVERIFY(!assisted);
        invocations[63] = invocations[0];
        invocations[0].ddx.valid = 15;
        QVERIFY(!sample({64, 1, 1}).has_value());
        invocations[63].active = false;
        invocations[0].active = false;
        const auto inactive = sample({64, 1, 1});
        QVERIFY(inactive.has_value());
        QVERIFY(std::all_of(inactive->begin(), inactive->end(), [](const auto &v) { return v.valid == 0; }));
        invocations[0].active = true;
        input.textures->views.at({7, {0}}).resource.reset();
        const auto unbound = sample({64, 1, 1});
        QVERIFY(unbound.has_value());
        QCOMPARE(unbound->front(), Value::known({}));
        QVERIFY(!assisted);
    }
    void batch_data() { hardware_data(); }
    void batch() {
        QFETCH(uint32_t, vendor);
        QFETCH(unsigned, configuration);
        const auto &item = references.at(vendor).at("cases").at(configuration);
        auto native = device(vendor);
        auto gpu = std::make_shared<GpuOperations>(native.Get(), !vendor);
        auto input = inputs(item, gpu);
        const auto original = input.textures->resources.at(1).storage.bytes;
        std::vector<SamplingInvocation> invocations(64);
        for (size_t index = 0; index < 63; ++index) {
            const auto &record = item["records"][index];
            auto value = [&](const char *key) {
                return Value::known(
                    std::bit_cast<std::array<uint32_t, 4>>(record[key].get<std::array<float, 4>>()));
            };
            invocations[index] = {value("parameters"), value("ddx"), value("ddy"), true};
        }
        bool assisted = false;
        const std::array<int32_t, 3> offset =
            item["offset"].get<unsigned>() ? std::array<int32_t, 3>{-1, 1, 0} : std::array<int32_t, 3>{};
        const auto result =
            input.textures->sampleGradBatch({7, {0}}, {6, {3}}, 3, 0x5555, {64, 1, 1}, invocations,
                                            {0, 1, 2, 3}, 15, offset, gpu.get(), assisted);
        QVERIFY(result.has_value());
        QCOMPARE(assisted, item["resource_bound"].get<bool>());
        QCOMPARE(result->size(), size_t(64));
        for (size_t index = 0; index < 63; ++index) {
            const auto expected = item["records"][index]["bits"].get<std::array<uint32_t, 4>>();
            if ((*result)[index] != Value::known(expected))
                qInfo().noquote() << QString::fromStdString(Json{
                    {"batch_sampling_mismatch", true},
                    {"vendor", vendor},
                    {"configuration", configuration},
                    {"record", index},
                    {"actual", (*result)[index].bits},
                    { "expected",
                      expected }}.dump());
            QCOMPARE((*result)[index], Value::known(expected));
        }
        QCOMPARE(result->back().valid, uint8_t(0));
        if (assisted)
            QCOMPARE(result->back().bits,
                     (std::array<uint32_t, 4>{0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef}));
        QCOMPARE(input.textures->resources.at(1).storage.bytes, original);
        qInfo().noquote() << QString::fromStdString(Json{
            {"native_sampling_batch_verified", true},
            {"vendor", vendor},
            {"configuration", configuration},
            {"records", 63},
            {"group_size", {64, 1, 1}},
            {"active_lanes", 63},
            { "helper",
              gpu->identity() }}.dump());
    }
#endif
    void shapes_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("dimension");
        QTest::addColumn<uint32_t>("format");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t dimension : {2u, 3u, 5u, 6u, 7u, 8u, 10u})
                for (uint32_t format : {2u, 28u})
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(vendor).arg(dimension).arg(format)))
                        << vendor << dimension << format;
    }
    void shapes() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, dimension);
        QFETCH(uint32_t, format);
        try {
            QTemporaryDir directory;
            const auto path = directory.filePath("sample-shape.gpa_frame");
            shapeCapture(dimension, format).save(path);
            Frame frame(path.toStdWString());
            ReplayOptions options;
            options.before = true;
            options.until = 100;
            options.debug = true;
            options.warp = !vendor;
            if (vendor)
                options.vendor = vendor;
            Replay replay(frame, options);
            const auto observed = snapshotResourceInputs(replay, 5);
            const auto original = replay.readTexture(4);
            options.before = false;
            Replay reference(frame, options);
            reference.run();
            const auto expected = reference.readBuffer(7);
            std::array<uint32_t, 4> bits;
            std::memcpy(bits.data(), expected.data(), 16);
            for (uint32_t stage : {0u, 1u, 5u}) {
                const auto binary = program(stage, 0, 5, false, dimension, true);
                auto instructions = readDxbcProgram(readDxbcParts(binary).front().second);
                // Unused offset axes are ignored by the instruction, not rejected.
                for (auto &words : instructions.instructions)
                    if ((words[0] & 2047) == samplingOpcode) {
                        if (dimension == 2 || dimension == 7)
                            words[1] |= (1 << 13) | (2 << 17);
                        if (dimension == 3 || dimension == 8)
                            words[1] |= 2 << 17;
                    }
                const Program code(makeDxbc({{0x58454853, writeDxbcProgram(instructions)}}));
                Lane lane(code, observed.inputs);
                lane.step();
                QVERIFY(lane.step().memoryGpuAssisted);
                QCOMPARE(lane.value({stage == 5 ? 0u : 2u, {0}}), Value::known({bits[0], 8, bits[2], 10}));
                if (gradient) {
                    auto selected = observed.inputs;
                    const uint8_t required = dimension == 2 || dimension == 7   ? 1
                                             : dimension == 3 || dimension == 8 ? 3
                                                                                : 7;
                    for (unsigned operand : {1u, 2u})
                        selected.constantBuffers[0][operand].valid = required;
                    Lane partial(code, selected);
                    partial.step();
                    QVERIFY(partial.step().memoryGpuAssisted);
                    QCOMPARE(partial.value({stage == 5 ? 0u : 2u, {0}}),
                             Value::known({bits[0], 8, bits[2], 10}));
                    // Reverse and negate the stored derivative, then restore it
                    // with DXBC source selection and a floating-point modifier.
                    selected = observed.inputs;
                    auto &derivative = selected.constantBuffers[0][1].bits;
                    std::swap(derivative[0], derivative[2]);
                    for (auto &component : derivative)
                        component ^= 0x80000000;
                    for (auto &words : instructions.instructions)
                        if ((words[0] & 2047) == samplingOpcode) {
                            words[11] = 0x80208c66;                 // cb0[1].zyxw with an extended operand.
                            words.insert(words.begin() + 12, 0x41); // Negate.
                            words[0] = (words[0] & 0x80ffffff) | (uint32_t(words.size()) << 24);
                        }
                    const Program modified(makeDxbc({{0x58454853, writeDxbcProgram(instructions)}}));
                    Lane swizzled(modified, selected);
                    swizzled.step();
                    QVERIFY(swizzled.step().memoryGpuAssisted);
                    QCOMPARE(swizzled.value({stage == 5 ? 0u : 2u, {0}}),
                             Value::known({bits[0], 8, bits[2], 10}));
                }
            }
            const Program code(observed.bytecode);
            Group group(code, observed.inputs);
            while (!group.finished())
                group.step();
            QCOMPARE(observed.inputs.memory->buffers().at(7).bytes, expected);
            QCOMPARE(replay.readTexture(4), original);
            qInfo().noquote() << QString::fromStdString(Json{
                {"sample_shape_verified", true},
                {"gradient", gradient},
                {"vendor", vendor},
                {"dimension", dimension},
                {"format", format},
                {"frame_sha256", observed.frameSha256},
                {"expected", bits},
                {"masked_stages", {"ps", "vs", "cs"}},
                {"group_output_verified", true}}.dump());
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void cancellation_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("stage");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t stage : {0u, 1u, 5u})
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(stage))) << vendor << stage;
    }
    void cancellation() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, stage);
        auto native = device(vendor);
        unsigned checks = 0;
        auto gpu = std::make_shared<GpuOperations>(native.Get(), !vendor,
                                                   [&] { return ++checks == (stage == 5 ? 2u : 3u); });
        checks = 0; // Exclude construction; cancel at the first post-submission readback check.
        const auto &item = references.at(vendor).at("cases").at(0);
        auto input = inputs(item, gpu);
        const auto original = input.textures->resources.at(1).storage.bytes;
        input.constantBuffers[0] = std::vector<Value>(gradient ? 3 : 1, Value::known({0, 0, 0, 0}));
        const Program code(program(stage, 0));
        Lane lane(code, input);
        lane.step();
        const auto before = lane.value({stage == 5 ? 0u : 2u, {0}});
        QVERIFY_THROWS_EXCEPTION(OperationCancelled, lane.step());
        QCOMPARE(checks, stage == 5 ? 2u : 3u);
        QCOMPARE(lane.value({stage == 5 ? 0u : 2u, {0}}), before);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, lane.step());
        QCOMPARE(gpu->operations(), uint64_t(0));
        input.gpu = std::make_shared<GpuOperations>(native.Get(), !vendor);
        Lane retry(code, input);
        retry.step();
        QVERIFY(retry.step().memoryGpuAssisted);
        QCOMPARE(retry.value({stage == 5 ? 0u : 2u, {0}}),
                 Value::known({std::bit_cast<uint32_t>(1.f), 0, 0, std::bit_cast<uint32_t>(.25f)}));
        QCOMPARE(input.textures->resources.at(1).storage.bytes, original);
    }
    void unknownAndInvalid() {
        const auto &item = references.at(0).at("cases").at(0);
        auto input = inputs(item, {});
        input.constantBuffers[0] = std::vector<Value>(gradient ? 3 : 1, Value::known({0, 0, 0, 0}));
        const Program code(program(5, 0, 5));
        auto checkUnknown = [&] {
            Lane lane(code, input);
            lane.step();
            const auto step = lane.step();
            QVERIFY(!step.memoryGpuAssisted);
            QCOMPARE(lane.value({0, {0}}).valid, uint8_t(10));
            QCOMPARE(lane.value({0, {0}}).bits[1], 8u);
            QCOMPARE(lane.value({0, {0}}).bits[3], 10u);
        };
        input.textures->samplers.clear();
        checkUnknown();
        input.textures->samplers[{6, {3}}] = {};
        input.textures->resources.at(1).storage.valid[0] = 0;
        checkUnknown();
        input.textures->resources.at(1).storage.valid[0] = 1;
        if (gradient) {
            for (unsigned operand : {1u, 2u}) {
                input.constantBuffers[0][operand].valid = 1; // Missing a required derivative component.
                checkUnknown();
                input.constantBuffers[0][operand].valid = 15;
            }
        } else {
            input.constantBuffers[0][0].valid = 3; // Missing LOD.
            checkUnknown();
            input.constantBuffers[0][0].valid = 15;
        }
        Lane missingGpu(code, input);
        missingGpu.step();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, missingGpu.step());
        input.textures->views.at({7, {0}}).resource.reset();
        Lane unbound(code, input);
        unbound.step();
        unbound.step();
        QCOMPARE(unbound.value({0, {0}}), Value::known({0, 8, 0, 10}));
        const Program saturated(program(5, 0, 15, true));
        Lane invalidSaturation(saturated, input);
        invalidSaturation.step();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, invalidSaturation.step());
        // Keep the chunk owner alive while testing malformed sampler declarations.
        const auto original = program(5, 0);
        auto decoded = readDxbcProgram(readDxbcParts(original).front().second);
        auto malformed = decoded;
        malformed.instructions[2][2] = 16;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 Program(makeDxbc({{0x58454853, writeDxbcProgram(malformed)}})));
        malformed = decoded;
        malformed.instructions.insert(malformed.instructions.begin() + 3, malformed.instructions[2]);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 Program(makeDxbc({{0x58454853, writeDxbcProgram(malformed)}})));
    }
};
QTEST_GUILESS_MAIN(DxbcSampleLevelTests)
#include "DxbcSampleLevelTests.moc"
