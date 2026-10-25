#include "application/DxbcGpuOperations.h"
#include "application/DxbcTextureInputs.h"
#include "core/Dxbc.h"
#include "replay/Replay.h"
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#include <bit>
using namespace flora;
using namespace flora::dxbc_exec;
using Json = nlohmann::json;
namespace {
using Words = std::vector<uint32_t>;
Words row(uint32_t opcode, std::initializer_list<Words> args = {}) {
    Words result{opcode};
    for (const auto &arg : args)
        result.insert(result.end(), arg.begin(), arg.end());
    result[0] |= uint32_t(result.size()) << 24;
    return result;
}
Program program(uint32_t stage, unsigned offset, uint32_t mask = 15, uint32_t modifier = 0) {
    const uint32_t target = stage == 5 ? 0x100002 : 0x102002;
    std::vector<Words> rows{row(89, {{0x208e46, 0, 1}}), row(88 | (3 << 11), {{0x1070f2, 0}, {0x5555}}),
                            row(90 | 0x800, {{0x106000, 3}})};
    if (stage == 5) {
        rows.push_back(row(104, {{1}}));
        rows.push_back(row(155, {{1, 1, 1}}));
    } else
        rows.push_back(row(101, {{target | 0xf0, 0}}));
    rows.push_back(row(54, {{target | 0xf0, 0}, {0x4002, 7, 8, 9, 10}}));
    Words reference = modifier ? Words{0x8020802a, 1 | (modifier << 6), 0, 0} : Words{0x20802a, 0, 0};
    const uint32_t control = offset ? 1 | (15 << 9) | (1 << 13) : 1;
    rows.push_back(row(
        71 | 0x80000000,
        {{control}, {target | (mask << 4), 0}, {0x208e46, 0, 0}, {0x107006, 0}, {0x106000, 3}, reference}));
    rows.push_back(row(62));
    return Program(makeDxbc({{0x58454853, writeDxbcProgram({{(stage << 16) | 0x50, 0}, rows})}}));
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

std::vector<uint8_t> read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read comparison sampling evidence");
    const auto bytes = file.readAll();
    return {bytes.begin(), bytes.end()};
}
template <class T> void append(std::vector<uint8_t> &out, const T &value) {
    const auto *data = reinterpret_cast<const uint8_t *>(&value);
    out.insert(out.end(), data, data + sizeof value);
}
Inputs inputs(const Json &item, std::shared_ptr<GpuOperations> gpu) {
    Inputs result;
    result.gpu = std::move(gpu);
    result.textures = std::make_shared<TextureInputs>();
    auto &texture = result.textures->resources[1];
    texture.resource.id = 1;
    texture.resource.type = 0x85;
    texture.minLod = item["resource_min_lod"].get<float>();
    texture.resource.desc = {8, 8, 4, 1, 41, 1, 0, 0, 8, 0, texture.minLod ? 0x80u : 0u};
    texture.subresources = textureSubresources(texture.resource);
    std::vector<uint8_t> data;
    for (unsigned mip = 0; mip < 4; ++mip)
        for (unsigned y = 0; y < (8u >> mip); ++y)
            for (unsigned x = 0; x < (8u >> mip); ++x)
                append(data, float((x + 2 * y + 3 * mip) % 8) / 8);
    texture.storage = Buffer::known(std::move(data));
    auto &view = result.textures->views[{7, {0}}];
    if (item["resource_bound"].get<bool>()) {
        view.resource = 1;
        view.format = 41;
        view.dimension = 4;
        view.firstMip = item["first_mip"].get<uint32_t>();
        view.mipCount = item["mip_count"].get<uint32_t>();
        view.nativeDescriptor = {41, 4, view.firstMip, view.mipCount, 0, 0};
    }
    auto &sampler = result.textures->samplers[{6, {3}}];
    if (!item["observed_sampler_descriptor"].is_null()) {
        sampler.object = 4;
        sampler.descriptor = item["observed_sampler_descriptor"].get<std::array<uint32_t, 13>>();
    }
    return result;
}
} // namespace
class DxbcSampleComparisonTests final : public QObject {
    Q_OBJECT
    QTemporaryDir evidence;
    std::map<std::pair<uint32_t, uint32_t>, Json> references;
  private slots:
    void initTestCase() {
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t stage : {0u, 1u, 5u}) {
                const QString name = stage == 0 ? "ps" : stage == 1 ? "vs" : "cs";
                Json batch;
                for (const QString mode : {QString("batch"), QString("single")}) {
                    const auto directory =
                        evidence.filePath(QString("%1-%2-%3").arg(vendor).arg(name).arg(mode));
                    QProcess probe;
                    probe.start(
                        QCoreApplication::applicationDirPath() + "/FloraSampleCompareProbe.exe",
                        {directory, QString::number(vendor), mode == "single" ? name + "-single" : name});
                    if (!probe.waitForFinished(60000)) {
                        probe.kill();
                        probe.waitForFinished(5000);
                        QFAIL("Independent comparison sampling probe timed out");
                    }
                    QVERIFY2(probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0,
                             probe.readAllStandardError().constData());
                    const auto report = Json::parse(read(directory + "/results.json"));
                    QCOMPARE(report["cases"].size(), size_t(56));
                    QCOMPARE(report["validation_errors"].get<unsigned>(), 0u);
                    if (mode == "batch")
                        batch = report;
                    else {
                        QCOMPARE(report["adapter_luid"], batch["adapter_luid"]);
                        for (size_t i = 0; i < 56; ++i)
                            if (batch["cases"][i]["status"] == "observed")
                                QCOMPARE(report["cases"][i]["records"], batch["cases"][i]["records"]);
                    }
                }
                references[{vendor, stage}] = std::move(batch);
            }
    }
    void hardware_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<unsigned>("configuration");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (unsigned configuration = 0; configuration < 56; ++configuration)
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(configuration)))
                    << vendor << configuration;
    }
    void hardware() {
        QFETCH(uint32_t, vendor);
        QFETCH(unsigned, configuration);
        try {
            auto native = device(vendor);
            auto gpu = std::make_shared<GpuOperations>(native.Get(), !vendor);
            uint64_t comparisons = 0, rejections = 0;
            for (uint32_t stage : {0u, 1u, 5u}) {
                const auto &report = references.at({vendor, stage}), &item = report["cases"][configuration];
                const auto id = gpu->identity();
                QCOMPARE((uint64_t(uint32_t(id["luid_high"].get<int32_t>())) << 32) |
                             id["luid_low"].get<uint32_t>(),
                         report["adapter_luid"].get<uint64_t>());
                auto input = inputs(item, gpu);
                const auto original = input.textures->resources.at(1).storage;
                const auto code = program(stage, item["offset"].get<unsigned>());
                if (item["status"] != "observed") {
                    QCOMPARE(item["status"].get<std::string>(),
                             std::string("undefined_missing_comparison_sampler"));
                    input.constantBuffers[0] = {Value::known({})};
                    Lane lane(code, input);
                    lane.step();
                    QVERIFY_THROWS_EXCEPTION(std::runtime_error, lane.step());
                    ++rejections;
                    continue;
                }
                for (const auto &record : item["records"]) {
                    input.constantBuffers[0] = {
                        Value::known(record["parameter_bits"].get<std::array<uint32_t, 4>>())};
                    Lane lane(code, input);
                    lane.step();
                    const auto step = lane.step();
                    const auto actual = lane.value({stage == 5 ? 0u : 2u, {0}});
                    const auto expected = Value::known(record["bits"].get<std::array<uint32_t, 4>>());
                    if (actual != expected)
                        qInfo().noquote() << QString::fromStdString(Json{
                            {"comparison_mismatch", true},
                            {"vendor", vendor},
                            {"stage", stage},
                            {"configuration", configuration},
                            {"input", record["parameter_bits"]},
                            {"expected", expected.bits},
                            {"actual", actual.bits},
                            {"valid", actual.valid}}.dump());
                    QCOMPARE(actual, expected);
                    QCOMPARE(step.memoryGpuAssisted, item["resource_bound"].get<bool>());
                    ++comparisons;
                }
                QCOMPARE(input.textures->resources.at(1).storage.bytes, original.bytes);
                QCOMPARE(input.textures->resources.at(1).storage.valid, original.valid);
                if (stage == 5) {
                    std::vector<uint8_t> parameters, expected;
                    for (const auto &record : item["records"]) {
                        append(parameters, record["parameter_bits"].get<std::array<uint32_t, 4>>());
                        append(expected, record["bits"].get<std::array<uint32_t, 4>>());
                    }
                    input.memory = std::make_shared<Memory>(
                        std::map<uint64_t, Buffer>{{2, Buffer::known(parameters)},
                                                   {3, Buffer::known(std::vector<uint8_t>(expected.size()))}},
                        std::map<Register, BufferView>{{{7, {1}}, {2, 0, parameters.size(), 16}},
                                                       {{30, {0}}, {3, 0, expected.size(), 16}}});
                    const Program compilerCode(
                        read(evidence.filePath(QString("%1-cs-batch/sample-compare-%2.dxbc")
                                                   .arg(vendor)
                                                   .arg(item["offset"].get<unsigned>()))));
                    Group group(compilerCode, input);
                    while (!group.finished())
                        group.step();
                    QCOMPARE(input.memory->buffers().at(3).bytes, expected);
                    QCOMPARE(input.memory->buffers().at(3).valid, std::vector<uint8_t>(expected.size(), 1));
                    QCOMPARE(input.memory->buffers().at(2).bytes, parameters);
                }
            }
            qInfo().noquote() << QString::fromStdString(Json{
                {"sample_comparison_verified", true},
                {"vendor", vendor},
                {"configuration", configuration},
                {"comparisons", comparisons},
                {"undefined_rejections", rejections},
                {"complete_group_verified", rejections == 0},
                {"helper", gpu->identity()}}.dump());
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void modifiersAndMasks_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("stage");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t stage : {0u, 1u, 5u})
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(stage))) << vendor << stage;
    }
    void modifiersAndMasks() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, stage);
        auto native = device(vendor);
        auto gpu = std::make_shared<GpuOperations>(native.Get(), !vendor);
        auto input = inputs(references.at({vendor, stage})["cases"][6], gpu);
        input.constantBuffers[0] = {Value::known(
            std::bit_cast<std::array<uint32_t, 4>>(std::array<float, 4>{.25f, .375f, -.25f, 0}))};
        input.constantBuffers[0][0].valid = 7;
        for (uint32_t modifier : {0u, 1u, 2u, 3u})
            for (uint32_t mask : {1u, 5u, 15u}) {
                const auto code = program(stage, 0, mask, modifier);
                Lane lane(code, input);
                lane.step();
                lane.step();
                auto expected = std::array<uint32_t, 4>{7, 8, 9, 10};
                for (unsigned i = 0; i < 4; ++i)
                    if (mask & (1u << i))
                        expected[i] = modifier == 0 || modifier == 3 ? 0x3f800000 : 0;
                QCOMPARE(lane.value({stage == 5 ? 0u : 2u, {0}}), Value::known(expected));
            }
    }
    void cancellation_data() { modifiersAndMasks_data(); }
    void cancellation() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, stage);
        auto native = device(vendor);
        unsigned checks = 0;
        auto gpu = std::make_shared<GpuOperations>(native.Get(), !vendor,
                                                   [&] { return ++checks == (stage == 5 ? 2u : 3u); });
        checks = 0;
        auto input = inputs(references.at({vendor, stage})["cases"][0], gpu);
        const auto original = input.textures->resources.at(1).storage;
        input.constantBuffers[0] = {Value::known({})};
        const auto code = program(stage, 0);
        Lane lane(code, input);
        lane.step();
        const auto before = lane.registers();
        QVERIFY_THROWS_EXCEPTION(OperationCancelled, lane.step());
        QCOMPARE(checks, stage == 5 ? 2u : 3u);
        QCOMPARE(lane.registers(), before);
        QCOMPARE(gpu->operations(), uint64_t(0));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, lane.step());
        input.gpu = std::make_shared<GpuOperations>(native.Get(), !vendor);
        Lane retry(code, input);
        retry.step();
        retry.step();
        QCOMPARE(retry.value({stage == 5 ? 0u : 2u, {0}}), Value::known({}));
        QCOMPARE(input.gpu->operations(), uint64_t(1));
        QCOMPARE(input.textures->resources.at(1).storage.bytes, original.bytes);
        QCOMPARE(input.textures->resources.at(1).storage.valid, original.valid);
    }
    void unknownAndInvalid() {
        auto input = inputs(references.at({0, 5})["cases"][0], {});
        input.constantBuffers[0] = {Value::known({})};
        const auto code = program(5, 0, 5);
        auto unknown = [&] {
            Lane lane(code, input);
            lane.step();
            QVERIFY(!lane.step().memoryGpuAssisted);
            QCOMPARE(lane.value({0, {0}}).valid, uint8_t(10));
        };
        auto sampler = input.textures->samplers.at({6, {3}});
        input.textures->samplers.clear();
        unknown();
        input.textures->samplers[{6, {3}}] = sampler;
        for (uint8_t validity : {uint8_t(3), uint8_t(5), uint8_t(6)}) {
            input.constantBuffers[0][0].valid = validity;
            unknown();
        }
        input.constantBuffers[0][0].valid = 15;
        input.textures->resources.at(1).storage.valid[0] = 0;
        unknown();
        input.textures->resources.at(1).storage.valid[0] = 1;
        Lane noGpu(code, input);
        noGpu.step();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, noGpu.step());
        input.textures->views.at({7, {0}}).resource.reset();
        Lane unbound(code, input);
        unbound.step();
        unbound.step();
        QCOMPARE(unbound.value({0, {0}}), Value::known({0, 8, 0, 10}));
        input.textures->samplers.at({6, {3}}).descriptor.reset();
        Lane noSampler(code, input);
        noSampler.step();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, noSampler.step());
        input.textures->samplers[{6, {3}}] = sampler;
        (*input.textures->samplers.at({6, {3}}).descriptor)[0] = 0;
        Lane wrongSampler(code, input);
        wrongSampler.step();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, wrongSampler.step());
    }
    void requestValidation() {
        auto input = inputs(references.at({0, 5})["cases"][0], {});
        bool assisted = true;
        const auto known = Value::known({});
        auto sample = [&](uint32_t dimension, std::array<uint32_t, 4> swizzle,
                          const std::optional<Value> &ddy = {}) {
            return input.textures->sampleTexture(5, {7, {0}}, {6, {3}}, dimension, 0x5555, known, known,
                                                 swizzle, 15, {}, nullptr, assisted, ddy, true);
        };
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, sample(5, {}));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, sample(3, {0, 1, 0, 0}));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, sample(3, {}, known));
        QVERIFY(!assisted);
    }
};
QTEST_GUILESS_MAIN(DxbcSampleComparisonTests)
#include "DxbcSampleComparisonTests.moc"
