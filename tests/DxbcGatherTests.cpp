#include "StateCapture.h"
#include "TextureOperationOracle.h"
#include "application/DxbcExecutionTrace.h"
#include "application/DxbcExecutionTraceReader.h"
#include "application/DxbcGpuOperations.h"
#include "application/DxbcTypedBuffer.h"
#include "core/Dxbc.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <bit>
using namespace flora;
using namespace flora::dxbc_exec;
namespace {
using Words = std::vector<uint32_t>;
Words row(uint32_t opcode, std::initializer_list<Words> args = {}) {
    Words words{opcode};
    for (const auto &arg : args)
        words.insert(words.end(), arg.begin(), arg.end());
    words[0] |= uint32_t(words.size()) << 24;
    return words;
}
std::vector<uint8_t> shader(uint32_t stage, uint32_t opcode, uint32_t dimension, uint32_t returns,
                            uint32_t component, uint32_t mask, bool offset) {
    const bool comparison = opcode == 126 || opcode == 128;
    const bool programmable = opcode == 127 || opcode == 128;
    std::vector<Words> rows{row(89, {{0x208e46, 2, 3}}),
                            row(88 | dimension << 11, {{0x1070f2, 3}, {returns}}),
                            row(90 | (comparison ? 0x800 : 0), {{0x106000, 4}}), row(104, {{4}})};
    if (stage == 5) {
        rows.push_back(row(155, {{1, 1, 1}}));
        rows.push_back(row(158, {{0x11e0f2, 3}, {16}}));
    } else
        rows.push_back(row(101, {{0x1020f2, 0}}));
    rows.push_back(row(54, {{0x1000f2, 3}, {0x4002, 17, 23, 31, 47}}));
    Words gather{opcode | 0x80000000u,
                 1u | 0x80000000u | (offset ? (15u << 9) | (1u << 13) : 0u),
                 2u | dimension << 6 | 0x80000000u,
                 3u | returns << 6,
                 0x100002u | mask << 4,
                 3,
                 0x208e46,
                 2,
                 0};
    if (programmable)
        gather.insert(gather.end(), {0x208e46, 2, 1});
    gather.insert(gather.end(), {0x1071b6, 3, 0x10600au | component << 4, 4});
    if (comparison)
        gather.insert(gather.end(), {0x20800a, 2, 2});
    gather[0] |= uint32_t(gather.size()) << 24;
    rows.push_back(std::move(gather));
    if (stage == 5)
        rows.push_back(row(168, {{0x11e0f2, 3}, {0x4001, 0}, {0x4001, 0}, {0x100e46, 3}}));
    else
        rows.push_back(row(54, {{0x1020f2, 0}, {0x100e46, 3}}));
    rows.push_back(row(62));
    const auto signature = compileHlsl(stage == 5   ? "[numthreads(1,1,1)]void main(){}"
                                       : stage == 0 ? "uint4 main():SV_Target0{return 0;}"
                                                    : "uint4 main():TEXCOORD0{return 0;}",
                                       stage == 5   ? "cs_5_0"
                                       : stage == 0 ? "ps_5_0"
                                                    : "vs_5_0")
                               .bytecode;
    auto parts = readDxbcParts(signature);
    std::erase_if(parts, [](const auto &part) {
        return part.first == 0x58454853 || part.first == 0x52444853 || part.first == 0x54415453;
    });
    const auto program = writeDxbcProgram({{stage << 16 | 0x50, 0}, rows});
    parts.emplace_back(0x58454853, program);
    return makeDxbc(parts);
}
void validateDevice(ID3D11Device *device) {
    Com<ID3D11InfoQueue> queue;
    check(device->QueryInterface(IID_PPV_ARGS(&queue)), "Read gather validation queue");
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0;
        check(queue->GetMessage(i, nullptr, &size), "Read gather diagnostic size");
        std::vector<uint8_t> bytes(size);
        auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
        check(queue->GetMessage(i, message, &size), "Read gather diagnostic");
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
            throw std::runtime_error(message->pDescription);
    }
}
std::vector<uint8_t> precisionShader(uint32_t stage, uint32_t opcode, uint32_t dimension,
                                     const GatherOperation &operation, uint32_t mask,
                                     const std::array<uint32_t, 4> &address) {
    const bool comparison = opcode == 126 || opcode == 128;
    const bool programmable = opcode == 127 || opcode == 128;
    const bool direct = operation.outputDestination;
    const uint32_t kind = direct ? 2 : 0, index = direct ? 0 : 3;
    const auto annotated = [](Words operand, uint32_t precision, uint32_t modifier = 0) {
        if (precision || modifier) {
            operand[0] |= 0x80000000u;
            operand.insert(operand.begin() + 1, 1u | precision << 14 | modifier << 6);
        }
        return operand;
    };
    const auto destination = [](uint32_t kind, uint32_t index, uint32_t mask = 15) {
        return Words{0x100002u | kind << 12 | mask << 4, index};
    };
    std::vector<Words> rows{row(106 | (1u << 16) | (operation.refactoringAllowed ? 1u << 11 : 0u)),
                            row(89, {{0x208e46, 2, 3}}), row(88 | dimension << 11, {{0x1070f2, 3}, {0x5555}}),
                            row(90 | (comparison ? 0x800 : 0), {{0x106000, 4}}), row(104, {{4}})};
    if (stage == 5) {
        rows.push_back(row(155, {{1, 1, 1}}));
        rows.push_back(row(158, {{0x11e0f2, 3}, {16}}));
    } else
        rows.push_back(row(101, {annotated(destination(2, 0), direct ? operation.precision[0] : 0)}));
    rows.push_back(
        row(54, {destination(kind, index), {0x4002, 0x3e000000, 0x3e800000, 0x3f000000, 0x3f400000}}));
    const auto operand = [&](unsigned selected, const std::array<uint32_t, 4> &value) {
        const auto source = operation.literal[selected]
                                ? selected == 2 ? Words{0x4001, value[0]}
                                                : Words{0x4002, value[0], value[1], value[2], value[3]}
                            : selected == 2 ? Words{0x20800a, 2, selected}
                                            : Words{0x208e46, 2, selected};
        return annotated(source, operation.precision[selected + 1], operation.modifiers[selected]);
    };
    Words gather{opcode | 0x80000000u, 0x80000001u, 2u | dimension << 6 | 0x80000000u, 3u | 0x5555u << 6};
    const auto append = [&](const Words &source) {
        gather.insert(gather.end(), source.begin(), source.end());
    };
    append(annotated(destination(kind, index, mask), operation.precision[0]));
    append(operand(0, address));
    if (programmable)
        append(operand(1, operation.offset.bits));
    append({0x1071b6, 3});
    append({0x10600a | operation.component << 4, 4});
    if (comparison)
        append(operand(2, operation.reference.bits));
    gather[0] |= uint32_t(gather.size()) << 24;
    rows.push_back(std::move(gather));
    if (!direct) {
        // Read untouched components at their original precision, then promote only
        // the components written by the minimum-precision operation.
        rows.push_back(row(54, {destination(stage == 5 ? 0 : 2, stage == 5 ? 2 : 0), {0x100e46, 3}}));
        rows.push_back(row(54, {destination(stage == 5 ? 0 : 2, stage == 5 ? 2 : 0, mask),
                                annotated({0x100e46, 3}, operation.precision[0])}));
    }
    if (stage == 5)
        rows.push_back(row(168, {{0x11e0f2, 3}, {0x4001, 0}, {0x4001, 0}, {0x100e46, 2}}));
    rows.push_back(row(62));
    const std::string type =
        direct && operation.precision[0] ? operation.precision[0] == 1 ? "min16float" : "min10float" : "uint";
    const auto signature =
        compileHlsl(stage == 5
                        ? "[numthreads(1,1,1)]void main(){}"
                        : type + "4 main():" + (stage == 0 ? "SV_Target0" : "TEXCOORD0") + "{return 0;}",
                    stage == 5   ? "cs_5_0"
                    : stage == 0 ? "ps_5_0"
                                 : "vs_5_0")
            .bytecode;
    auto parts = readDxbcParts(signature);
    std::erase_if(parts, [](const auto &part) {
        return part.first == 0x58454853 || part.first == 0x52444853 || part.first == 0x54415453;
    });
    const auto program = writeDxbcProgram({{stage << 16 | 0x50, 0}, rows});
    parts.emplace_back(0x58454853, program);
    return makeDxbc(parts);
}
} // namespace
class DxbcGatherTests final : public QObject {
    Q_OBJECT
  private slots:
    void minimumPrecision_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("stage");
        QTest::addColumn<uint32_t>("dimension");
        QTest::addColumn<uint32_t>("opcode");
        QTest::addColumn<uint32_t>("precision");
        for (auto vendor : {0u, 4318u, 32902u})
            for (auto stage : {0u, 1u, 5u})
                for (auto dimension : {3u, 6u, 8u, 10u})
                    for (auto opcode : {109u, 126u, 127u, 128u}) {
                        if ((opcode == 127 || opcode == 128) && (dimension == 6 || dimension == 10))
                            continue;
                        for (auto precision : {1u, 2u})
                            QTest::newRow(qPrintable(QString("%1-%2-%3-%4-%5")
                                                         .arg(vendor)
                                                         .arg(stage)
                                                         .arg(dimension)
                                                         .arg(opcode)
                                                         .arg(precision)))
                                << vendor << stage << dimension << opcode << precision;
                    }
    }
    void minimumPrecision() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, stage);
        QFETCH(uint32_t, dimension);
        QFETCH(uint32_t, opcode);
        QFETCH(uint32_t, precision);
        try {
            QTemporaryDir temporary;
            const auto path = temporary.filePath("gather.gpa_frame");
            auto capture = testing::computeCapture(true);
            capture.save(path);
            Frame frame(path.toStdWString());
            ReplayOptions options;
            options.warp = vendor == 0;
            if (vendor)
                options.vendor = vendor;
            options.debug = options.before = true;
            options.until = 100;
            Replay replay(frame, options);
            auto gpu = std::make_shared<GpuOperations>(replay.nativeDevice(), vendor == 0);
            const bool comparison = opcode == 126 || opcode == 128;
            const bool programmable = opcode == 127 || opcode == 128;
            const bool cube = dimension == 6 || dimension == 10;
            const uint32_t layers = dimension == 10 ? 12 : dimension == 6 ? 6 : dimension == 8 ? 3 : 1;
            const uint32_t format = comparison ? 41 : 2;
            TextureInput texture;
            texture.resource.id = 1;
            texture.resource.type = 0x85;
            texture.resource.desc = {8, 8, 1, layers, format, 1, 0, 0, 8, 0, cube ? 4u : 0u};
            texture.subresources = textureSubresources(texture.resource);
            const auto &last = texture.subresources.back();
            texture.storage = Buffer::known(std::vector<uint8_t>(last.offset + last.size));
            for (size_t i = 0; i < texture.storage.bytes.size() / 4; ++i) {
                const uint32_t bits = 0x3f000001u + uint32_t((i * 193) % 0x80000);
                std::memcpy(texture.storage.bytes.data() + i * 4, &bits, 4);
            }
            TextureInputView view;
            view.resource = 1;
            view.format = format;
            view.dimension = dimension == 3 ? 4 : dimension == 8 ? 5 : dimension == 6 ? 9 : 10;
            view.layerCount = layers;
            view.nativeDescriptor = {format, view.dimension, 0, 1, 0, dimension == 10 ? layers / 6 : layers};
            SamplerInput sampler;
            D3D11_SAMPLER_DESC sd{};
            sd.Filter =
                comparison ? D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
            sd.MaxAnisotropy = 1;
            sd.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            sampler.descriptor.emplace();
            std::memcpy(sampler.descriptor->data(), &sd, sizeof sd);
            Inputs inputs;
            inputs.gpu = gpu;
            inputs.textures = std::make_shared<TextureInputs>();
            inputs.textures->resources[1] = texture;
            inputs.textures->views[{7, {3}}] = view;
            inputs.textures->samplers[{6, {4}}] = sampler;
            nlohmann::json records = nlohmann::json::array();
            for (uint32_t mode = 0; mode < 8; ++mode)
                for (uint32_t direct = 0; direct < (stage == 5 ? 1u : 2u); ++direct) {
                    GatherOperation operation;
                    operation.opcode = opcode;
                    operation.component = comparison ? 0 : mode % 4;
                    operation.precision = {mode == 2 ? 0u : precision, mode ? precision : 0u,
                                           programmable && mode ? mode & 1 ? 5u : 4u : 0u,
                                           comparison && mode ? precision : 0u};
                    operation.outputDestination = direct != 0;
                    operation.refactoringAllowed = mode & 1;
                    operation.modifiers = {mode % 4, 0, comparison ? mode % 4 : 0};
                    operation.literal[mode == 5 && programmable ? 1
                                      : mode == 6 && comparison ? 2
                                                                : 0] = mode >= 4;
                    operation.offset = Value::known({31, 17, 0, 0});
                    operation.reference = Value::known({0x3f000901, 0, 0, 0});
                    const std::array<float, 4> coords = cube ? std::array<float, 4>{1, .2501f, -.5001f, 1}
                                                             : std::array<float, 4>{.43749f, .56251f, 1, 0};
                    const auto address = std::bit_cast<std::array<uint32_t, 4>>(coords);
                    const uint32_t mask = mode & 1 ? 9 : 15;
                    const auto code = precisionShader(stage, opcode, dimension, operation, mask, address);
                    const auto expected =
                        testing::directTexture(replay.nativeDevice(), code, stage, texture, view,
                                               {address, operation.offset.bits, operation.reference.bits},
                                               direct ? operation.precision[0] : 0, &sampler);
                    validateDevice(replay.nativeDevice());
                    inputs.constantBuffers[2] = {Value::known(address), operation.offset,
                                                 operation.reference};
                    Program program(code);
                    Lane lane(program, inputs);
                    lane.step();
                    const auto step = lane.step();
                    QVERIFY(step.memoryGpuAssisted);
                    const Register target{direct ? 2u : 0u, {direct ? 0u : 3u}};
                    const auto actual = lane.value(target);
                    if (actual != Value::known(expected))
                        qInfo().noquote() << "Gather precision mismatch"
                                          << QString::fromStdString(nlohmann::json{
                                                 {"mode", mode},
                                                 {"direct", direct},
                                                 {"precision", operation.precision},
                                                 {"actual", actual.bits},
                                                 {"expected", expected}}.dump());
                    QCOMPARE(actual, Value::known(expected));
                    inputs.textures->resources.at(1).storage.valid[0] = 0;
                    Lane unknown(program, inputs);
                    unknown.step();
                    unknown.step();
                    QCOMPARE(unknown.value(target).valid, uint8_t(15 ^ mask));
                    inputs.textures->resources.at(1).storage.valid[0] = 1;
                    std::array<int, 3> sourceValidity{-1, -1, -1};
                    for (unsigned source = 0; source < 3; ++source) {
                        if ((source == 1 && !programmable) || (source == 2 && !comparison))
                            continue;
                        inputs.constantBuffers.at(2)[source].valid = 0;
                        Lane missing(program, inputs);
                        missing.step();
                        const auto missingStep = missing.step();
                        QCOMPARE(missingStep.memoryGpuAssisted, operation.literal[source]);
                        sourceValidity[source] = missing.value(target).valid;
                        QCOMPARE(sourceValidity[source], operation.literal[source] ? 15 : int(15 ^ mask));
                        if (operation.literal[source])
                            QCOMPARE(missing.value(target), actual);
                        inputs.constantBuffers.at(2)[source].valid = 15;
                    }
                    bool traceChecked = false;
                    if (mode == 0 && !direct && dimension == 3) {
                        InputSnapshot snapshot;
                        snapshot.event = 19;
                        snapshot.shader = 7;
                        snapshot.bytecode = code;
                        snapshot.stage = stage == 0 ? 4 : stage == 1 ? 0 : 5;
                        snapshot.frameSha256 = std::string(64, '0');
                        snapshot.inputs = inputs;
                        if (stage == 5)
                            snapshot.inputs.memory = std::make_shared<Memory>(
                                std::map<uint64_t, Buffer>{{2, Buffer::known(std::vector<uint8_t>(16))}},
                                std::map<Register, BufferView>{{{30, {3}}, {2, 0, 16, 16}}});
                        const auto tracePath = temporary.filePath("precision-trace.jsonl");
                        if (stage == 5)
                            exportGroupExecutionTrace(tracePath, snapshot);
                        else if (stage == 1)
                            exportVertexExecutionTrace(tracePath, snapshot);
                        else
                            exportPixelExecutionTrace(tracePath, snapshot, {});
                        QFile file(tracePath);
                        QVERIFY(file.open(QIODevice::ReadOnly));
                        const auto digest =
                            QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256)
                                .toHex()
                                .toStdString();
                        file.close();
                        ExecutionTraceReader reader(tracePath, digest);
                        reader.moveTo(reader.size());
                        QCOMPARE(reader.registerValue(0, target), actual);
                        reader.moveTo(0);
                        QCOMPARE(reader.registerValue(0, target).valid, uint8_t(0));
                        reader.moveTo(reader.size());
                        QCOMPARE(reader.registerValue(0, target), actual);
                        traceChecked = true;
                    }
                    records.push_back({{"mode", mode},
                                       {"direct", direct},
                                       {"mask", mask},
                                       {"precision", operation.precision},
                                       {"modifiers", operation.modifiers},
                                       {"literal", operation.literal},
                                       {"component", operation.component},
                                       {"address", address},
                                       {"offset", operation.offset.bits},
                                       {"reference", operation.reference.bits},
                                       {"expected", expected},
                                       {"actual", actual.bits},
                                       {"valid", actual.valid},
                                       {"unknown_valid", unknown.value(target).valid},
                                       {"source_validity", sourceValidity},
                                       {"trace_roundtrip", traceChecked},
                                       {"code", QByteArray(reinterpret_cast<const char *>(code.data()),
                                                           qsizetype(code.size()))
                                                    .toHex()
                                                    .toStdString()}});
                }
            validateDevice(replay.nativeDevice());
            const auto root = qEnvironmentVariable("FLORA_GATHER_PRECISION_EVIDENCE");
            if (!root.isEmpty()) {
                QDir directory(root);
                if (!directory.mkpath("."))
                    throw std::runtime_error("Cannot create gather precision evidence directory");
                QFile file(directory.filePath(QString::fromLatin1(QTest::currentDataTag()) + ".json"));
                if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
                    throw std::runtime_error("Cannot create fresh gather precision evidence");
                const nlohmann::json report{{"schema", 1},
                                            {"probe", "minimum_precision_gather"},
                                            {"vendor", vendor},
                                            {"stage", stage},
                                            {"dimension", dimension},
                                            {"opcode", opcode},
                                            {"precision", precision},
                                            {"helper", gpu->identity()},
                                            {"records", records},
                                            {"passed", true}};
                const auto bytes = report.dump(2);
                if (file.write(bytes.data(), qint64(bytes.size())) != qint64(bytes.size()))
                    throw std::runtime_error("Cannot write gather precision evidence");
            }
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void originalShaders_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("stage");
        QTest::addColumn<uint32_t>("dimension");
        QTest::addColumn<uint32_t>("opcode");
        QTest::addColumn<uint32_t>("format");
        for (auto vendor : {0u, 4318u, 32902u})
            for (auto stage : {0u, 1u, 5u})
                for (auto dimension : {3u, 6u, 8u, 10u})
                    for (auto opcode : {109u, 126u, 127u, 128u}) {
                        if ((opcode == 127 || opcode == 128) && (dimension == 6 || dimension == 10))
                            continue;
                        for (auto format : {2u, 3u, 4u, 28u, 31u}) {
                            if ((opcode == 126 || opcode == 128) && format != 2)
                                continue;
                            const auto name = QString("%1-%2-%3-%4-%5")
                                                  .arg(vendor)
                                                  .arg(stage)
                                                  .arg(dimension)
                                                  .arg(opcode)
                                                  .arg(format)
                                                  .toUtf8();
                            QTest::newRow(name) << vendor << stage << dimension << opcode << format;
                        }
                    }
    }
    void originalShaders() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, stage);
        QFETCH(uint32_t, dimension);
        QFETCH(uint32_t, opcode);
        QFETCH(uint32_t, format);
        try {
            QTemporaryDir temporary;
            auto capture = testing::computeCapture(true);
            const auto path = temporary.filePath("device.gpa_frame");
            capture.save(path);
            Frame frame(path.toStdWString());
            ReplayOptions options;
            options.warp = vendor == 0;
            if (vendor)
                options.vendor = vendor;
            options.debug = options.before = true;
            options.until = 100;
            Replay replay(frame, options);
            auto gpu = std::make_shared<GpuOperations>(replay.nativeDevice(), vendor == 0);
            const bool comparison = opcode == 126 || opcode == 128;
            const bool programmable = opcode == 127 || opcode == 128;
            const bool cube = dimension == 6 || dimension == 10;
            const auto layers = dimension == 10 ? 12u : dimension == 6 ? 6u : dimension == 8 ? 3u : 1u;
            const auto actualFormat = comparison ? 41u : format;
            const auto returns = TextureInputs::loadReturnType(actualFormat) * 0x1111;
            const bool unsupported = format == 3 || format == 4;
            TextureInput texture;
            texture.resource.id = 1;
            texture.resource.type = 0x85;
            texture.resource.desc = {8, 8, 3, layers, actualFormat, 1, 0, 0, 8, 0, (cube ? 4u : 0u) | 0x80u};
            texture.subresources = textureSubresources(texture.resource);
            const auto &last = texture.subresources.back();
            texture.storage = Buffer::known(std::vector<uint8_t>(last.offset + last.size));
            for (size_t i = 0; i < texture.storage.bytes.size() / 4; ++i) {
                const uint32_t bits = format == 2   ? std::bit_cast<uint32_t>(float((i * 13) % 31) / 32.f)
                                      : format == 3 ? uint32_t(0x80000000u + i * 193)
                                                    : uint32_t(-731 - int32_t(i * 13));
                std::memcpy(texture.storage.bytes.data() + i * 4, &bits, 4);
            }
            Inputs inputs;
            inputs.gpu = gpu;
            inputs.textures = std::make_shared<TextureInputs>();
            nlohmann::json records = nlohmann::json::array();
            for (uint32_t mode = 0; mode < (unsupported ? 1u : 4u); ++mode)
                for (uint32_t component = 0; component < (comparison || unsupported ? 1u : 4u); ++component) {
                    TextureInputView view;
                    view.resource = 1;
                    view.format = actualFormat;
                    view.dimension = dimension == 3 ? 4 : dimension == 8 ? 5 : dimension == 6 ? 9 : 10;
                    view.firstMip = mode == 2 ? 1 : 0;
                    view.mipCount = 3 - view.firstMip;
                    view.firstLayer = mode == 2 ? dimension == 8 ? 1 : dimension == 10 ? 6 : 0 : 0;
                    view.layerCount = layers - view.firstLayer;
                    view.nativeDescriptor = {
                        actualFormat,    view.dimension,
                        view.firstMip,   view.mipCount,
                        view.firstLayer, dimension == 10 ? view.layerCount / 6 : view.layerCount};
                    texture.minLod = mode == 3 ? .75f : 0.f;
                    SamplerInput sampler;
                    D3D11_SAMPLER_DESC sd{};
                    sd.Filter = comparison ? D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR
                                           : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
                    sd.AddressU = sd.AddressV = sd.AddressW =
                        mode == 1 ? D3D11_TEXTURE_ADDRESS_BORDER : D3D11_TEXTURE_ADDRESS_WRAP;
                    sd.MaxAnisotropy = 1;
                    sd.ComparisonFunc = mode == 3 ? D3D11_COMPARISON_GREATER : D3D11_COMPARISON_LESS_EQUAL;
                    sd.MinLOD = 1;
                    sd.MaxLOD = 2;
                    for (unsigned c = 0; c < 4; ++c)
                        sd.BorderColor[c] = .25f * float(c);
                    sampler.descriptor.emplace();
                    std::memcpy(sampler.descriptor->data(), &sd, sizeof sd);
                    std::array<float, 4> coords =
                        cube ? std::array<float, 4>{1, mode == 1 ? 1.f : .25f, mode == 1 ? 1.f : -.5f, 0}
                             : std::array<float, 4>{mode == 1 ? -0.0625f : .4375f, .5625f, 0, 0};
                    if (mode == 0 && (dimension == 8 || dimension == 10))
                        coords[dimension == 8 ? 2 : 3] = 1;
                    const std::array<uint32_t, 4> address = std::bit_cast<std::array<uint32_t, 4>>(coords);
                    const std::array<uint32_t, 4> offset{mode == 1   ? uint32_t(-33)
                                                         : mode == 3 ? 64u
                                                                     : 31u,
                                                         mode == 1 ? 65u : uint32_t(-32), 17, 23};
                    const std::array<uint32_t, 4> reference{std::bit_cast<uint32_t>(.5f), 0, 0, 0};
                    const uint32_t mask = mode & 1 ? 9 : 15;
                    const auto code = shader(stage, opcode, dimension, returns, component, mask,
                                             !programmable && !cube && mode == 1);
                    const std::vector<std::array<uint32_t, 4>> values{address, offset, reference};
                    const auto expected = testing::directTexture(replay.nativeDevice(), code, stage, texture,
                                                                 view, values, 0, &sampler);
                    inputs.constantBuffers[2] = {Value::known(address), Value::known(offset),
                                                 Value::known(reference)};
                    inputs.textures->resources[1] = texture;
                    inputs.textures->views[{7, {3}}] = view;
                    inputs.textures->samplers[{6, {4}}] = sampler;
                    Program program(code);
                    Lane lane(program, inputs);
                    lane.step();
                    if (unsupported) {
                        UINT support = 0;
                        check(replay.nativeDevice()->CheckFormatSupport(DXGI_FORMAT(format), &support),
                              "Read original gather format support");
                        QVERIFY(!(support & D3D11_FORMAT_SUPPORT_SHADER_GATHER));
                        Com<ID3D11InfoQueue> queue;
                        check(replay.nativeDevice()->QueryInterface(IID_PPV_ARGS(&queue)),
                              "Read original gather rejection");
                        const auto diagnosticCode =
                            shader(stage, 109, dimension, returns, component, mask, false);
                        // The local debug layer does not flag every gather4_po format mismatch.
                        // An ordinary gather using the same binding provides a separate format control.
                        if (programmable)
                            testing::directTexture(replay.nativeDevice(), diagnosticCode, stage, texture,
                                                   view, values, 0, &sampler);
                        std::string diagnostic;
                        uint32_t diagnosticId = 0;
                        for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
                            SIZE_T size = 0;
                            check(queue->GetMessage(i, nullptr, &size), "Read gather rejection size");
                            std::vector<uint8_t> bytes(size);
                            auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
                            check(queue->GetMessage(i, message, &size), "Read gather rejection");
                            if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR) {
                                QCOMPARE(message->ID,
                                         D3D11_MESSAGE_ID_DEVICE_DRAW_RESOURCE_FORMAT_GATHER_UNSUPPORTED);
                                diagnostic = message->pDescription;
                                diagnosticId = uint32_t(message->ID);
                            }
                        }
                        QVERIFY(!diagnostic.empty());
                        queue->ClearStoredMessages();
                        std::string helperError;
                        try {
                            lane.step();
                        } catch (const std::runtime_error &error) {
                            helperError = error.what();
                        }
                        QCOMPARE(
                            helperError,
                            std::string("Selected device does not support sampling this texture format"));
                        QCOMPARE(lane.value({0, {3}}), Value::known({17, 23, 31, 47}));
                        records.push_back({{"mode", mode},
                                           {"component", component},
                                           {"supported", false},
                                           {"format_support", support},
                                           {"diagnostic_id", diagnosticId},
                                           {"diagnostic_opcode", 109},
                                           {"diagnostic_code",
                                            QByteArray(reinterpret_cast<const char *>(diagnosticCode.data()),
                                                       qsizetype(diagnosticCode.size()))
                                                .toHex()
                                                .toStdString()},
                                           {"diagnostic", diagnostic},
                                           {"helper_error", helperError},
                                           {"code", QByteArray(reinterpret_cast<const char *>(code.data()),
                                                               qsizetype(code.size()))
                                                        .toHex()
                                                        .toStdString()}});
                        continue;
                    }
                    validateDevice(replay.nativeDevice());
                    const auto step = lane.step();
                    const auto actual = lane.value({0, {3}});
                    QCOMPARE(actual.bits, expected);
                    QCOMPARE(actual.valid, uint8_t(15));
                    QVERIFY(step.memoryGpuAssisted);
                    QCOMPARE(step.changes.size(), size_t(1));
                    QCOMPARE(step.changes[0].before, Value::known({17, 23, 31, 47}));
                    inputs.textures->resources.at(1).storage.valid[0] = 0;
                    Lane unknown(program, inputs);
                    unknown.step();
                    unknown.step();
                    QCOMPARE(unknown.value({0, {3}}).valid, uint8_t(15 ^ mask));
                    inputs.textures->resources.at(1).storage.valid[0] = 1;
                    QCOMPARE(inputs.textures->resources.at(1).storage.bytes, texture.storage.bytes);
                    bool traceChecked = false;
                    if (mode == 0 && component == 0 && dimension == 3 && format == 2) {
                        InputSnapshot snapshot;
                        snapshot.event = 19;
                        snapshot.shader = 7;
                        snapshot.bytecode = code;
                        snapshot.stage = stage == 0 ? 4 : stage == 1 ? 0 : 5;
                        snapshot.frameSha256 = std::string(64, '0');
                        snapshot.inputs = inputs;
                        if (stage == 5)
                            snapshot.inputs.memory = std::make_shared<Memory>(
                                std::map<uint64_t, Buffer>{{2, Buffer::known(std::vector<uint8_t>(16))}},
                                std::map<Register, BufferView>{{{30, {3}}, {2, 0, 16, 16}}});
                        const auto tracePath = temporary.filePath("trace.jsonl");
                        if (stage == 5)
                            exportGroupExecutionTrace(tracePath, snapshot);
                        else if (stage == 1)
                            exportVertexExecutionTrace(tracePath, snapshot);
                        else
                            exportPixelExecutionTrace(tracePath, snapshot, {});
                        QFile file(tracePath);
                        QVERIFY(file.open(QIODevice::ReadOnly));
                        const auto digest =
                            QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256)
                                .toHex()
                                .toStdString();
                        file.close();
                        ExecutionTraceReader reader(tracePath, digest);
                        reader.moveTo(reader.size());
                        QCOMPARE(reader.registerValue(0, {0, {3}}), Value::known(expected));
                        reader.moveTo(0);
                        QCOMPARE(reader.registerValue(0, {0, {3}}).valid, uint8_t(0));
                        reader.moveTo(reader.size());
                        QCOMPARE(reader.registerValue(0, {0, {3}}), Value::known(expected));
                        traceChecked = true;
                    }
                    records.push_back({{"mode", mode},
                                       {"trace_roundtrip", traceChecked},
                                       {"component", component},
                                       {"mask", mask},
                                       {"address", address},
                                       {"offset", offset},
                                       {"reference", reference},
                                       {"expected", expected},
                                       {"actual", actual.bits},
                                       {"valid", actual.valid},
                                       {"unknown_valid", unknown.value({0, {3}}).valid},
                                       {"first_mip", view.firstMip},
                                       {"first_layer", view.firstLayer},
                                       {"resource_min_lod", texture.minLod},
                                       {"sampler", *sampler.descriptor},
                                       {"code", QByteArray(reinterpret_cast<const char *>(code.data()),
                                                           qsizetype(code.size()))
                                                    .toHex()
                                                    .toStdString()}});
                }
            validateDevice(replay.nativeDevice());
            const nlohmann::json report{{"schema", 1},
                                        {"probe", "native_texture_gather"},
                                        {"vendor", vendor},
                                        {"stage", stage},
                                        {"dimension", dimension},
                                        {"opcode", opcode},
                                        {"format", actualFormat},
                                        {"helper", gpu->identity()},
                                        {"records", records},
                                        {"passed", true}};
            const auto root = qEnvironmentVariable("FLORA_GATHER_EVIDENCE");
            if (!root.isEmpty()) {
                QDir directory(root);
                if (!directory.mkpath("."))
                    throw std::runtime_error("Cannot create gather evidence directory");
                QFile file(directory.filePath(QString::fromLatin1(QTest::currentDataTag()) + ".json"));
                if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
                    throw std::runtime_error("Cannot create fresh gather evidence");
                const auto bytes = report.dump(2);
                if (file.write(bytes.data(), qint64(bytes.size())) != qint64(bytes.size()))
                    throw std::runtime_error("Cannot write gather evidence");
            }
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void invalidAndUnavailable() {
        TextureInputs textures;
        const Register binding{7, {3}}, sampler{6, {4}};
        GatherOperation operation;
        bool assisted = true;
        const auto call = [&](uint32_t dimension = 3) {
            return textures.gatherTexture(5, binding, sampler, dimension, 0x5555, Value::known({}), operation,
                                          {3, 2, 1, 0}, 9, {}, nullptr, assisted);
        };
        QCOMPARE(call().valid, uint8_t(0));
        QVERIFY(!assisted);
        textures.views[binding] = {};
        textures.samplers[sampler] = {};
        QCOMPARE(call(), Value::known({}));
        QVERIFY(!assisted);
        operation.opcode = 128;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, call());
        textures.samplers[sampler].descriptor =
            std::array<uint32_t, 13>{0x95, 1, 1, 1, 0, 1, 4, 0, 0, 0, 0, 0, 0};
        QCOMPARE(call(), Value::known({}));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, call(6));
        operation.component = 1;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, call());
        operation.opcode = 109;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, call(2));
        operation.opcode = 0;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, call());
        for (unsigned operand = 0; operand < 4; ++operand) {
            operation = GatherOperation{};
            operation.opcode = 128;
            operation.precision[operand] = operand == 0 ? 3 : operand == 2 ? 1 : 4;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, call());
        }
        operation = GatherOperation{};
        operation.modifiers[0] = 4;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, call());
        operation = GatherOperation{};
        operation.literal[1] = true;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, call());
        operation = GatherOperation{};
        operation.outputDestination = true;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, call());
        for (auto opcode : {109u, 126u, 127u, 128u}) {
            auto code = shader(5, opcode, 3, 0x5555, 0, 15, false);
            auto parts = readDxbcParts(code);
            const auto found = std::find_if(parts.begin(), parts.end(),
                                            [](const auto &part) { return part.first == 0x58454853; });
            QVERIFY(found != parts.end());
            auto words = std::vector<uint8_t>(found->second.begin(), found->second.end());
            words[0] = opcode == 109 ? 0x40 : 0x41;
            found->second = words;
            const auto invalid = makeDxbc(parts);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, (void)Program(invalid));
        }
    }
};
QTEST_GUILESS_MAIN(DxbcGatherTests)
#include "DxbcGatherTests.moc"
