#include "NativeCommands.h"
#include "QueryReplayIndex.h"
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
CommandSignatureRecipe decodeCommandSignature(format::Bytes bytes) {
    Reader r(bytes);
    CommandSignatureRecipe recipe;
    recipe.device = r.read<uint64_t>();
    require(r.flag(), "Missing command signature description");
    recipe.byteStride = r.read<UINT>();
    const auto count = r.read<UINT>();
    static_assert(sizeof(D3D12_INDIRECT_ARGUMENT_DESC) == 16);
    recipe.arguments = array<D3D12_INDIRECT_ARGUMENT_DESC>(r, count);
    recipe.nodeMask = r.read<UINT>();
    recipe.rootSignature = r.read<uint64_t>();
    require(r.read<GUID>() == __uuidof(ID3D12CommandSignature), "Unsupported command signature interface");
    if (r.flag()) {
        require(r.read<uint64_t>() == 1, "Invalid command signature output");
        recipe.object = r.read<uint64_t>();
    }
    recipe.result = r.read<HRESULT>();
    r.end();
    return recipe;
}
struct NativeCommands::Impl {
    NativeDevice &device;
    const StreamDirectory &directory;
    NativeResources &resources;
    NativeDescriptors &descriptors;
    NativePipelines pipelines;
    struct Query {
        uint64_t record{}, object{};
        D3D12_QUERY_HEAP_DESC desc{};
        ComPtr<ID3D12QueryHeap> native;
        std::map<UINT, D3D12_QUERY_TYPE> active, completed;
    };
    std::map<uint64_t, std::vector<Query>> queries;
    struct Signature {
        uint64_t record{};
        CommandSignatureRecipe recipe;
        ComPtr<ID3D12CommandSignature> native;
    };
    std::map<uint64_t, std::vector<Signature>> signatures;
    Impl(NativeDevice &d, const StreamDirectory &s, NativeResources &r, NativeDescriptors &h,
         const CancelCheck &c)
        : device(d), directory(s), resources(r), descriptors(h), pipelines(d, s, c) {
        for (size_t i = 0; i < s.packets().size(); ++i) {
            if (i % 1024 == 0)
                checkCancellation(c);
            const auto &p = s.packets()[i];
            if (p.type == 0x12793fa7d46faULL) {
                auto recipe = decodeCommandSignature(s.payload(p));
                if (SUCCEEDED(recipe.result) && recipe.object) {
                    const auto object = recipe.object;
                    signatures[object].push_back({i + 1, std::move(recipe), {}});
                }
                continue;
            }
            if (p.type != 0x127937e9603edULL)
                continue;
            Reader reader(s.payload(p));
            reader.skip(8);
            require(reader.flag(), "Missing query heap description");
            Query query;
            query.record = i + 1;
            query.desc = reader.read<D3D12_QUERY_HEAP_DESC>();
            require(reader.read<GUID>() == __uuidof(ID3D12QueryHeap), "Unsupported query heap interface");
            if (reader.flag()) {
                require(reader.read<uint64_t>() == 1, "Invalid query heap output");
                query.object = reader.read<uint64_t>();
            }
            const auto hr = reader.read<HRESULT>();
            reader.end();
            if (SUCCEEDED(hr) && query.object)
                queries[query.object].push_back(std::move(query));
        }
    }
    Signature &signature(uint64_t object, uint64_t at) {
        const auto it = signatures.find(object);
        require(it != signatures.end(), "Missing command signature recipe");
        Signature *selected{};
        for (auto &entry : it->second)
            if (entry.record <= at)
                selected = &entry;
        require(selected, "Command signature is not defined at this boundary");
        const auto &recipe = selected->recipe;
        require(recipe.arguments.size() == 1 && !recipe.rootSignature,
                "Indirect root arguments and GPU address patching are not implemented");
        const auto type = recipe.arguments.front().Type;
        const UINT minimum =
            type == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW            ? sizeof(D3D12_DRAW_ARGUMENTS)
            : type == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED  ? sizeof(D3D12_DRAW_INDEXED_ARGUMENTS)
            : type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH      ? sizeof(D3D12_DISPATCH_ARGUMENTS)
            : type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH ? sizeof(D3D12_DISPATCH_MESH_ARGUMENTS)
                                                                 : 0;
        require(minimum, "Unsupported indirect argument type; address-bearing arguments require patching");
        require(recipe.byteStride >= minimum && recipe.byteStride % 4 == 0,
                "Invalid indirect argument stride");
        if (!selected->native) {
            if (type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH) {
                D3D12_FEATURE_DATA_D3D12_OPTIONS7 options{};
                check(device.device()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options,
                                                           sizeof(options)),
                      "Query mesh shader support");
                require(options.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED,
                        "Device does not support captured mesh shader dispatches");
            }
            const D3D12_COMMAND_SIGNATURE_DESC desc{recipe.byteStride, UINT(recipe.arguments.size()),
                                                    recipe.arguments.data(), recipe.nodeMask};
            check(device.device()->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&selected->native)),
                  "Create captured command signature");
        }
        return *selected;
    }
    Query &query(uint64_t object, uint64_t at, UINT first, UINT count) {
        auto it = queries.find(object);
        require(it != queries.end(), "Missing query heap recipe");
        Query *selected{};
        for (auto &q : it->second)
            if (q.record <= at)
                selected = &q;
        // Keyframe serialization can describe a heap after a list that already
        // references it. Accept only a unique keyframe lifetime, as for resources;
        // a later frame creation must never satisfy an earlier reference.
        if (!selected && at <= directory.initializationCount()) {
            for (auto &q : it->second)
                if (q.record <= directory.initializationCount()) {
                    require(!selected, "Ambiguous initialization query heap lifetime");
                    selected = &q;
                }
        }
        require(selected, "Query heap is not defined at this boundary");
        require(first <= selected->desc.Count && count <= selected->desc.Count - first,
                "Query range exceeds its heap");
        if (!selected->native)
            check(device.device()->CreateQueryHeap(&selected->desc, IID_PPV_ARGS(&selected->native)),
                  "Create captured query heap");
        return *selected;
    }
};
NativeCommands::NativeCommands(NativeDevice &d, const StreamDirectory &s, NativeResources &r,
                               NativeDescriptors &h, const CancelCheck &c)
    : impl_(std::make_unique<Impl>(d, s, r, h, c)) {}
