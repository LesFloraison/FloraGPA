#include "application/DxbcGpuOperations.h"
#include "application/DxbcInterfaceBindings.h"
#include "application/DxbcTextureInputs.h"
#include "application/HlslCompilation.h"
#include "replay/Replay.h"
#include <QDir>
#include <QFile>
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
class DxbcInterfaceResourceTests final : public QObject {
    Q_OBJECT
    QTemporaryDir temporary;
    QString root;
  private slots:
    void initTestCase() {
        root = qEnvironmentVariable("FLORA_INTERFACE_RESOURCE_EVIDENCE", temporary.path());
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
        const bool sample = mode != 0;
        QFETCH(bool, second);
        try {
            const bool decoy = qEnvironmentVariable("FLORA_INTERFACE_RESOURCE_DECOY") == "1";
            if (decoy && (!created || index || mode != 2))
                throw std::runtime_error("Decoy control requires a relocated first-element Gather case");
            const std::string operation = mode == 0   ? "tex.Load(int3(0,1,0))"
                                          : mode == 1 ? "tex.SampleLevel(samp,float2(1.25,.75),0)"
                                          : mode == 2 ? "tex.GatherRed(samp,float2(1.25,.75)).x"
                                                      : "tex.SampleCmpLevelZero(samp,float2(1.25,.75),60)";
            const std::string samplerType = mode == 3 ? "SamplerComparisonState" : "SamplerState";
            const std::string source = "interface I{float get();};class A:I{Texture2D<float> tex;" +
                                       samplerType + " samp;float get(){return " + operation +
                                       ";}};class B:I{Texture2D<float> tex;" + samplerType +
                                       " samp;float get(){return " + operation +
                                       "+100;}};A first[2];B second[2];I selected;"
                                       "RWStructuredBuffer<float> dst:register(u0);[numthreads(1,1,1)]"
                                       "void main(){dst[0]=selected.get();}";
            const auto code = compileHlsl(source, "cs_5_0").bytecode;
            const auto stem = QDir(root).filePath(QTest::currentDataTag());
            save(stem + ".dxbc", code);
            auto gpu = device(vendor);
            Com<ID3D11DeviceContext> context;
            gpu->GetImmediateContext(&context);
            Com<ID3D11ClassLinkage> linkage;
            Com<ID3D11ComputeShader> shader;
            check(gpu->CreateClassLinkage(&linkage), "Create class resource linkage");
            check(gpu->CreateComputeShader(code.data(), code.size(), linkage.Get(), &shader),
                  "Create original class resource shader");
            const auto textureSlot = created ? 5 + index : (second ? 2 : 0) + index;
            const auto samplerSlot = created ? 7 + index : (second ? 2 : 0) + index;
            Com<ID3D11ClassInstance> instance;
            if (created)
                check(linkage->CreateClassInstance(second ? "B" : "A", 0, 0, textureSlot, samplerSlot,
                                                   &instance),
                      "Create relocated resource class");
            else
                check(linkage->GetClassInstance(second ? "second" : "first", index, &instance),
                      "Get named resource class");
            D3D11_CLASS_INSTANCE_DESC desc{};
            instance->GetDesc(&desc);
            const InterfaceClassInstance observed{
                1,
                {desc.InstanceId, desc.InstanceIndex, desc.TypeId, desc.ConstantBuffer,
                 desc.BaseConstantBufferOffset, desc.BaseTexture, desc.BaseSampler, uint32_t(desc.Created)},
                name(instance.Get(), false),
                name(instance.Get(), true)};
            const std::array<float, 4> pixels{float(10 + index), float(20 + index), float(30 + index),
                                              float(40 + index)};
            const D3D11_TEXTURE2D_DESC td{
                2, 2, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE,
                0, 0};
            const D3D11_SUBRESOURCE_DATA initial{pixels.data(), 8, 16};
            Com<ID3D11Texture2D> texture;
            Com<ID3D11ShaderResourceView> srv;
            check(gpu->CreateTexture2D(&td, &initial, &texture), "Create original class texture");
            check(gpu->CreateShaderResourceView(texture.Get(), nullptr, &srv), "Create original class SRV");
            D3D11_SAMPLER_DESC sd{};
            sd.Filter =
                mode == 3 ? D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_POINT;
            sd.AddressU = sd.AddressV = sd.AddressW =
                index ? D3D11_TEXTURE_ADDRESS_BORDER : D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.BorderColor[0] = 91;
            sd.ComparisonFunc = mode == 3 ? D3D11_COMPARISON_LESS : D3D11_COMPARISON_NEVER;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            Com<ID3D11SamplerState> sampler;
            check(gpu->CreateSamplerState(&sd, &sampler), "Create original class sampler");
            D3D11_BUFFER_DESC bd{
                4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                4};
            Com<ID3D11Buffer> output, staging;
            Com<ID3D11UnorderedAccessView> uav;
            check(gpu->CreateBuffer(&bd, nullptr, &output), "Create original class output");
            check(gpu->CreateUnorderedAccessView(output.Get(), nullptr, &uav), "Create original class UAV");
            bd = {4, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
            check(gpu->CreateBuffer(&bd, nullptr, &staging), "Create original class readback");
            auto *bound = instance.Get();
            context->CSSetShader(shader.Get(), &bound, 1);
            context->CSSetShaderResources(textureSlot, 1, srv.GetAddressOf());
            if (mode == 3) {
                // The debug layer validates every static comparison declaration,
                // including slots belonging to inactive class instances.
                const std::array<ID3D11SamplerState *, 4> declared{sampler.Get(), sampler.Get(),
                                                                   sampler.Get(), sampler.Get()};
                context->CSSetSamplers(0, UINT(declared.size()), declared.data());
            }
            context->CSSetSamplers(samplerSlot, 1, sampler.GetAddressOf());
            if (decoy) {
                const std::array<float, 4> otherPixels{210, 220, 230, 240};
                const D3D11_SUBRESOURCE_DATA otherInitial{otherPixels.data(), 8, 16};
                Com<ID3D11Texture2D> otherTexture;
                Com<ID3D11ShaderResourceView> otherView;
                check(gpu->CreateTexture2D(&td, &otherInitial, &otherTexture),
                      "Create unrelated control texture");
                check(gpu->CreateShaderResourceView(otherTexture.Get(), nullptr, &otherView),
                      "Create unrelated control SRV");
                context->CSSetShaderResources(0, 1, otherView.GetAddressOf());
                context->CSSetSamplers(0, 1, sampler.GetAddressOf());
            }
            context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
            const UINT seed[]{0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef};
            context->ClearUnorderedAccessViewUint(uav.Get(), seed);
            context->Dispatch(1, 1, 1);
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
            const auto originalText = Json{
                {"original", original},
                {"descriptor", observed.descriptor},
                {"name", observed.instanceName},
                {"type",
                 observed.typeName}}.dump(2);
            save(stem + ".original.json",
                 Bytes(reinterpret_cast<const uint8_t *>(originalText.data()), originalText.size()));
            const auto bindings = resolveInterfaceBindings(code, std::span(&observed, 1));
            Inputs input;
            input.interfaceTables = bindings.tables;
            input.registers = bindings.registers;
            input.gpu = std::make_shared<GpuOperations>(gpu.Get(), !vendor);
            input.memory = std::make_shared<Memory>(
                std::map<uint64_t, Buffer>{{2, Buffer::known(std::vector<uint8_t>(4))}},
                std::map<Register, BufferView>{{{30, {0}}, {2, 0, 4, 4}}});
            input.textures = std::make_shared<TextureInputs>();
            auto &native = input.textures->resources[3];
            native.resource.id = 3;
            native.resource.type = 0x85;
            native.resource.desc.resize(sizeof td / 4);
            std::memcpy(native.resource.desc.data(), &td, sizeof td);
            const auto *bytes = reinterpret_cast<const uint8_t *>(pixels.data());
            native.storage = Buffer::known({bytes, bytes + sizeof pixels});
            native.subresources = textureSubresources(native.resource);
            auto &view = input.textures->views[{7, {textureSlot}}];
            view.resource = 3;
            view.format = DXGI_FORMAT_R32_FLOAT;
            view.dimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            view.nativeDescriptor = {DXGI_FORMAT_R32_FLOAT, D3D11_SRV_DIMENSION_TEXTURE2D, 0, 1, 0, 0};
            auto &nativeSampler = input.textures->samplers[{6, {samplerSlot}}];
            nativeSampler.object = 4;
            nativeSampler.descriptor.emplace();
            std::memcpy(nativeSampler.descriptor->data(), &sd, sizeof sd);
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
            const auto expected = std::bit_cast<uint32_t>((mode == 3 ? float(index)
                                                           : sample  ? (index ? 91.f : 40.f)
                                                                     : pixels[2]) +
                                                          (second ? 100.f : 0.f));
            const Json report{{"vendor", vendor},
                              {"decoy", decoy},
                              {"created", created},
                              {"index", index},
                              {"sample", sample},
                              {"mode", mode},
                              {"second", second},
                              {"bindings", bindings.identity},
                              {"original", original},
                              {"native", actual},
                              {"expected", expected},
                              {"steps", steps},
                              {"gpu", input.gpu->identity()}};
            const auto text = report.dump(2);
            save(stem + ".json", Bytes(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
            if (vendor == 0 && !index && created && !second && mode == 1) {
                for (unsigned invalid = 0; invalid < 3; ++invalid) {
                    auto broken = input;
                    auto &pointer = broken.registers.at({29, {0}});
                    if (invalid == 0)
                        pointer.valid &= ~4u;
                    if (invalid == 1)
                        pointer.bits[2] = 128;
                    if (invalid == 2)
                        pointer.bits[3] = 16;
                    auto execute = [&] {
                        Group rejected(program, broken);
                        while (!rejected.finished())
                            rejected.step();
                    };
                    QVERIFY_THROWS_EXCEPTION(std::runtime_error, execute());
                }
            }
            QCOMPARE(actual, expected);
            QCOMPARE(original, expected);
            QCOMPARE(result.valid, std::vector<uint8_t>(4, 1));
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
QTEST_GUILESS_MAIN(DxbcInterfaceResourceTests)
#include "DxbcInterfaceResourceTests.moc"
