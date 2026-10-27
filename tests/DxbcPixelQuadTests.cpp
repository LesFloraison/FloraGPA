#include "application/DxbcExecution.h"
#include "application/DxbcGpuOperations.h"
#include "application/HlslCompilation.h"
#include "core/Dxbc.h"
#include "replay/Replay.h"
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#include <bit>
#include <cmath>
using namespace flora;
using namespace flora::dxbc_exec;
using Json = nlohmann::json;
namespace {
Com<ID3D11Device> device(uint32_t vendor) {
    Com<IDXGIAdapter1> selected;
    if (vendor) {
        Com<IDXGIFactory1> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create derivative test factory");
        for (UINT i = 0;; ++i) {
            Com<IDXGIAdapter1> adapter;
            const auto hr = factory->EnumAdapters1(i, &adapter);
            if (hr == DXGI_ERROR_NOT_FOUND)
                break;
            check(hr, "Enumerate derivative adapter");
            DXGI_ADAPTER_DESC1 desc{};
            check(adapter->GetDesc1(&desc), "Read derivative adapter");
            if (desc.VendorId == vendor && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                selected = adapter;
                break;
            }
        }
        if (!selected)
            throw std::runtime_error("Requested derivative adapter is unavailable");
    }
    Com<ID3D11Device> result;
    check(D3D11CreateDevice(selected.Get(), vendor ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP, nullptr,
                            D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &result, nullptr,
                            nullptr),
          "Create derivative test device");
    return result;
}
std::vector<uint8_t> read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read derivative evidence");
    const auto bytes = file.readAll();
    return {bytes.begin(), bytes.end()};
}
using Words = std::vector<uint32_t>;
Words row(uint32_t opcode, std::initializer_list<Words> args = {}) {
    Words result{opcode};
    for (const auto &arg : args)
        result.insert(result.end(), arg.begin(), arg.end());
    result[0] |= uint32_t(result.size()) << 24;
    return result;
}
Program simple(uint32_t opcode = 123, uint32_t mask = 15, uint32_t modifier = 0, bool saturate = false,
               bool divergent = false) {
    std::vector<Words> rows{row(98 | (2 << 11), {{0x1010f2, 0}}), row(101, {{0x1020f2, 0}}),
                            row(54, {{0x1020f2, 0}, {0x4002, 7, 8, 9, 10}})};
    if (divergent) {
        rows.push_back(row(31 | (1 << 18), {{0x10100a, 0}}));
        rows.push_back(row(62));
        rows.push_back(row(21));
    }
    const Words source = modifier ? Words{0x80101e46, 1 | (modifier << 6), 0} : Words{0x101e46, 0};
    rows.push_back(row(opcode | (saturate ? 0x2000 : 0), {{0x102002 | (mask << 4), 0}, source}));
    rows.push_back(row(62));
    return Program(makeDxbc({{0x58454853, writeDxbcProgram({{0x50, 0}, rows})}}));
}
std::array<Inputs, 4> basic(std::shared_ptr<GpuOperations> gpu) {
    std::array<Inputs, 4> inputs;
    for (unsigned i = 0; i < 4; ++i) {
        inputs[i].gpu = gpu;
        inputs[i].registers[{1, {0}}] =
            Value::known({std::bit_cast<uint32_t>(float(i)), std::bit_cast<uint32_t>(-float(i)), 0, 0});
    }
    return inputs;
}
void run(PixelQuad &quad) {
    while (!quad.finished())
        quad.step();
}
} // namespace
class DxbcPixelQuadTests final : public QObject {
    Q_OBJECT
    QTemporaryDir evidence;
    std::map<uint32_t, Json> reports;
    std::map<uint32_t, Com<ID3D11Device>> devices;
    std::map<uint32_t, std::shared_ptr<GpuOperations>> helpers;
  private slots:
    void initTestCase() {
        for (uint32_t vendor : {0u, 4318u, 32902u}) {
            QProcess probe;
            const auto directory = evidence.filePath(QString::number(vendor));
            probe.start(QCoreApplication::applicationDirPath() + "/FloraPixelDerivativeProbe.exe",
                        {directory, QString::number(vendor)});
            if (!probe.waitForFinished(60000)) {
                probe.kill();
                probe.waitForFinished(5000);
                QFAIL("Independent pixel derivative probe timed out");
            }
            QVERIFY2(probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0,
                     probe.readAllStandardError().constData());
            reports[vendor] = Json::parse(read(directory + "/results.json"));
            QCOMPARE(reports[vendor]["cases"].size(), size_t(336));
            QCOMPARE(reports[vendor]["compiler_rejections"].size(), size_t(6));
            QCOMPARE(reports[vendor]["validation_errors"].get<unsigned>(), 0u);
            devices[vendor] = device(vendor);
            helpers[vendor] = std::make_shared<GpuOperations>(devices[vendor].Get(), vendor == 0);
        }
    }
    void originalPrograms_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<unsigned>("index");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (unsigned i = 0; i < 336; ++i)
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(i))) << vendor << i;
    }
    void originalPrograms() {
        QFETCH(uint32_t, vendor);
        QFETCH(unsigned, index);
        const auto &item = reports.at(vendor)["cases"][index];
        const auto directory = evidence.filePath(QString::number(vendor));
        const Program program(read(directory + "/" + QString::fromStdString(item["shader"])));
        std::array<Inputs, 4> inputs;
        for (unsigned i = 0; i < 4; ++i) {
            inputs[i].gpu = helpers.at(vendor);
            for (const auto &bits : item["inputs"])
                inputs[i].constantBuffers[0].push_back(Value::known(bits.get<std::array<uint32_t, 4>>()));
            inputs[i].constantBuffers[0].push_back(
                Value::known({item["discarded"].get<unsigned>(), 0, 0, 0}));
            inputs[i].registers[{1, {0}}] =
                Value::known({std::bit_cast<uint32_t>(float(i % 2) + .5f),
                              std::bit_cast<uint32_t>(float(i / 2) + .5f), 0, 0x3f800000});
        }
        PixelQuad quad(program, inputs, item["coverage"].get<uint8_t>());
        uint64_t steps = 0, assisted = 0, helperSteps = 0;
        while (!quad.finished()) {
            const auto step = quad.step();
            QCOMPARE(step.sequence, steps++);
            assisted += step.step.instructionGpuAssisted;
            helperSteps += step.helper;
        }
        QCOMPARE(quad.activeMask(),
                 uint8_t(item["coverage"].get<unsigned>() & ~item["discarded"].get<unsigned>()));
        QCOMPARE(quad.derivativeBatches(), uint64_t(1));
        QCOMPARE(assisted, uint64_t(4));
        unsigned compared = 0;
        for (unsigned i = 0; i < 4; ++i) {
            if (!(quad.activeMask() & (1u << i)))
                continue;
            const auto value = quad.lane(i).value({2, {0}});
            QCOMPARE(value.valid, uint8_t(15));
            for (unsigned c = 0; c < 4; ++c) {
                const auto expected = item["output"][i][c].get<uint32_t>();
                // NaN payloads are not specified across arithmetic implementations.
                if (std::isnan(std::bit_cast<float>(expected)))
                    QVERIFY(std::isnan(std::bit_cast<float>(value.bits[c])));
                else
                    QCOMPARE(value.bits[c], expected);
                ++compared;
            }
        }
        if (item["coverage"] != 15 || item["discarded"] != 0)
            QVERIFY(helperSteps > 0);
        qInfo().noquote() << QString::fromStdString(Json{
            {"vendor", vendor},
            {"case", index},
            {"components", compared},
            {"steps", steps},
            {"batches", quad.derivativeBatches()},
            {"adapter", helpers.at(vendor)->identity()},
            {"model", PixelQuad::executionModel}}.dump());
    }
    void validityAndModifiers_data() {
        QTest::addColumn<uint32_t>("vendor");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            QTest::newRow(qPrintable(QString::number(vendor))) << vendor;
    }
    void validityAndModifiers() {
        QFETCH(uint32_t, vendor);
        for (unsigned opcode : {11u, 12u, 122u, 123u, 124u, 125u}) {
            auto inputs = basic(helpers.at(vendor));
            inputs[3].registers.at({1, {0}}).valid = 14;
            PixelQuad unknown(simple(opcode), inputs);
            run(unknown);
            for (unsigned i = 0; i < 4; ++i) {
                const bool xKnown = opcode == 123 ? i < 2 : opcode == 125 ? i % 2 == 0 : false;
                QCOMPARE(unknown.lane(i).value({2, {0}}).valid, uint8_t(xKnown ? 15 : 14));
            }
            for (unsigned modifier = 0; modifier < 4; ++modifier) {
                PixelQuad modified(simple(opcode, 3, modifier, true), basic(helpers.at(vendor)));
                run(modified);
                for (unsigned i = 0; i < 4; ++i) {
                    const auto value = modified.lane(i).value({2, {0}});
                    QCOMPARE(value.valid, uint8_t(15));
                    QCOMPARE(value.bits, (std::array<uint32_t, 4>{
                                             modifier & 1 ? 0u : 0x3f800000u,
                                             modifier == 1 || modifier == 2 ? 0x3f800000u : 0u, 9, 10}));
                }
            }
        }
    }
    void directBits_data() { validityAndModifiers_data(); }
    void directBits() {
        QFETCH(uint32_t, vendor);
        const std::map<std::string, unsigned> opcodes{{"ddx", 11},         {"ddy", 12},
                                                      {"ddx_coarse", 122}, {"ddx_fine", 123},
                                                      {"ddy_coarse", 124}, {"ddy_fine", 125}};
        unsigned vectors = 0;
        for (const auto &item : reports.at(vendor)["cases"]) {
            if (item["flow"] != "uniform" || item["coverage"] != 15)
                continue;
            std::array<Value, 4> values;
            for (unsigned i = 0; i < 4; ++i)
                values[i] = Value::known(item["inputs"][i].get<std::array<uint32_t, 4>>());
            const auto results = helpers.at(vendor)->pixelDerivatives(opcodes.at(item["operation"]), values);
            for (unsigned i = 0; i < 4; ++i) {
                QCOMPARE(results[i].valid, uint8_t(15));
                QCOMPARE(results[i].bits, (item["output"][i].get<std::array<uint32_t, 4>>()));
                ++vectors;
            }
        }
        QCOMPARE(vectors, 48u);
    }
    void helperStoresSuppressed() {
        const auto binary =
            compileHlsl("RWByteAddressBuffer output:register(u1);"
                        "uint4 main(float4 p:SV_Position):SV_Target{output.Store(0,1);return 0;}",
                        "ps_5_0")
                .bytecode;
        const Program program(binary);
        auto inputs = basic({});
        const auto memory =
            std::make_shared<Memory>(std::map<uint64_t, Buffer>{{1, Buffer::known({7, 0, 0, 0})}},
                                     std::map<Register, BufferView>{{{30, {1}}, {1, 0, 4, 0}}});
        for (auto &input : inputs)
            input.memory = memory;
        PixelQuad helper(program, inputs, 0);
        run(helper);
        QCOMPARE(memory->buffers().at(1).bytes, (std::vector<uint8_t>{7, 0, 0, 0}));
        QVERIFY(helper.finished());
    }
    void rendezvousBudget() {
        std::vector<Words> rows{row(98 | (2 << 11), {{0x1010f2, 0}}),
                                row(101, {{0x1020f2, 0}}),
                                row(31 | (1 << 18), {{0x10100a, 0}}),
                                row(48),
                                row(58),
                                row(22),
                                row(21),
                                row(123, {{0x1020f2, 0}, {0x101e46, 0}}),
                                row(62)};
        const Program program(makeDxbc({{0x58454853, writeDxbcProgram({{0x50, 0}, rows})}}));
        PixelQuad quad(program, basic(helpers.at(0)), 15, 10000);
        const auto before = helpers.at(0)->operations();
        unsigned steps = 0;
        try {
            while (!quad.finished()) {
                quad.step();
                ++steps;
            }
            QFAIL("Unbounded pixel derivative wait was accepted");
        } catch (const std::runtime_error &error) {
            QVERIFY(QString(error.what()).contains("rendezvous exceeded"));
        }
        QVERIFY(steps >= 4096 && steps < 4200);
        QCOMPARE(helpers.at(0)->operations(), before);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, quad.step());
    }
    void cancellation_data() { validityAndModifiers_data(); }
    void cancellation() {
        QFETCH(uint32_t, vendor);
        bool armed = false;
        unsigned checks = 0;
        auto gpu = std::make_shared<GpuOperations>(devices.at(vendor).Get(), vendor == 0,
                                                   [&] { return armed && ++checks == 3; });
        const auto program = simple();
        PixelQuad cancelled(program, basic(gpu));
        armed = true;
        QVERIFY_THROWS_EXCEPTION(OperationCancelled, run(cancelled));
        QCOMPARE(checks, 3u);
        QCOMPARE(gpu->operations(), uint64_t(0));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, cancelled.step());
        armed = false;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, gpu->pixelDerivatives(123, {}));
        PixelQuad retry(program, basic(helpers.at(vendor)));
        run(retry);
        QCOMPARE(retry.lane(0).value({2, {0}}).bits[0], 0x3f800000u);
    }
    void explicitFailures() {
        const auto program = simple();
        PixelQuad noGpu(program, basic({}));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, run(noGpu));
        Lane alone(program, basic(helpers.at(0))[0]);
        alone.step();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, alone.step());
        PixelQuad divergent(simple(123, 15, 0, false, true), basic(helpers.at(0)));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, run(divergent));
        PixelQuad bounded(program, basic(helpers.at(0)), 15, 1);
        bounded.step();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, bounded.step());
        auto mixed = basic(helpers.at(0));
        mixed[1].gpu = helpers.at(4318);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, PixelQuad(program, mixed));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, PixelQuad(program, basic({}), 16));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, helpers.at(0)->pixelDerivatives(0, {}));
        std::array<Value, 4> invalid{};
        invalid[0].valid = 16;
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, helpers.at(0)->pixelDerivatives(123, invalid));
    }
};
QTEST_GUILESS_MAIN(DxbcPixelQuadTests)
#include "DxbcPixelQuadTests.moc"