NativeCommands::~NativeCommands() = default;
NativePipelines &NativeCommands::pipelines() { return impl_->pipelines; }
void NativeCommands::record(ID3D12GraphicsCommandList *list, uint64_t id, uint64_t capturedList, std::vector<std::function<void()>> &executionChecks,
                            const nlohmann::json &queryCoverage) {
    auto &s = *impl_; const auto &packet = s.directory.packets().at(size_t(id - 1)); Reader r(s.directory.payload(packet));
    require(r.read<uint64_t>() == capturedList, "Graphics command belongs to another list");
    switch (packet.type) {
    case 0x1ae3ccddf9f8cULL: {
        const auto object = r.read<uint64_t>();
        const auto maximum = r.read<UINT>();
        const auto arguments = r.read<uint64_t>(), offset = r.read<uint64_t>(), counter = r.read<uint64_t>(),
                   counterOffset = r.read<uint64_t>();
        r.end();
        auto &signature = s.signature(object, id);
        auto buffer = s.resources.resource(arguments, id);
        const auto desc = buffer->GetDesc();
        require(desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && offset % 4 == 0 &&
                    offset <= desc.Width &&
                    uint64_t(maximum) * signature.recipe.byteStride <= desc.Width - offset,
                "Indirect arguments exceed the buffer or are misaligned");
        auto countBuffer = counter ? s.resources.resource(counter, id) : nullptr;
        if (countBuffer) {
            const auto countDesc = countBuffer->GetDesc();
            require(countDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && counterOffset % 4 == 0 &&
                        counterOffset <= countDesc.Width && 4 <= countDesc.Width - counterOffset,
                    "Indirect count exceeds the buffer or is misaligned");
        }
        executionChecks.push_back([arguments, counter, id, &resources = s.resources] {
            resources.requireState(arguments, id, 0, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            if (counter)
                resources.requireState(counter, id, 0, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        });
        list->ExecuteIndirect(signature.native.Get(), maximum, buffer, offset, countBuffer, counterOffset);
        break;
    }
    case 0x1ae3c56ea49c1ULL:
    case 0x1ae3cdecc319dULL: {
        const auto gpu = r.read<uint64_t>(), cpu = r.read<uint64_t>(), object = r.read<uint64_t>();
        require(r.flag(), "Missing UAV clear values");
        const auto values = r.take(16);
        const auto count = r.read<UINT>();
        auto rects = array<D3D12_RECT>(r, count);
        r.end();
        const auto gpuHandle = s.descriptors.gpu(gpu, id);
        const auto cpuHandle = s.descriptors.consumedCpu(cpu, id, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto resource = s.resources.resource(object, id);
        require((resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0,
                "UAV clear references a resource without UAV access");
        if (packet.type == 0x1ae3c56ea49c1ULL) {
            float data[4];
            memcpy(data, values.data(), 16);
            list->ClearUnorderedAccessViewFloat(gpuHandle, cpuHandle, resource, data, count, rects.data());
        } else {
            UINT data[4];
            memcpy(data, values.data(), 16);
            list->ClearUnorderedAccessViewUint(gpuHandle, cpuHandle, resource, data, count, rects.data());
        }
        break;
    }
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
        if (r.flag()) depth = s.descriptors.consumedCpu(r.read<uint64_t>(), id, D3D12_DESCRIPTOR_HEAP_TYPE_DSV); r.end();
        require(handles.size() == (contiguous && count ? 1 : count), "Render target handle count mismatch");
        std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> native; for (auto h : handles) native.push_back(s.descriptors.consumedCpu(h, id, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, contiguous ? count : 1));
        list->OMSetRenderTargets(count, native.data(), contiguous, depth ? &*depth : nullptr); break;
    }
    case 0x1ae3c5e296342ULL: {
        const auto handle = r.read<uint64_t>(); const auto flags = r.read<D3D12_CLEAR_FLAGS>(); const auto depth = r.read<float>(); const auto stencil = r.read<UINT8>();
        const auto count = r.read<UINT>(); auto rects = array<D3D12_RECT>(r, count); r.end();
        list->ClearDepthStencilView(s.descriptors.consumedCpu(handle, id, D3D12_DESCRIPTOR_HEAP_TYPE_DSV), flags, depth, stencil, count, rects.data()); break;
    }
    case 0x1ae3cdc0903fcULL: {
        const auto handle = r.read<uint64_t>(); require(r.flag(), "Missing clear color"); float color[4]; const auto data = r.take(sizeof(color)); memcpy(color, data.data(), sizeof(color));
        const auto count = r.read<UINT>(); auto rects = array<D3D12_RECT>(r, count); r.end();
        list->ClearRenderTargetView(s.descriptors.consumedCpu(handle, id, D3D12_DESCRIPTOR_HEAP_TYPE_RTV), color, count, rects.data()); break;
    }
    case 0x1ae3ce6df7dc5ULL: {
        const auto object = r.read<uint64_t>(); std::optional<D3D12_DISCARD_REGION> region; std::vector<D3D12_RECT> rects;
        if (r.flag()) { region.emplace(); region->NumRects = r.read<UINT>(); rects = array<D3D12_RECT>(r, region->NumRects); region->pRects = rects.data(); region->FirstSubresource = r.read<UINT>(); region->NumSubresources = r.read<UINT>(); }
        r.end(); list->DiscardResource(s.resources.resource(object, id), region ? &*region : nullptr); break;
    }
    case 0x1ae3c48bf1cd1ULL: {
        const auto factor = array<float>(r, 4); r.end(); list->OMSetBlendFactor(factor.data()); break;
    }
    case 0x10144f9457a81ULL: {
        const auto minimum = r.read<float>(), maximum = r.read<float>();
        r.end();
        D3D12_FEATURE_DATA_D3D12_OPTIONS2 options{};
        check(s.device.device()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &options, sizeof(options)),
              "Query depth bounds support");
        require(options.DepthBoundsTestSupported, "Replay device does not support depth bounds");
        ComPtr<ID3D12GraphicsCommandList1> extended;
        check(list->QueryInterface(IID_PPV_ARGS(&extended)), "Query depth bounds command list");
        extended->OMSetDepthBounds(minimum, maximum);
        break;
    }
    case 0x10140788f8c34ULL: {
        const auto rate = r.read<D3D12_SHADING_RATE>();
        std::optional<std::array<D3D12_SHADING_RATE_COMBINER, 2>> combiners;
        if (r.flag()) {
            require(r.read<uint64_t>() == 2, "Shading rate requires two combiners");
            combiners = r.read<std::array<D3D12_SHADING_RATE_COMBINER, 2>>();
            for (auto combiner : *combiners)
                require(combiner >= D3D12_SHADING_RATE_COMBINER_PASSTHROUGH &&
                            combiner <= D3D12_SHADING_RATE_COMBINER_SUM,
                        "Invalid shading rate combiner");
        }
        r.end();
        require(rate == D3D12_SHADING_RATE_1X1 || rate == D3D12_SHADING_RATE_1X2 ||
                    rate == D3D12_SHADING_RATE_2X1 || rate == D3D12_SHADING_RATE_2X2 ||
                    rate == D3D12_SHADING_RATE_2X4 || rate == D3D12_SHADING_RATE_4X2 ||
                    rate == D3D12_SHADING_RATE_4X4,
                "Invalid shading rate");
        D3D12_FEATURE_DATA_D3D12_OPTIONS6 options{};
        check(s.device.device()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &options, sizeof(options)),
              "Query variable shading rate support");
        require(options.VariableShadingRateTier != D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED,
                "Replay device does not support variable shading rates");
        require(rate <= D3D12_SHADING_RATE_2X2 || options.AdditionalShadingRatesSupported,
                "Replay device does not support the captured additional shading rate");
        ComPtr<ID3D12GraphicsCommandList5> extended;
        check(list->QueryInterface(IID_PPV_ARGS(&extended)), "Query shading rate command list");
        extended->RSSetShadingRate(rate, combiners ? combiners->data() : nullptr);
        break;
    }
    case 0x101407e5f6cc3ULL: {
        const auto object = r.read<uint64_t>();
        r.end();
        D3D12_FEATURE_DATA_D3D12_OPTIONS6 options{};
        check(s.device.device()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &options, sizeof(options)),
              "Query shading rate image support");
        require(options.VariableShadingRateTier >= D3D12_VARIABLE_SHADING_RATE_TIER_2,
                "Replay device does not support shading rate images");
        auto image = object ? s.resources.resource(object, id) : nullptr;
        ComPtr<ID3D12GraphicsCommandList5> extended;
        check(list->QueryInterface(IID_PPV_ARGS(&extended)), "Query shading rate image command list");
        extended->RSSetShadingRateImage(image);
        break;
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
        auto buffer = s.resources.queryDestination(destination, id); require(buffer->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER, "Query destination must be a buffer");
        validateQueryDestination(type, count, offset, buffer->GetDesc().Width);
        std::vector<QueryResultRange> ranges{{start, count, offset, uint64_t(count) * queryResultStride(type), true}};
        if (!queryCoverage.is_null()) {
            require(queryCoverage.at("source_record") == std::to_string(id) &&
                        queryCoverage.at("destination") == std::to_string(destination) &&
                        queryCoverage.at("query_heap") == std::to_string(object), "Query execution coverage mismatch");
            if (count) ranges = queryResultRanges(queryCoverage);
        }
        for (const auto &range : ranges) {
            executionChecks.push_back([&heap, range, type, id, destination, &resources = s.resources] {
                for (UINT i = 0; i < range.count; ++i) {
                    const auto slot = range.start + i;
                    const bool completed = heap.completed.contains(slot) && heap.completed.at(slot) == type;
                    if (completed != range.available)
                        throw std::runtime_error("Query execution coverage changed at source record " + std::to_string(id) +
                            ", slot " + std::to_string(slot));
                }
                if (range.bytes) resources.queryWrite(destination, id, range.offset, range.bytes, range.available);
                else resources.requireState(destination, id, 0, D3D12_RESOURCE_STATE_COPY_DEST);
            });
            if (range.available) list->ResolveQueryData(heap.native.Get(), type, range.start, range.count, buffer, range.offset);
        }
        break;
    }
    default: throw std::runtime_error("Unsupported native frame command: " + streamPacketName(packet.type));
    }
}
}
