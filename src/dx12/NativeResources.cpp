#include "NativeResources.h"
#include "AddressIndex.h"
#include "BackBufferIndex.h"
#include "QueryReplayIndex.h"
#include <wrl/client.h>
#include <algorithm>
#include <map>
#include <set>
#include <sstream>

namespace flora::dx12 {
namespace {
using format::Reader;
using Microsoft::WRL::ComPtr;
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
void check(HRESULT hr, const char *operation) {
    if (FAILED(hr)) { std::ostringstream s; s << operation << " failed: 0x" << std::hex << uint32_t(hr); throw std::runtime_error(s.str()); }
}
uint64_t output(Reader &r, REFIID expected, HRESULT &hr) {
    const auto iid = r.read<GUID>(); require(iid == expected, "Unsupported resource/heap interface");
    uint64_t object{};
    if (r.flag()) { require(r.read<uint64_t>() == 1, "Invalid resource output array"); object = r.read<uint64_t>(); }
    hr = r.read<HRESULT>(); r.end(); return object;
}
template<class T> T required(Reader &r, const char *message) { require(r.flag(), message); return r.read<T>(); }
D3D12_HEAP_FLAGS replayHeapFlags(D3D12_HEAP_FLAGS flags) {
    // Write-watch tracks CPU dirty pages during capture. Replay applies recorded
    // patches explicitly and never calls GetWriteWatch. Keeping this capture
    // flag also breaks RenderDoc 1.45's upload-to-default replay remapping.
    return D3D12_HEAP_FLAGS(UINT(flags) & ~UINT(D3D12_HEAP_FLAG_ALLOW_WRITE_WATCH));
}
uint64_t resolve(const std::map<uint64_t, std::vector<uint64_t>> &objects, uint64_t object, uint64_t at, size_t initialization) {
    const auto it = objects.find(object);
    if (it == objects.end()) throw std::runtime_error("Missing DX12 allocation recipe for object " + std::to_string(object));
    uint64_t selected{};
    // A repeated object address is a new creation boundary, even inside the
    // keyframe (Hellblade2 repeats upload-buffer declarations). Keep both
    // native allocations keyed by record so older recorded lists retain theirs.
    for (const auto id : it->second) if (id <= at) selected = id;
    if (!selected && at <= initialization) {
        for (const auto id : it->second) if (id <= initialization) {
            require(!selected, "Ambiguous initialization allocation lifetime"); selected = id;
        }
    }
    require(selected, "Allocation is not defined at the selected boundary"); return selected;
}
std::optional<D3D12_RESOURCE_STATES> legacyState(const ResourceRecipe &r) {
    if (r.kind != ResourceRecipe::Kind::Committed3) return r.initialState;
    if (r.description.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && r.initialLayout == D3D12_BARRIER_LAYOUT_UNDEFINED)
        return r.properties.Type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ :
            r.properties.Type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COMMON;
    switch (r.initialLayout) {
    case D3D12_BARRIER_LAYOUT_COMMON: return D3D12_RESOURCE_STATE_COMMON;
    case D3D12_BARRIER_LAYOUT_RENDER_TARGET: return D3D12_RESOURCE_STATE_RENDER_TARGET;
    case D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ: return D3D12_RESOURCE_STATE_DEPTH_READ;
    case D3D12_BARRIER_LAYOUT_COPY_SOURCE: return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case D3D12_BARRIER_LAYOUT_COPY_DEST: return D3D12_RESOURCE_STATE_COPY_DEST;
    case D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE: return D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    case D3D12_BARRIER_LAYOUT_RESOLVE_DEST: return D3D12_RESOURCE_STATE_RESOLVE_DEST;
    default: return {};
    }
}
}
bool isResourceRecipe(uint64_t type) {
    return type == 0x12793f58d8f79ULL || type == 0x127935d88ccfaULL || type == 0x127937ac5a8a9ULL || type == 0x16cea06d8db48ULL;
}
ResourceRecipe decodeResourceRecipe(uint64_t type, format::Bytes bytes) {
    require(isResourceRecipe(type), "Record is not a resource creation"); Reader r(bytes); ResourceRecipe out;
    out.device = r.read<uint64_t>();
    if (type == 0x12793f58d8f79ULL || type == 0x16cea06d8db48ULL) {
        static_assert(sizeof(D3D12_HEAP_PROPERTIES) == 20);
        out.properties = required<D3D12_HEAP_PROPERTIES>(r, "Missing heap properties"); out.heapFlags = r.read<D3D12_HEAP_FLAGS>();
        if (type == 0x16cea06d8db48ULL) out.kind = ResourceRecipe::Kind::Committed3;
    } else if (type == 0x127935d88ccfaULL) {
        out.kind = ResourceRecipe::Kind::Placed; out.heap = r.read<uint64_t>(); out.heapOffset = r.read<uint64_t>();
    } else out.kind = ResourceRecipe::Kind::Reserved;
    static_assert(sizeof(D3D12_RESOURCE_DESC) == 56 && sizeof(D3D12_RESOURCE_DESC1) == 64);
    if (out.kind == ResourceRecipe::Kind::Committed3) {
        out.description1 = required<D3D12_RESOURCE_DESC1>(r, "Missing resource description");
        // DESC1 adds sampler-feedback dimensions at byte 52, replacing padding.
        memcpy(&out.description, &out.description1, 52); out.initialLayout = r.read<D3D12_BARRIER_LAYOUT>();
    } else {
        out.description = required<D3D12_RESOURCE_DESC>(r, "Missing resource description"); out.initialState = r.read<D3D12_RESOURCE_STATES>();
    }
    static_assert(sizeof(D3D12_CLEAR_VALUE) == 20);
    if (r.flag()) out.clear = r.read<D3D12_CLEAR_VALUE>();
    if (out.kind == ResourceRecipe::Kind::Committed3) {
        out.protectedSession = r.read<uint64_t>(); const auto count = r.read<UINT>();
        if (r.flag()) {
            const auto entries = r.read<uint64_t>(); require(entries == count && entries <= r.remaining() / 4, "Invalid castable format array");
            for (uint64_t i = 0; i < entries; ++i) out.castableFormats.push_back(r.read<DXGI_FORMAT>());
        }
        require(out.castableFormats.size() == count, "Missing castable formats");
    }
    out.object = output(r, __uuidof(ID3D12Resource), out.result); return out;
}
HeapRecipe decodeHeapRecipe(format::Bytes bytes) {
    Reader r(bytes); HeapRecipe out; out.device = r.read<uint64_t>();
    static_assert(sizeof(D3D12_HEAP_DESC) == 48); out.description = required<D3D12_HEAP_DESC>(r, "Missing heap description");
    out.object = output(r, __uuidof(ID3D12Heap), out.result); return out;
}
struct NativeResources::Impl {
    NativeDevice &device; const StreamDirectory &directory; CancelCheck cancelled;
    AddressIndex addresses;
    BackBufferIndex backbuffers;
    uint64_t limit{}, allocated{}, peakAllocated{}, created{}, uploaded{}, uploadRecords{}, writeWatchRemoved{};
    uint64_t equivalentAddressBindings{};
    std::set<uint64_t> retired;
    std::map<uint64_t, std::vector<uint64_t>> resourceRecords, heapRecords;
    std::map<uint64_t, ComPtr<ID3D12Heap>> heaps;
    struct Resource {
        ComPtr<ID3D12Resource> native;
        uint64_t allocation{};
        std::optional<D3D12_RESOURCE_STATES> uniform;
        std::map<UINT, D3D12_RESOURCE_STATES> states;
        bool touched = false;
        std::set<UINT> promotedReads;
        UINT subresources = 1;
    };
    std::map<uint64_t, Resource> resources;
    std::map<uint64_t, QueryReadbackState> isolatedQueries;
    Impl(NativeDevice &d, const StreamDirectory &s, uint64_t l, CancelCheck c) : device(d), directory(s), cancelled(std::move(c)), addresses(s, cancelled), backbuffers(s, cancelled), limit(l) {}
    void budget(uint64_t size, uint64_t record) const {
        if (size == UINT64_MAX) throw std::runtime_error("Device rejected allocation description at creation record " + std::to_string(record));
        if (allocated > limit || size > limit - allocated)
            throw std::runtime_error("Native resource allocation budget exceeded at creation record " + std::to_string(record) +
                ": requested " + std::to_string(size) + ", allocated " + std::to_string(allocated) + ", limit " + std::to_string(limit));
    }
    ResourceRecipe recipe(uint64_t record) const {
        if (const auto it = backbuffers.buffers().find(record); it != backbuffers.buffers().end()) {
            ResourceRecipe r; r.object = it->second.object; r.description = it->second.description;
            r.properties.Type = D3D12_HEAP_TYPE_DEFAULT; r.properties.CreationNodeMask = r.properties.VisibleNodeMask = 1;
            r.initialState = D3D12_RESOURCE_STATE_PRESENT; return r;
        }
        const auto &p = directory.packets().at(size_t(record - 1)); return decodeResourceRecipe(p.type, directory.payload(p));
    }
    ID3D12Heap *heap(uint64_t object, uint64_t at, const D3D12_RESOURCE_ALLOCATION_INFO &placement, uint64_t offset) {
        const auto record = resolve(heapRecords, object, at, directory.initializationCount());
        const auto recipe = decodeHeapRecipe(directory.payload(directory.packets()[size_t(record - 1)]));
        require(placement.SizeInBytes != UINT64_MAX && placement.Alignment && offset % placement.Alignment == 0 &&
            offset <= recipe.description.SizeInBytes && placement.SizeInBytes <= recipe.description.SizeInBytes - offset, "Captured resource placement is incompatible with this device");
        if (auto it = heaps.find(record); it != heaps.end()) return it->second.Get();
        budget(recipe.description.SizeInBytes, record); ComPtr<ID3D12Heap> native;
        auto desc = recipe.description; desc.Flags = replayHeapFlags(desc.Flags);
        check(device.device()->CreateHeap(&desc, IID_PPV_ARGS(&native)), "Create captured heap");
        writeWatchRemoved += desc.Flags != recipe.description.Flags;
        allocated += recipe.description.SizeInBytes; peakAllocated = (std::max)(peakAllocated, allocated);
        auto result = native.Get(); heaps.emplace(record, std::move(native)); return result;
    }
};
NativeResources::NativeResources(NativeDevice &device, const StreamDirectory &directory, uint64_t allocationLimit, const CancelCheck &cancelled)
    : impl_(std::make_unique<Impl>(device, directory, allocationLimit, cancelled)) {
    require(allocationLimit, "Resource allocation budget cannot be zero");
    for (size_t i = 0; i < directory.packets().size(); ++i) {
        if (i % 1024 == 0) checkCancellation(cancelled); const auto &p = directory.packets()[i];
        if (const auto it = impl_->backbuffers.buffers().find(i + 1); it != impl_->backbuffers.buffers().end()) {
            impl_->resourceRecords[it->second.object].push_back(i + 1);
        } else if (isResourceRecipe(p.type)) {
            const auto r = decodeResourceRecipe(p.type, directory.payload(p));
            if (SUCCEEDED(r.result) && r.object) impl_->resourceRecords[r.object].push_back(i + 1);
        } else if (p.type == 0x1279335a5747bULL) {
            const auto r = decodeHeapRecipe(directory.payload(p));
            if (SUCCEEDED(r.result) && r.object) impl_->heapRecords[r.object].push_back(i + 1);
        }
    }
}
NativeResources::~NativeResources() = default;
uint64_t NativeResources::creationRecord(uint64_t object, uint64_t atRecord) const {
    return resolve(impl_->resourceRecords, object, atRecord, impl_->directory.initializationCount());
}
ResourceRecipe NativeResources::recipe(uint64_t object, uint64_t atRecord) const { return impl_->recipe(creationRecord(object, atRecord)); }
nlohmann::json NativeResources::placement(uint64_t creation) const {
    auto &s = *impl_; checkCancellation(s.cancelled);
    require(creation && creation <= s.directory.packets().size(), "Invalid placement resource creation");
    const auto recipe = s.recipe(creation);
    require(SUCCEEDED(recipe.result) && recipe.object &&
        creationRecord(recipe.object, creation) == creation, "Placement requires a successful resource creation");
    nlohmann::json out{{"status", "not_placed"}, {"resource_creation", std::to_string(creation)},
        {"resource_object", std::to_string(recipe.object)},
        {"source", "replay_device_allocation_requirements"},
        {"scope", "Captured placement and heap creation with requirements from the current replay device. "
                  "This does not recover the original adapter's allocation size or establish active alias ownership, "
                  "content preservation or synchronization."}};
    if (recipe.kind != ResourceRecipe::Kind::Placed) return out;
    const auto heapCreation = resolve(s.heapRecords, recipe.heap, creation, s.directory.initializationCount());
    const auto heap = decodeHeapRecipe(s.directory.payload(s.directory.packets().at(size_t(heapCreation - 1))));
    const auto allocation = s.device.device()->GetResourceAllocationInfo(0, 1, &recipe.description);
    require(allocation.SizeInBytes && allocation.SizeInBytes != UINT64_MAX && allocation.Alignment,
        "Replay device rejected the placed resource allocation description");
    const bool fits = recipe.heapOffset % allocation.Alignment == 0 &&
        recipe.heapOffset <= heap.description.SizeInBytes &&
        allocation.SizeInBytes <= heap.description.SizeInBytes - recipe.heapOffset;
    out.update({{"status", fits ? "available" : "incompatible"},
        {"heap_object", std::to_string(recipe.heap)}, {"heap_creation", std::to_string(heapCreation)},
        {"heap_size", std::to_string(heap.description.SizeInBytes)}, {"heap_offset", std::to_string(recipe.heapOffset)},
        {"allocation_size", std::to_string(allocation.SizeInBytes)}, {"alignment", std::to_string(allocation.Alignment)},
        {"allocation_api", "ID3D12Device::GetResourceAllocationInfo"},
        {"heap_range_end", fits ? nlohmann::json(std::to_string(recipe.heapOffset + allocation.SizeInBytes)) : nlohmann::json()}});
    if (!fits) out["reason"] = "Replay-device allocation does not fit the captured heap or offset alignment";
    return out;
}
ID3D12Resource *NativeResources::resource(uint64_t object, uint64_t atRecord) {
    const auto creation = creationRecord(object, atRecord);
    if (impl_->isolatedQueries.contains(creation))
        throw std::runtime_error("Ordinary access to isolated query READBACK creation " + std::to_string(creation) +
            " at record " + std::to_string(atRecord) + " is unsupported; it may consume unavailable query bytes");
    return materialize(object, atRecord);
}
void NativeResources::isolateQueryReadback(uint64_t creation) {
    auto &s = *impl_;
    if (s.isolatedQueries.contains(creation)) return;
    require(s.resources.empty(), "Query readbacks must be isolated before materializing resources");
    const auto recipe = s.recipe(creation);
    require(s.resourceRecords.at(recipe.object).size() == 1 && !recipe.protectedSession &&
                recipe.description.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && recipe.description.Width &&
                legacyState(recipe) == D3D12_RESOURCE_STATE_COPY_DEST,
            "Isolated query readback requires one buffer lifetime in COPY_DEST");
    auto properties = recipe.properties; auto flags = recipe.heapFlags;
    if (recipe.kind == ResourceRecipe::Kind::Placed) {
        require(s.heapRecords.at(recipe.heap).size() == 1, "Isolated query heap has multiple captured lifetimes");
        const auto heapCreation = s.heapRecords.at(recipe.heap).front();
        const auto heap = decodeHeapRecipe(s.directory.payload(s.directory.packets().at(size_t(heapCreation - 1))));
        properties = heap.description.Properties; flags = heap.description.Flags;
        const auto size = s.device.device()->GetResourceAllocationInfo(0, 1, &recipe.description).SizeInBytes;
        require(size && size != UINT64_MAX && recipe.heapOffset <= heap.description.SizeInBytes &&
                    size <= heap.description.SizeInBytes - recipe.heapOffset, "Invalid isolated query placement");
        // Check every captured placement, including future aliases. Native
        // allocation sizes include padding and are stricter than buffer widths.
        for (const auto &[object, records] : s.resourceRecords) for (const auto record : records) {
            checkCancellation(s.cancelled);
            if (record == creation) continue;
            const auto other = s.recipe(record);
            if (other.kind != ResourceRecipe::Kind::Placed || other.heap != recipe.heap) continue;
            require(other.description.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER,
                    "Isolated query heap contains a non-buffer placement");
            const auto otherSize = s.device.device()->GetResourceAllocationInfo(0, 1, &other.description).SizeInBytes;
            require(otherSize && otherSize != UINT64_MAX && other.heapOffset <= heap.description.SizeInBytes &&
                        otherSize <= heap.description.SizeInBytes - other.heapOffset, "Invalid query heap alias placement");
            require(recipe.heapOffset + size <= other.heapOffset || other.heapOffset + otherSize <= recipe.heapOffset,
                    "Isolated query readback overlaps another captured resource allocation");
        }
    } else require(recipe.kind == ResourceRecipe::Kind::Committed, "Unsupported isolated query allocation kind");
    require(properties.Type == D3D12_HEAP_TYPE_READBACK &&
                !(flags & (D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER)),
            "Isolated query output requires a private READBACK heap");
    s.isolatedQueries.emplace(creation, QueryReadbackState(recipe.description.Width));
}
ID3D12Resource *NativeResources::queryDestination(uint64_t object, uint64_t atRecord) {
    return materialize(object, atRecord);
}
void NativeResources::queryWrite(uint64_t object, uint64_t atRecord, uint64_t offset, uint64_t bytes, bool available) {
    auto &s = *impl_;
    if (auto found = s.isolatedQueries.find(creationRecord(object, atRecord)); found != s.isolatedQueries.end()) {
        const auto &resource = s.resources.at(found->first);
        require(resource.uniform == D3D12_RESOURCE_STATE_COPY_DEST && resource.states.empty(),
                "Isolated query readback state changed");
        found->second.write(offset, bytes, available);
    } else {
        require(available, "Missing query write requires an isolated readback");
        requireState(object, atRecord, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    }
}
ID3D12Resource *NativeResources::readableBuffer(uint64_t object, uint64_t atRecord, uint64_t offset, uint64_t bytes) {
    if (const auto found = impl_->isolatedQueries.find(creationRecord(object, atRecord)); found != impl_->isolatedQueries.end())
        found->second.requireReadable(offset, bytes);
    return materialize(object, atRecord);
}
ID3D12Resource *NativeResources::materialize(uint64_t object, uint64_t atRecord) {
    auto &s = *impl_; checkCancellation(s.cancelled); const auto record = creationRecord(object, atRecord);
    require(!s.retired.contains(record), "Captured resource was released before this boundary");
    if (auto it = s.resources.find(record); it != s.resources.end()) return it->second.native.Get();
    const auto r = s.recipe(record); const auto clear = r.clear ? &*r.clear : nullptr;
    ComPtr<ID3D12Resource> native; uint64_t allocation{};
    if (r.kind == ResourceRecipe::Kind::Placed) {
        const auto info = s.device.device()->GetResourceAllocationInfo(0, 1, &r.description);
        auto heap = s.heap(r.heap, atRecord, info, r.heapOffset);
        check(s.device.device()->CreatePlacedResource(heap, r.heapOffset, &r.description, r.initialState, clear, IID_PPV_ARGS(&native)), "Create captured placed resource");
    } else if (r.kind == ResourceRecipe::Kind::Reserved) {
        check(s.device.device()->CreateReservedResource(&r.description, r.initialState, clear, IID_PPV_ARGS(&native)), "Create captured reserved resource");
    } else if (r.kind == ResourceRecipe::Kind::Committed3) {
        require(!r.protectedSession, "Protected resource-session reconstruction is unavailable");
        ComPtr<ID3D12Device10> device10; check(s.device.device()->QueryInterface(IID_PPV_ARGS(&device10)), "Query resource-layout device");
        // Castable formats affect validity as well as allocation size (the
        // Hellblade2 BC1 UAV resources also permit R32G32_UINT views).
        if (!r.castableFormats.empty()) {
            ComPtr<ID3D12Device12> device12; check(s.device.device()->QueryInterface(IID_PPV_ARGS(&device12)), "Query castable resource allocation device");
            const UINT count = UINT(r.castableFormats.size()); const DXGI_FORMAT *formats = r.castableFormats.data();
            allocation = device12->GetResourceAllocationInfo3(0, 1, &r.description1, &count, &formats, nullptr).SizeInBytes;
        } else {
            ComPtr<ID3D12Device8> device8; check(s.device.device()->QueryInterface(IID_PPV_ARGS(&device8)), "Query resource allocation device");
            allocation = device8->GetResourceAllocationInfo2(0, 1, &r.description1, nullptr).SizeInBytes;
        }
        s.budget(allocation, record);
        check(device10->CreateCommittedResource3(&r.properties, replayHeapFlags(r.heapFlags), &r.description1, r.initialLayout, clear, nullptr,
            UINT(r.castableFormats.size()), r.castableFormats.data(), IID_PPV_ARGS(&native)), "Create captured resource with barrier layout");
        s.writeWatchRemoved += replayHeapFlags(r.heapFlags) != r.heapFlags;
    } else {
        allocation = s.device.device()->GetResourceAllocationInfo(0, 1, &r.description).SizeInBytes; s.budget(allocation, record);
        check(s.device.device()->CreateCommittedResource(&r.properties, replayHeapFlags(r.heapFlags), &r.description, r.initialState, clear, IID_PPV_ARGS(&native)), "Create captured committed resource");
        s.writeWatchRemoved += replayHeapFlags(r.heapFlags) != r.heapFlags;
    }
    s.allocated += allocation; s.peakAllocated = (std::max)(s.peakAllocated, s.allocated); ++s.created; auto result = native.Get();
    const auto name = L"Flora DX12 resource " + std::to_wstring(object) + L" creation " + std::to_wstring(record); native->SetName(name.c_str());
    UINT subresources = 1;
    const auto nativeDescription = native->GetDesc();
    if (nativeDescription.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        D3D12_FEATURE_DATA_FORMAT_INFO info{nativeDescription.Format}; check(s.device.device()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof(info)), "Query resource planes");
        subresources = UINT(nativeDescription.MipLevels) * (nativeDescription.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : nativeDescription.DepthOrArraySize) * info.PlaneCount;
    }
    s.resources.emplace(record, Impl::Resource{std::move(native), allocation, legacyState(r), {}, false, {}, subresources}); return result;
}
void NativeResources::restoreMappedData(uint64_t record) {
    auto &s = *impl_; const auto &p = s.directory.packets().at(size_t(record - 1));
    require(p.type == 0x20000000000000ULL, "Expected captured memory-copy operation");
    Reader r(s.directory.payload(p)); const auto object = r.read<uint64_t>(); const auto data = s.directory.resourceData(r.read<uint64_t>()); r.end();
    const auto recipe = this->recipe(object, record);
    require(recipe.description.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && data.capturedObject == object,
        "Captured memory copy is not a buffer snapshot or patch");
    Reader descriptor(s.directory.resources().bytes(data.descriptorOffset, 64)); descriptor.skip(8);
    require(descriptor.read<uint64_t>() == 0, "Unsupported memory-copy subresource"); descriptor.skip(4);
    require(descriptor.read<UINT>() == 0, "Unsupported memory-copy descriptor flags"); descriptor.skip(8);
    const auto destinationOffset = descriptor.read<uint64_t>(); descriptor.skip(16);
    require(destinationOffset <= recipe.description.Width && data.size <= recipe.description.Width - destinationOffset, "Captured memory patch exceeds its buffer");
    require(descriptor.read<uint64_t>() == 0, "Unsupported memory-copy extension");
    auto native = resource(object, record); void *mapped{}; D3D12_RANGE noRead{0, 0}; check(native->Map(0, &noRead, &mapped), "Map captured buffer for initialization");
    try {
        for (size_t offset = 0; offset < data.bytes.size();) {
            checkCancellation(s.cancelled); const auto size = (std::min)(size_t(8 * 1024 * 1024), data.bytes.size() - offset);
            memcpy(static_cast<uint8_t *>(mapped) + destinationOffset + offset, data.bytes.data() + offset, size); offset += size;
        }
    } catch (...) { native->Unmap(0, nullptr); throw; }
    D3D12_RANGE written{SIZE_T(destinationOffset), SIZE_T(destinationOffset + data.size)}; native->Unmap(0, &written); s.uploaded += data.size; ++s.uploadRecords;
}
ID3D12Heap *NativeResources::tileHeap(uint64_t object, uint64_t atRecord) {
    for (const auto &[creation, state] : impl_->isolatedQueries)
        require(impl_->recipe(creation).heap != object, "Tile mapping of an isolated query heap is unsupported");
    return impl_->heap(object, atRecord, D3D12_RESOURCE_ALLOCATION_INFO{0, D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES}, 0);
}
D3D12_GPU_VIRTUAL_ADDRESS NativeResources::gpuAddress(uint64_t captured, uint64_t atRecord, uint64_t bytes) {
    if (!captured) return 0;
    auto &s = *impl_;
    auto references = s.addresses.candidates(captured, atRecord, bytes);
    std::erase_if(references, [&](const auto &reference) { return s.retired.contains(reference.creation); });
    require(!references.empty(), "Captured GPU address has no live resource at this boundary");
    uint64_t heapCreation{}, heapByte{};
    if (references.size() > 1) {
        for (const auto &reference : references) {
            const auto recipe = s.recipe(reference.creation);
            require(recipe.kind == ResourceRecipe::Kind::Placed &&
                        recipe.heapOffset <= UINT64_MAX - reference.offset,
                    "Overlapping GPU addresses require equivalent placed-buffer storage");
            const auto heap = resolve(s.heapRecords, recipe.heap, reference.creation, s.directory.initializationCount());
            require(heap == resolve(s.heapRecords, recipe.heap, atRecord, s.directory.initializationCount()),
                    "GPU address alias references an expired heap lifetime");
            const auto byte = recipe.heapOffset + reference.offset;
            require(!heapCreation || (heapCreation == heap && heapByte == byte),
                    "Overlapping GPU addresses refer to different heap bytes");
            heapCreation = heap;
            heapByte = byte;
        }
    }
    D3D12_GPU_VIRTUAL_ADDRESS address{};
    for (const auto &reference : references) {
        const auto base = resource(reference.object, atRecord)->GetGPUVirtualAddress();
        require(base && reference.offset <= UINT64_MAX - base, "Native resource has no usable GPU virtual address");
        const auto candidate = base + reference.offset;
        require(!address || address == candidate, "Equivalent captured aliases have different native GPU addresses");
        address = candidate;
    }
    if (references.size() > 1) ++s.equivalentAddressBindings;
    return address;
}
void NativeResources::releaseCompleted(uint64_t releaseRecord) {
    auto &s = *impl_; const auto &p = s.directory.packets().at(size_t(releaseRecord - 1));
    require(p.type == 0x14538e8691d1cULL, "Expected captured IUnknownRelease");
    Reader r(s.directory.payload(p)); const auto object = r.read<uint64_t>(); const auto references = r.read<ULONG>(); r.end();
    if (references || !s.resourceRecords.contains(object)) return;
    const auto record = creationRecord(object, releaseRecord); s.retired.insert(record);
    if (auto it = s.resources.find(record); it != s.resources.end()) { s.allocated -= it->second.allocation; s.resources.erase(it); }
}
D3D12_RESOURCE_STATES NativeResources::state(uint64_t object, uint64_t atRecord, UINT subresource) {
    resource(object, atRecord); const auto &r = impl_->resources.at(creationRecord(object, atRecord));
    require(r.uniform.has_value(), "Enhanced resource layout has no verified legacy state");
    if (subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
        const auto expected = r.states.size() == r.subresources ? r.states.begin()->second : *r.uniform;
        for (const auto &[index, value] : r.states) require(value == expected, "Resource subresources do not share one state");
        return expected;
    }
    require(subresource < r.subresources, "Resource state subresource is out of range");
    if (auto it = r.states.find(subresource); it != r.states.end()) return it->second;
    return *r.uniform;
}
void NativeResources::transition(uint64_t object, uint64_t atRecord, UINT subresource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    const auto current = state(object, atRecord, subresource);
    const auto desc = resource(object, atRecord)->GetDesc();
    const auto promotable = [&](D3D12_RESOURCE_STATES value) {
        if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) return false;
        const UINT all = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER |
            D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_STREAM_OUT | D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT |
            D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_RESOLVE_SOURCE | D3D12_RESOURCE_STATE_RESOLVE_DEST;
        const UINT texture = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_COPY_DEST;
        return !(UINT(value) & ~(desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ||
            (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) ? all : texture));
    };
    if (current != before && !(current == D3D12_RESOURCE_STATE_COMMON && promotable(before)))
        throw std::runtime_error("Captured barrier state mismatch for resource " + std::to_string(object) + " at record " + std::to_string(atRecord));
    auto &r = impl_->resources.at(creationRecord(object, atRecord));
    r.touched = true;
    if (subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) r.promotedReads.clear();
    else {
        if (r.promotedReads.erase(D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)) for (UINT i = 0; i < r.subresources; ++i) r.promotedReads.insert(i);
        r.promotedReads.erase(subresource);
    }
    if (subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) { r.uniform = after; r.states.clear(); }
    else r.states[subresource] = after;
}
void NativeResources::requireState(uint64_t object, uint64_t atRecord, UINT subresource, D3D12_RESOURCE_STATES required) {
    const auto current = state(object, atRecord, subresource);
    auto &r = impl_->resources.at(creationRecord(object, atRecord)); r.touched = true;
    if ((current & required) == required) return;
    // Promotion is legal only from COMMON (or a compatible implicit read).
    const UINT reads = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT | D3D12_RESOURCE_STATE_COPY_SOURCE;
    const bool read = !(UINT(required) & ~reads);
    require(current == D3D12_RESOURCE_STATE_COMMON || (read && (r.promotedReads.contains(subresource) || r.promotedReads.contains(D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES))), "Resource use needs an explicit transition");
    const auto next = D3D12_RESOURCE_STATES(UINT(current) | UINT(required));
    // transition validates that the requested COMMON promotion is supported by
    // this resource class, without issuing an extra native barrier.
    if (current == D3D12_RESOURCE_STATE_COMMON) transition(object, atRecord, subresource, required, next);
    else if (subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) { r.uniform = next; r.states.clear(); }
    else r.states[subresource] = next;
    if (read) r.promotedReads.insert(subresource);
}
void NativeResources::completeSubmission(D3D12_COMMAND_LIST_TYPE queueType) {
    for (auto &[id, r] : impl_->resources) {
        if (!r.touched) continue; r.touched = false;
        D3D12_HEAP_PROPERTIES heap{}; D3D12_HEAP_FLAGS flags{};
        // Reserved resources have tile mappings, not a parent heap. Querying
        // GetHeapProperties on them is itself a D3D12 validation error.
        if (impl_->recipe(id).kind != ResourceRecipe::Kind::Reserved &&
            SUCCEEDED(r.native->GetHeapProperties(&heap, &flags)) &&
            (heap.Type == D3D12_HEAP_TYPE_UPLOAD || heap.Type == D3D12_HEAP_TYPE_READBACK || heap.Type == D3D12_HEAP_TYPE_GPU_UPLOAD)) continue;
        const auto desc = r.native->GetDesc();
        if (queueType == D3D12_COMMAND_LIST_TYPE_COPY || desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ||
            (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) || r.promotedReads.contains(D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)) {
            r.uniform = D3D12_RESOURCE_STATE_COMMON; r.states.clear();
        } else for (const auto sub : r.promotedReads) r.states[sub] = D3D12_RESOURCE_STATE_COMMON;
        r.promotedReads.clear();
    }
}
uint64_t NativeResources::remainingAllocationBudget() const { return impl_->limit - impl_->allocated; }
void NativeResources::reserveRetainedAllocation(uint64_t bytes, uint64_t sourceRecord) {
    auto &s = *impl_; s.budget(bytes, sourceRecord);
    s.allocated += bytes; s.peakAllocated = (std::max)(s.peakAllocated, s.allocated);
}
nlohmann::json NativeResources::information() const {
    auto isolated = nlohmann::json::array();
    for (const auto &[creation, state] : impl_->isolatedQueries) {
        auto entry = state.information(); entry["creation"] = std::to_string(creation); isolated.push_back(std::move(entry));
    }
    return {{"created_resources", impl_->created}, {"resident_resources", impl_->resources.size()}, {"retired_resources", impl_->retired.size()}, {"created_heaps", impl_->heaps.size()},
        {"isolated_query_readbacks", std::move(isolated)},
        {"query_availability_scope", "Submission-state model; CPU inspection requires a successfully completed GPU boundary"},
        {"allocation_bytes", std::to_string(impl_->allocated)}, {"allocation_limit", std::to_string(impl_->limit)},
        {"peak_allocation_bytes", std::to_string(impl_->peakAllocated)},
        {"equivalent_placed_address_bindings", std::to_string(impl_->equivalentAddressBindings)},
        {"address_alias_policy", "identical_placed_heap_byte_range_and_native_gpu_address"},
        {"restored_memory_records", std::to_string(impl_->uploadRecords)}, {"uploaded_bytes", std::to_string(impl_->uploaded)},
        {"write_watch_flags_removed", std::to_string(impl_->writeWatchRemoved)}};
}
}
