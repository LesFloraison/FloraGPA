#include "AddressIndex.h"
#include "NativeResources.h"
#include <algorithm>
#include <optional>

namespace flora::dx12 {
uint64_t AddressIndex::creation(uint64_t object, uint64_t at) const {
    const auto it = creations_.find(object); if (it == creations_.end()) throw std::runtime_error("GPU address resource recipe is missing");
    uint64_t result{}; for (const auto record : it->second) if (record <= at) result = record;
    if (!result && at <= directory_.initializationCount())
        for (const auto record : it->second) if (record <= directory_.initializationCount()) {
            if (result) throw std::runtime_error("GPU address has ambiguous forward resource lifetimes"); result = record;
        }
    if (!result) throw std::runtime_error("GPU address resource is not defined at this boundary"); return result;
}
AddressIndex::AddressIndex(const StreamDirectory &directory, const CancelCheck &cancelled) : directory_(directory) {
    for (size_t i = 0; i < directory.packets().size(); ++i) {
        if (i % 1024 == 0) checkCancellation(cancelled); const auto &p = directory.packets()[i];
        if (!isResourceRecipe(p.type)) continue; const auto r = decodeResourceRecipe(p.type, directory.payload(p));
        if (SUCCEEDED(r.result) && r.object) creations_[r.object].push_back(i + 1);
    }
    for (size_t i = 0; i < directory.packets().size(); ++i) {
        if (i % 1024 == 0) checkCancellation(cancelled); const auto &p = directory.packets()[i];
        if (p.type != 0x1e6508ef58ff0ULL) continue;
        format::Reader r(directory.payload(p)); const auto object = r.read<uint64_t>(), base = r.read<uint64_t>(); r.end();
        if (!base) continue; const auto id = creation(object, i + 1); const auto &packet = directory.packets()[size_t(id - 1)];
        const auto recipe = decodeResourceRecipe(packet.type, directory.payload(packet));
        if (recipe.description.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || !recipe.description.Width || recipe.description.Width > UINT64_MAX - base)
            throw std::runtime_error("Invalid captured GPU buffer address range");
        ranges_.push_back({base, base + recipe.description.Width, i + 1, object, id, 0});
    }
    std::sort(ranges_.begin(), ranges_.end(), [](const Range &a, const Range &b) { return a.base < b.base; });
    uint64_t end{}; for (auto &range : ranges_) { end = (std::max)(end, range.end); range.prefixEnd = end; }
}
AddressReference AddressIndex::resolve(uint64_t address, uint64_t at, uint64_t bytes) const {
    const auto references = candidates(address, at, bytes);
    if (references.size() != 1)
        throw std::runtime_error("Captured GPU address overlaps multiple resource lifetimes; alias selection is required");
    return references.front();
}
std::vector<AddressReference> AddressIndex::candidates(uint64_t address, uint64_t at, uint64_t bytes) const {
    if (!address || !bytes || !at || at > directory_.packets().size()) throw std::runtime_error("Invalid GPU address resolution request");
    auto it = std::upper_bound(ranges_.begin(), ranges_.end(), address, [](uint64_t value, const Range &r) { return value < r.base; });
    std::vector<AddressReference> result;
    while (it != ranges_.begin()) {
        --it; if (it->prefixEnd <= address) break;
        if (address < it->base || address >= it->end || bytes > it->end - address) continue;
        if (it->query > at && !(at <= directory_.initializationCount() && it->query <= directory_.initializationCount())) continue;
        if (creation(it->object, at) != it->creation) continue;
        const AddressReference reference{it->object, it->creation, address - it->base, it->end - address};
        if (std::none_of(result.begin(), result.end(), [&](const auto &existing) {
                return existing.creation == reference.creation && existing.offset == reference.offset;
            }))
            result.push_back(reference);
    }
    if (result.empty()) throw std::runtime_error("Captured GPU address is not mapped at record " + std::to_string(at) + ": " + std::to_string(address));
    return result;
}
}
