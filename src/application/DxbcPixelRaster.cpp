#include "DxbcPixelRaster.h"
#include "DxbcGpuCompletion.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <d3d11sdklayers.h>

namespace flora::dxbc_exec {
namespace {
uint32_t mipSize(uint32_t size, uint32_t mip) {
    if (mip >= 32)
        throw std::runtime_error("Invalid pixel raster target mip");
    return std::max(1u, size >> mip);
}
void resourceSize(PixelRasterTarget &target, ID3D11Resource *resource, uint32_t mip, uint32_t firstLayer) {
    uint32_t availableLayers = 1;
    resource->GetType(&target.dimension);
    if (target.dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
        target.height = target.layers = 1;
    } else if (target.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
        Com<ID3D11Texture1D> texture;
        check(resource->QueryInterface(IID_PPV_ARGS(&texture)), "Query pixel raster 1D target");
        D3D11_TEXTURE1D_DESC desc{};
        texture->GetDesc(&desc);
        target.width = mipSize(desc.Width, mip);
        availableLayers = desc.ArraySize;
    } else if (target.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        Com<ID3D11Texture2D> texture;
        check(resource->QueryInterface(IID_PPV_ARGS(&texture)), "Query pixel raster 2D target");
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        target.width = mipSize(desc.Width, mip);
        target.height = mipSize(desc.Height, mip);
        target.samples = desc.SampleDesc;
        availableLayers = desc.ArraySize;
    } else if (target.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
        Com<ID3D11Texture3D> texture;
        check(resource->QueryInterface(IID_PPV_ARGS(&texture)), "Query pixel raster 3D target");
        D3D11_TEXTURE3D_DESC desc{};
        texture->GetDesc(&desc);
        target.width = mipSize(desc.Width, mip);
        target.height = mipSize(desc.Height, mip);
        availableLayers = mipSize(desc.Depth, mip);
    } else
        throw std::runtime_error("Unsupported pixel raster resource dimension");
    if (firstLayer >= availableLayers)
        throw std::runtime_error("Pixel raster first layer exceeds resource");
    if (target.layers == UINT32_MAX)
        target.layers = availableLayers - firstLayer;
    if (!target.layers || target.layers > availableLayers - firstLayer)
        throw std::runtime_error("Pixel raster layer extent exceeds resource");
}
DXGI_ADAPTER_DESC adapter(ID3D11Device *device, Com<IDXGIAdapter> &result) {
    Com<IDXGIDevice> dxgi;
    check(device->QueryInterface(IID_PPV_ARGS(&dxgi)), "Query pixel raster adapter");
    check(dxgi->GetAdapter(&result), "Get pixel raster adapter");
    DXGI_ADAPTER_DESC desc{};
    check(result->GetDesc(&desc), "Describe pixel raster adapter");
    return desc;
}
void validate(ID3D11Device *device) {
    Com<ID3D11InfoQueue> queue;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))))
        return;
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0;
        check(queue->GetMessage(i, nullptr, &size), "Size pixel raster validation");
        std::vector<uint8_t> bytes(size);
        auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
        check(queue->GetMessage(i, message, &size), "Read pixel raster validation");
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
            throw std::runtime_error(message->pDescription);
    }
}
} // namespace
PixelRasterState pixelRasterState(ID3D11DeviceContext *context, std::optional<uint32_t> colorSlot) {
    if (!context || (colorSlot && *colorSlot >= 8))
        throw std::runtime_error("Invalid pixel raster state request");
    PixelRasterState result;
    auto &rs = result.rasterizer;
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_BACK;
    rs.DepthClipEnable = true;
    Com<ID3D11RasterizerState> raster;
    context->RSGetState(&raster);
    if (raster) {
        Com<ID3D11RasterizerState2> r2;
        Com<ID3D11RasterizerState1> r1;
        if (SUCCEEDED(raster.As(&r2)))
            r2->GetDesc2(&rs);
        else if (SUCCEEDED(raster.As(&r1))) {
            D3D11_RASTERIZER_DESC1 old{};
            r1->GetDesc1(&old);
            std::memcpy(&rs, &old, sizeof old);
        } else {
            D3D11_RASTERIZER_DESC old{};
            raster->GetDesc(&old);
            std::memcpy(&rs, &old, sizeof old);
        }
    }
    UINT count = 16;
    result.viewports.resize(count);
    context->RSGetViewports(&count, result.viewports.data());
    result.viewports.resize(count);
    count = 16;
    result.scissors.resize(count);
    context->RSGetScissorRects(&count, result.scissors.data());
    result.scissors.resize(count);
    context->OMGetBlendState(nullptr, nullptr, &result.sampleMask);
    std::array<ID3D11RenderTargetView *, 8> raw{};
    Com<ID3D11DepthStencilView> depth;
    context->OMGetRenderTargets(8, raw.data(), &depth);
    std::array<Com<ID3D11RenderTargetView>, 8> colors;
    for (size_t i = 0; i < raw.size(); ++i)
        colors[i].Attach(raw[i]);
    Com<ID3D11RenderTargetView> color;
    if (colorSlot) {
        color = colors[*colorSlot];
        if (!color)
            throw std::runtime_error("Requested pixel raster color view is not bound");
    } else
        for (const auto &candidate : colors)
            if (candidate) {
                color = candidate;
                break;
            }
    auto &target = result.target;
    Com<ID3D11Resource> resource;
    uint32_t mip = 0, firstLayer = 0;
    if (color) {
        D3D11_RENDER_TARGET_VIEW_DESC desc{};
        color->GetDesc(&desc);
        color->GetResource(&resource);
        target.format = desc.Format;
        switch (desc.ViewDimension) {
        case D3D11_RTV_DIMENSION_BUFFER:
            target.width = desc.Buffer.NumElements;
            break;
        case D3D11_RTV_DIMENSION_TEXTURE1D:
            mip = desc.Texture1D.MipSlice;
            break;
        case D3D11_RTV_DIMENSION_TEXTURE1DARRAY:
            mip = desc.Texture1DArray.MipSlice;
            target.layers = desc.Texture1DArray.ArraySize;
            firstLayer = desc.Texture1DArray.FirstArraySlice;
            break;
        case D3D11_RTV_DIMENSION_TEXTURE2D:
            mip = desc.Texture2D.MipSlice;
            break;
        case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
            mip = desc.Texture2DArray.MipSlice;
            target.layers = desc.Texture2DArray.ArraySize;
            firstLayer = desc.Texture2DArray.FirstArraySlice;
            break;
        case D3D11_RTV_DIMENSION_TEXTURE2DMS:
            break;
        case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY:
            target.layers = desc.Texture2DMSArray.ArraySize;
            firstLayer = desc.Texture2DMSArray.FirstArraySlice;
            break;
        case D3D11_RTV_DIMENSION_TEXTURE3D:
            mip = desc.Texture3D.MipSlice;
            target.layers = desc.Texture3D.WSize;
            firstLayer = desc.Texture3D.FirstWSlice;
            break;
        default:
            throw std::runtime_error("Unsupported pixel raster color view");
        }
    } else if (depth) {
        D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
        depth->GetDesc(&desc);
        depth->GetResource(&resource);
        target.format = desc.Format;
        target.depth = true;
        switch (desc.ViewDimension) {
        case D3D11_DSV_DIMENSION_TEXTURE1D:
            mip = desc.Texture1D.MipSlice;
            break;
        case D3D11_DSV_DIMENSION_TEXTURE1DARRAY:
            mip = desc.Texture1DArray.MipSlice;
            target.layers = desc.Texture1DArray.ArraySize;
            firstLayer = desc.Texture1DArray.FirstArraySlice;
            break;
        case D3D11_DSV_DIMENSION_TEXTURE2D:
            mip = desc.Texture2D.MipSlice;
            break;
        case D3D11_DSV_DIMENSION_TEXTURE2DARRAY:
            mip = desc.Texture2DArray.MipSlice;
            target.layers = desc.Texture2DArray.ArraySize;
            firstLayer = desc.Texture2DArray.FirstArraySlice;
            break;
        case D3D11_DSV_DIMENSION_TEXTURE2DMS:
            break;
        case D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY:
            target.layers = desc.Texture2DMSArray.ArraySize;
            firstLayer = desc.Texture2DMSArray.FirstArraySlice;
            break;
        default:
            throw std::runtime_error("Unsupported pixel raster depth view");
        }
    }
    if (resource)
        resourceSize(target, resource.Get(), mip, firstLayer);
    return result;
}
nlohmann::json pixelRasterIdentity(const PixelRasterState &state) {
    const auto &t = state.target;
    const auto &r = state.rasterizer;
    nlohmann::json result{{"target",
                           {{"dimension", t.dimension},
                            {"width", t.width},
                            {"height", t.height},
                            {"layers", t.layers},
                            {"format", t.format},
                            {"samples", t.samples.Count},
                            {"quality", t.samples.Quality},
                            {"depth", t.depth}}},
                          {"rasterizer",
                           {{"fill", r.FillMode},
                            {"cull", r.CullMode},
                            {"front_ccw", r.FrontCounterClockwise},
                            {"depth_bias", r.DepthBias},
                            {"depth_bias_clamp", r.DepthBiasClamp},
                            {"slope_depth_bias", r.SlopeScaledDepthBias},
                            {"depth_clip", r.DepthClipEnable},
                            {"scissor", r.ScissorEnable},
                            {"multisample", r.MultisampleEnable},
                            {"antialiased_line", r.AntialiasedLineEnable},
                            {"forced_samples", r.ForcedSampleCount},
                            {"conservative", r.ConservativeRaster}}},
                          {"viewports", nlohmann::json::array()},
                          {"scissors", nlohmann::json::array()},
                          {"sample_mask", state.sampleMask}};
    for (const auto &v : state.viewports)
        result["viewports"].push_back({v.TopLeftX, v.TopLeftY, v.Width, v.Height, v.MinDepth, v.MaxDepth});
    for (const auto &s : state.scissors)
        result["scissors"].push_back({s.left, s.top, s.right, s.bottom});
    return result;
}
PixelRasterInputs capturePixelRasterInputs(ID3D11Device *reference, bool warp, const PixelRasterState &state,
                                           const PixelGeometryBridge &bridge, const PixelInputShader &pixel,
                                           uint64_t maxBytes, CancelCheck cancelled, uint32_t timeoutMs) {
    checkCancellation(cancelled);
    if (!reference || !pixel.records || !timeoutMs || timeoutMs > 60000 || !maxBytes ||
        bridge.verticesPerPrimitive < 1 || bridge.verticesPerPrimitive > 3 || !bridge.stride ||
        bridge.stride % 16 || bridge.stride > 512 || bridge.vertices.empty() ||
        bridge.vertices.size() % (size_t(bridge.verticesPerPrimitive) * bridge.stride) ||
        bridge.vertices.size() / bridge.stride > UINT32_MAX ||
        bridge.identity.at("input_register") != pixel.reg || state.viewports.size() > 16 ||
        state.scissors.size() > 16)
        throw std::runtime_error("Invalid private pixel raster request");
    const auto primitiveCount = bridge.vertices.size() / bridge.stride / bridge.verticesPerPrimitive;
    if (bridge.identity.value("primitive_count", uint64_t(1)) != primitiveCount)
        throw std::runtime_error("Private pixel geometry range count differs from storage");
    const auto &selection = *pixel.records;
    const auto &target = state.target;
    const bool layerOutput =
        std::any_of(bridge.identity.at("output_mapping").begin(), bridge.identity.at("output_mapping").end(),
                    [](const auto &field) { return field.at("system_value") == 4; });
    if (layerOutput && !selection.layerInput)
        throw std::runtime_error("Pixel raster layer output requires an observed layer input");
    const auto samples =
        state.rasterizer.ForcedSampleCount ? state.rasterizer.ForcedSampleCount : target.samples.Count;
    if (!selection.maxRecords || selection.maxRecords > 4096 || !samples || samples > 32 ||
        selection.sample >= samples ||
        (target.dimension != D3D11_RESOURCE_DIMENSION_UNKNOWN &&
         (!target.width || !target.height || !target.layers || selection.x >= target.width ||
          selection.y >= target.height || selection.layer >= target.layers)))
        throw std::runtime_error("Private pixel raster selection is outside the target");
    for (const auto &v : state.viewports)
        if (!std::isfinite(v.TopLeftX) || !std::isfinite(v.TopLeftY) || !std::isfinite(v.Width) ||
            !std::isfinite(v.Height) || !std::isfinite(v.MinDepth) || !std::isfinite(v.MaxDepth) ||
            v.Width < 0 || v.Height < 0)
            throw std::runtime_error("Invalid private pixel raster viewport");
    const UINT recordBytes = 16 + 144 * selection.maxRecords;
    uint64_t required = uint64_t(recordBytes) * 2 + bridge.vertices.size(), targetBytes = 0;
    if (target.dimension != D3D11_RESOURCE_DIMENSION_UNKNOWN) {
        const auto [pitch, rows] = pitches(target.width, target.height, target.format);
        targetBytes = uint64_t(pitch) * rows;
        if (!targetBytes || target.layers > UINT64_MAX / targetBytes ||
            target.samples.Count > UINT64_MAX / (targetBytes * target.layers))
            throw std::runtime_error("Private pixel raster allocation overflow");
        targetBytes *= uint64_t(target.layers) * target.samples.Count;
        if (targetBytes > UINT64_MAX - required)
            throw std::runtime_error("Private pixel raster allocation overflow");
        required += targetBytes;
    }
    if (required > maxBytes)
        throw std::runtime_error("Private pixel raster allocation budget exceeded");
    Com<IDXGIAdapter> sourceAdapter;
    const auto expected = adapter(reference, sourceAdapter);
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    const auto level = reference->GetFeatureLevel();
    const auto flags = reference->GetCreationFlags() & D3D11_CREATE_DEVICE_DEBUG;
    check(D3D11CreateDevice(warp ? nullptr : sourceAdapter.Get(),
                            warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, &level, 1,
                            D3D11_SDK_VERSION, &device, nullptr, &context),
          "Create private pixel raster device");
    Com<IDXGIAdapter> actualAdapter;
    const auto actual = adapter(device.Get(), actualAdapter);
    if (std::memcmp(&actual.AdapterLuid, &expected.AdapterLuid, sizeof(LUID)))
        throw std::runtime_error("Private pixel raster adapter changed");
    checkCancellation(cancelled);
    Com<ID3D11Resource> carrier;
    const UINT bind = target.depth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET;
    if (target.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        const D3D11_TEXTURE2D_DESC desc{target.width,
                                        target.height,
                                        1,
                                        target.layers,
                                        target.format,
                                        target.samples,
                                        D3D11_USAGE_DEFAULT,
                                        bind,
                                        0,
                                        0};
        Com<ID3D11Texture2D> texture;
        check(device->CreateTexture2D(&desc, nullptr, &texture), "Create private pixel 2D target");
        carrier = texture;
    } else if (target.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
        const D3D11_TEXTURE1D_DESC desc{
            target.width, 1, target.layers, target.format, D3D11_USAGE_DEFAULT, bind, 0, 0};
        Com<ID3D11Texture1D> texture;
        check(device->CreateTexture1D(&desc, nullptr, &texture), "Create private pixel 1D target");
        carrier = texture;
    } else if (target.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
        const D3D11_TEXTURE3D_DESC desc{
            target.width, target.height, target.layers, 1, target.format, D3D11_USAGE_DEFAULT, bind, 0, 0};
        Com<ID3D11Texture3D> texture;
        check(device->CreateTexture3D(&desc, nullptr, &texture), "Create private pixel 3D target");
        carrier = texture;
    } else if (target.dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
        if (targetBytes > UINT32_MAX)
            throw std::runtime_error("Private pixel buffer target exceeds UINT32");
        const D3D11_BUFFER_DESC desc{UINT(targetBytes), D3D11_USAGE_DEFAULT, bind, 0, 0, 0};
        Com<ID3D11Buffer> buffer;
        check(device->CreateBuffer(&desc, nullptr, &buffer), "Create private pixel buffer target");
        carrier = buffer;
    } else if (target.dimension != D3D11_RESOURCE_DIMENSION_UNKNOWN)
        throw std::runtime_error("Invalid private pixel target dimension");
    Com<ID3D11RenderTargetView> color;
    Com<ID3D11DepthStencilView> depth;
    if (carrier) {
        if (target.depth)
            check(device->CreateDepthStencilView(carrier.Get(), nullptr, &depth),
                  "Create private pixel depth view");
        else if (target.dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
            D3D11_RENDER_TARGET_VIEW_DESC view{};
            view.Format = target.format;
            view.ViewDimension = D3D11_RTV_DIMENSION_BUFFER;
            view.Buffer.NumElements = target.width;
            check(device->CreateRenderTargetView(carrier.Get(), &view, &color),
                  "Create private pixel buffer view");
        } else
            check(device->CreateRenderTargetView(carrier.Get(), nullptr, &color),
                  "Create private pixel color view");
    }
    // Keep every possible 2x2 neighbor of the selected pixel inside the private
    // guard. Intersect existing scissors without translating viewport/sample
    // coordinates. This bounds work even for full-screen source primitives.
    if (selection.x > INT32_MAX - 2 || selection.y > INT32_MAX - 2)
        throw std::runtime_error("Pixel raster coordinate exceeds signed viewport bounds");
    auto privateRaster = state.rasterizer;
    privateRaster.ScissorEnable = true;
    const D3D11_RECT guard{LONG(selection.x) - 1, LONG(selection.y) - 1, LONG(selection.x) + 2,
                           LONG(selection.y) + 2};
    std::vector<D3D11_RECT> privateScissors(state.viewports.size(), guard);
    if (state.rasterizer.ScissorEnable)
        for (size_t i = 0; i < privateScissors.size(); ++i) {
            auto &rect = privateScissors[i];
            if (i >= state.scissors.size())
                rect = {0, 0, 0, 0};
            else {
                const auto &original = state.scissors[i];
                rect = {std::max(rect.left, original.left), std::max(rect.top, original.top),
                        std::min(rect.right, original.right), std::min(rect.bottom, original.bottom)};
                rect.right = std::max(rect.left, rect.right);
                rect.bottom = std::max(rect.top, rect.bottom);
            }
        }
    Com<ID3D11RasterizerState> raster;
    Com<ID3D11Device3> device3;
    Com<ID3D11Device1> device1;
    if (SUCCEEDED(device.As(&device3))) {
        Com<ID3D11RasterizerState2> object;
        check(device3->CreateRasterizerState2(&privateRaster, &object), "Create private pixel rasterizer2");
        raster = object;
    } else if (state.rasterizer.ConservativeRaster != D3D11_CONSERVATIVE_RASTERIZATION_MODE_OFF)
        throw std::runtime_error("Private pixel device lacks conservative rasterization");
    else if (SUCCEEDED(device.As(&device1))) {
        D3D11_RASTERIZER_DESC1 desc{};
        std::memcpy(&desc, &privateRaster, sizeof desc);
        Com<ID3D11RasterizerState1> object;
        check(device1->CreateRasterizerState1(&desc, &object), "Create private pixel rasterizer1");
        raster = object;
    } else {
        if (state.rasterizer.ForcedSampleCount)
            throw std::runtime_error("Private pixel device lacks forced sample count");
        D3D11_RASTERIZER_DESC desc{};
        std::memcpy(&desc, &privateRaster, sizeof desc);
        check(device->CreateRasterizerState(&desc, &raster), "Create private pixel rasterizer");
    }
    Com<ID3D11VertexShader> vs;
    Com<ID3D11GeometryShader> gs;
    Com<ID3D11PixelShader> ps;
    check(device->CreateVertexShader(bridge.vertexShader.data(), bridge.vertexShader.size(), nullptr, &vs),
          "Create private pixel VS");
    check(device->CreateGeometryShader(bridge.geometryShader.data(), bridge.geometryShader.size(), nullptr,
                                       &gs),
          "Create private pixel GS");
    check(device->CreatePixelShader(pixel.bytecode.data(), pixel.bytecode.size(), nullptr, &ps),
          "Create private pixel observation PS");
    std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
    for (UINT i = 0; i < bridge.stride / 16; ++i)
        elements.push_back(
            {"FLORA_GEOMETRY", i, DXGI_FORMAT_R32G32B32A32_UINT, 0, i * 16, D3D11_INPUT_PER_VERTEX_DATA, 0});
    Com<ID3D11InputLayout> layout;
    check(device->CreateInputLayout(elements.data(), UINT(elements.size()), bridge.vertexShader.data(),
                                    bridge.vertexShader.size(), &layout),
          "Create private pixel layout");
    Com<ID3D11Buffer> vertices, output, staging;
    const D3D11_BUFFER_DESC vertexDesc{
        UINT(bridge.vertices.size()), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
    const D3D11_SUBRESOURCE_DATA vertexData{bridge.vertices.data(), 0, 0};
    check(device->CreateBuffer(&vertexDesc, &vertexData, &vertices), "Create private pixel vertices");
    D3D11_BUFFER_DESC desc{recordBytes,
                           D3D11_USAGE_DEFAULT,
                           D3D11_BIND_UNORDERED_ACCESS,
                           0,
                           D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS,
                           0};
    check(device->CreateBuffer(&desc, nullptr, &output), "Create private pixel records");
    D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_TYPELESS;
    view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    view.Buffer.NumElements = recordBytes / 4;
    view.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    Com<ID3D11UnorderedAccessView> uav;
    check(device->CreateUnorderedAccessView(output.Get(), &view, &uav), "Create private pixel record UAV");
    const UINT zero[4]{};
    context->ClearUnorderedAccessViewUint(uav.Get(), zero);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check(device->CreateBuffer(&desc, nullptr, &staging), "Create private pixel record staging");
    D3D11_DEPTH_STENCIL_DESC depthDesc{};
    Com<ID3D11DepthStencilState> depthState;
    check(device->CreateDepthStencilState(&depthDesc, &depthState), "Create private pixel pre-depth state");
    context->OMSetDepthStencilState(depthState.Get(), 0);
    D3D11_BLEND_DESC blendDesc{};
    Com<ID3D11BlendState> blend;
    check(device->CreateBlendState(&blendDesc, &blend), "Create private pixel no-color-write state");
    context->OMSetBlendState(blend.Get(), nullptr, state.sampleMask);
    context->OMSetRenderTargetsAndUnorderedAccessViews(color ? 1 : 0, color ? color.GetAddressOf() : nullptr,
                                                       depth.Get(), 1, 1, uav.GetAddressOf(), nullptr);
    context->RSSetState(raster.Get());
    context->RSSetViewports(UINT(state.viewports.size()), state.viewports.data());
    context->RSSetScissorRects(UINT(privateScissors.size()), privateScissors.data());
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->GSSetShader(gs.Get(), nullptr, 0);
    context->PSSetShader(ps.Get(), nullptr, 0);
    context->IASetInputLayout(layout.Get());
    const UINT offset = 0;
    context->IASetVertexBuffers(0, 1, vertices.GetAddressOf(), &bridge.stride, &offset);
    context->IASetPrimitiveTopology(bridge.verticesPerPrimitive == 1 ? D3D11_PRIMITIVE_TOPOLOGY_POINTLIST
                                    : bridge.verticesPerPrimitive == 2
                                        ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST
                                        : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    checkCancellation(cancelled);
    context->Draw(UINT(bridge.vertices.size() / bridge.stride), 0);
    context->ClearState();
    context->CopyResource(staging.Get(), output.Get());
    GpuCompletion completion(device.Get(), context.Get(), cancelled, timeoutMs);
    completion.submit();
    const auto mapped = completion.map(staging.Get());
    PixelRasterInputs result;
    result.storage.resize(recordBytes);
    std::memcpy(result.storage.data(), mapped.pData, recordBytes);
    context->Unmap(staging.Get(), 0);
    validate(device.Get());
    checkCancellation(cancelled);
    const auto count = Reader(result.storage).read<uint32_t>();
    if (count > selection.maxRecords)
        throw std::runtime_error("Private pixel input record overflow");
    for (uint32_t i = 0; i < count; ++i) {
        PixelInputRecord record{};
        std::memcpy(&record, result.storage.data() + 16 + i * 144, 144);
        const auto &location = record.words[4], &identity = record.words[8], &direction = record.words[7];
        if (location[0] != selection.x || location[1] != selection.y || location[2] > 1 || location[3] > 1 ||
            identity != std::array<uint32_t, 4>{selection.layer, selection.sample, 0, 0} ||
            std::bit_cast<float>(direction[0]) != (location[2] ? -1.f : 1.f) ||
            std::bit_cast<float>(direction[1]) != (location[3] ? -1.f : 1.f) ||
            (direction[2] & 0x7fffffffu) || (direction[3] & 0x7fffffffu))
            throw std::runtime_error("Invalid private pixel quad identity/orientation");
        const auto lane = location[2] + 2 * location[3];
        if (!(record.words[6][lane] & (1u << selection.sample)) ||
            record.words[5][lane] != (pixel.sampleFrequency ? selection.sample : 0u))
            throw std::runtime_error("Private pixel record lacks selected sample coverage");
        result.records.push_back(record);
    }
    result.identity = {{"backend", "dxbc-pixel-raster-inputs/1"},
                       {"adapter_vendor", actual.VendorId},
                       {"adapter_device", actual.DeviceId},
                       {"adapter_luid",
                        (uint64_t(uint32_t(actual.AdapterLuid.HighPart)) << 32) | actual.AdapterLuid.LowPart},
                       {"feature_level", uint32_t(level)},
                       {"warp", warp},
                       {"x", selection.x},
                       {"y", selection.y},
                       {"sample", selection.sample},
                       {"layer", selection.layer},
                       {"records", count},
                       {"max_records", selection.maxRecords},
                       {"record_stride", 144},
                       {"logical_resource_bytes", required},
                       {"target_logical_bytes", targetBytes},
                       {"target_samples", target.samples.Count},
                       {"raster_samples", samples},
                       {"sample_mask", state.sampleMask},
                       {"private_scissor_guard", {guard.left, guard.top, guard.right, guard.bottom}},
                       {"sample_frequency", pixel.sampleFrequency},
                       {"input_register", pixel.reg},
                       {"shader_sha256", pixel.originalShaderSha256},
                       {"valid_mask", pixel.mask},
                       {"scope", bridge.identity.contains("primitive_count")
                                     ? "isolated_geometry_range_pre_depth_inputs"
                                     : "isolated_primitive_pre_depth_inputs"},
                       {"original_fragment_eligibility", false},
                       {"record_order", "unspecified"},
                       {"geometry", bridge.identity}};
    result.identity["raster_state"] = pixelRasterIdentity(state);
    return result;
}
} // namespace flora::dxbc_exec
