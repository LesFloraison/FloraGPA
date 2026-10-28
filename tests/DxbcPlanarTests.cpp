#include "StateCapture.h"
#include "TextureOperationOracle.h"
#include "application/DxbcGpuOperations.h"
#include "application/DxbcInputSnapshot.h"
#include "core/Dxbc.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <bit>
using namespace flora;
using namespace flora::dxbc_exec;
namespace {
using Words = std::vector<uint32_t>;
Words row(uint32_t opcode, std::initializer_list<Words> operands = {}) {
    Words result{opcode};
    for (const auto &operand : operands)
        result.insert(result.end(), operand.begin(), operand.end());
    result[0] |= uint32_t(result.size()) << 24;
    return result;
}
std::vector<uint8_t> shader(uint32_t stage, uint32_t dimension, uint32_t returns, uint32_t operation,
                            uint32_t mode) {
    std::vector<Words> rows{row(89, {{0x208e46, 2, 2}}), row(104, {{4}}),
                            row(88 | dimension << 11, {{0x1070f2, 3}, {returns * 0x1111}})};
    if (operation == 72 || operation == 109)
        rows.push_back(row(90, {{0x106000, 4}}));
    if (stage == 5) {
        rows.push_back(row(155, {{1, 1, 1}}));
        rows.push_back(row(158, {{0x11e0f2, 3}, {16}}));
    } else
        rows.push_back(row(101, {{0x1020f2, 0}}));
    const uint32_t mask = mode & 1 ? 9 : 15;
    rows.push_back(row(54, {{0x1000f2, 3}, {0x4002, 17, 23, 31, 47}}));
    const Words target{0x100002u | mask << 4, 3};
    if (operation == 45)
        rows.push_back(row(45, {target, {0x208e46, 2, 0}, {0x1071b6, 3}}));
    else if (operation == 61) {
        const auto query = mode == 0 ? 2u : mode == 1 ? 0u : 1u;
        rows.push_back(row(61 | query << 11, {target, {0x20800a, 2, 0}, {0x107e46, 3}}));
    } else if (operation == 72)
        rows.push_back(row(72, {target, {0x208e46, 2, 0}, {0x1071b6, 3}, {0x106000, 4}, {0x20800a, 2, 1}}));
    else
        rows.push_back(row(109, {target, {0x208e46, 2, 0}, {0x1071b6, 3}, {0x10600au | (mode % 2) << 4, 4}}));
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
    std::erase_if(parts, [](const auto &p) {
        return p.first == 0x58454853 || p.first == 0x52444853 || p.first == 0x54415453 ||
               p.first == 0x46454452;
    });
    const auto program = writeDxbcProgram({{stage << 16 | 0x50, 0}, rows});
    parts.emplace_back(0x58454853, program);
    return makeDxbc(parts);
}
void validation(ID3D11Device *device) {
    Com<ID3D11InfoQueue> queue;
    check(device->QueryInterface(IID_PPV_ARGS(&queue)), "Read planar reference validation queue");
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0;
        check(queue->GetMessage(i, nullptr, &size), "Read planar diagnostic size");
        std::vector<uint8_t> bytes(size);
        auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
        check(queue->GetMessage(i, message, &size), "Read planar diagnostic");
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
            throw std::runtime_error(message->pDescription);
    }
}
nlohmann::json sourceCapability(ID3D11Device *device, const TextureInput &input) {
    Com<ID3D11InfoQueue> queue;
    check(device->QueryInterface(IID_PPV_ARGS(&queue)), "Read planar capability queue");
    validation(device);
    queue->ClearStoredMessages();
    UINT support = 0;
    const auto formatStatus = device->CheckFormatSupport(DXGI_FORMAT(input.resource.desc[4]), &support);
    D3D11_TEXTURE2D_DESC descriptor{};
    std::memcpy(&descriptor, input.resource.desc.data(), sizeof descriptor);
    std::vector<D3D11_SUBRESOURCE_DATA> data;
    for (const auto &sub : input.subresources)
        data.push_back({input.storage.bytes.data() + sub.offset, UINT(sub.rowPitch), UINT(sub.slicePitch)});
    Com<ID3D11Texture2D> texture;
    const auto createStatus = device->CreateTexture2D(&descriptor, data.data(), &texture);
    nlohmann::json diagnostics = nlohmann::json::array();
    bool unsupported = false;
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0;
        check(queue->GetMessage(i, nullptr, &size), "Read planar capability diagnostic size");
        std::vector<uint8_t> bytes(size);
        auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
        check(queue->GetMessage(i, message, &size), "Read planar capability diagnostic");
        unsupported |= message->ID == D3D11_MESSAGE_ID_CREATETEXTURE2D_UNSUPPORTEDFORMAT;
        diagnostics.push_back({{"id", uint32_t(message->ID)},
                               {"severity", uint32_t(message->Severity)},
                               {"description", message->pDescription}});
    }
    const bool supported = SUCCEEDED(createStatus);
    if (supported) {
        check(formatStatus, "Query supported planar format");
        if (!(support & D3D11_FORMAT_SUPPORT_TEXTURE2D))
            throw std::runtime_error("Planar creation disagrees with format support");
        validation(device);
    } else if (createStatus != E_INVALIDARG || !unsupported || (support & D3D11_FORMAT_SUPPORT_TEXTURE2D))
        throw std::runtime_error("Unqualified planar source creation failure");
    queue->ClearStoredMessages();
    return {{"format_status", uint32_t(formatStatus)},
            {"support", support},
            {"create_status", uint32_t(createStatus)},
            {"supported", supported},
            {"diagnostics", diagnostics}};
}
nlohmann::json frameRoundTrip(uint32_t vendor, const TextureInput &texture, const TextureInputView &view,
                              Bytes code, const std::array<uint32_t, 4> &address,
                              const std::array<uint32_t, 4> &expected) {
    auto capture = testing::computeCapture(true);
    const std::set<Id> replaced{2, 3, 4, 5, 6, 10, 11, 12, 16, 17, 90, 110, 111};
    std::erase_if(capture.entries, [&](const auto &entry) { return replaced.contains(entry.id); });
    auto resource = testing::statePack(Id(0), Id(0));
    for (auto word : texture.resource.desc)
        testing::append(resource, word);
    testing::append(resource, Id(5));
    capture.add(4, 5, 0x85, resource);
    auto data = testing::word(uint32_t(texture.storage.bytes.size()));
    data.insert(data.end(), texture.storage.bytes.begin(), texture.storage.bytes.end());
    capture.add(5, 9, 1, data);
    auto srv = testing::statePack(Id(0), Id(0), Id(4));
    for (auto word : view.nativeDescriptor)
        testing::append(srv, word);
    capture.add(6, 5, 0x8c, srv);
    capture.add(2, 5, 0x83,
                testing::statePack(
                    Id(0), Id(0),
                    D3D11_BUFFER_DESC{32, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0}, Id(3)));
    capture.add(3, 9, 1, testing::statePack(32u, address, std::array<uint32_t, 4>{}));
    capture.add(10, 5, 0x83,
                testing::statePack(Id(0), Id(0),
                                   D3D11_BUFFER_DESC{16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                                                     D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16},
                                   Id(11)));
    capture.add(11, 9, 1, testing::statePack(16u, std::array<uint32_t, 4>{}));
    D3D11_UNORDERED_ACCESS_VIEW_DESC output{};
    output.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    output.Buffer.NumElements = 1;
    capture.add(12, 5, 0x8f, testing::statePack(Id(0), Id(0), Id(10), output));
    auto binary = testing::statePack(uint64_t(code.size()));
    binary.insert(binary.end(), code.begin(), code.end());
    testing::append(binary, Id(0));
    capture.add(16, 9, 0x81, binary);
    State state{};
    state.sampleMask = UINT32_MAX;
    state.omStart = 8;
    state.stages[5].shader = 15;
    state.stages[5].cb[2] = 2;
    state.stages[5].srv[3] = 6;
    state.csCount = 4;
    state.csUav[3] = 12;
    capture.add(17, 3, 3, testing::snapshot(state));
    QTemporaryDir directory;
    const auto path = directory.filePath("planar-roundtrip.gpa_frame");
    capture.save(path);
    const auto root = qEnvironmentVariable("FLORA_PLANAR_EVIDENCE");
    if (!root.isEmpty()) {
        QDir destination(root);
        if (!destination.mkpath(".") ||
            !QFile::copy(path,
                         destination.filePath(QString::fromLatin1(QTest::currentDataTag()) + ".gpa_frame")))
            throw std::runtime_error("Cannot preserve planar frame fixture");
    }
    Frame frame(path.toStdWString());
    ReplayOptions options;
    options.warp = vendor == 0;
    if (vendor)
        options.vendor = vendor;
    options.before = options.debug = true;
    options.until = 100;
    Replay replay(frame, options);
    const auto observed = snapshotResourceInputs(replay, 5);
    const auto before = replay.readTexture(4);
    const auto &captured = observed.inputs.textures->views.at({7, {3}});
    if (before != texture.storage.bytes ||
        observed.inputs.textures->resources.at(4).storage.bytes != before || captured.resource != 4 ||
        captured.planeSlice != view.planeSlice || captured.format != view.format ||
        observed.bindings.at({7, {3}}).view != 6 ||
        observed.bytecode != std::vector<uint8_t>(code.begin(), code.end()))
        throw std::runtime_error("Planar frame snapshot lost its identity, plane or storage");
    options.before = false;
    Replay reference(frame, options);
    reference.run();
    const auto result = reference.readBuffer(10);
    std::array<uint32_t, 4> words{};
    if (result.size() != sizeof words)
        throw std::runtime_error("Invalid planar replay output size");
    std::memcpy(words.data(), result.data(), sizeof words);
    if (words != expected)
        throw std::runtime_error("Planar frame replay disagrees with original shader");
    Program program(observed.bytecode);
    Group group(program, observed.inputs);
    while (!group.finished())
        group.step();
    if (observed.inputs.memory->buffers().at(10).bytes != result || replay.readTexture(4) != before)
        throw std::runtime_error("Planar native execution changed source storage or output");
    validation(replay.nativeDevice());
    validation(reference.nativeDevice());
    return {{"frame_sha256", observed.frameSha256},
            {"event", observed.event},
            {"shader", observed.shader},
            {"resource", 4},
            {"view", 6},
            {"plane", *captured.planeSlice},
            {"result", words},
            {"source_unchanged", true},
            {"helper", observed.inputs.gpu->identity()}};
}
} // namespace
class DxbcPlanarTests final : public QObject {
    Q_OBJECT
  private slots:
    void invalidMetadata() {
        Resource resource{};
        resource.type = 0x85;
        resource.desc = {8, 8, 1, 2, 103, 1, 0, 0, 8, 0, 0};
        TextureInputView view;
        view.format = 49;
        view.dimension = 5;
        view.planeSlice = 1;
        view.nativeDescriptor = {49, 5, 0, 1, 0, 2};
        const auto selected = textureInputPlane(resource, view, 0, 1).value();
        QCOMPARE(selected.width, 4u);
        QCOMPARE(selected.height, 4u);
        QCOMPARE(selected.offset, uint64_t(160));
        QCOMPARE(selected.size, uint64_t(32));
        for (unsigned mode = 0; mode < 10; ++mode) {
            auto badResource = resource;
            auto badView = view;
            uint32_t mip = 0, layer = 1;
            if (mode == 0)
                badView.planeSlice = 2;
            if (mode == 1)
                badView.format = 61;
            if (mode == 2)
                badResource.desc[0] = 7;
            if (mode == 3)
                badResource.desc[5] = 4;
            if (mode == 4)
                badResource.desc[2] = 2;
            if (mode == 5)
                badView.dimension = 8;
            if (mode == 6)
                badView.nativeDescriptor.pop_back();
            if (mode == 7)
                mip = 1;
            if (mode == 8)
                layer = 2;
            if (mode == 9)
                badResource.desc[4] = 28;
            QVERIFY_EXCEPTION_THROWN(textureInputPlane(badResource, badView, mip, layer), std::runtime_error);
        }
        view.planeSlice.reset();
        QCOMPARE(textureInputPlane(resource, view, 0, 1)->offset, selected.offset);
    }
    void originalShaders_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("stage");
        QTest::addColumn<uint32_t>("format");
        QTest::addColumn<uint32_t>("plane");
        QTest::addColumn<bool>("integer");
        QTest::addColumn<bool>("array");
        for (auto vendor : {0u, 4318u, 32902u})
            for (auto stage : {0u, 1u, 5u})
                for (auto format : {103u, 104u, 105u})
                    for (auto plane : {0u, 1u})
                        for (bool integer : {false, true})
                            for (bool array : {false, true})
                                QTest::newRow(qPrintable(QString("%1-%2-%3-%4-%5-%6")
                                                             .arg(vendor)
                                                             .arg(stage)
                                                             .arg(format)
                                                             .arg(plane)
                                                             .arg(integer)
                                                             .arg(array)))
                                    << vendor << stage << format << plane << integer << array;
    }
    void originalShaders() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, stage);
        QFETCH(uint32_t, format);
        QFETCH(uint32_t, plane);
        QFETCH(bool, integer);
        QFETCH(bool, array);
        try {
            QTemporaryDir temporary;
            auto capture = testing::computeCapture(true);
            const auto path = temporary.filePath("planar.gpa_frame");
            capture.save(path);
            Frame frame(path.toStdWString());
            ReplayOptions options;
            options.warp = vendor == 0;
            if (vendor)
                options.vendor = vendor;
            options.before = options.debug = true;
            options.until = 100;
            Replay replay(frame, options);
            auto gpu = std::make_shared<GpuOperations>(replay.nativeDevice(), vendor == 0);
            const uint32_t viewFormat = format == 103 ? plane     ? integer ? 50 : 49
                                                        : integer ? 62
                                                                  : 61
                                        : plane       ? integer ? 36 : 35
                                        : integer     ? 57
                                                      : 56;
            const uint32_t dimension = array ? 8 : 3, layers = array ? 2 : 1;
            TextureInput texture;
            texture.resource.id = 7;
            texture.resource.type = 0x85;
            texture.resource.desc = {8, 8, 1, layers, format, 1, 0, 0, 8, 0, 0};
            texture.subresources = textureSubresources(texture.resource);
            const auto &last = texture.subresources.back();
            texture.storage = Buffer::known(std::vector<uint8_t>(last.offset + last.size));
            for (size_t i = 0; i < texture.storage.bytes.size(); ++i)
                texture.storage.bytes[i] = uint8_t(i * 37 + i / 13 + 7);
            const auto capability = sourceCapability(replay.nativeDevice(), texture);
            const bool supported = capability.at("supported");
            SamplerInput sampler;
            D3D11_SAMPLER_DESC sd{};
            sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxAnisotropy = 1;
            sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            sampler.descriptor.emplace();
            std::memcpy(sampler.descriptor->data(), &sd, sizeof sd);
            Inputs inputs;
            inputs.gpu = gpu;
            inputs.textures = std::make_shared<TextureInputs>();
            inputs.textures->resources[7] = texture;
            inputs.textures->samplers[{6, {4}}] = sampler;
            nlohmann::json records = nlohmann::json::array();
            std::string nativeRejection;
            if (!supported) {
                TextureInputView view;
                view.resource = 7;
                view.format = viewFormat;
                view.dimension = array ? 5 : 4;
                view.layerCount = layers;
                view.planeSlice = plane;
                view.nativeDescriptor = {viewFormat, view.dimension, 0, 1, 0, layers};
                inputs.textures->views[{7, {3}}] = view;
                inputs.constantBuffers[2] = {Value::known({1, 1, 0, 0}), Value::known({})};
                const Program program(shader(stage, dimension, integer ? 4 : 5, 45, 0));
                Lane lane(program, inputs);
                lane.step();
                try {
                    lane.step();
                } catch (const std::exception &error) {
                    nativeRejection = error.what();
                }
                QVERIFY(QString::fromStdString(nativeRejection)
                            .contains("Create private texture snapshot: HRESULT 0x80070057"));
                QCOMPARE(gpu->operations(), uint64_t(0));
            }
            for (auto operation : {45u, 61u, 72u, 109u}) {
                if (!supported)
                    break;
                if (integer && (operation == 72 || operation == 109))
                    continue;
                for (uint32_t mode = 0; mode < 4; ++mode) {
                    TextureInputView view;
                    view.resource = 7;
                    view.format = viewFormat;
                    view.dimension = array ? 5 : 4;
                    view.firstLayer = array && mode == 3 ? 1 : 0;
                    view.layerCount = layers - view.firstLayer;
                    if (mode != 1)
                        view.planeSlice = plane;
                    view.nativeDescriptor = {viewFormat, view.dimension,  0,
                                             1,          view.firstLayer, view.layerCount};
                    inputs.textures->views[{7, {3}}] = view;
                    const uint32_t width = plane ? 4 : 8;
                    const uint32_t relativeLayer = array && mode != 3 ? 1 : 0;
                    const std::array<uint32_t, 4> address =
                        operation == 45 ? std::array<uint32_t, 4>{mode == 2 ? width : 1, 1, relativeLayer, 0}
                        : operation == 61 ? std::array<uint32_t, 4>{mode == 2 ? 1u : 0u, 0, 0, 0}
                                          : std::bit_cast<std::array<uint32_t, 4>>(std::array<float, 4>{
                                                .4375f, .5625f, float(relativeLayer), 0});
                    const auto code = shader(stage, dimension, integer ? 4 : 5, operation, mode);
                    const auto expected = testing::directTexture(replay.nativeDevice(), code, stage, texture,
                                                                 view, {address, {}}, 0, &sampler);
                    validation(replay.nativeDevice());
                    inputs.constantBuffers[2] = {Value::known(address), Value::known({})};
                    const Program program(code);
                    Lane lane(program, inputs);
                    lane.step();
                    const auto step = lane.step();
                    const auto actual = lane.value({0, {3}});
                    if (actual != Value::known(expected))
                        qInfo().noquote() << QString::fromStdString(nlohmann::json{
                            {"operation", operation},
                            {"mode", mode},
                            {"actual", actual.bits},
                            {"valid", actual.valid},
                            {"expected", expected}}.dump());
                    QCOMPARE(actual, Value::known(expected));
                    const auto selectedLayer = view.firstLayer + relativeLayer;
                    const auto selectedPlane =
                        texturePlane(texture.resource, 0, selectedLayer, plane ? "uv" : "y", viewFormat)
                            .value();
                    const auto otherPlane =
                        texturePlane(texture.resource, 0, selectedLayer, plane ? "y" : "uv").value();
                    inputs.textures->resources.at(7).storage.valid[otherPlane.offset] = 0;
                    Lane other(program, inputs);
                    other.step();
                    other.step();
                    QCOMPARE(other.value({0, {3}}), actual);
                    inputs.textures->resources.at(7).storage.valid[otherPlane.offset] = 1;
                    const uint32_t pixelBytes = (format == 103 ? 1 : 2) * (plane ? 2 : 1);
                    const auto selectedByte = selectedPlane.offset + selectedPlane.rowPitch + pixelBytes;
                    inputs.textures->resources.at(7).storage.valid[selectedByte] = 0;
                    Lane unknown(program, inputs);
                    unknown.step();
                    unknown.step();
                    const auto missing = unknown.value({0, {3}});
                    const uint32_t mask = mode & 1 ? 9 : 15;
                    const uint32_t wantedValid = operation == 61 || (operation == 45 && mode == 2) ? 15
                                                 : operation == 45                                 ? 7
                                                                   : 15 ^ mask;
                    QCOMPARE(missing.valid, uint8_t(wantedValid));
                    inputs.textures->resources.at(7).storage.valid[selectedByte] = 1;
                    QCOMPARE(inputs.textures->resources.at(7).storage.bytes, texture.storage.bytes);
                    nlohmann::json frameEvidence = nullptr;
                    if (stage == 5 && format == 103 && operation == 45 && mode == 0)
                        frameEvidence = frameRoundTrip(vendor, texture, view, code, address, expected);
                    records.push_back({{"operation", operation},
                                       {"mode", mode},
                                       {"address", address},
                                       {"first_layer", view.firstLayer},
                                       {"plane_explicit", view.planeSlice.has_value()},
                                       {"expected", expected},
                                       {"actual", actual.bits},
                                       {"valid", actual.valid},
                                       {"other_plane_valid", other.value({0, {3}}).valid},
                                       {"unknown_valid", missing.valid},
                                       {"assisted", step.memoryGpuAssisted},
                                       {"frame", frameEvidence},
                                       {"code", QByteArray(reinterpret_cast<const char *>(code.data()),
                                                           qsizetype(code.size()))
                                                    .toHex()
                                                    .toStdString()}});
                }
            }
            validation(replay.nativeDevice());
            const auto root = qEnvironmentVariable("FLORA_PLANAR_EVIDENCE");
            if (!root.isEmpty()) {
                QDir directory(root);
                if (!directory.mkpath("."))
                    throw std::runtime_error("Cannot create planar evidence directory");
                QFile file(directory.filePath(QString::fromLatin1(QTest::currentDataTag()) + ".json"));
                if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
                    throw std::runtime_error("Cannot create fresh planar evidence");
                const nlohmann::json report{{"schema", 1},
                                            {"probe", "native_planar_srv"},
                                            {"vendor", vendor},
                                            {"stage", stage},
                                            {"format", format},
                                            {"plane", plane},
                                            {"integer", integer},
                                            {"array", array},
                                            {"view_format", viewFormat},
                                            {"helper", gpu->identity()},
                                            {"capability", capability},
                                            {"native_rejection", nativeRejection},
                                            {"records", records},
                                            {"passed", true}};
                const auto bytes = report.dump(2);
                if (file.write(bytes.data(), qint64(bytes.size())) != qint64(bytes.size()))
                    throw std::runtime_error("Cannot write planar evidence");
            }
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
};
QTEST_GUILESS_MAIN(DxbcPlanarTests)
#include "DxbcPlanarTests.moc"
