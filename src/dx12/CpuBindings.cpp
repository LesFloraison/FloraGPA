#define NOMINMAX
#include "CpuBindings.h"
#include "DescriptorRecipes.h"
#include "PipelineRecipes.h"
#include "AddressIndex.h"
#include "BackBufferIndex.h"
#include <wrl/client.h>
#include <algorithm>
#include <map>
#include <set>

namespace flora::dx12 {
namespace {
using Json = nlohmann::json;
using format::Reader;
using Microsoft::WRL::ComPtr;
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
uint64_t previous(const std::vector<uint64_t> &records, uint64_t at) {
    const auto end = std::upper_bound(records.begin(), records.end(), at);
    return end == records.begin() ? 0 : *std::prev(end);
}
const char *category(uint64_t type) {
    switch (type) {
    case 0x1279384597f8cULL: return "CBV";
    case 0x127932c396f41ULL: return "SRV";
    case 0x12793e4a19efeULL: return "UAV";
    case 0x1279306e469b5ULL: return "RTV";
    case 0x12793378a8487ULL: return "DSV";
    default: return "Sampler";
    }
}
}
struct CpuBindings::Impl {
    const StreamDirectory &directory;
    const SubmissionIndex &submissions;
    AddressIndex addresses;
    struct Heap { DescriptorHeapRecipe recipe; uint64_t cpu{}, gpu{}, increment{}, end = UINT64_MAX; };
    struct View { DescriptorWrite write; uint64_t record{}, creation{}; };
    struct Revision { uint64_t record{}, source{}; };
    using Slot = std::pair<uint64_t, uint32_t>;
    std::map<uint64_t, Heap> heaps;
    std::map<uint64_t, std::vector<uint64_t>> heapObjects, resourceObjects, roots;
    std::map<uint64_t, D3D12_RESOURCE_DESC> resources;
    std::map<uint64_t, uint64_t> released;
    std::map<Slot, std::vector<Revision>> descriptors;
    void remember(Slot slot, uint64_t record, uint64_t source) {
        auto [it, added] = descriptors.try_emplace(slot);
        charge(sizeof(Revision) * 2 + (added ? 80 : 0));
        it->second.push_back({record, source});
    }
    // Per-state-slot deltas act as sparse checkpoints: a query needs one binary
    // search per bound slot, independent of distance from the frame beginning.
    std::map<size_t, std::map<std::string, std::vector<uint64_t>>> changes;
    size_t bytes{};
    void charge(size_t amount) {
        bytes += amount;
        require(bytes <= 64ULL * 1024 * 1024, "CPU binding index exceeds 64 MiB budget");
    }
    format::Bytes payload(uint64_t id) const { return directory.payload(directory.packets().at(size_t(id - 1))); }
    uint64_t creation(const std::map<uint64_t, std::vector<uint64_t>> &objects, uint64_t object, uint64_t at) const {
        const auto found = objects.find(object);
        require(found != objects.end(), "Object lifetime is not indexed");
        auto id = previous(found->second, at);
        if (!id && at <= directory.initializationCount() && found->second.size() == 1 &&
            found->second.front() <= directory.initializationCount()) id = found->second.front();
        require(id && (!released.contains(id) || released.at(id) > at), "Object lifetime is unavailable at this boundary");
        return id;
    }
    Slot resolve(uint64_t handle, uint64_t at, bool gpu) const {
        std::optional<Slot> result;
        for (const auto &[id, heap] : heaps) {
            const auto start = gpu ? heap.gpu : heap.cpu;
            if (!start || !heap.increment || !handle || handle < start ||
                handle - start >= heap.increment * heap.recipe.description.NumDescriptors || at >= heap.end) continue;
            if (id > at && !(at <= directory.initializationCount() && id <= directory.initializationCount())) continue;
            if (creation(heapObjects, heap.recipe.object, at) != id) continue;
            require((handle - start) % heap.increment == 0 && !result, "Ambiguous descriptor address");
            result = Slot{id, uint32_t((handle - start) / heap.increment)};
        }
        require(result.has_value(), "Descriptor heap address is unavailable");
        return *result;
    }
    uint64_t source(Slot slot, uint64_t at, bool forward = false) const {
        const auto found = descriptors.find(slot);
        require(found != descriptors.end(), "Descriptor has no captured contents");
        const auto &versions = found->second;
        auto end = std::upper_bound(versions.begin(), versions.end(), at,
            [](uint64_t record, const Revision &r) { return record < r.record; });
        if (end == versions.begin()) {
            require(forward && at <= directory.initializationCount() && versions.front().record <= directory.initializationCount(),
                    "Descriptor contents require replay confirmation");
            const auto firstSource = versions.front().source;
            require(firstSource != 0, "Forward descriptor provenance is unresolved");
            const auto first = decodeDescriptorWrite(directory.packets().at(size_t(firstSource - 1)).type, payload(firstSource));
            for (const auto &revision : versions) {
                if (revision.record > directory.initializationCount()) break;
                require(revision.source != 0, "Forward descriptor provenance is unresolved");
                const auto other = decodeDescriptorWrite(directory.packets().at(size_t(revision.source - 1)).type, payload(revision.source));
                require(other.type == first.type && other.resource == first.resource && other.counter == first.counter && other.description.index() == first.description.index(),
                    "Ambiguous keyframe CPU descriptor contents");
                if (first.resource) require(creation(resourceObjects, first.resource, firstSource) ==
                    creation(resourceObjects, other.resource, revision.source), "Keyframe descriptor resource lifetime changed");
                if (first.counter) require(creation(resourceObjects, first.counter, firstSource) ==
                    creation(resourceObjects, other.counter, revision.source), "Keyframe descriptor counter lifetime changed");
                const bool same = std::visit([&](const auto &description) {
                    using T = std::decay_t<decltype(description)>;
                    if constexpr (std::is_same_v<T, std::monostate>) return true;
                    else return memcmp(&description, &std::get<T>(other.description), sizeof(T)) == 0;
                }, first.description);
                require(same, "Ambiguous keyframe CPU descriptor view");
            }
            end = std::next(versions.begin());
        }
        require(std::prev(end)->source != 0, "Descriptor copy provenance is unresolved");
        return std::prev(end)->source;
    }
    std::shared_ptr<const View> view(Slot slot, uint64_t at, bool forward = false) const {
        const auto id = source(slot, at, forward);
        auto entry = std::make_shared<View>(); entry->record = id;
        entry->write = decodeDescriptorWrite(directory.packets().at(size_t(id - 1)).type, payload(id));
        if (entry->write.resource) entry->creation = creation(resourceObjects, entry->write.resource, id);
        return entry;
    }
    Impl(const StreamDirectory &d, const SubmissionIndex &s, const CancelCheck &cancelled)
        : directory(d), submissions(s), addresses(d, cancelled) {
        std::map<std::pair<uint64_t, uint32_t>, uint32_t> increments;
        BackBufferIndex buffers(d, cancelled);
        for (const auto &[id, b] : buffers.buffers()) { resources[id] = b.description; resourceObjects[b.object].push_back(id); charge(160); }
        for (uint64_t id = 1; id <= d.packets().size(); ++id) {
            require(bytes <= 64ULL * 1024 * 1024, "CPU binding index exceeds budget");
            if (id % 1024 == 0) checkCancellation(cancelled);
            const auto type = d.packets()[size_t(id - 1)].type;
            try {
                if (isResourceRecipe(type)) {
                    const auto r = decodeResourceRecipe(type, payload(id));
                    if (SUCCEEDED(r.result) && r.object) { resources[id] = r.description; resourceObjects[r.object].push_back(id); charge(160); }
                } else if (type == 0x127932eb2eb14ULL) {
                    const auto r = decodeDescriptorHeap(payload(id));
                    if (SUCCEEDED(r.result) && r.object) { heaps[id].recipe = r; heapObjects[r.object].push_back(id); charge(200); }
                } else if (type == 0x127932ffcb166ULL) {
                    Reader r(payload(id)); const auto device = r.read<uint64_t>(); const auto kind = r.read<uint32_t>();
                    increments[{device, kind}] = r.read<uint32_t>(); r.end();
                } else if (type == 0x127938051b445ULL) {
                    const auto r = decodeRootSignature(payload(id));
                    if (SUCCEEDED(r.result) && r.object) { roots[r.object].push_back(id); charge(32); }
                }
            } catch (const std::exception &) { throw; }
        }
        for (auto &[object, ids] : resourceObjects) std::sort(ids.begin(), ids.end());
        for (uint64_t id = 1; id <= d.packets().size(); ++id) {
            if (id % 1024 == 0) checkCancellation(cancelled);
            const auto type = d.packets()[size_t(id - 1)].type;
            if (type == 0x15a3eb3b8e877ULL || type == 0x15a3edf3ce2bbULL) {
                Reader r(payload(id)); const auto object = r.read<uint64_t>();
                if (!r.flag()) continue;
                const auto address = r.read<uint64_t>(); r.end();
                try { auto &heap = heaps.at(creation(heapObjects, object, id));
                    (type == 0x15a3eb3b8e877ULL ? heap.cpu : heap.gpu) = address;
                } catch (const std::exception &) {}
            } else if (type == 0x14538e8691d1cULL || type == 0x1e650e8691d1cULL ||
                       type == 0x15a3ee8691d1cULL || type == 0x12949e8691d1cULL) {
                Reader r(payload(id)); const auto object = r.read<uint64_t>();
                if (r.read<uint32_t>() == 0) for (const auto *objects : {&heapObjects, &resourceObjects, &roots}) {
                    const auto found = objects->find(object); if (found == objects->end()) continue;
                    const auto created = previous(found->second, id);
                    if (created) { released[created] = id; if (heaps.contains(created)) heaps[created].end = id; }
                }
            }
        }
        for (auto &[id, heap] : heaps) {
            heap.increment = increments[{heap.recipe.device, uint32_t(heap.recipe.description.Type)}];
            require(heap.increment && heap.recipe.description.NumDescriptors <= UINT64_MAX / heap.increment, "Invalid descriptor stride");
        }
        for (uint64_t id = 1; id <= d.packets().size(); ++id) {
            if (id % 1024 == 0) checkCancellation(cancelled);
            const auto type = d.packets()[size_t(id - 1)].type;
            if (isDescriptorWrite(type)) {
                try {
                    const auto write = decodeDescriptorWrite(type, payload(id));
                    remember(resolve(write.destination, id, false), id, id);
                } catch (const std::exception &) {
                    require(bytes <= 64ULL * 1024 * 1024, "CPU binding index exceeds budget");
                    // An unknown write must replace the previous known contents.
                    const auto write = decodeDescriptorWrite(type, payload(id));
                    try { remember(resolve(write.destination, id, false), id, 0); }
                    catch (const std::exception &) { require(bytes <= 64ULL * 1024 * 1024, "CPU binding index exceeds budget"); }
                }
            } else if (type == 0x12793300ba04eULL || type == 0x1279345e6c880ULL) {
                const auto copy = decodeDescriptorCopy(type, payload(id));
                std::vector<uint64_t> sources;
                const auto expand = [&](const auto &handles, const auto &counts) {
                    std::vector<Slot> result;
                    for (size_t n = 0; n < handles.size(); ++n) {
                        const auto slot = resolve(handles[n], id, false);
                        require(counts[n] <= heaps.at(slot.first).recipe.description.NumDescriptors - slot.second, "Descriptor copy exceeds heap");
                        require(counts[n] <= 1048576 - result.size(), "Descriptor copy exceeds CPU index limit");
                        for (uint32_t i = 0; i < counts[n]; ++i) result.emplace_back(slot.first, slot.second + i);
                    }
                    return result;
                };
                const auto destinations = expand(copy.destinations, copy.destinationSizes);
                try {
                    for (const auto &slot : expand(copy.sources, copy.sourceSizes)) {
                        try { sources.push_back(source(slot, id)); } catch (const std::exception &) { sources.push_back(0); }
                    }
                } catch (const std::exception &) { sources.clear(); }
                for (size_t i = 0; i < destinations.size(); ++i) {
                    remember(destinations[i], id, sources.size() == destinations.size() ? sources[i] : 0);
                }
            }
            if (const auto recording = s.recordingForRecord(id)) {
                std::string key;
                switch (type) {
                case 0x1ae3c6959946eULL: key = "G/root"; break;
                case 0x1ae3c9fea880cULL: key = "C/root"; break;
                case 0x1ae3c4254a275ULL: key = "heaps"; break;
                case 0x1ae3c7884148bULL: key = "OM"; break;
                case 0x1ae3ccbc6c8c9ULL: key = "IB"; break;
                case 0x1ae3ccddf9f8cULL: key = "indirect"; break;
                case 0x1ae3c0857cd39ULL: case 0x1ae3cac95ac55ULL: case 0x1ae3c0089f9bdULL:
                case 0x1ae3cbf4ff4b7ULL: case 0x1ae3c3a460e6bULL: case 0x1ae3cff376c13ULL: {
                    Reader r(payload(id)); r.skip(8);
                    const bool graphics = type == 0x1ae3c0857cd39ULL || type == 0x1ae3cac95ac55ULL || type == 0x1ae3c0089f9bdULL;
                    key = std::string(graphics ? "G/arg/" : "C/arg/") + std::to_string(r.read<uint32_t>()); break;
                }
                case 0x1ae3cc2293674ULL: {
                    Reader r(payload(id)); r.skip(8); const auto first = r.read<uint32_t>(), count = r.read<uint32_t>();
                    require(first <= 32 && count <= 32 - first, "Invalid vertex buffer binding range");
                    for (uint32_t i = first; i < first + count; ++i) { changes[*recording]["VB/" + std::to_string(i)].push_back(id); charge(16); }
                    break;
                }
                default:
                    if ((type >> 32) == 0x1ae3c && streamPacketName(type).starts_with("Unknown")) key = "unknown-state";
                    break;
                }
                if (!key.empty()) { changes[*recording][key].push_back(id); charge(32); }
            }
        }
        require(bytes <= 64ULL * 1024 * 1024, "CPU binding index exceeds budget");
    }
    Json bufferReference(uint64_t address, uint64_t record, uint64_t count, Json &details) const {
        const auto ref = addresses.resolve(address, record, count ? count : 1);
        require(creation(resourceObjects, ref.object, record) == ref.creation, "Buffer address lifetime is no longer active");
        details["byte_offset"] = std::to_string(ref.offset);
        details["byte_size"] = std::to_string(count ? count : ref.available);
        return {{"mapping_status", "capture_resource_marker"}, {"resource_creation", std::to_string(ref.creation)},
                {"resource_object", std::to_string(ref.object)}};
    }
    Json describe(const View &entry, Json &details) const {
        const auto &w = entry.write;
        details = {{"category", category(w.type)}, {"cpu_provenance", "captured_descriptor"},
                   {"descriptor_write", std::to_string(entry.record)}};
        if (const auto cb = std::get_if<D3D12_CONSTANT_BUFFER_VIEW_DESC>(&w.description))
            return bufferReference(cb->BufferLocation, entry.record, cb->SizeInBytes, details);
        if (!w.resource) return {{"mapping_status", "unbound"}};
        require(entry.creation && resources.contains(entry.creation), "Resource creation is unresolved");
        const auto &resource = resources.at(entry.creation);
        auto range = [&](uint32_t mip, uint32_t mips, uint32_t slice, uint32_t slices) {
            details.update({{"first_mip", mip}, {"mip_count", mips}, {"first_slice", slice}, {"slice_count", slices}});
        };
        // Unsupported view layouts remain explicitly pending, never broadened.
        if (std::holds_alternative<std::monostate>(w.description)) {
            require(resource.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER, "Default buffer view requires replay confirmation");
            range(0, w.type == 0x127932c396f41ULL ? (resource.MipLevels ? resource.MipLevels : UINT32_MAX) : 1,
                  0, resource.DepthOrArraySize);
        } else if (const auto r = std::get_if<D3D12_RENDER_TARGET_VIEW_DESC>(&w.description)) {
            if (r->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2D) range(r->Texture2D.MipSlice, 1, 0, 1);
            else if (r->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DARRAY) range(r->Texture2DArray.MipSlice, 1, r->Texture2DArray.FirstArraySlice, r->Texture2DArray.ArraySize);
            else if (r->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DMS) range(0, 1, 0, 1);
            else if (r->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE3D) range(r->Texture3D.MipSlice, 1, r->Texture3D.FirstWSlice, r->Texture3D.WSize);
            else throw std::runtime_error("RTV layout pending replay confirmation");
        } else if (const auto r = std::get_if<D3D12_DEPTH_STENCIL_VIEW_DESC>(&w.description)) {
            if (r->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2D) range(r->Texture2D.MipSlice, 1, 0, 1);
            else if (r->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2DARRAY) range(r->Texture2DArray.MipSlice, 1, r->Texture2DArray.FirstArraySlice, r->Texture2DArray.ArraySize);
            else if (r->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2DMS) range(0, 1, 0, 1);
            else throw std::runtime_error("DSV layout pending replay confirmation");
        } else if (const auto r = std::get_if<D3D12_SHADER_RESOURCE_VIEW_DESC>(&w.description)) {
            if (r->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) range(r->Texture2D.MostDetailedMip, r->Texture2D.MipLevels, 0, 1);
            else if (r->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY) range(r->Texture2DArray.MostDetailedMip, r->Texture2DArray.MipLevels, r->Texture2DArray.FirstArraySlice, r->Texture2DArray.ArraySize);
            else if (r->ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBE) range(r->TextureCube.MostDetailedMip, r->TextureCube.MipLevels, 0, 6);
            else if (r->ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBEARRAY) range(r->TextureCubeArray.MostDetailedMip, r->TextureCubeArray.MipLevels, r->TextureCubeArray.First2DArrayFace, r->TextureCubeArray.NumCubes * 6);
            else if (r->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE3D) range(r->Texture3D.MostDetailedMip, r->Texture3D.MipLevels, 0, std::max(1u, uint32_t(resource.DepthOrArraySize) >> r->Texture3D.MostDetailedMip));
            else if (r->ViewDimension == D3D12_SRV_DIMENSION_BUFFER && r->Buffer.StructureByteStride) {
                details["byte_offset"] = std::to_string(r->Buffer.FirstElement * r->Buffer.StructureByteStride);
                details["byte_size"] = std::to_string(uint64_t(r->Buffer.NumElements) * r->Buffer.StructureByteStride);
            } else throw std::runtime_error("SRV layout pending replay confirmation");
        } else if (const auto r = std::get_if<D3D12_UNORDERED_ACCESS_VIEW_DESC>(&w.description)) {
            if (r->ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2D) range(r->Texture2D.MipSlice, 1, 0, 1);
            else if (r->ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2DARRAY) range(r->Texture2DArray.MipSlice, 1, r->Texture2DArray.FirstArraySlice, r->Texture2DArray.ArraySize);
            else if (r->ViewDimension == D3D12_UAV_DIMENSION_TEXTURE3D) range(r->Texture3D.MipSlice, 1, r->Texture3D.FirstWSlice, r->Texture3D.WSize);
            else if (r->ViewDimension == D3D12_UAV_DIMENSION_BUFFER && r->Buffer.StructureByteStride) {
                details["byte_offset"] = std::to_string(r->Buffer.FirstElement * r->Buffer.StructureByteStride);
                details["byte_size"] = std::to_string(uint64_t(r->Buffer.NumElements) * r->Buffer.StructureByteStride);
            } else throw std::runtime_error("UAV layout pending replay confirmation");
        } else throw std::runtime_error("Descriptor type requires replay confirmation");
        return {{"mapping_status", "capture_resource_marker"}, {"resource_creation", std::to_string(entry.creation)},
                {"resource_object", std::to_string(w.resource)}};
    }
    Json snapshot(uint64_t execution) const;
};
Json CpuBindings::Impl::snapshot(uint64_t execution) const {
    const auto e = submissions.execution(execution);
    const auto workType = directory.packets().at(size_t(e.record - 1)).type;
    require(streamPacketWork(workType),
            "CPU binding snapshots require a Draw, Dispatch or indirect execution");
    const bool compute = workType == 0x1ae3cef0e6c17ULL, indirect = workType == 0x1ae3ccddf9f8cULL;
    const auto recording = submissions.recordingForRecord(e.record);
    require(recording && e.recordingComplete, "CPU bindings require a complete recording generation");
    Json rows = Json::array(), gaps = Json::array();
    const auto found = changes.find(*recording);
    if (found == changes.end()) return {{"pipeline_rows", rows}, {"cpu_snapshot", true}};
    const auto &state = found->second;
    auto last = [&](const std::string &key) {
        const auto it = state.find(key); return it == state.end() ? uint64_t(0) : previous(it->second, e.record);
    };
    const auto unknownState = last("unknown-state"), indirectRecord = last("indirect");
    auto row = [&](const std::string &stage, const std::string &binding, const std::string &kind,
                   uint32_t slot, uint32_t space, auto read) {
        Json details, ref;
        try {
            require(!indirect, "Indirect operation binding scope requires replay confirmation");
            ref = read(details);
            if (ref.value("mapping_status", "") == "capture_resource_marker")
                require(creation(resourceObjects, std::stoull(ref.at("resource_object").get<std::string>()), e.submission) ==
                    std::stoull(ref.at("resource_creation").get<std::string>()), "Bound resource lifetime changed before submission");
        }
        catch (const std::exception &error) { ref = {{"mapping_status", "pending_cpu_binding"}, {"reason", error.what()}}; }
        if (stage != "OM" && stage != "IA") details = {
            {"reflection_binding", {{"register", slot}, {"space", space}}},
            {"descriptor", details.is_null() ? Json::object() : details}};
        if (stage != "OM" && stage != "IA") details["descriptor"]["category"] = kind;
        details["cpu_binding_status"] = ref.value("mapping_status", "") == "capture_resource_marker" ? "captured" : "pending";
        rows.push_back({{"section", stage}, {"binding", binding}, {"resource", ref}, {"details", details}});
    };
    if (const auto id = compute ? 0 : last("OM")) {
        Reader r(payload(id)); r.skip(8); const auto count = r.read<uint32_t>();
        require(count <= 8, "Invalid RTV binding count");
        std::vector<uint64_t> handles;
        if (r.flag()) { const auto size = r.read<uint64_t>(); require(size <= 8, "Invalid RTV handles");
            for (uint64_t i = 0; i < size; ++i) handles.push_back(r.read<uint64_t>()); }
        const auto contiguous = r.read<BOOL>(); const auto depth = r.flag() ? r.read<uint64_t>() : 0; r.end();
        require(handles.size() == (contiguous && count ? 1 : count), "Incomplete RTV handles");
        for (uint32_t i = 0; i < count; ++i) row("OM", "RTV " + std::to_string(i), "RTV", i, 0, [&](Json &details) {
            require(id > unknownState, "Unknown command may have changed output bindings");
            auto slot = resolve(handles[contiguous ? 0 : i], id, false); if (contiguous) slot.second += i;
            require(slot.second < heaps.at(slot.first).recipe.description.NumDescriptors, "RTV range exceeds heap");
            return describe(*view(slot, id, true), details);
        });
        if (depth) row("OM", "DSV", "DSV", 0, 0, [&](Json &details) {
            require(id > unknownState, "Unknown command may have changed depth binding");
            return describe(*view(resolve(depth, id, false), id, true), details);
        });
    }
    if (const auto id = compute ? 0 : last("IB")) row("IA", "Index buffer", "IB", 0, 0, [&](Json &details) {
        require(id > std::max(indirectRecord, unknownState), "Index binding may be GPU-generated; pending replay confirmation");
        Reader r(payload(id)); r.skip(8);
        if (!r.flag()) return Json{{"mapping_status", "unbound"}};
        const auto buffer = r.read<D3D12_INDEX_BUFFER_VIEW>(); r.end();
        return bufferReference(buffer.BufferLocation, id, buffer.SizeInBytes, details);
    });
    for (uint32_t i = 0; !compute && i < 32; ++i) if (const auto id = last("VB/" + std::to_string(i)))
        row("IA", "VB " + std::to_string(i), "VB", i, 0, [&](Json &details) {
            require(id > std::max(indirectRecord, unknownState), "Vertex binding may be GPU-generated; pending replay confirmation");
            Reader r(payload(id)); r.skip(8); const auto start = r.read<uint32_t>(), count = r.read<uint32_t>();
            require(r.flag() && r.read<uint64_t>() == count && i >= start && i - start < count, "Incomplete vertex buffers");
            r.skip(size_t(i - start) * sizeof(D3D12_VERTEX_BUFFER_VIEW)); const auto b = r.read<D3D12_VERTEX_BUFFER_VIEW>();
            details["byte_stride"] = b.StrideInBytes;
            return bufferReference(b.BufferLocation, id, b.SizeInBytes, details);
        });
    std::set<uint64_t> boundHeaps;
    const auto heapRecord = last("heaps");
    if (heapRecord) {
        Reader r(payload(heapRecord)); r.skip(8); const auto count = r.read<uint32_t>();
        require(count <= 2 && r.flag() && r.read<uint64_t>() == count, "Invalid bound heaps");
        for (uint32_t i = 0; i < count; ++i) boundHeaps.insert(r.read<uint64_t>());
        r.end();
    }
    for (const std::string stage : {"G", "C"}) {
        if (!indirect && (stage == "C") != compute) continue;
        const auto rootRecord = last(stage + "/root"); if (!rootRecord) continue;
        try {
            require(rootRecord > unknownState, "Unknown command may have changed root signature");
            Reader r(payload(rootRecord)); r.skip(8); const auto object = r.read<uint64_t>(); r.end();
            if (!object) continue;
            const auto recipe = decodeRootSignature(payload(creation(roots, object, rootRecord)));
            ComPtr<ID3D12VersionedRootSignatureDeserializer> deserializer;
            require(SUCCEEDED(D3D12CreateVersionedRootSignatureDeserializer(recipe.bytecode.data(), recipe.bytecode.size(), IID_PPV_ARGS(&deserializer))), "Root signature cannot be decoded");
            const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *description{};
            require(SUCCEEDED(deserializer->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &description)), "Root signature version is unavailable");
            const auto &root = description->Desc_1_1;
            require(root.NumParameters <= 64, "Invalid root parameter count");
            for (uint32_t p = 0; p < root.NumParameters; ++p) {
                const auto &parameter = root.pParameters[p];
                const auto argRecord = last(stage + "/arg/" + std::to_string(p));
                const char *visibility = "ALL";
                if (stage == "C") visibility = "CS";
                else switch (parameter.ShaderVisibility) {
                    case D3D12_SHADER_VISIBILITY_VERTEX: visibility = "VS"; break;
                    case D3D12_SHADER_VISIBILITY_HULL: visibility = "HS"; break;
                    case D3D12_SHADER_VISIBILITY_DOMAIN: visibility = "DS"; break;
                    case D3D12_SHADER_VISIBILITY_GEOMETRY: visibility = "GS"; break;
                    case D3D12_SHADER_VISIBILITY_PIXEL: visibility = "PS"; break;
                    case D3D12_SHADER_VISIBILITY_AMPLIFICATION: visibility = "AS"; break;
                    case D3D12_SHADER_VISIBILITY_MESH: visibility = "MS"; break;
                    default: break;
                }
                if (parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) continue;
                auto argument = [&] {
                    require(argRecord >= rootRecord && argRecord > indirectRecord, "Root argument is unset or GPU-generated; pending replay confirmation");
                    Reader a(payload(argRecord)); a.skip(8); require(a.read<uint32_t>() == p, "Root parameter index mismatch");
                    const auto value = a.read<uint64_t>(); a.end(); return value;
                };
                if (parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
                    uint64_t offset{};
                    require(parameter.DescriptorTable.NumDescriptorRanges <= 1024, "Root range count exceeds budget");
                    for (uint32_t n = 0; n < parameter.DescriptorTable.NumDescriptorRanges; ++n) {
                        const auto &range = parameter.DescriptorTable.pDescriptorRanges[n];
                        if (range.OffsetInDescriptorsFromTableStart != D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND) offset = range.OffsetInDescriptorsFromTableStart;
                        const auto kind = range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SRV ? "SRV" :
                            range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV ? "UAV" : "CBV";
                        if (range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER) { offset += range.NumDescriptors; continue; }
                        const auto count = std::min(range.NumDescriptors, 1024u);
                        if (count != range.NumDescriptors) gaps.push_back("Unbounded descriptor table is limited to 1024 entries; remaining entries pending");
                        for (uint32_t i = 0; i < count && rows.size() < 4096; ++i) row(visibility,
                            std::string(kind) + " " + std::to_string(range.BaseShaderRegister + i), kind, range.BaseShaderRegister + i, range.RegisterSpace,
                            [&](Json &details) {
                                require(argRecord >= heapRecord, "Descriptor heaps changed after root table binding");
                                require(heapRecord > unknownState, "Unknown command may have changed descriptor heaps");
                                const auto slot = resolve(argument(), e.submission, true);
                                const auto &heap = heaps.at(slot.first);
                                require(creation(heapObjects, heap.recipe.object, heapRecord) == slot.first,
                                    "Bound descriptor heap lifetime changed before execution");
                                require(boundHeaps.contains(heap.recipe.object) && offset + i < heap.recipe.description.NumDescriptors - slot.second,
                                    "Root table is outside bound heap");
                                const auto selected = view({slot.first, slot.second + uint32_t(offset + i)}, e.submission);
                                require(std::string(category(selected->write.type)) == kind, "Descriptor type differs from root range");
                                return describe(*selected, details);
                            });
                        offset += range.NumDescriptors;
                    }
                } else {
                    const auto kind = parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV ? "CBV" :
                        parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_UAV ? "UAV" : "SRV";
                    row(visibility, std::string(kind) + " " + std::to_string(parameter.Descriptor.ShaderRegister), kind,
                        parameter.Descriptor.ShaderRegister, parameter.Descriptor.RegisterSpace, [&](Json &details) {
                            return bufferReference(argument(), argRecord, 0, details);
                        });
                }
            }
        } catch (const std::exception &error) { gaps.push_back(error.what()); }
    }
    return {{"ok", true}, {"backend", "dx12"}, {"execution", std::to_string(execution)},
        {"cpu_snapshot", true}, {"pipeline_rows", rows}, {"pipeline_scope", {{"gaps", gaps},
            {"source", "CPU recording/submission index; potential bindings, GPU observation pending"}}},
        {"index_bytes", bytes}, {"source_record", std::to_string(e.record)}, {"generation", std::to_string(e.generation)}};
}
CpuBindings::CpuBindings(const StreamDirectory &d, const SubmissionIndex &s, const CancelCheck &c) : impl_(std::make_unique<Impl>(d, s, c)) {}
CpuBindings::~CpuBindings() = default;
Json CpuBindings::snapshot(uint64_t execution) const { return impl_->snapshot(execution); }
}
