#include "NativeCommands.h"
#include <wrl/client.h>
#include <map>
#include <sstream>
#include <set>

namespace flora::dx12 {
namespace {
using format::Reader;
using Microsoft::WRL::ComPtr;
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
void check(HRESULT hr, const char *operation) {
    if (FAILED(hr)) { std::ostringstream s; s << operation << " failed: 0x" << std::hex << uint32_t(hr); throw std::runtime_error(s.str()); }
}
template<class T> std::vector<T> array(Reader &r, UINT count) {
    std::vector<T> result;
    if (r.flag()) {
        const auto size = r.read<uint64_t>(); require(size == count && size <= r.remaining() / sizeof(T), "Invalid command array");
        result.resize(count); const auto bytes = r.take(size * sizeof(T)); if (size) memcpy(result.data(), bytes.data(), bytes.size());
    }
    require(result.size() == count, "Missing command array"); return result;
}
}
struct NativeCommands::Impl {
    NativeDevice &device; const StreamDirectory &directory; NativeResources &resources; NativeDescriptors &descriptors;
    NativePipelines pipelines;
    struct Query {
        uint64_t record{}, object{}; D3D12_QUERY_HEAP_DESC desc{}; ComPtr<ID3D12QueryHeap> native;
        std::map<UINT, D3D12_QUERY_TYPE> active, completed;
    };
    std::map<uint64_t, std::vector<Query>> queries;
    Impl(NativeDevice &d, const StreamDirectory &s, NativeResources &r, NativeDescriptors &h, const CancelCheck &c)
        : device(d), directory(s), resources(r), descriptors(h), pipelines(d, s, c) {
        for (size_t i = 0; i < s.packets().size(); ++i) {
            if (i % 1024 == 0) checkCancellation(c); const auto &p = s.packets()[i]; if (p.type != 0x127937e9603edULL) continue;
            Reader reader(s.payload(p)); reader.skip(8); require(reader.flag(), "Missing query heap description"); Query query; query.record = i + 1;
            query.desc = reader.read<D3D12_QUERY_HEAP_DESC>(); require(reader.read<GUID>() == __uuidof(ID3D12QueryHeap), "Unsupported query heap interface");
            if (reader.flag()) { require(reader.read<uint64_t>() == 1, "Invalid query heap output"); query.object = reader.read<uint64_t>(); }
            const auto hr = reader.read<HRESULT>(); reader.end(); if (SUCCEEDED(hr) && query.object) queries[query.object].push_back(std::move(query));
        }
    }
    Query &query(uint64_t object, uint64_t at, UINT first, UINT count) {
        auto it = queries.find(object); require(it != queries.end(), "Missing query heap recipe"); Query *selected{};
        for (auto &q : it->second) if (q.record <= at) selected = &q;
        require(selected, "Query heap is not defined at this boundary");
        require(first <= selected->desc.Count && count <= selected->desc.Count - first, "Query range exceeds its heap");
        if (!selected->native) check(device.device()->CreateQueryHeap(&selected->desc, IID_PPV_ARGS(&selected->native)), "Create captured query heap");
        return *selected;
    }
};
NativeCommands::NativeCommands(NativeDevice &d, const StreamDirectory &s, NativeResources &r, NativeDescriptors &h, const CancelCheck &c)
    : impl_(std::make_unique<Impl>(d, s, r, h, c)) {}
NativeCommands::~NativeCommands() = default;
NativePipelines &NativeCommands::pipelines() { return impl_->pipelines; }
void NativeCommands::record(ID3D12GraphicsCommandList *list, uint64_t id, uint64_t capturedList, std::vector<std::function<void()>> &executionChecks) {
    auto &s = *impl_; const auto &packet = s.directory.packets().at(size_t(id - 1)); Reader r(s.directory.payload(packet));
    require(r.read<uint64_t>() == capturedList, "Graphics command belongs to another list");
    switch (packet.type) {
    case 0x1ae3c190d8b1eULL: {
        const auto vertices = r.read<UINT>(), instances = r.read<UINT>(), start = r.read<UINT>(), instance = r.read<UINT>(); r.end();
        list->DrawInstanced(vertices, instances, start, instance); break;
    }
    case 0x1ae3c86b1c3b9ULL: {
        const auto indices = r.read<UINT>(), instances = r.read<UINT>(), start = r.read<UINT>(); const auto base = r.read<INT>(); const auto instance = r.read<UINT>(); r.end();
        list->DrawIndexedInstanced(indices, instances, start, base, instance); break;
    }
    case 0x1ae3cef0e6c17ULL: {
        const auto x = r.read<UINT>(), y = r.read<UINT>(), z = r.read<UINT>(); r.end(); list->Dispatch(x, y, z); break;
    }
    case 0x1ae3c82b462fcULL: {
        const auto object = r.read<uint64_t>(); r.end(); list->SetPipelineState(s.pipelines.pipeline(object, id)); break;
    }
    case 0x1ae3c6959946eULL: case 0x1ae3c9fea880cULL: {
        const auto object = r.read<uint64_t>(); r.end(); auto root = object ? s.pipelines.rootSignature(object, id) : nullptr;
        if (packet.type == 0x1ae3c6959946eULL) list->SetGraphicsRootSignature(root); else list->SetComputeRootSignature(root); break;
    }
    case 0x1ae3c0857cd39ULL: case 0x1ae3cbf4ff4b7ULL: {
        const auto index = r.read<UINT>(); const auto handle = r.read<uint64_t>(); r.end(); auto gpu = s.descriptors.gpu(handle, id);
        if (packet.type == 0x1ae3c0857cd39ULL) list->SetGraphicsRootDescriptorTable(index, gpu); else list->SetComputeRootDescriptorTable(index, gpu); break;
    }
    case 0x1ae3cac95ac55ULL: case 0x1ae3c3a460e6bULL: case 0x1ae3c0089f9bdULL: case 0x1ae3cff376c13ULL: {
        const auto index = r.read<UINT>(); const auto captured = r.read<uint64_t>(); r.end(); const auto address = s.resources.gpuAddress(captured, id);
        if (packet.type == 0x1ae3cac95ac55ULL) list->SetGraphicsRootConstantBufferView(index, address);
        else if (packet.type == 0x1ae3c3a460e6bULL) list->SetComputeRootConstantBufferView(index, address);
        else if (packet.type == 0x1ae3c0089f9bdULL) list->SetGraphicsRootUnorderedAccessView(index, address);
        else list->SetComputeRootUnorderedAccessView(index, address); break;
    }
    case 0x1ae3cb21a43bbULL: {
        const auto index = r.read<UINT>(), count = r.read<UINT>(); require(r.flag(), "Missing root constants");
        const auto bytes = r.read<uint64_t>(); require(bytes == uint64_t(count) * 4 && count <= 64, "Invalid root constant byte count");
        const auto data = r.take(bytes); const auto offset = r.read<UINT>(); r.end();
        list->SetGraphicsRoot32BitConstants(index, count, data.data(), offset); break;
    }
    case 0x1ae3c9154ebfeULL: {
        const auto index = r.read<UINT>(), value = r.read<UINT>(), offset = r.read<UINT>(); r.end(); list->SetComputeRoot32BitConstant(index, value, offset); break;
    }
    case 0x1ae3c4254a275ULL: {
        const auto count = r.read<UINT>(); require(count <= 2, "Too many bound descriptor heaps"); const auto objects = array<uint64_t>(r, count); r.end();
        std::vector<ID3D12DescriptorHeap *> heaps; for (auto object : objects) heaps.push_back(s.descriptors.heap(object, id));
        list->SetDescriptorHeaps(count, heaps.data()); break;
    }
    case 0x1ae3c23cdef3dULL: {
        const auto count = r.read<UINT>(); require(count <= 16, "Too many viewports"); auto views = array<D3D12_VIEWPORT>(r, count); r.end(); list->RSSetViewports(count, views.data()); break;
    }
    case 0x1ae3cb2a5e5b7ULL: {
        const auto count = r.read<UINT>(); require(count <= 16, "Too many scissors"); auto rects = array<D3D12_RECT>(r, count); r.end(); list->RSSetScissorRects(count, rects.data()); break;
    }
    case 0x1ae3c311a2513ULL: {
        const auto topology = r.read<D3D12_PRIMITIVE_TOPOLOGY>(); r.end(); list->IASetPrimitiveTopology(topology); break;
    }
    case 0x1ae3cc2293674ULL: {
        const auto start = r.read<UINT>(), count = r.read<UINT>(); require(start <= 32 && count <= 32 - start, "Vertex buffer slots exceed the input assembler");
        auto views = array<D3D12_VERTEX_BUFFER_VIEW>(r, count); r.end();
        for (auto &view : views) view.BufferLocation = s.resources.gpuAddress(view.BufferLocation, id, view.SizeInBytes);
        list->IASetVertexBuffers(start, count, views.data()); break;
    }
    case 0x1ae3ccbc6c8c9ULL: {
        std::optional<D3D12_INDEX_BUFFER_VIEW> view; if (r.flag()) view = r.read<D3D12_INDEX_BUFFER_VIEW>(); r.end();
        if (view) view->BufferLocation = s.resources.gpuAddress(view->BufferLocation, id, view->SizeInBytes);
        list->IASetIndexBuffer(view ? &*view : nullptr); break;
    }
    case 0x1ae3c7884148bULL: {
        const auto count = r.read<UINT>(); require(count <= 8, "Too many render targets");
        // The wire array has its own length: a contiguous binding carries one handle.
        std::vector<uint64_t> handles; if (r.flag()) { const auto size = r.read<uint64_t>(); require(size <= 8, "Invalid render target handle array"); for (uint64_t i = 0; i < size; ++i) handles.push_back(r.read<uint64_t>()); }
        const auto contiguous = r.read<BOOL>(); std::optional<D3D12_CPU_DESCRIPTOR_HANDLE> depth;
        if (r.flag()) depth = s.descriptors.cpu(r.read<uint64_t>(), id, D3D12_DESCRIPTOR_HEAP_TYPE_DSV); r.end();
        require(handles.size() == (contiguous && count ? 1 : count), "Render target handle count mismatch");
        std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> native; for (auto h : handles) native.push_back(s.descriptors.cpu(h, id, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, contiguous ? count : 1));
        list->OMSetRenderTargets(count, native.data(), contiguous, depth ? &*depth : nullptr); break;
    }
    case 0x1ae3c5e296342ULL: {
        const auto handle = r.read<uint64_t>(); const auto flags = r.read<D3D12_CLEAR_FLAGS>(); const auto depth = r.read<float>(); const auto stencil = r.read<UINT8>();
        const auto count = r.read<UINT>(); auto rects = array<D3D12_RECT>(r, count); r.end();
        list->ClearDepthStencilView(s.descriptors.cpu(handle, id, D3D12_DESCRIPTOR_HEAP_TYPE_DSV), flags, depth, stencil, count, rects.data()); break;
    }
    case 0x1ae3cdc0903fcULL: {
        const auto handle = r.read<uint64_t>(); require(r.flag(), "Missing clear color"); float color[4]; const auto data = r.take(sizeof(color)); memcpy(color, data.data(), sizeof(color));
        const auto count = r.read<UINT>(); auto rects = array<D3D12_RECT>(r, count); r.end();
        list->ClearRenderTargetView(s.descriptors.cpu(handle, id, D3D12_DESCRIPTOR_HEAP_TYPE_RTV), color, count, rects.data()); break;
    }
    case 0x1ae3ce6df7dc5ULL: {
        const auto object = r.read<uint64_t>(); std::optional<D3D12_DISCARD_REGION> region; std::vector<D3D12_RECT> rects;
        if (r.flag()) { region.emplace(); region->NumRects = r.read<UINT>(); rects = array<D3D12_RECT>(r, region->NumRects); region->pRects = rects.data(); region->FirstSubresource = r.read<UINT>(); region->NumSubresources = r.read<UINT>(); }
        r.end(); list->DiscardResource(s.resources.resource(object, id), region ? &*region : nullptr); break;
    }
    case 0x1ae3c48bf1cd1ULL: {
        require(r.flag(), "Missing blend factor"); float factor[4]; const auto data = r.take(sizeof(factor)); memcpy(factor, data.data(), sizeof(factor)); r.end(); list->OMSetBlendFactor(factor); break;
    }
    case 0x1ae3cd41bfa9cULL: { const auto stencil = r.read<UINT>(); r.end(); list->OMSetStencilRef(stencil); break; }
    case 0x1ae3c6601dd2cULL: case 0x1ae3c64a8e138ULL: {
        const auto object = r.read<uint64_t>(); const auto type = r.read<D3D12_QUERY_TYPE>(); const auto index = r.read<UINT>(); r.end(); auto &heap = s.query(object, id, index, 1);
        const bool begin = packet.type == 0x1ae3c6601dd2cULL;
        executionChecks.push_back([&heap, type, index, begin] {
            if (begin) { require(type != D3D12_QUERY_TYPE_TIMESTAMP && !heap.active.contains(index), "Invalid query begin boundary"); heap.active[index] = type; heap.completed.erase(index); }
            else {
                require(type == D3D12_QUERY_TYPE_TIMESTAMP || (heap.active.contains(index) && heap.active.at(index) == type), "Query end has no matching begin");
                heap.active.erase(index); heap.completed[index] = type;
            }
        });
        if (begin) list->BeginQuery(heap.native.Get(), type, index); else list->EndQuery(heap.native.Get(), type, index); break;
    }
    case 0x1ae3cdc411c2dULL: {
        const auto object = r.read<uint64_t>(); const auto type = r.read<D3D12_QUERY_TYPE>(); const auto start = r.read<UINT>(), count = r.read<UINT>();
        const auto destination = r.read<uint64_t>(), offset = r.read<uint64_t>(); r.end(); auto &heap = s.query(object, id, start, count);
        auto buffer = s.resources.resource(destination, id); require(buffer->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER, "Query destination must be a buffer");
        const UINT stride = type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS ? sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS) :
            type >= D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 && type <= D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3 ? sizeof(D3D12_QUERY_DATA_SO_STATISTICS) : 8;
        require(offset % 8 == 0 && offset <= buffer->GetDesc().Width && uint64_t(count) * stride <= buffer->GetDesc().Width - offset, "Query result exceeds its buffer");
        executionChecks.push_back([&heap, start, count, type, id, destination, &resources = s.resources] {
            for (UINT i = 0; i < count; ++i) if (!heap.completed.contains(start + i) || heap.completed.at(start + i) != type)
                throw std::runtime_error("Query resolve at source record " + std::to_string(id) + " needs historical query " + std::to_string(start + i) + " from heap " + std::to_string(heap.object) + "; no completed replay query supplies its value");
            resources.requireState(destination, id, 0, D3D12_RESOURCE_STATE_COPY_DEST);
        });
        list->ResolveQueryData(heap.native.Get(), type, start, count, buffer, offset); break;
    }
    default: throw std::runtime_error("Unsupported native frame command: " + streamPacketName(packet.type));
    }
}
}
