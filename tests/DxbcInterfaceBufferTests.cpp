#include "application/DxbcGpuOperations.h"
#include "application/DxbcInterfaceBindings.h"
#include "application/DxbcTextureInputs.h"
#include "application/HlslCompilation.h"
#include "replay/Replay.h"
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>
#include <bit>
#include <chrono>
#include <d3d11sdklayers.h>
using namespace flora;
using namespace flora::dxbc_exec;
using Json = nlohmann::json;
namespace {
Com<ID3D11Device> device(uint32_t vendor) {
    Com<IDXGIAdapter1> selected;
    if (vendor) {
        Com<IDXGIFactory1> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create class resource factory");
        for (UINT i = 0;; ++i) {
            Com<IDXGIAdapter1> adapter;
            const auto status = factory->EnumAdapters1(i, &adapter);
            if (status == DXGI_ERROR_NOT_FOUND)
                break;
            check(status, "Enumerate class resource adapter");
            DXGI_ADAPTER_DESC1 desc{};
            check(adapter->GetDesc1(&desc), "Read class resource adapter");
            if (desc.VendorId == vendor && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                selected = adapter;
                break;
            }
        }
        if (!selected)
            throw std::runtime_error("Requested class resource adapter is absent");
    }
    Com<ID3D11Device> result;
    check(D3D11CreateDevice(selected.Get(), vendor ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP, nullptr,
                            D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &result, nullptr,
                            nullptr),
          "Create class resource device");
    return result;
}
std::string name(ID3D11ClassInstance *instance, bool type) {
    SIZE_T size = 0;
    if (type)
        instance->GetTypeName(nullptr, &size);
    else
        instance->GetInstanceName(nullptr, &size);
    if (size > 4096)
        throw std::runtime_error("Class resource name budget");
    std::vector<char> text(size + 1);
    if (type)
        instance->GetTypeName(text.data(), &size);
    else
        instance->GetInstanceName(text.data(), &size);
    return text.data();
}
void save(const QString &path, Bytes bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size()) != qint64(bytes.size()))
        throw std::runtime_error("Cannot save class resource evidence");
}
} // namespace
class DxbcInterfaceBufferTests final : public QObject {
    Q_OBJECT
    QTemporaryDir temporary;
    QString root;
  private slots:
    void initTestCase() {
        root = qEnvironmentVariable("FLORA_INTERFACE_BUFFER_EVIDENCE", temporary.path());
        QVERIFY(QDir().mkpath(root));
    }
    void original_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("index");
        QTest::addColumn<bool>("created");
        QTest::addColumn<uint32_t>("mode");
        QTest::addColumn<bool>("second");
        for (auto vendor : {0u, 4318u, 32902u})
            for (auto index : {0u, 1u})
                for (auto created : {false, true})
                    for (auto mode : {0u, 1u, 2u, 3u})
                        for (auto second : {false, true})
                            QTest::newRow(qPrintable(QString("%1-%2-%3-%4-%5")
                                                         .arg(vendor)
                                                         .arg(index)
                                                         .arg(created)
                                                         .arg(mode)
                                                         .arg(second)))
                                << vendor << index << created << mode << second;
    }
    void original() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, index);
        QFETCH(bool, created);
        QFETCH(uint32_t, mode);

        QFETCH(bool, second);
        if (!qEnvironmentVariableIsSet("FLORA_INTERFACE_BUFFER_CHILD")) {
            QProcess child;
            auto environment = QProcessEnvironment::systemEnvironment();
            environment.insert("FLORA_INTERFACE_BUFFER_CHILD", "1");
            environment.insert("FLORA_INTERFACE_BUFFER_EVIDENCE", QDir(root).absolutePath());
            child.setProcessEnvironment(environment);
            child.setProcessChannelMode(QProcess::MergedChannels);
            const auto log = QDir(root).absoluteFilePath(QString(QTest::currentDataTag()) + ".child.txt");
            child.start(QCoreApplication::applicationFilePath(),
                        {QString("original:%1").arg(QTest::currentDataTag()), "-o", log + ",txt"});
            QVERIFY(child.waitForStarted(5000));
            const bool completed = child.waitForFinished(30000);
            if (!completed) {
                child.kill();
                QVERIFY(child.waitForFinished(5000));
            }
            const auto output = child.readAll();
            save(QDir(root).filePath(QString(QTest::currentDataTag()) + ".process.txt"),
                 Bytes(reinterpret_cast<const uint8_t *>(output.data()), output.size()));
            const auto status = Json{{"completed", completed},
                                     {"exit_code", child.exitCode()},
                                     {"crashed", child.exitStatus() == QProcess::CrashExit}}
                                    .dump(2);
            save(QDir(root).filePath(QString(QTest::currentDataTag()) + ".status.json"),
                 Bytes(reinterpret_cast<const uint8_t *>(status.data()), status.size()));
            QVERIFY2(completed, "Original class buffer child exceeded its deadline");
            QFile result(log);
            QVERIFY(result.open(QIODevice::ReadOnly));
            const auto diagnostics = result.readAll();
            QVERIFY2(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
                     diagnostics.constData());
            return;
        }
        try {
            const std::array<std::string, 4> types{"ByteAddressBuffer", "StructuredBuffer<uint4>",
                                                   "Buffer<uint>", "Texture2D<uint>"};
            const std::array<std::string, 4> bodies{"uint n;data.GetDimensions(n);return data.Load(i*4)+n;",
                                                    "uint n,s;data.GetDimensions(n,s);return data[i].z+n+s;",
                                                    "uint n;data.GetDimensions(n);return data[i]+n;",
                                                    "uint w,h,n;data.GetDimensions(i,w,h,n);return w+h+n;"};
            auto other = bodies[mode];
            other.insert(other.find("return ") + 7, "100+");
            const std::string source = "interface I{uint get(uint i);};class A:I{" + types[mode] +
                                       " data;uint get(uint i){" + bodies[mode] + "}};class B:I{" +
                                       types[mode] + " data;uint get(uint i){" + other +
                                       "}};A first[2];B second[2];I selected;"
                                       "RWStructuredBuffer<uint> dst:register(u0);[numthreads(1,1,1)]void "
                                       "main(){dst[0]=selected.get(" +
                                       (mode == 3 ? "1" : "2") + ");}";
            const auto code = compileHlsl(source, "cs_5_0").bytecode;
            const auto stem = QDir(root).filePath(QTest::currentDataTag());
            save(stem + ".dxbc", code);
            qInfo("Creating the original D3D11 device");
            auto gpu = device(vendor);
            Com<ID3D11DeviceContext> context;
            gpu->GetImmediateContext(&context);
            Com<ID3D11ClassLinkage> linkage;
            Com<ID3D11ComputeShader> shader;
            check(gpu->CreateClassLinkage(&linkage), "Create class resource linkage");
            qInfo("Creating the original class shader");
            check(gpu->CreateComputeShader(code.data(), code.size(), linkage.Get(), &shader),
                  "Create original class resource shader");
            qInfo("Created the original class shader");
            const auto textureSlot = created ? 5 + index : (second ? 2 : 0) + index;

            Com<ID3D11ClassInstance> instance;
            if (created)
                check(linkage->CreateClassInstance(second ? "B" : "A", 0, 0, textureSlot, 0, &instance),
                      "Create relocated resource class");
            else
                check(linkage->GetClassInstance(second ? "second" : "first", index, &instance),
                      "Get named resource class");
            D3D11_CLASS_INSTANCE_DESC desc{};
            qInfo("Reading the original class instance");
            instance->GetDesc(&desc);
            const InterfaceClassInstance observed{
                1,
                {desc.InstanceId, desc.InstanceIndex, desc.TypeId, desc.ConstantBuffer,
                 desc.BaseConstantBufferOffset, desc.BaseTexture, desc.BaseSampler, uint32_t(desc.Created)},
                name(instance.Get(), false),
                name(instance.Get(), true)};
            std::array<uint32_t, 32> words{};
            for (uint32_t i = 0; i < words.size(); ++i)
                words[i] = 100 + i;
            const auto first = mode == 0 ? 4 + 4 * index : 1 + index, count = 4 + index;
            const auto stride = mode == 1 ? 16u : 0u;
            const auto unit = mode == 1 ? 16u : 4u;
            Com<ID3D11Resource> resource;
            D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
            D3D11_TEXTURE2D_DESC td{16u << index,
                                    8u << index,
                                    4,
                                    1,
                                    DXGI_FORMAT_R32_UINT,
                                    {1, 0},
                                    D3D11_USAGE_DEFAULT,
                                    D3D11_BIND_SHADER_RESOURCE,
                                    0,
                                    0};
            if (mode == 3) {
                Com<ID3D11Texture2D> texture;
                check(gpu->CreateTexture2D(&td, nullptr, &texture), "Create original dimension texture");
                resource = texture;
                viewDesc.Format = td.Format;
                viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                viewDesc.Texture2D = {1, 3};
            } else {
                const D3D11_BUFFER_DESC sourceDesc{sizeof words,
                                                   D3D11_USAGE_IMMUTABLE,
                                                   D3D11_BIND_SHADER_RESOURCE,
                                                   0,
                                                   mode == 0   ? D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS
                                                   : mode == 1 ? D3D11_RESOURCE_MISC_BUFFER_STRUCTURED
                                                               : 0u,
                                                   stride};
                const D3D11_SUBRESOURCE_DATA initial{words.data(), 0, 0};
                Com<ID3D11Buffer> buffer;
                check(gpu->CreateBuffer(&sourceDesc, &initial, &buffer), "Create original class buffer");
                resource = buffer;
                viewDesc.Format = mode == 0   ? DXGI_FORMAT_R32_TYPELESS
                                  : mode == 1 ? DXGI_FORMAT_UNKNOWN
                                              : DXGI_FORMAT_R32_UINT;
                viewDesc.ViewDimension =
                    mode == 0 ? D3D11_SRV_DIMENSION_BUFFEREX : D3D11_SRV_DIMENSION_BUFFER;
                viewDesc.BufferEx = {first, count, mode == 0 ? D3D11_BUFFEREX_SRV_FLAG_RAW : 0u};
            }
            Com<ID3D11ShaderResourceView> srv;
            const auto viewStatus = gpu->CreateShaderResourceView(resource.Get(), &viewDesc, &srv);
            if (FAILED(viewStatus)) {
                Com<ID3D11InfoQueue> queue;
                check(gpu.As(&queue), "Read failed class view diagnostics");
                for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
                    SIZE_T size = 0;
                    check(queue->GetMessage(i, nullptr, &size), "Read failed view diagnostic size");
                    std::vector<uint8_t> data(size);
                    auto *message = reinterpret_cast<D3D11_MESSAGE *>(data.data());
                    check(queue->GetMessage(i, message, &size), "Read failed view diagnostic");
                    qWarning("%s", message->pDescription);
                }
            }
            check(viewStatus, "Create original class view range");
            qInfo("Created the original class view");
            D3D11_BUFFER_DESC bd{
                4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                4};
            Com<ID3D11Buffer> output, staging;
            Com<ID3D11UnorderedAccessView> uav;
            check(gpu->CreateBuffer(&bd, nullptr, &output), "Create original class output");
            check(gpu->CreateUnorderedAccessView(output.Get(), nullptr, &uav), "Create original class UAV");
            bd = {4, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
            check(gpu->CreateBuffer(&bd, nullptr, &staging), "Create original class readback");
            const auto bindings = resolveInterfaceBindings(code, std::span(&observed, 1));
            Inputs input;
            input.interfaceTables = bindings.tables;
            input.registers = bindings.registers;
            input.gpu = std::make_shared<GpuOperations>(gpu.Get(), !vendor);
            const auto *bytes = reinterpret_cast<const uint8_t *>(words.data());
            std::map<uint64_t, Buffer> storage{{2, Buffer::known(std::vector<uint8_t>(4))},
                                               {3, Buffer::known({bytes, bytes + sizeof words})}};
            std::map<Register, BufferView> views{{{30, {0}}, {2, 0, 4, 4}}};
            if (mode != 3)
                views[{7, {textureSlot}}] = {
                    3,      first * unit, count * unit,
                    stride, {},           mode == 2 ? uint32_t(DXGI_FORMAT_R32_UINT) : 0u};
            input.memory =
                std::make_shared<Memory>(storage, views, 256 * 1024 * 1024, 16 * 1024 * 1024, input.gpu);
            input.textures = std::make_shared<TextureInputs>();
            if (mode == 3) {
                auto &metadata = input.textures->metadata[3];
                metadata.resource.id = 3;
                metadata.resource.type = 0x85;
                metadata.resource.desc.resize(sizeof td / 4);
                std::memcpy(metadata.resource.desc.data(), &td, sizeof td);
                auto &view = input.textures->views[{7, {textureSlot}}];
                view.resource = 3;
                view.format = DXGI_FORMAT_R32_UINT;
                view.dimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                view.firstMip = 1;
                view.mipCount = 3;
                view.nativeDescriptor.resize(sizeof viewDesc / 4);
                std::memcpy(view.nativeDescriptor.data(), &viewDesc, sizeof viewDesc);
            }
            const Program program(code);
            QVERIFY(std::any_of(program.instructions().begin(), program.instructions().end(),
                                [](const auto &instruction) { return instruction.opcode == 120; }));
            Group group(program, input);
            uint32_t steps = 0;
            while (!group.finished()) {
                group.step();
                if (++steps > 1000)
                    throw std::runtime_error("Native class resource step budget");
            }
            const auto &result = input.memory->buffers().at(2);
            uint32_t actual;
            std::memcpy(&actual, result.bytes.data(), 4);
            const auto expected = (mode == 0   ? 122 + 8 * index
                                   : mode == 1 ? 134 + 5 * index
                                   : mode == 2 ? 107 + 2 * index
                                               : 3 + (6u << index)) +
                                  (second ? 100 : 0);
            const auto nativeText =
                Json{{"native", actual}, {"expected", expected},         {"valid", result.valid},
                     {"steps", steps},   {"gpu", input.gpu->identity()}, {"bindings", bindings.identity}}
                    .dump(2);
            save(stem + ".native.json",
                 Bytes(reinterpret_cast<const uint8_t *>(nativeText.data()), nativeText.size()));
            QCOMPARE(input.memory->buffers().at(3).bytes, storage.at(3).bytes);
            QCOMPARE(input.memory->buffers().at(3).valid, storage.at(3).valid);
            QCOMPARE(actual, expected);
            QCOMPARE(result.valid, std::vector<uint8_t>(4, 1));
            const auto dispatch = [&](ID3D11ComputeShader *selected, bool useClass) {
                auto *bound = instance.Get();
                context->CSSetShader(selected, useClass ? &bound : nullptr, useClass ? 1 : 0);
                context->CSSetShaderResources(textureSlot, 1, srv.GetAddressOf());
                context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
                const UINT seed[]{0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef};
                context->ClearUnorderedAccessViewUint(uav.Get(), seed);
                qInfo("Dispatching original shader (class: %d)", useClass);
                context->Dispatch(1, 1, 1);
                qInfo("Dispatched original shader (class: %d)", useClass);
                context->CopyResource(staging.Get(), output.Get());
                Com<ID3D11Query> completion;
                const D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
                check(gpu->CreateQuery(&qd, &completion), "Create original class deadline query");
                context->End(completion.Get());
                context->Flush();
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                for (;;) {
                    const auto hr = context->GetData(completion.Get(), nullptr, 0, 0);
                    check(hr, "Wait for original class resources");
                    if (hr == S_OK)
                        break;
                    if (std::chrono::steady_clock::now() >= deadline)
                        throw std::runtime_error("Original class resource deadline");
                    Sleep(1);
                }
                D3D11_MAPPED_SUBRESOURCE mapped{};
                check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
                      "Read original class resources");
                uint32_t original;
                std::memcpy(&original, mapped.pData, 4);
                context->Unmap(staging.Get(), 0);
                return original;
            };
            const std::string directSource =
                types[mode] + " data:register(t" + std::to_string(textureSlot) + ");uint get(uint i){" +
                (second ? other : bodies[mode]) +
                "}RWStructuredBuffer<uint> dst:register(u0);[numthreads(1,1,1)]void main(){"
                "dst[0]=get(" +
                (mode == 3 ? "1" : "2") + ");}";
            const auto directCode = compileHlsl(directSource, "cs_5_0").bytecode;
            save(stem + ".control.dxbc", directCode);
            Com<ID3D11ComputeShader> directShader;
            check(gpu->CreateComputeShader(directCode.data(), directCode.size(), nullptr, &directShader),
                  "Create direct resource control shader");
            const auto control = dispatch(directShader.Get(), false);
            const auto controlText = Json{{"control", control}, {"expected", expected}}.dump(2);
            save(stem + ".control.json",
                 Bytes(reinterpret_cast<const uint8_t *>(controlText.data()), controlText.size()));
            QCOMPARE(control, expected);
            const auto original = dispatch(shader.Get(), true);
            const auto originalText = Json{
                {"original", original},
                {"descriptor", observed.descriptor},
                {"name", observed.instanceName},
                {"type",
                 observed.typeName}}.dump(2);
            save(stem + ".original.json",
                 Bytes(reinterpret_cast<const uint8_t *>(originalText.data()), originalText.size()));
            const Json report{{"vendor", vendor},     {"created", created},
                              {"index", index},       {"mode", mode},
                              {"second", second},     {"bindings", bindings.identity},
                              {"original", original}, {"native", actual},
                              {"expected", expected}, {"steps", steps},
                              {"first", first},       {"count", count},
                              {"stride", stride},     {"gpu", input.gpu->identity()}};
            const auto text = report.dump(2);
            save(stem + ".json", Bytes(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
            QCOMPARE(original, expected);
            Com<ID3D11InfoQueue> queue;
            check(gpu->QueryInterface(IID_PPV_ARGS(&queue)), "Read class resource validation");
            for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
                SIZE_T size = 0;
                check(queue->GetMessage(i, nullptr, &size), "Read class diagnostic size");
                std::vector<uint8_t> data(size);
                auto *message = reinterpret_cast<D3D11_MESSAGE *>(data.data());
                check(queue->GetMessage(i, message, &size), "Read class diagnostic");
                QVERIFY2(message->Severity > D3D11_MESSAGE_SEVERITY_ERROR, message->pDescription);
            }
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
};
QTEST_GUILESS_MAIN(DxbcInterfaceBufferTests)
#include "DxbcInterfaceBufferTests.moc"
