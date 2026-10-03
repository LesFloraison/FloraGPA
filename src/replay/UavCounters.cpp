#include "core/UavCounters.h"
#include "Replay.h"
#include "Unpredicated.h"
#include <d3dcompiler.h>

namespace flora {
void Replay::requireCreatedCounter(Id view) const {
    if (undefinedCreatedCounters_.contains(view))
        throw std::runtime_error(
            "Created UAV " + std::to_string(view) +
            " has no defined counter value; an explicit captured reset or experiment value is required");
}
uint32_t Replay::readCounter(Id view) {
    requireCreatedCounter(view);
    Unpredicated guard(context_.Get());
    if (!describeCounter(frame_, view))
        throw std::runtime_error("UAV has no hidden counter");
    auto uav = get<ID3D11UnorderedAccessView>(view);
    D3D11_BUFFER_DESC desc{4, D3D11_USAGE_DEFAULT, 0, 0, 0, 0};
    Com<ID3D11Buffer> target, staging;
    check(device_->CreateBuffer(&desc, nullptr, &target), "Create counter readback target");
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check(device_->CreateBuffer(&desc, nullptr, &staging), "Create counter staging buffer");
    context_->CopyStructureCount(target.Get(), 0, uav);
    context_->CopyResource(staging.Get(), target.Get());
    D3D11_MAPPED_SUBRESOURCE data{};
    check(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &data), "Read UAV counter");
    uint32_t value;
    std::memcpy(&value, data.pData, 4);
    context_->Unmap(staging.Get(), 0);
    return value;
}
void Replay::writeCounter(Id view, uint32_t value) {
    if (!describeCounter(frame_, view))
        throw std::runtime_error("UAV has no hidden counter");
    writeCounter(get<ID3D11UnorderedAccessView>(view), value);
    undefinedCreatedCounters_.erase(view);
}
void Replay::writeCounter(ID3D11UnorderedAccessView *view, uint32_t value) {
    Unpredicated guard(context_.Get());
    D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
    view->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_UNKNOWN || desc.ViewDimension != D3D11_UAV_DIMENSION_BUFFER ||
        (desc.Buffer.Flags != D3D11_BUFFER_UAV_FLAG_APPEND &&
         desc.Buffer.Flags != D3D11_BUFFER_UAV_FLAG_COUNTER))
        throw std::runtime_error("UAV has no hidden counter");
    Com<ID3D11ComputeShader> wrap;
    if (value == UINT32_MAX) {
        Com<ID3D11Resource> resource;
        Com<ID3D11Buffer> buffer;
        view->GetResource(&resource);
        check(resource.As(&buffer), "Get counter buffer");
        D3D11_BUFFER_DESC b{};
        buffer->GetDesc(&b);
        auto stride = b.StructureByteStride;
        if (!stride || stride % 4 || stride > 2048)
            throw std::runtime_error("Unsupported counter-wrap stride");
        auto key = std::make_pair(stride, desc.Buffer.Flags);
        auto it = counterWrapShaders_.find(key);
        if (it == counterWrapShaders_.end()) {
            bool append = desc.Buffer.Flags == D3D11_BUFFER_UAV_FLAG_APPEND;
            const std::string source = "struct T{uint data[" + std::to_string(stride / 4) + "];};" +
                                       (append ? "ConsumeStructuredBuffer" : "RWStructuredBuffer") +
                                       "<T> target:register(u0);[numthreads(1,1,1)]void main(){target." +
                                       (append ? "Consume" : "DecrementCounter") + "();}";
            Com<ID3DBlob> binary, diagnostics;
            check(D3DCompile(source.data(), source.size(), "counter-wrap.hlsl", nullptr, nullptr, "main",
                             "cs_5_0", 0, 0, &binary, &diagnostics),
                  "Compile counter-wrap helper");
            check(device_->CreateComputeShader(binary->GetBufferPointer(), binary->GetBufferSize(), nullptr,
                                               &wrap),
                  "Create counter-wrap shader");
            counterWrapShaders_.emplace(key, wrap);
        } else
            wrap = it->second;
    }
    // -1 means KEEP to CSSetUnorderedAccessViews; decrement zero to write the exact all-ones value.
    // ExecuteCommandList(TRUE) preserves the immediate bindings, but intentionally retains UAV state.
    Com<ID3D11DeviceContext> deferred;
    check(device_->CreateDeferredContext(0, &deferred), "Create counter helper context");
    UINT initial = wrap ? 0 : value;
    deferred->CSSetUnorderedAccessViews(0, 1, &view, &initial);
    if (wrap) {
        deferred->CSSetShader(wrap.Get(), nullptr, 0);
        deferred->Dispatch(1, 1, 1);
    }
    Com<ID3D11CommandList> commands;
    check(deferred->FinishCommandList(FALSE, &commands), "Finish counter helper commands");
    context_->ExecuteCommandList(commands.Get(), TRUE);
    check(device_->GetDeviceRemovedReason(), "Counter write device status");
}
} // namespace flora
