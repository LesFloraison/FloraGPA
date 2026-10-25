#include "application/DxbcGpuOperations.h"
#include "application/DxbcTextureInputs.h"
#include "application/HlslCompilation.h"
#include "replay/Replay.h"
#include <QCryptographicHash>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#include <bit>
using namespace flora;
using namespace flora::dxbc_exec;
using Json = nlohmann::json;
namespace {
std::vector<uint8_t> read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read independent group sampling evidence");
    const auto data = file.readAll();
    return {data.begin(), data.end()};
}
Com<ID3D11Device> device(uint32_t vendor) {
    Com<IDXGIAdapter1> selected;
    if (vendor) {
        Com<IDXGIFactory1> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create group sampling factory");
        for (UINT index = 0;; ++index) {
            Com<IDXGIAdapter1> adapter;
            const auto hr = factory->EnumAdapters1(index, &adapter);
            if (hr == DXGI_ERROR_NOT_FOUND)
                break;
            check(hr, "Enumerate group sampling adapter");
            DXGI_ADAPTER_DESC1 desc{};
            check(adapter->GetDesc1(&desc), "Read group sampling adapter");
            if (desc.VendorId == vendor && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                selected = adapter;
                break;
            }
        }
        if (!selected)
            throw std::runtime_error("Requested group sampling adapter unavailable");
    }
    Com<ID3D11Device> result;
    check(D3D11CreateDevice(selected.Get(), vendor ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP, nullptr,
                            D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &result, nullptr,
                            nullptr),
          "Create group sampling device");
    return result;
}
template <class T> void append(std::vector<uint8_t> &target, const T &value) {
    const auto *data = reinterpret_cast<const uint8_t *>(&value);
    target.insert(target.end(), data, data + sizeof value);
}
Inputs inputs(const Json &report, const Json &item, const std::shared_ptr<GpuOperations> &gpu) {
    Inputs result;
    result.gpu = gpu;
    result.textures = std::make_shared<TextureInputs>();
    auto &texture = result.textures->resources[1];
    texture.resource.id = 1;
    texture.resource.type = 0x85;
    texture.resource.desc = {8, 8, 4, 1, 2, 1, 0, 0, 8, 0, 0};
    texture.minLod = 0;
    texture.subresources = textureSubresources(texture.resource);
    std::vector<uint8_t> pixels;
    for (uint32_t word : report["texture_bits"].get<std::vector<uint32_t>>())
        append(pixels, word);
    texture.storage = Buffer::known(std::move(pixels));
    auto &view = result.textures->views[{7, {0}}];
    view.resource = 1;
    view.format = 2;
    view.dimension = 4;
    view.mipCount = 4;
    view.nativeDescriptor = {2, 4, 0, 4, 0, 0};
    auto &sampler = result.textures->samplers[{6, {3}}];
    sampler.object = 4;
    sampler.descriptor = item["sampler_descriptor"].get<std::array<uint32_t, 13>>();
    std::vector<uint8_t> parameters, output;
    for (const auto &record : item["parameters"])
        append(parameters, record.get<std::array<float, 4>>());
    for (size_t i = 0; i < item["output"].size(); ++i)
        append(output, std::array<uint32_t, 4>{0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef});
    result.memory =
        std::make_shared<Memory>(std::map<uint64_t, Buffer>{{2, Buffer::known(parameters)},
                                                            {3, Buffer::known(output)},
                                                            {5, Buffer::known(std::vector<uint8_t>(4))}},
                                 std::map<Register, BufferView>{{{7, {1}}, {2, 0, parameters.size(), 16}},
                                                                {{30, {0}}, {3, 0, output.size(), 16}},
                                                                {{30, {1}}, {5, 0, 4, 0}}});
    return result;
}
Program pollingProgram() {
    const auto compiled = compileHlsl(R"(
        Texture2D<float4> t:register(t0); SamplerState s:register(s3);
        RWStructuredBuffer<uint4> dst:register(u0);
        globallycoherent RWByteAddressBuffer control:register(u1);
        [numthreads(2,1,1)]void main(uint i:SV_GroupIndex) {
            [branch]if(i==0) {
                dst[0]=asuint(t.SampleGrad(s,float2(.25,.375),float2(.25,0),float2(0,.25)));
                control.InterlockedOr(0,1);
            } else {
                uint observed;
                [allow_uav_condition][loop]do {control.InterlockedOr(0,0,observed);}while(observed==0);
                dst[4]=uint4(7,11,13,17);
            }
        })",
                                      "cs_5_0");
    const Program program(compiled.bytecode);
    Json ops = Json::array();
    for (const auto &instruction : program.instructions())
        ops.push_back(instruction.opcode);
    for (auto required : {73, 48, 22, 182})
        if (std::find(ops.begin(), ops.end(), Json(required)) == ops.end())
            throw std::runtime_error("Compiler removed the polling fixture's required operations");
    qInfo().noquote() << QString::fromStdString(
        Json{{"polling_fixture_opcodes", ops}, {"thread_group", program.threadGroupSize()}}.dump());
    return program;
}
} // namespace
class DxbcSamplingGroupTests final : public QObject {
    Q_OBJECT
    QTemporaryDir evidence;
    std::map<uint32_t, Json> references;
  private slots:
    void initTestCase() {
        for (uint32_t vendor : {0u, 4318u, 32902u}) {
            QProcess probe;
            const auto directory = evidence.filePath(QString::number(vendor));
            probe.start(QCoreApplication::applicationDirPath() + "/FloraSampleGroupProbe.exe",
                        {directory, QString::number(vendor)});
            if (!probe.waitForFinished(60000)) {
                probe.kill();
                probe.waitForFinished(5000);
                QFAIL("Independent sampling group probe timed out");
            }
            QVERIFY2(probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0,
                     probe.readAllStandardError().constData());
            auto &report = references[vendor];
            report = Json::parse(read(directory + "/results.json"));
            QCOMPARE(report["cases"].size(), size_t(96));
            QCOMPARE(report["validation_errors"].get<unsigned>(), 0u);
        }
    }
    void hardware_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<unsigned>("configuration");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (unsigned configuration = 0; configuration < 96; ++configuration)
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(configuration)))
                    << vendor << configuration;
    }
    void hardware() {
        QFETCH(uint32_t, vendor);
        QFETCH(unsigned, configuration);
        try {
            const auto &report = references.at(vendor), &item = report["cases"][configuration];
            auto native = device(vendor);
            auto gpu = std::make_shared<GpuOperations>(native.Get(), !vendor);
            const auto id = gpu->identity();
            const auto luid =
                (uint64_t(uint32_t(id["luid_high"].get<int32_t>())) << 32) | id["luid_low"].get<uint32_t>();
            QCOMPARE(luid, report["adapter_luid"].get<uint64_t>());
            auto input = inputs(report, item, gpu);
            const auto original = input.textures->resources.at(1).storage;
            const auto parameters = input.memory->buffers().at(2);
            const auto binary = read(evidence.filePath(
                QString("%1/%2").arg(vendor).arg(QString::fromStdString(item["shader"].get<std::string>()))));
            const Program program(binary);
            QCOMPARE(program.threadGroupSize(), (item["group_size"].get<std::array<uint32_t, 3>>()));
            Group group(program, input);
            uint64_t steps = 0, barriers = 0;
            while (!group.finished()) {
                const auto step = group.step();
                QCOMPARE(step.sequence, steps++);
                barriers += step.releasedBarrier;
            }
            std::vector<uint8_t> expected;
            for (const auto &record : item["output"])
                append(expected, record.get<std::array<uint32_t, 4>>());
            const auto &output = input.memory->buffers().at(3);
            Json differences = Json::array();
            for (size_t offset = 0; offset < expected.size(); offset += 4) {
                uint32_t a, b;
                std::memcpy(&a, output.bytes.data() + offset, 4);
                std::memcpy(&b, expected.data() + offset, 4);
                if (a != b)
                    differences.push_back({{"offset", offset}, {"actual", a}, {"expected", b}});
            }
            qInfo().noquote() << QString::fromStdString(Json{
                {"sampling_group", true},
                {"shader_sha256",
                 QCryptographicHash::hash(
                     QByteArrayView(reinterpret_cast<const char *>(binary.data()), binary.size()),
                     QCryptographicHash::Sha256)
                     .toHex()
                     .toStdString()},
                {"vendor", vendor},
                {"configuration", configuration},
                {"flow", item["flow"]},
                {"group_size", item["group_size"]},
                {"filter", item["filter"]},
                {"offset", item["offset"]},
                {"differences", differences},
                {"steps", steps},
                {"barriers", barriers},
                {"sampling_batches", group.samplingBatches()},
                {"gpu_operations", gpu->operations()},
                {"execution_model", Group::executionModel},
                {"helper", id}}.dump());
            QCOMPARE(output.valid, std::vector<uint8_t>(expected.size(), 1));
            QCOMPARE(input.memory->buffers().at(2).bytes, parameters.bytes);
            QCOMPARE(input.memory->buffers().at(2).valid, parameters.valid);
            QCOMPARE(input.textures->resources.at(1).storage.bytes, original.bytes);
            QCOMPARE(input.textures->resources.at(1).storage.valid, original.valid);
            QCOMPARE(barriers, item["flow"] == "barrier-loop" ? uint64_t(6) : uint64_t(0));
            const auto batches = item["flow"] == "branches"                                        ? 2u
                                 : item["flow"] == "uneven-loop" || item["flow"] == "barrier-loop" ? 3u
                                                                                                   : 1u;
            QCOMPARE(group.samplingBatches(), uint64_t(batches));
            QCOMPARE(gpu->operations(), uint64_t(batches));
            QVERIFY2(differences.empty(), "Native complete sampling group output differs");
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void boundedProgress_data() {
        QTest::addColumn<uint32_t>("vendor");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            QTest::newRow(qPrintable(QString::number(vendor))) << vendor;
    }
    void boundedProgress() {
        QFETCH(uint32_t, vendor);
        try {
            auto native = device(vendor);
            auto gpu = std::make_shared<GpuOperations>(native.Get(), !vendor);
            auto input = inputs(references.at(vendor), references.at(vendor)["cases"][0], gpu);
            const auto program = pollingProgram();
            Group group(program, input, {}, 20000);
            uint64_t steps = 0;
            while (!group.finished()) {
                group.step();
                ++steps;
            }
            qInfo() << "Polling fixture executed steps:" << steps;
            QVERIFY(steps > 4096 && steps < 5000);
            QCOMPARE(group.samplingBatches(), uint64_t(1));
            QCOMPARE(gpu->operations(), uint64_t(1));
            const auto &bytes = input.memory->buffers().at(3).bytes;
            std::array<uint32_t, 4> first, second;
            std::memcpy(first.data(), bytes.data(), 16);
            std::memcpy(second.data(), bytes.data() + 64, 16);
            QCOMPARE(first,
                     (std::bit_cast<std::array<uint32_t, 4>>(std::array<float, 4>{1.25f, .125f, .125f, 1})));
            QCOMPARE(second, (std::array<uint32_t, 4>{7, 11, 13, 17}));
            qInfo().noquote() << QString::fromStdString(Json{
                {"bounded_sampling_progress", true},
                {"vendor", vendor},
                {"steps", steps},
                {"sampling_batches",
                 group.samplingBatches()}}.dump());
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void cancelWaiting_data() { boundedProgress_data(); }
    void cancelWaiting() {
        QFETCH(uint32_t, vendor);
        try {
            auto native = device(vendor);
            auto gpu = std::make_shared<GpuOperations>(native.Get(), !vendor);
            auto input = inputs(references.at(vendor), references.at(vendor)["cases"][0], gpu);
            const auto original = input.memory->buffers().at(3);
            const auto program = pollingProgram();
            unsigned checks = 0;
            Group group(program, input, {}, 20000, [&] { return ++checks > 1000; });
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, while (!group.finished()) { group.step(); });
            QCOMPARE(gpu->operations(), uint64_t(0));
            QCOMPARE(input.memory->buffers().at(3).bytes, original.bytes);
            QCOMPARE(input.memory->buffers().at(3).valid, original.valid);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, group.step());
            Group retry(program, input, {}, 20000);
            while (!retry.finished())
                retry.step();
            QCOMPARE(retry.samplingBatches(), uint64_t(1));
            qInfo().noquote() << QString::fromStdString(Json{
                {"sampling_wait_cancelled", true},
                {"vendor", vendor},
                {"checks", checks},
                {"retry_finished", retry.finished()}}.dump());
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
};
QTEST_GUILESS_MAIN(DxbcSamplingGroupTests)
#include "DxbcSamplingGroupTests.moc"
