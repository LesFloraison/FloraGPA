#include "NativeDescriptors.h"
#include <wrl/client.h>
#include <map>
#include <unordered_map>
#include <sstream>

namespace flora::dx12 {
namespace {
using format::Reader;
using Microsoft::WRL::ComPtr;
void require(bool value, const char *reason) { if (!value) throw std::runtime_error(reason); }
void check(HRESULT hr, const char *operation) {
    if (FAILED(hr)) { std::ostringstream s; s << operation << " failed: 0x" << std::hex << uint32_t(hr); throw std::runtime_error(s.str()); }
}
D3D12_DESCRIPTOR_HEAP_TYPE viewHeap(uint64_t type) {
    if (type == 0x1279306e469b5ULL) return D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (type == 0x12793378a8487ULL) return D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if (type == 0x127934a282fc7ULL) return D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    return D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
}
}
struct NativeDescriptors::Impl {
    NativeDevice &device; const StreamDirectory &directory; NativeResources &resources; CancelCheck cancelled;
    struct Slot { uint64_t write{}, update{}; };
    struct Heap {
        DescriptorHeapRecipe recipe; uint64_t cpu{}, gpu{}; UINT capturedIncrement{}, nativeIncrement{};
        ComPtr<ID3D12DescriptorHeap> native;
        std::unordered_map<UINT, Slot> slots;
    };
    std::map<uint64_t, Heap> heaps;
    std::map<uint64_t, std::vector<uint64_t>> objects;
    std::map<std::pair<uint64_t, UINT>, UINT> increments;
    uint64_t writes{}, copies{}, copiedSlots{};
    Impl(NativeDevice &d, const StreamDirectory &s, NativeResources &r, CancelCheck c) : device(d), directory(s), resources(r), cancelled(std::move(c)) {}
    uint64_t creation(uint64_t object, uint64_t at) const {
        const auto it = objects.find(object); require(it != objects.end(), "Captured descriptor heap is missing");
        uint64_t selected{}; for (const auto id : it->second) if (id <= at) selected = id;
        if (!selected && at <= directory.initializationCount()) for (const auto id : it->second) if (id <= directory.initializationCount()) {
            require(!selected, "Ambiguous forward descriptor heap lifetime"); selected = id;
        }
        return selected;
    }
    ID3D12DescriptorHeap *native(uint64_t creation) {
        auto &h = heaps.at(creation); if (h.native) return h.native.Get(); checkCancellation(cancelled);
        check(device.device()->CreateDescriptorHeap(&h.recipe.description, IID_PPV_ARGS(&h.native)), "Recreate captured descriptor heap");
        h.nativeIncrement = device.device()->GetDescriptorHandleIncrementSize(h.recipe.description.Type);
        require(h.nativeIncrement, "Device returned a zero descriptor increment");
        const auto name = L"Flora descriptor heap " + std::to_wstring(h.recipe.object) + L" creation " + std::to_wstring(creation);
        h.native->SetName(name.c_str()); return h.native.Get();
    }
};
NativeDescriptors::NativeDescriptors(NativeDevice &device, const StreamDirectory &directory, NativeResources &resources, const CancelCheck &cancelled)
    : impl_(std::make_unique<Impl>(device, directory, resources, cancelled)) {
    auto &s = *impl_;
    for (size_t i = 0; i < directory.packets().size(); ++i) {
        if (i % 1024 == 0) checkCancellation(cancelled); const auto &p = directory.packets()[i];
        if (p.type == 0x127932eb2eb14ULL) {
            const auto r = decodeDescriptorHeap(directory.payload(p)); if (FAILED(r.result) || !r.object) continue;
            require(UINT(r.description.Type) < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES && r.description.NumDescriptors &&
                (UINT(r.description.Flags) & ~UINT(D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)) == 0, "Invalid captured descriptor heap description");
            Impl::Heap heap; heap.recipe = r; s.heaps.emplace(i + 1, std::move(heap)); s.objects[r.object].push_back(i + 1);
        } else if (p.type == 0x127932ffcb166ULL) {
            Reader r(directory.payload(p)); const auto owner = r.read<uint64_t>(); const auto type = r.read<UINT>(), increment = r.read<UINT>(); r.end();
            require(type < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES && increment, "Invalid captured descriptor increment");
            const auto [it, inserted] = s.increments.emplace(std::pair{owner, type}, increment);
            require(inserted || it->second == increment, "Descriptor increment changed for one captured device");
        }
    }
    for (size_t i = 0; i < directory.packets().size(); ++i) {
        if (i % 1024 == 0) checkCancellation(cancelled); const auto &p = directory.packets()[i];
        if (p.type != 0x15a3eb3b8e877ULL && p.type != 0x15a3edf3ce2bbULL) continue;
        Reader r(directory.payload(p)); const auto object = r.read<uint64_t>(); require(r.flag(), "Missing captured descriptor heap start");
        const auto start = r.read<uint64_t>(); r.end(); const auto id = s.creation(object, i + 1); require(id, "Descriptor start precedes its heap lifetime");
        auto &heap = s.heaps.at(id); auto &value = p.type == 0x15a3eb3b8e877ULL ? heap.cpu : heap.gpu;
        require(!value || value == start, "Descriptor heap start changed within one lifetime"); value = start;
    }
    for (auto &[id, heap] : s.heaps) {
        const auto it = s.increments.find({heap.recipe.device, UINT(heap.recipe.description.Type)});
        require(it != s.increments.end(), "Captured descriptor increment is missing"); heap.capturedIncrement = it->second;
        const uint64_t size = uint64_t(heap.capturedIncrement) * heap.recipe.description.NumDescriptors;
        require(size <= UINT64_MAX - heap.cpu && size <= UINT64_MAX - heap.gpu, "Captured descriptor heap address overflow");
    }
}
NativeDescriptors::~NativeDescriptors() = default;
DescriptorReference NativeDescriptors::resolve(uint64_t handle, uint64_t at, bool gpu) const {
    require(handle && at && at <= impl_->directory.packets().size(), "Invalid captured descriptor reference");
    std::optional<DescriptorReference> selected;
    for (const auto &[id, heap] : impl_->heaps) {
        const auto start = gpu ? heap.gpu : heap.cpu; const uint64_t size = uint64_t(heap.capturedIncrement) * heap.recipe.description.NumDescriptors;
        if (!start || handle < start || handle - start >= size || impl_->creation(heap.recipe.object, at) != id) continue;
        require((handle - start) % heap.capturedIncrement == 0, "Descriptor handle is not aligned to the captured increment");
        require(!selected, "Descriptor handle overlaps multiple heap lifetimes");
        selected = DescriptorReference{heap.recipe.object, id, UINT((handle - start) / heap.capturedIncrement), heap.recipe.description.Type};
    }
    require(selected.has_value(), "Captured descriptor handle is not mapped at this boundary"); return *selected;
}
ID3D12DescriptorHeap *NativeDescriptors::heap(uint64_t object, uint64_t at) {
    const auto id = impl_->creation(object, at); require(id, "Descriptor heap is not defined at this boundary"); return impl_->native(id);
}
D3D12_CPU_DESCRIPTOR_HANDLE NativeDescriptors::cpu(uint64_t handle, uint64_t at, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count) {
    const auto ref = resolve(handle, at, false); auto &h = impl_->heaps.at(ref.creation);
    require(ref.type == type && count <= h.recipe.description.NumDescriptors - ref.index, "Descriptor range exceeds its heap or uses the wrong heap type");
    const auto native = impl_->native(ref.creation); return {native->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(ref.index) * h.nativeIncrement};
}
D3D12_GPU_DESCRIPTOR_HANDLE NativeDescriptors::gpu(uint64_t handle, uint64_t at) {
    const auto ref = resolve(handle, at, true); auto &h = impl_->heaps.at(ref.creation);
    require((h.recipe.description.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0, "GPU descriptor reference needs a shader-visible heap");
    const auto native = impl_->native(ref.creation); return {native->GetGPUDescriptorHandleForHeapStart().ptr + uint64_t(ref.index) * h.nativeIncrement};
}
bool NativeDescriptors::apply(uint64_t record) {
    auto &s = *impl_; checkCancellation(s.cancelled); require(record && record <= s.directory.packets().size(), "Invalid descriptor record");
    const auto &packet = s.directory.packets()[size_t(record - 1)]; const auto bytes = s.directory.payload(packet);
    if (isDescriptorWrite(packet.type)) {
        auto r = decodeDescriptorWrite(packet.type, bytes); const auto target = cpu(r.destination, record, viewHeap(packet.type));
        auto resource = r.resource ? s.resources.resource(r.resource, record) : nullptr;
        auto counter = r.counter ? s.resources.resource(r.counter, record) : nullptr;
        if (auto desc = std::get_if<D3D12_CONSTANT_BUFFER_VIEW_DESC>(&r.description)) desc->BufferLocation = s.resources.gpuAddress(desc->BufferLocation, record, desc->SizeInBytes);
        if (auto desc = std::get_if<D3D12_SHADER_RESOURCE_VIEW_DESC>(&r.description); desc && desc->ViewDimension == D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE)
            desc->RaytracingAccelerationStructure.Location = s.resources.gpuAddress(desc->RaytracingAccelerationStructure.Location, record);
        switch (packet.type) {
        case 0x1279384597f8cULL: s.device.device()->CreateConstantBufferView(std::get_if<D3D12_CONSTANT_BUFFER_VIEW_DESC>(&r.description), target); break;
        case 0x127932c396f41ULL: s.device.device()->CreateShaderResourceView(resource, std::get_if<D3D12_SHADER_RESOURCE_VIEW_DESC>(&r.description), target); break;
        case 0x12793e4a19efeULL: s.device.device()->CreateUnorderedAccessView(resource, counter, std::get_if<D3D12_UNORDERED_ACCESS_VIEW_DESC>(&r.description), target); break;
        case 0x1279306e469b5ULL: s.device.device()->CreateRenderTargetView(resource, std::get_if<D3D12_RENDER_TARGET_VIEW_DESC>(&r.description), target); break;
        case 0x12793378a8487ULL: s.device.device()->CreateDepthStencilView(resource, std::get_if<D3D12_DEPTH_STENCIL_VIEW_DESC>(&r.description), target); break;
        case 0x127934a282fc7ULL: {
            const auto desc = std::get_if<D3D12_SAMPLER_DESC>(&r.description); require(desc, "Captured sampler description is missing"); s.device.device()->CreateSampler(desc, target); break;
        }
        }
        const auto ref = resolve(r.destination, record, false); s.heaps.at(ref.creation).slots[ref.index] = {record, record}; ++s.writes; return true;
    }
    if (packet.type != 0x12793300ba04eULL && packet.type != 0x1279345e6c880ULL) return false;
    const auto copy = decodeDescriptorCopy(packet.type, bytes); std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> destinations, sources;
    std::vector<DescriptorReference> destinationRefs, sourceRefs; uint64_t destinationCount{}, sourceCount{};
    for (size_t i = 0; i < copy.destinations.size(); ++i) {
        destinations.push_back(cpu(copy.destinations[i], record, copy.type, copy.destinationSizes[i])); destinationRefs.push_back(resolve(copy.destinations[i], record, false)); destinationCount += copy.destinationSizes[i];
    }
    for (size_t i = 0; i < copy.sources.size(); ++i) {
        sources.push_back(cpu(copy.sources[i], record, copy.type, copy.sourceSizes[i])); sourceRefs.push_back(resolve(copy.sources[i], record, false)); sourceCount += copy.sourceSizes[i];
        require(!(s.heaps.at(sourceRefs.back().creation).recipe.description.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE), "Descriptor copy source is shader visible");
    }
    require(sourceCount == destinationCount, "Descriptor copy range totals do not match");
    require(sourceCount <= 4ULL * 1024 * 1024, "Descriptor copy provenance exceeds the 64 MiB staging limit");
    // Snapshot provenance before updating any destination, including overlapping
    // ranges. Unwritten source slots stay explicitly unknown after a copy.
    std::vector<Impl::Slot> provenance; provenance.reserve(size_t(sourceCount));
    for (size_t i = 0; i < sourceRefs.size(); ++i) {
        const auto &ref = sourceRefs[i]; const auto &heap = s.heaps.at(ref.creation);
        for (UINT n = 0; n < copy.sourceSizes[i]; ++n) { const auto it = heap.slots.find(ref.index + n); provenance.push_back(it == heap.slots.end() ? Impl::Slot{} : it->second); }
    }
    s.device.device()->CopyDescriptors(UINT(destinations.size()), destinations.data(), copy.destinationSizes.data(), UINT(sources.size()), sources.data(), copy.sourceSizes.data(), copy.type);
    size_t index{};
    for (size_t i = 0; i < destinationRefs.size(); ++i) {
        const auto &ref = destinationRefs[i]; auto &heap = s.heaps.at(ref.creation);
        for (UINT n = 0; n < copy.destinationSizes[i]; ++n) { auto slot = provenance[index++]; if (slot.write) { slot.update = record; heap.slots[ref.index + n] = slot; } else heap.slots.erase(ref.index + n); }
    }
    ++s.copies; s.copiedSlots += sourceCount; return true;
}
nlohmann::json NativeDescriptors::information() const {
    uint64_t native{}, slots{}, descriptors{};
    for (const auto &[id, h] : impl_->heaps) { native += bool(h.native); slots += h.slots.size(); if (h.native) descriptors += h.recipe.description.NumDescriptors; }
    return {{"native_heaps", native}, {"descriptor_capacity", std::to_string(descriptors)}, {"known_slots", std::to_string(slots)},
        {"write_calls", std::to_string(impl_->writes)}, {"copy_calls", std::to_string(impl_->copies)}, {"copied_slots", std::to_string(impl_->copiedSlots)},
        {"shader_access_verified", false}};
}
}
