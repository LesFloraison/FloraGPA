#include "HlslCompilation.h"
#include "NativePixel.h"
#include "replay/Replay.h"
#include <bit>
#include <cmath>
#include <d3d11sdklayers.h>
#include <dxgi.h>
namespace flora {
using Json = nlohmann::json;
class NativePixelDecoderState {
    Com<ID3D11Device> device_;
    Com<ID3D11DeviceContext> context_;
    Com<ID3D11ComputeShader> shader_;
    Com<ID3D11Buffer> output_, staging_, selection_;
    Com<ID3D11UnorderedAccessView> uav_;
    DXGI_ADAPTER_DESC adapter_{};
    bool debug_ = false;
    static DXGI_ADAPTER_DESC adapter(ID3D11Device *device, Com<IDXGIAdapter> &object) {
        Com<IDXGIDevice> dxgi;
        check(device->QueryInterface(IID_PPV_ARGS(&dxgi)), "Get pixel decoder DXGI device");
        check(dxgi->GetAdapter(&object), "Get pixel decoder adapter");
        DXGI_ADAPTER_DESC result{};
        check(object->GetDesc(&result), "Get pixel decoder adapter description");
        return result;
    }

  public:
    explicit NativePixelDecoderState(Replay &replay) {
        debug_ = replay.options().debug;
        Com<IDXGIAdapter> original;
        const auto expected = adapter(replay.nativeDevice(), original);
        const auto warp = replay.options().warp;
        const auto level = replay.featureLevel();
        if (level < D3D_FEATURE_LEVEL_11_0)
            throw std::runtime_error("Native pixel GPU decoding requires feature level 11.0");
        check(D3D11CreateDevice(warp ? nullptr : original.Get(),
                                warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                replay.options().debug ? D3D11_CREATE_DEVICE_DEBUG : 0, &level, 1,
                                D3D11_SDK_VERSION, &device_, nullptr, &context_),
              "Create isolated native pixel decoder device");
        Com<IDXGIAdapter> actual;
        adapter_ = adapter(device_.Get(), actual);
        if (adapter_.AdapterLuid.HighPart != expected.AdapterLuid.HighPart ||
            adapter_.AdapterLuid.LowPart != expected.AdapterLuid.LowPart)
            throw std::runtime_error("Native pixel decoder adapter differs from replay adapter");
        const auto code =
            compileHlsl(
                "Texture2D<float4> sourceTexture:register(t0);"
                "RWStructuredBuffer<uint4> result:register(u0);"
                "cbuffer Selection:register(b0){uint2 pixel;uint2 padding;}"
                "[numthreads(1,1,1)]void main(){result[0]=asuint(sourceTexture.Load(int3(pixel,0)));}",
                "cs_5_0")
                .bytecode;
        check(device_->CreateComputeShader(code.data(), code.size(), nullptr, &shader_),
              "Create native pixel decoder shader");
        D3D11_BUFFER_DESC output{
            16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
            16};
        check(device_->CreateBuffer(&output, nullptr, &output_), "Create native pixel decoder output");
        D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
        view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        view.Buffer.NumElements = 1;
        check(device_->CreateUnorderedAccessView(output_.Get(), &view, &uav_),
              "Create native pixel decoder UAV");
        output.Usage = D3D11_USAGE_STAGING;
        output.BindFlags = output.MiscFlags = output.StructureByteStride = 0;
        output.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(device_->CreateBuffer(&output, nullptr, &staging_), "Create native pixel decoder staging");
        D3D11_BUFFER_DESC constant{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        check(device_->CreateBuffer(&constant, nullptr, &selection_), "Create native pixel selection");
    }
    Json decode(Bytes storage, uint32_t format, uint32_t x, uint32_t y) {
        const bool block = (format >= 70 && format <= 84) || (format >= 94 && format <= 99);
        const auto extent = block ? 4u : 1u;
        if (storage.size() != pitches(extent, extent, format).first)
            throw std::invalid_argument("Native pixel GPU storage length mismatch");
        const D3D11_TEXTURE2D_DESC desc{extent,
                                        extent,
                                        1,
                                        1,
                                        DXGI_FORMAT(format),
                                        {1, 0},
                                        D3D11_USAGE_IMMUTABLE,
                                        D3D11_BIND_SHADER_RESOURCE,
                                        0,
                                        0};
        const D3D11_SUBRESOURCE_DATA data{storage.data(), UINT(storage.size()), UINT(storage.size())};
        Com<ID3D11Texture2D> texture;
        check(device_->CreateTexture2D(&desc, &data, &texture), "Create native pixel decoding texture");
        Com<ID3D11ShaderResourceView> view;
        check(device_->CreateShaderResourceView(texture.Get(), nullptr, &view),
              "Create native pixel decoding view");
        const std::array<uint32_t, 4> coordinate{x % extent, y % extent, 0, 0};
        context_->UpdateSubresource(selection_.Get(), 0, nullptr, coordinate.data(), 0, 0);
        auto *source = view.Get();
        auto *target = uav_.Get();
        auto *constant = selection_.Get();
        context_->CSSetShader(shader_.Get(), nullptr, 0);
        context_->CSSetShaderResources(0, 1, &source);
        context_->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
        context_->CSSetConstantBuffers(0, 1, &constant);
        context_->Dispatch(1, 1, 1);
        context_->ClearState();
        context_->CopyResource(staging_.Get(), output_.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped),
              "Read native pixel decoding result");
        std::array<uint32_t, 4> bits;
        std::memcpy(bits.data(), mapped.pData, sizeof bits);
        context_->Unmap(staging_.Get(), 0);
        if (debug_) {
            Com<ID3D11InfoQueue> queue;
            check(device_.As(&queue), "Get native pixel decoder validation queue");
            for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
                SIZE_T size = 0;
                check(queue->GetMessage(i, nullptr, &size), "Get decoder message size");
                std::vector<uint8_t> messageBytes(size);
                auto *message = reinterpret_cast<D3D11_MESSAGE *>(messageBytes.data());
                check(queue->GetMessage(i, message, &size), "Get decoder message");
                if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
                    throw std::runtime_error(message->pDescription);
            }
            queue->ClearStoredMessages();
        }
        Json rgba = Json::array();
        for (const auto value : bits) {
            const auto number = std::bit_cast<float>(value);
            rgba.push_back(std::isnan(number)   ? Json("nan")
                           : std::isinf(number) ? Json(number < 0 ? "-inf" : "inf")
                                                : Json(number));
        }
        Json channels = Json::array({true, true, true, true});
        if (format == 85 || format == 95 || format == 96)
            channels[3] = false;
        if (format == 80 || format == 81)
            channels = {true, false, false, false};
        if (format == 83 || format == 84)
            channels = {true, true, false, false};
        const bool srgb = format == 72 || format == 75 || format == 78 || format == 99;
        return {{"status", "decoded"},
                {"depth", nullptr},
                {"stencil", nullptr},
                {"source", "native_gpu_load"},
                {"gpu_adapter",
                 {{"luid_low", adapter_.AdapterLuid.LowPart}, {"luid_high", adapter_.AdapterLuid.HighPart}}},
                {"color",
                 {{"numeric_type", "float"},
                  {"rgba", rgba},
                  {"stored_channels", channels},
                  {"srgb_to_linear", srgb},
                  {"component_bits", bits}}}};
    }
};
NativePixelDecoder::NativePixelDecoder(Replay &replay) : replay_(replay) {}
NativePixelDecoder::~NativePixelDecoder() = default;
Json NativePixelDecoder::decode(Bytes storage, uint32_t format, uint32_t x, uint32_t y) {
    auto result = decodeNativePixel(storage, format);
    result["source"] = "storage_math";
    const bool block =
        !nativePixelTypeless(format) && ((format >= 70 && format <= 84) || (format >= 94 && format <= 99));
    if (format != 85 && format != 86 && !block)
        return result;
    if (!state_)
        state_ = std::make_unique<NativePixelDecoderState>(replay_);
    auto hardware = state_->decode(storage, format, x, y);
    hardware["storage_math_color"] = result.at("color");
    return hardware;
}
} // namespace flora
