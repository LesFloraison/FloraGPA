#include "application/DxbcCheckpointModel.h"
#include "application/DxbcExecution.h"
#include "application/DxbcGpuOperations.h"
#include "application/DxbcTextureInputs.h"
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

Program instruction(uint32_t opcode, unsigned mask = 15, unsigned modifier = 0, uint32_t swizzle = 0xe4,
                    bool indexed = false, uint32_t version = 0x50, uint32_t stage = 0,
                    uint32_t dimension = 3) {
    const Words address = modifier ? Words{0x80101e46, 1 | (modifier << 6), 0} : Words{0x101e46, 0};
    const Words resource =
        indexed ? Words{0x00d07006 | (swizzle << 4), 0, 0x10100a, 1} : Words{0x107006 | (swizzle << 4), 0};
    const Words sampler = indexed ? Words{0x00d06000, 0, 0x10101a, 1} : Words{0x106000, 0};
    Words sample = row(opcode, {{0x102002 | (mask << 4), 0}, address, resource, sampler});
    if (opcode != 69 && opcode != 108) {
        sample.insert(sample.end(), {0x10102a, 0});
        sample[0] = (sample[0] & 0xffffff) | (uint32_t(sample.size()) << 24);
    }
    if (indexed) {
        sample.insert(sample.begin() + 1, {0x80000002 | (dimension << 6), 3u | (0x5555u << 6)});
        sample[0] = (sample[0] & 0xffffff) | (uint32_t(sample.size()) << 24) | 0x80000000u;
    }
    return Program(
        makeDxbc({{0x58454853,
                   writeDxbcProgram({{(stage << 16) | version, 0},
                                     {row(88 | (dimension << 11), {{0x1070f2, 0}, {0x5555}}),
                                      row(90 | (opcode == 70 ? 0x800 : 0), {{0x106000, 0}}),
                                      row(98 | (2 << 11), {{0x1010f2, 0}}),
                                      row(98 | (1 << 11), {{0x101032, 1}}), row(101, {{0x1020f2, 0}}),
                                      row(54, {{0x1020f2, 0}, {0x4002, 7, 8, 9, 10}}), sample, row(62)}})}}));
}
std::shared_ptr<TextureInputs> textures(const Json &item) {
    auto result = std::make_shared<TextureInputs>();
    auto &texture = result->resources[1];
    texture.resource.id = 1;
    const auto dimension = item.value("dimension", 3u);
    const bool cube = dimension == 6 || dimension == 10;
    const auto layers = dimension == 10                    ? 12u
                        : dimension == 6                   ? 6u
                        : dimension == 7 || dimension == 8 ? 3u
                                                           : 1u;
    texture.resource.type = dimension == 2 || dimension == 7 ? 0x84 : dimension == 5 ? 0x86 : 0x85;
    texture.minLod = item["resource_min_lod"].get<float>();
    const auto misc = (texture.minLod ? 0x80u : 0u) | (cube ? 4u : 0u);
    if (texture.resource.type == 0x84)
        texture.resource.desc = {8, 4, layers, 41, 0, 8, 0, misc};
    else if (texture.resource.type == 0x86)
        texture.resource.desc = {8, 8, 8, 4, 41, 0, 8, 0, misc};
    else
        texture.resource.desc = {8, 8, 4, layers, 41, 1, 0, 0, 8, 0, misc};
    texture.subresources = textureSubresources(texture.resource);
    std::vector<uint8_t> bytes;
    for (const auto &sub : texture.subresources)
        for (unsigned z = 0; z < sub.depth; ++z)
            for (unsigned y = 0; y < sub.height; ++y)
                for (unsigned x = 0; x < sub.width; ++x) {
                    const float value = float(sub.mip) + .25f + float(x) / 8 + float(y) / 64;
                    const auto *begin = reinterpret_cast<const uint8_t *>(&value);
                    bytes.insert(bytes.end(), begin, begin + 4);
                }
    texture.storage = Buffer::known(std::move(bytes));
    auto &view = result->views[{7, {0}}];
    if (item["resource_bound"].get<bool>()) {
        view.resource = 1;
        view.format = 41;
        view.dimension = dimension == 2   ? 2
                         : dimension == 3 ? 4
                         : dimension == 5 ? 8
                         : dimension == 6 ? 9
                         : dimension == 7 ? 3
                         : dimension == 8 ? 5
                                          : 10;
        view.firstMip = item["first_mip"].get<unsigned>();
        view.mipCount = item["mip_count"].get<unsigned>();
        view.firstLayer = item.value("first_layer", 0u);
        view.layerCount = item.value("layer_count", 1u);
        view.nativeDescriptor = {41, view.dimension, view.firstMip, view.mipCount, 0, 0};
        if (dimension == 7 || dimension == 8 || dimension == 10) {
            view.nativeDescriptor[4] = view.firstLayer;
            view.nativeDescriptor[5] = view.layerCount / (cube ? 6 : 1);
        }
    }
    auto &sampler = result->samplers[{6, {0}}];
    if (!item["sampler_descriptor"].is_null()) {
        sampler.object = 2;
        sampler.descriptor = item["sampler_descriptor"].get<std::array<uint32_t, 13>>();
    }
    return result;
}
void execute(PixelQuad &quad) {
    while (!quad.finished())
        quad.step();
}
} // namespace
class DxbcPixelSamplingTests final : public QObject {
    Q_OBJECT
    QTemporaryDir evidence;
    std::map<uint32_t, Json> reports;
    std::map<uint32_t, Com<ID3D11Device>> devices;
    std::map<uint32_t, std::shared_ptr<GpuOperations>> helpers;
    std::map<std::pair<uint32_t, std::string>, std::unique_ptr<Program>> programs;
    const Json &reference(uint32_t vendor, uint32_t opcode) const {
        const std::string op = opcode == 69 ? "sample" : opcode == 70 ? "sample_c" : "sample_b";
        for (const auto &item : reports.at(vendor)["cases"])
            if (item["operation"] == op && item["configuration"] == 0 && item["dataset"] == 0 &&
                item["coverage"] == 15 && item["offset"] == 0)
                return item;
        throw std::runtime_error("Missing point sampling fixture");
    }
    std::array<Inputs, 4> operands(uint32_t vendor, uint32_t opcode) const {
        const auto &item = reference(vendor, opcode);
        std::array<Inputs, 4> input;
        auto storage = textures(item);
        for (unsigned i = 0; i < 4; ++i) {
            input[i].textures = storage;
            input[i].gpu = helpers.at(vendor);
            input[i].registers[{1, {0}}] = Value::known(item["inputs"][i].get<std::array<uint32_t, 4>>());
        }
        return input;
    }
  private slots:
    void originalLod_data() {
        QTest::addColumn<uint32_t>("vendor");
        for (auto vendor : {0u, 4318u, 32902u})
            QTest::newRow(qPrintable(QString::number(vendor))) << vendor;
    }
    void originalLod() {
        QFETCH(uint32_t, vendor);
        runLod(vendor, 3);
    }
    void originalLodShapes_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("dimension");
        for (auto vendor : {0u, 4318u, 32902u})
            for (auto dimension : {2u, 5u, 6u, 7u, 8u, 10u})
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(dimension))) << vendor << dimension;
    }
    void originalLodShapes() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, dimension);
        runLod(vendor, dimension);
    }

  private:
    void runLod(uint32_t vendor, uint32_t dimension) {
        try {
            const auto directory =
                evidence.filePath(dimension == 3 ? QString("lod-%1").arg(vendor)
                                                 : QString("lod-%1-%2").arg(vendor).arg(dimension));
            QProcess probe;
            QStringList arguments{directory, QString::number(vendor), "lod"};
            if (dimension != 3)
                arguments.push_back(QString::number(dimension));
            probe.start(QCoreApplication::applicationDirPath() + "/FloraPixelSamplingProbe.exe", arguments);
            if (!probe.waitForFinished(60000)) {
                probe.kill();
                probe.waitForFinished(5000);
                QFAIL("Independent LOD probe timed out");
            }
            QVERIFY2(probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0,
                     probe.readAllStandardError().constData());
            auto report = Json::parse(read(directory + "/results.json"));
            QCOMPARE(report.at("unexpected_validation_errors"), Json(0));
            QCOMPARE(report.at("cases").size(), size_t(448));
            std::map<std::string, std::unique_ptr<Program>> code;
            uint64_t components = 0;
            for (auto &item : report["cases"]) {
                const auto name = item.at("shader").get<std::string>();
                if (!code.contains(name)) {
                    code[name] =
                        std::make_unique<Program>(read(directory + "/" + QString::fromStdString(name)));
                    QVERIFY(std::any_of(code.at(name)->instructions().begin(),
                                        code.at(name)->instructions().end(),
                                        [](const auto &i) { return i.opcode == 108; }));
                }
                const auto storage = textures(item);
                std::array<Inputs, 4> input;
                for (unsigned lane = 0; lane < 4; ++lane) {
                    input[lane].textures = storage;
                    input[lane].gpu = helpers.at(vendor);
                    for (const auto &bits : item["inputs"])
                        input[lane].constantBuffers[0].push_back(
                            Value::known(bits.get<std::array<uint32_t, 4>>()));
                    input[lane].constantBuffers[0].push_back(
                        Value::known({item["discarded"].get<unsigned>(), 0, 0, 0}));
                    input[lane].registers[{1, {0}}] =
                        Value::known({std::bit_cast<uint32_t>(float(lane % 2) + .5f),
                                      std::bit_cast<uint32_t>(float(lane / 2) + .5f), 0, 0x3f800000});
                }
                // LOD uses dimensions and sampler state; unknown texel bytes cannot affect it.
                std::fill(storage->resources.at(1).storage.valid.begin(),
                          storage->resources.at(1).storage.valid.end(), 0);
                PixelQuad quad(*code.at(name), input, item["coverage"].get<uint8_t>());
                execute(quad);
                QCOMPARE(quad.activeMask(),
                         uint8_t(item["coverage"].get<unsigned>() & ~item["discarded"].get<unsigned>()));
                item["native"] = Json::array();
                for (unsigned lane = 0; lane < 4; ++lane) {
                    if (!(quad.activeMask() & (1u << lane))) {
                        item["native"].push_back(nullptr);
                        continue;
                    }
                    const auto value = quad.lane(lane).value({2, {0}});
                    item["native"].push_back({{"bits", value.bits}, {"valid", value.valid}});
                    QCOMPARE(value.valid, uint8_t(15));
                    if (value.bits != item["output"][lane].get<std::array<uint32_t, 4>>())
                        qInfo().noquote() << QString::fromStdString(item.dump());
                    QCOMPARE(value.bits, (item["output"][lane].get<std::array<uint32_t, 4>>()));
                    components += 4;
                }
            }
            report["native_helper"] = helpers.at(vendor)->identity();
            report["compared_components"] = components;
            QFile output(directory + "/native-results.json");
            QVERIFY(output.open(QIODevice::WriteOnly | QIODevice::NewOnly));
            const auto bytes = QByteArray::fromStdString(report.dump(2));
            QCOMPARE(output.write(bytes), bytes.size());
            qInfo().noquote() << QString::fromStdString(Json{
                {"lod", true},
                {"vendor", vendor},
                {"cases", report["cases"].size()},
                {"components", components},
                {"unknown_texel_contents", true}}.dump());
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
  private slots:
    void initTestCase() {
        if (qEnvironmentVariableIsSet("FLORA_KEEP_PIXEL_SAMPLING_EVIDENCE")) {
            evidence.setAutoRemove(false);
            qInfo().noquote() << "Independent sampling evidence:" << evidence.path();
        }
        for (uint32_t vendor : {0u, 4318u, 32902u}) {
            QProcess probe;
            const auto directory = evidence.filePath(QString::number(vendor));
            probe.start(QCoreApplication::applicationDirPath() + "/FloraPixelSamplingProbe.exe",
                        {directory, QString::number(vendor)});
            if (!probe.waitForFinished(180000)) {
                probe.kill();
                probe.waitForFinished(5000);
                QFAIL("Independent implicit sampling probe timed out");
            }
            QVERIFY2(probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0,
                     probe.readAllStandardError().constData());
            reports[vendor] = Json::parse(read(directory + "/results.json"));
            QCOMPARE(reports[vendor]["cases"].size(), size_t(3136));
            QCOMPARE(reports[vendor]["unexpected_validation_errors"].get<unsigned>(), 0u);
            devices[vendor] = device(vendor);
            helpers[vendor] = std::make_shared<GpuOperations>(devices[vendor].Get(), vendor == 0);
            for (const auto &item : reports[vendor]["cases"]) {
                const auto name = item["shader"].get<std::string>();
                const auto key = std::make_pair(vendor, name);
                if (!programs.contains(key))
                    programs[key] =
                        std::make_unique<Program>(read(directory + "/" + QString::fromStdString(name)));
            }
        }
    }
    void originalPrograms_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("operation");
        QTest::addColumn<unsigned>("configuration");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (const QString operation : {QString("sample"), QString("sample_b"), QString("sample_c")})
                for (unsigned i = 0; i < (operation == "sample_c" ? 21u : 14u); ++i)
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(vendor).arg(operation).arg(i)))
                        << vendor << operation << i;
    }
    void originalPrograms() {
        QFETCH(uint32_t, vendor);
        QFETCH(QString, operation);
        QFETCH(unsigned, configuration);
        uint64_t cases = 0, components = 0, rejections = 0, steps = 0, batches = 0;
        for (const auto &item : reports.at(vendor)["cases"]) {
            if (item["operation"] != operation.toStdString() || item["configuration"] != configuration)
                continue;
            ++cases;
            const auto storage = textures(item);
            const auto original = storage->resources.at(1).storage;
            std::array<Inputs, 4> input;
            for (unsigned i = 0; i < 4; ++i) {
                input[i].gpu = helpers.at(vendor);
                input[i].textures = storage;
                for (const auto &bits : item["inputs"])
                    input[i].constantBuffers[0].push_back(Value::known(bits.get<std::array<uint32_t, 4>>()));
                input[i].constantBuffers[0].push_back(
                    Value::known({item["discarded"].get<unsigned>(), 0, 0, 0}));
                input[i].registers[{1, {0}}] =
                    Value::known({std::bit_cast<uint32_t>(float(i % 2) + .5f),
                                  std::bit_cast<uint32_t>(float(i / 2) + .5f), 0, 0x3f800000});
            }
            const auto &program = *programs.at({vendor, item["shader"].get<std::string>()});
            PixelQuad quad(program, input, item["coverage"].get<uint8_t>());
            if (item["status"] != "observed") {
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, execute(quad));
                ++rejections;
                continue;
            }
            uint64_t assisted = 0;
            while (!quad.finished()) {
                const auto step = quad.step();
                ++steps;
                assisted += step.step.memoryGpuAssisted;
            }
            QCOMPARE(quad.activeMask(),
                     uint8_t(item["coverage"].get<unsigned>() & ~item["discarded"].get<unsigned>()));
            QCOMPARE(quad.derivativeBatches(), uint64_t(0));
            QCOMPARE(quad.samplingBatches(), uint64_t(1));
            batches += quad.samplingBatches();
            QCOMPARE(assisted, item["resource_bound"].get<bool>() ? uint64_t(4) : uint64_t(0));
            for (unsigned i = 0; i < 4; ++i) {
                if (!(quad.activeMask() & (1u << i)))
                    continue;
                const auto actual = quad.lane(i).value({2, {0}});
                QCOMPARE(actual.valid, uint8_t(15));
                const auto expected = item["output"][i].get<std::array<uint32_t, 4>>();
                if (actual.bits != expected)
                    qInfo().noquote() << QString::fromStdString(Json{
                        {"mismatch", true},
                        {"vendor", vendor},
                        {"operation", operation.toStdString()},
                        {"configuration", configuration},
                        {"dataset", item["dataset"]},
                        {"coverage", item["coverage"]},
                        {"offset", item["offset"]},
                        {"lane", i},
                        {"actual", actual.bits},
                        {"expected", expected}}.dump());
                QCOMPARE(actual.bits, expected);
                components += 4;
            }
            QCOMPARE(storage->resources.at(1).storage.bytes, original.bytes);
            QCOMPARE(storage->resources.at(1).storage.valid, original.valid);
        }
        QCOMPARE(cases, uint64_t(64));
        QCOMPARE(rejections, operation == "sample_c" && configuration == 13 ? uint64_t(64) : uint64_t(0));
        qInfo().noquote() << QString::fromStdString(Json{
            {"implicit_sampling", true},
            {"vendor", vendor},
            {"operation", operation.toStdString()},
            {"configuration", configuration},
            {"cases", cases},
            {"components", components},
            {"undefined_rejections", rejections},
            {"batches", batches},
            {"steps", steps},
            {"helper", helpers.at(vendor)->identity()},
            {"model", PixelQuad::executionModel}}.dump());
    }
    void semantics_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("opcode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t opcode : {69u, 70u, 74u})
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(opcode))) << vendor << opcode;
    }
    void minimumPrecision_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<unsigned>("scenario");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (unsigned scenario = 1; scenario <= 6; ++scenario)
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(scenario))) << vendor << scenario;
    }
    void minimumPrecision() {
        QFETCH(uint32_t, vendor);
        QFETCH(unsigned, scenario);
        const auto directory = evidence.filePath(QString("precision-%1-%2").arg(vendor).arg(scenario));
        QProcess probe;
        probe.start(QCoreApplication::applicationDirPath() + "/FloraPixelSamplingProbe.exe",
                    {directory, QString::number(vendor), QString::number(scenario)});
        if (!probe.waitForFinished(60000)) {
            probe.kill();
            probe.waitForFinished(5000);
            QFAIL("Independent precision sampling probe timed out");
        }
        QVERIFY2(probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0,
                 probe.readAllStandardError().constData());
        const auto report = Json::parse(read(directory + "/results.json"));
        QCOMPARE(report["cases"].size(), size_t(576));
        QCOMPARE(report["unexpected_validation_errors"].get<unsigned>(), 0u);
        std::map<std::string, std::unique_ptr<Program>> original;
        Json observed = report;
        observed["scope"] = "native_pixel_sampling_execution";
        observed["helper"] = helpers.at(vendor)->identity();
        uint64_t components = 0;
        size_t caseIndex = 0;
        for (const auto &item : report["cases"]) {
            const auto name = item["shader"].get<std::string>();
            if (!original.contains(name)) {
                original[name] =
                    std::make_unique<Program>(read(directory + "/" + QString::fromStdString(name)));
                unsigned samples = 0;
                for (const auto &instruction : original.at(name)->instructions()) {
                    if (instruction.opcode != 69 && instruction.opcode != 70 && instruction.opcode != 74)
                        continue;
                    ++samples;
                    const auto operands = checkpoint::instructionOperands(instruction.words);
                    const auto precisionAt = [&](size_t index) {
                        const auto first = operands.ranges.at(index).first;
                        return instruction.words[first] >> 31 ? (instruction.words[first + 1] >> 14) & 7 : 0u;
                    };
                    const auto reduced = scenario >= 5 ? 2u : 1u;
                    QCOMPARE(precisionAt(0), scenario == 1 || scenario >= 4 ? reduced : 0u);
                    QCOMPARE(precisionAt(1), scenario == 2 || scenario == 4 || scenario == 6 ? reduced : 0u);
                    if (instruction.opcode != 69)
                        QCOMPARE(precisionAt(4),
                                 scenario == 3 || scenario == 4 || scenario == 6 ? reduced : 0u);
                }
                QCOMPARE(samples, 1u);
            }
            auto storage = textures(item);
            std::array<Inputs, 4> input;
            for (unsigned i = 0; i < 4; ++i) {
                input[i].gpu = helpers.at(vendor);
                input[i].textures = storage;
                for (const auto &bits : item["inputs"])
                    input[i].constantBuffers[0].push_back(Value::known(bits.get<std::array<uint32_t, 4>>()));
                input[i].constantBuffers[0].push_back(
                    Value::known({item["discarded"].get<unsigned>(), 0, 0, 0}));
                input[i].registers[{1, {0}}] =
                    Value::known({std::bit_cast<uint32_t>(float(i % 2) + .5f),
                                  std::bit_cast<uint32_t>(float(i / 2) + .5f), 0, 0x3f800000});
            }
            PixelQuad quad(*original.at(name), input, item["coverage"].get<uint8_t>());
            execute(quad);
            QCOMPARE(quad.activeMask(),
                     uint8_t(item["coverage"].get<unsigned>() & ~item["discarded"].get<unsigned>()));
            auto &observation = observed["cases"][caseIndex++];
            observation["output"] = Json::array();
            observation["validity"] = Json::array();
            for (unsigned i = 0; i < 4; ++i) {
                const auto actual = quad.lane(i).value({2, {0}});
                observation["output"].push_back(actual.bits);
                observation["validity"].push_back(actual.valid);
                if (!(quad.activeMask() & (1u << i)))
                    continue;
                const auto expected = item["output"][i].get<std::array<uint32_t, 4>>();
                QCOMPARE(actual.valid, uint8_t(15));
                if (actual.bits != expected)
                    qInfo().noquote() << QString::fromStdString(
                        Json{{"case", item}, {"lane", i}, {"actual", actual.bits}, {"expected", expected}}
                            .dump());
                QCOMPARE(actual.bits, expected);
                components += 4;
            }
        }
        QFile file(directory + "/native-results.json");
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        const auto serialized = observed.dump(2);
        QCOMPARE(file.write(serialized.data(), qint64(serialized.size())), qint64(serialized.size()));
        qInfo().noquote() << QString::fromStdString(Json{
            {"precision_scenario", scenario},
            {"vendor", vendor},
            {"cases", report["cases"].size()},
            {"components", components}}.dump());
    }
    void semantics() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, opcode);
        const auto code = instruction(opcode, 5, 0, opcode == 70 ? 0 : 0x1b);
        auto input = operands(vendor, opcode);
        PixelQuad quad(code, input);
        std::vector<PixelQuadStep> trace;
        while (!quad.finished())
            trace.push_back(quad.step());
        for (unsigned lane = 0; lane < 4; ++lane) {
            const auto actual = quad.lane(lane).value({2, {0}});
            const uint32_t comparison = lane < 2 ? 0x3f800000 : 0;
            QCOMPARE(actual, (Value::known({opcode == 70 ? comparison : 0x3f800000, 8,
                                            opcode == 70 ? comparison : 0, 10})));
            auto registers = quad.lane(lane).registers();
            for (auto it = trace.rbegin(); it != trace.rend(); ++it)
                if (it->lane == lane)
                    for (auto change = it->step.changes.rbegin(); change != it->step.changes.rend(); ++change)
                        registers[change->destination] = change->before;
            QCOMPARE(registers.at({2, {0}}), Value{});
            QCOMPARE(registers.at({1, {0}}), input[lane].registers.at({1, {0}}));
        }
        for (unsigned modifier = 0; modifier < 4; ++modifier) {
            PixelQuad modified(instruction(opcode, 15, modifier, 0), operands(vendor, opcode));
            execute(modified);
            for (unsigned lane = 0; lane < 4; ++lane) {
                const auto value = modified.lane(lane).value({2, {0}});
                const auto depth = modifier & 1 ? .25f : .671875f;
                const auto referenceValue = std::bit_cast<float>(input[lane].registers.at({1, {0}}).bits[2]);
                const auto expected =
                    std::bit_cast<uint32_t>(opcode == 70 ? float(referenceValue <= depth) : depth);
                QCOMPARE(value, Value::known({expected, expected, expected, expected}));
            }
        }
    }
    void indexedSampling_data() { semantics_data(); }
    void indexedSampling() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, opcode);
        auto input = operands(vendor, opcode);
        auto &storage = *input[0].textures;
        storage.views[{7, {3}}] = storage.views.at({7, {0}});
        storage.views.erase({7, {0}});
        storage.samplers[{6, {5}}] = storage.samplers.at({6, {0}});
        storage.samplers.erase({6, {0}});
        for (auto &lane : input)
            lane.registers[{1, {1}}] = Value::known({3, 5, 0, 0});
        // The independent original shader explicitly returns the sampled red component in xxxx.
        const auto code = instruction(opcode, 15, 0, 0, true);
        for (auto coverage : {uint8_t(15), uint8_t(1)}) {
            PixelQuad quad(code, input, coverage);
            execute(quad);
            QCOMPARE(quad.samplingBatches(), uint64_t(1));
            QCOMPARE(quad.activeMask(), coverage);
            for (unsigned lane = 0; lane < 4; ++lane)
                QCOMPARE(
                    quad.lane(lane).value({2, {0}}),
                    Value::known(reference(vendor, opcode)["output"][lane].get<std::array<uint32_t, 4>>()));
        }
        auto divergent = input;
        divergent[1].registers[{1, {1}}].bits[0] = 4;
        PixelQuad mismatch(code, divergent);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, execute(mismatch));
        auto unknown = input;
        unknown[2].registers[{1, {1}}].valid = 1;
        PixelQuad missing(code, unknown);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, execute(missing));
        qInfo().noquote() << QString::fromStdString(Json{
            {"indexed_sampling", true},
            {"vendor", vendor},
            {"opcode", opcode},
            {"texture_slot", 3},
            {"sampler_slot", 5},
            {"coverage", {15, 1}},
            {"reference", reference(vendor, opcode)},
            {"helper", helpers.at(vendor)->identity()}}.dump());
    }
    void lodBoundaries_data() { originalLod_data(); }
    void lodBoundaries() {
        QFETCH(uint32_t, vendor);
        const auto input = operands(vendor, 69);
        PixelQuad reference(instruction(108), input);
        execute(reference);
        PixelQuad masked(instruction(108, 5, 0, 0x1b), input);
        execute(masked);
        PixelQuad sm41(instruction(108, 15, 0, 0xe4, false, 0x41), input);
        execute(sm41);
        for (unsigned lane = 0; lane < 4; ++lane) {
            const auto expected = reference.lane(lane).value({2, {0}});
            QCOMPARE(expected.valid, uint8_t(15));
            QCOMPARE(masked.lane(lane).value({2, {0}}),
                     (Value::known({expected.bits[3], 8, expected.bits[1], 10})));
            QCOMPARE(sm41.lane(lane).value({2, {0}}), expected);
        }
        auto relocated = operands(vendor, 69);
        auto &storage = *relocated[0].textures;
        storage.views[{7, {3}}] = storage.views.at({7, {0}});
        storage.views.erase({7, {0}});
        storage.samplers[{6, {5}}] = storage.samplers.at({6, {0}});
        storage.samplers.erase({6, {0}});
        for (auto &lane : relocated)
            lane.registers[{1, {1}}] = Value::known({3, 5, 0, 0});
        PixelQuad indexed(instruction(108, 15, 0, 0xe4, true), relocated, 1);
        execute(indexed);
        for (unsigned lane = 0; lane < 4; ++lane)
            QCOMPARE(indexed.lane(lane).value({2, {0}}), reference.lane(lane).value({2, {0}}));
        auto unknown = operands(vendor, 69);
        unknown[1].registers[{1, {0}}].valid = 14;
        PixelQuad coordinates(instruction(108), unknown);
        execute(coordinates);
        for (unsigned lane = 0; lane < 4; ++lane)
            QCOMPARE(coordinates.lane(lane).value({2, {0}}).valid, uint8_t(0));
        auto unbound = operands(vendor, 69);
        unbound[0].textures->views[{7, {0}}] = {};
        PixelQuad nullView(instruction(108), unbound);
        execute(nullView);
        for (unsigned lane = 0; lane < 4; ++lane)
            QCOMPARE(nullView.lane(lane).value({2, {0}}), Value::known({}));
        unbound[0].textures->views.clear();
        PixelQuad missing(instruction(108), unbound);
        execute(missing);
        for (unsigned lane = 0; lane < 4; ++lane)
            QCOMPARE(missing.lane(lane).value({2, {0}}).valid, uint8_t(0));
        QVERIFY_THROWS_EXCEPTION(std::exception, instruction(108, 15, 0, 0xe4, false, 0x40));
        for (auto stage : {1u, 5u})
            QVERIFY_THROWS_EXCEPTION(std::exception, instruction(108, 15, 0, 0xe4, false, 0x50, stage));
        Lane alone(instruction(108), input[0]);
        alone.step();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, alone.step());
    }
    void lodCoordinateValidity_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("dimension");
        for (auto vendor : {0u, 4318u, 32902u})
            for (auto dimension : {2u, 3u, 5u, 6u, 7u, 8u, 10u})
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(dimension))) << vendor << dimension;
    }
    void lodCoordinateValidity() {
        try {
            QFETCH(uint32_t, vendor);
            QFETCH(uint32_t, dimension);
            auto item = reports.at(vendor)["cases"].at(0);
            item["dimension"] = dimension;
            item["first_layer"] = dimension == 10 ? 6u : dimension == 7 || dimension == 8 ? 1u : 0u;
            item["layer_count"] = dimension == 6 || dimension == 10  ? 6u
                                  : dimension == 7 || dimension == 8 ? 2u
                                                                     : 1u;
            // Use linear mip filtering for the semantic validity checks.
            item["sampler_descriptor"][0] = 21;
            auto input = operands(vendor, 69);
            const auto storage = textures(item);
            for (auto &lane : input)
                lane.textures = storage;
            auto code = instruction(108, 15, 0, 0xe4, false, 0x50, 0, dimension);
            PixelQuad reference(code, input);
            execute(reference);
            const uint8_t spatial = dimension == 2 || dimension == 7   ? 1
                                    : dimension == 3 || dimension == 8 ? 3
                                                                       : 7;
            for (auto &lane : input) {
                lane.registers[{1, {0}}].valid = spatial;
                for (unsigned c = 0; c < 4; ++c)
                    if (!(spatial & (1u << c)))
                        lane.registers[{1, {0}}].bits[c] = 0x7fc12345;
            }
            PixelQuad irrelevant(code, input);
            execute(irrelevant);
            for (unsigned lane = 0; lane < 4; ++lane) {
                QCOMPARE(reference.lane(lane).value({2, {0}}).valid, uint8_t(15));
                QCOMPARE(irrelevant.lane(lane).value({2, {0}}), reference.lane(lane).value({2, {0}}));
            }
            input[1].registers[{1, {0}}].valid &= ~1u;
            PixelQuad unknown(code, input);
            execute(unknown);
            for (unsigned lane = 0; lane < 4; ++lane)
                QCOMPARE(unknown.lane(lane).value({2, {0}}).valid, uint8_t(0));
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void cancellation_data() {
        semantics_data();
        for (auto vendor : {0u, 4318u, 32902u})
            QTest::newRow(qPrintable(QString("%1-108").arg(vendor))) << vendor << 108u;
    }
    void cancellation() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, opcode);
        bool armed = false;
        unsigned checks = 0;
        auto gpu = std::make_shared<GpuOperations>(devices.at(vendor).Get(), vendor == 0,
                                                   [&] { return armed && ++checks == 3; });
        auto input = operands(vendor, opcode == 108 ? 69u : opcode);
        for (auto &lane : input)
            lane.gpu = gpu;
        PixelQuad quad(instruction(opcode, 15, 0, opcode == 70 ? 0 : 0xe4), input);
        armed = true;
        QVERIFY_THROWS_EXCEPTION(OperationCancelled, execute(quad));
        QCOMPARE(checks, 3u);
        QCOMPARE(gpu->operations(), uint64_t(0));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, quad.step());
        armed = false;
        PixelQuad poisoned(instruction(opcode, 15, 0, opcode == 70 ? 0 : 0xe4), input);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, execute(poisoned));
        PixelQuad retry(instruction(opcode, 15, 0, opcode == 70 ? 0 : 0xe4),
                        operands(vendor, opcode == 108 ? 69u : opcode));
        execute(retry);
        QCOMPARE(retry.lane(0).value({2, {0}}).valid, uint8_t(15));
    }
    void unknownAndInvalid() {
        for (unsigned opcode : {69u, 70u, 74u}) {
            const auto code = instruction(opcode, 15, 0, opcode == 70 ? 0 : 0xe4);
            auto input = operands(0, opcode);
            input[3].registers.at({1, {0}}).valid = 14;
            PixelQuad unknown(code, input);
            execute(unknown);
            for (unsigned i = 0; i < 4; ++i)
                QCOMPARE(unknown.lane(i).value({2, {0}}).valid, uint8_t(0));
            input = operands(0, opcode);
            input[1].registers.at({1, {0}}).valid = 11;
            PixelQuad parameter(code, input);
            execute(parameter);
            for (unsigned i = 0; i < 4; ++i)
                QCOMPARE(parameter.lane(i).value({2, {0}}).valid, uint8_t(opcode != 69 && i == 1 ? 0 : 15));
            input = operands(0, opcode);
            input[0].textures->resources.at(1).storage.valid[0] = 0;
            PixelQuad storage(code, input);
            execute(storage);
            QCOMPARE(storage.lane(0).value({2, {0}}).valid, uint8_t(0));
            input = operands(0, opcode);
            for (auto &lane : input)
                lane.gpu.reset();
            PixelQuad missing(code, input);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, execute(missing));
            Lane alone(code, operands(0, opcode)[0]);
            alone.step();
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, alone.step());
        }
        for (uint32_t bad : {0x41800000u, 0xc1810000u, 0x7f800000u, 0x7fc12345u}) {
            auto input = operands(0, 74);
            input[0].registers.at({1, {0}}).bits[2] = bad;
            PixelQuad invalid(instruction(74), input);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, execute(invalid));
        }
    }
};
QTEST_GUILESS_MAIN(DxbcPixelSamplingTests)
#include "DxbcPixelSamplingTests.moc"
