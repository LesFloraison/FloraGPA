#include "QueryReplayIndex.h"
#include "NativeResources.h"
#include "QueueDependencyIndex.h"
#include <d3d12.h>
#include <set>

namespace flora::dx12 {
namespace {
using Json = nlohmann::json;
using format::Reader;
constexpr uint64_t create = 0x127937e9603edULL, begin = 0x1ae3c6601dd2cULL,
                   end = 0x1ae3c64a8e138ULL, resolve = 0x1ae3cdc411c2dULL;
void require(bool valid, const char *message) { if (!valid) throw std::runtime_error(message); }
struct Heap {
    struct Completion { uint32_t type{}; uint64_t execution{}, submission{}, queue{}; };
    uint64_t creation{};
    D3D12_QUERY_HEAP_DESC desc{};
    std::map<uint32_t, uint32_t> active;
    std::map<uint32_t, Completion> completed;
    std::map<std::pair<uint32_t, uint32_t>, std::set<uint64_t>> recordedEnds;
};
// Resource recipes are indexed without creating a device or reading resource
// contents. Initialization may enumerate a unique heap after its placed buffer.
class Destinations {
    const StreamDirectory &directory_;
    std::map<uint64_t, std::vector<uint64_t>> resources_, heaps_;
    uint64_t at(const std::map<uint64_t, std::vector<uint64_t>> &index, uint64_t object, uint64_t record) const {
        const auto found = index.find(object);
        require(found != index.end(), "Query destination resource or heap creation is missing");
        uint64_t selected{};
        for (const auto creation : found->second) if (creation <= record) selected = creation;
        if (!selected && record <= directory_.initializationCount())
            for (const auto creation : found->second) if (creation <= directory_.initializationCount()) {
                require(!selected, "Ambiguous initialization query destination lifetime"); selected = creation;
            }
        require(selected, "Query destination precedes its resource or heap creation");
        return selected;
    }
  public:
    Destinations(const StreamDirectory &directory, const CancelCheck &cancelled) : directory_(directory) {
        for (size_t i = 0; i < directory.packets().size(); ++i) {
            if (i % 1024 == 0) checkCancellation(cancelled);
            const auto &p = directory.packets()[i];
            try {
                if (isResourceRecipe(p.type)) {
                    const auto recipe = decodeResourceRecipe(p.type, directory.payload(p));
                    if (SUCCEEDED(recipe.result) && recipe.object) resources_[recipe.object].push_back(i + 1);
                } else if (p.type == 0x1279335a5747bULL) {
                    const auto recipe = decodeHeapRecipe(directory.payload(p));
                    if (SUCCEEDED(recipe.result) && recipe.object) heaps_[recipe.object].push_back(i + 1);
                }
            } catch (const std::exception &) {} // Report only when a resolve needs this invalid recipe.
        }
    }
    Json describe(uint64_t object, uint64_t record) const {
        const auto creation = at(resources_, object, record);
        const auto &p = directory_.packets().at(size_t(creation - 1));
        const auto recipe = decodeResourceRecipe(p.type, directory_.payload(p));
        Json out{{"creation", std::to_string(creation)}, {"dimension", recipe.description.Dimension},
            {"width", std::to_string(recipe.description.Width)}, {"heap_type", nullptr},
            {"scope", "Captured allocation metadata; does not prove absence of GPU consumers or recover query values"}};
        if (recipe.kind == ResourceRecipe::Kind::Placed) {
            const auto heapCreation = at(heaps_, recipe.heap, creation);
            const auto heap = decodeHeapRecipe(directory_.payload(directory_.packets().at(size_t(heapCreation - 1))));
            out.update({{"heap", std::to_string(recipe.heap)}, {"heap_creation", std::to_string(heapCreation)},
                {"heap_offset", std::to_string(recipe.heapOffset)}, {"heap_size", std::to_string(heap.description.SizeInBytes)},
                {"heap_type", heap.description.Properties.Type}});
        } else if (recipe.kind != ResourceRecipe::Kind::Reserved) out["heap_type"] = recipe.properties.Type;
        return out;
    }
};
}
uint32_t queryResultStride(uint32_t type) {
    switch (type) {
    case D3D12_QUERY_TYPE_OCCLUSION: case D3D12_QUERY_TYPE_BINARY_OCCLUSION: case D3D12_QUERY_TYPE_TIMESTAMP: return 8;
    case D3D12_QUERY_TYPE_PIPELINE_STATISTICS: return sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
    case D3D12_QUERY_TYPE_PIPELINE_STATISTICS1: return sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS1);
    case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0: case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM1:
    case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM2: case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3:
        return sizeof(D3D12_QUERY_DATA_SO_STATISTICS);
    default: throw std::runtime_error("Unsupported query result layout");
    }
}
void validateQueryDestination(uint32_t type, uint32_t count, uint64_t offset, uint64_t width) {
    const auto size = uint64_t(count) * queryResultStride(type);
    require(offset % 8 == 0 && offset <= width && size <= width - offset, "Query result exceeds its buffer or has an unaligned offset");
}
QueryReplayIndex::QueryReplayIndex(const StreamDirectory &directory, const SubmissionIndex &index,
                                 const CancelCheck &cancelled) {
    std::map<uint64_t, std::vector<Heap>> heaps;
    const Destinations destinations(directory, cancelled);
    std::optional<QueueDependencyIndex> dependencies;
    std::map<std::pair<uint64_t, uint64_t>, bool> orderedSubmissions;
    const auto ordered = [&](const Heap::Completion &producer, const SubmittedList &consumer) {
        if (producer.queue == consumer.queue) return true;
        const auto key = std::pair{producer.submission, consumer.submission};
        if (const auto found = orderedSubmissions.find(key); found != orderedSubmissions.end()) return found->second;
        if (!dependencies) dependencies.emplace(directory, 0, cancelled);
        const bool result = dependencies->precedes(key.first, key.second, cancelled);
        if (orderedSubmissions.size() >= 4096) orderedSubmissions.clear();
        orderedSubmissions.emplace(key, result);
        return result;
    };
    const auto heapAt = [&](uint64_t object, uint64_t record) -> Heap & {
        const auto found = heaps.find(object);
        require(found != heaps.end(), "Query heap creation is missing or invalid");
        Heap *selected{};
        for (auto &heap : found->second) if (heap.creation <= record) selected = &heap;
        if (!selected && record <= directory.initializationCount())
            for (auto &heap : found->second) if (heap.creation <= directory.initializationCount()) {
                require(!selected, "Ambiguous initialization query heap lifetime"); selected = &heap;
            }
        require(selected, "Query precedes its heap creation"); return *selected;
    };
    for (size_t i = 0; i < directory.packets().size(); ++i) {
        if (i % 1024 == 0) checkCancellation(cancelled);
        const auto &packet = directory.packets()[i];
        if (packet.type != create) continue;
        // Malformed creations remain inspectable. Any executed query using
        // one receives an explicit invalid-data issue below.
        try {
            Reader r(directory.payload(packet)); r.skip(8);
            require(r.flag(), "Missing query heap description");
            Heap heap; heap.creation = i + 1; heap.desc = r.read<D3D12_QUERY_HEAP_DESC>();
            require(r.read<GUID>() == __uuidof(ID3D12QueryHeap), "Unsupported query heap interface");
            uint64_t object{};
            if (r.flag()) { require(r.read<uint64_t>() == 1, "Invalid query heap output"); object = r.read<uint64_t>(); }
            const auto hr = r.read<HRESULT>(); r.end();
            if (SUCCEEDED(hr) && object) heaps[object].push_back(std::move(heap));
        } catch (const std::exception &) {}
    }
    // Recording a query is not executing it. Preserve this distinction in
    // missing-result evidence, including historical/unsubmitted recordings.
    for (size_t i = 0; i < directory.packets().size(); ++i) {
        if (i % 1024 == 0) checkCancellation(cancelled);
        const auto &packet = directory.packets()[i]; if (packet.type != end) continue;
        try {
            Reader r(directory.payload(packet)); r.skip(8); const auto object = r.read<uint64_t>();
            const auto type = r.read<uint32_t>(), slot = r.read<uint32_t>(); r.end();
            heapAt(object, i + 1).recordedEnds[{type, slot}].insert(i + 1);
        } catch (const std::exception &) {}
    }
    uint64_t visited{};
    for (const auto &list : index.lists()) {
        checkCancellation(cancelled);
        if (!list.recording) continue; // The native planner rejects unresolved lists independently.
        const auto &recording = index.recordings().at(*list.recording);
        for (size_t n = 0; n < recording.records.size(); ++n) {
            if (++visited % 1024 == 0) checkCancellation(cancelled);
            require(visited <= 4000000, "Query coverage exceeds four million submitted commands");
            const auto record = recording.records[n], execution = list.firstExecution + n;
            const auto &packet = directory.packets().at(size_t(record - 1));
            if (packet.type != begin && packet.type != end && packet.type != resolve) continue;
            Json entry{{"execution", std::to_string(execution)}, {"source_record", std::to_string(record)},
                {"submission", std::to_string(list.submission)}, {"queue", std::to_string(list.queue)},
                {"recording_generation", std::to_string(recording.generation)}, {"status", "covered"}};
            try {
                Reader r(directory.payload(packet)); r.skip(8); const auto object = r.read<uint64_t>();
                const auto type = r.read<uint32_t>(), start = r.read<uint32_t>();
                auto &heap = heapAt(object, record);
                entry.update({{"query_heap", std::to_string(object)}, {"heap_creation", std::to_string(heap.creation)},
                    {"query_type", type}, {"start", start}});
                require(recording.hasBeginning && recording.closed, "Query is in an incomplete command-list recording");
                if (packet.type != resolve) {
                    r.end(); require(start < heap.desc.Count, "Query index exceeds heap bounds");
                    if (packet.type == begin) {
                        require(type != D3D12_QUERY_TYPE_TIMESTAMP && !heap.active.contains(start), "Invalid query begin boundary");
                        heap.active[start] = type; heap.completed.erase(start);
                    } else {
                        require(type == D3D12_QUERY_TYPE_TIMESTAMP || (heap.active.contains(start) && heap.active.at(start) == type),
                                "Query end has no matching replayed begin");
                        heap.active.erase(start); heap.completed[start] = {type, execution, list.submission, list.queue};
                    }
                    continue;
                }
                const auto count = r.read<uint32_t>();
                const auto destination = r.read<uint64_t>(), offset = r.read<uint64_t>(); r.end();
                require(start <= heap.desc.Count && count <= heap.desc.Count - start, "Query resolve exceeds heap bounds");
                require(count <= 1000000, "Query coverage range exceeds one million slots");
                entry.update({{"count", count}, {"destination", std::to_string(destination)}, {"destination_offset", std::to_string(offset)}});
                Json ranges = Json::array(), examples = Json::array(), unorderedExamples = Json::array();
                uint32_t missing{}, recordedMissing{}, rangeStart{}, rangeCount{}, unorderedCount{}, crossQueueCount{};
                const auto flush = [&] {
                    if (rangeCount) ranges.push_back({{"start", rangeStart}, {"count", rangeCount}});
                    rangeCount = 0;
                };
                for (uint32_t j = 0; j < count; ++j) {
                    if (j % 1024 == 0) checkCancellation(cancelled);
                    const auto slot = start + j;
                    if (!heap.completed.contains(slot) || heap.completed.at(slot).type != type) {
                        if (!rangeCount) rangeStart = slot;
                        ++rangeCount; ++missing;
                        if (const auto ends = heap.recordedEnds.find({type, slot}); ends != heap.recordedEnds.end()) {
                            ++recordedMissing;
                            if (examples.size() < 32) {
                                Json sources = Json::array();
                                for (auto source : ends->second) {
                                    if (sources.size() == 8) break;
                                    sources.push_back(std::to_string(source));
                                }
                                examples.push_back({{"index", slot}, {"end_record_count", ends->second.size()}, {"first_end_records", std::move(sources)}});
                            }
                        }
                    } else {
                        flush();
                        const auto &producer = heap.completed.at(slot);
                        if (producer.queue != list.queue) ++crossQueueCount;
                        if (!ordered(producer, list)) {
                            ++unorderedCount;
                            if (unorderedExamples.size() < 32)
                                unorderedExamples.push_back({{"index", slot}, {"end_execution", std::to_string(producer.execution)},
                                    {"producer_submission", std::to_string(producer.submission)}, {"producer_queue", std::to_string(producer.queue)}});
                        }
                    }
                }
                flush();
                entry.update({{"completed_count", count - missing}, {"missing_count", missing}, {"missing_ranges", std::move(ranges)},
                    {"missing_slots_with_recorded_end", recordedMissing}, {"missing_slots_without_recorded_end", missing - recordedMissing},
                    {"missing_with_recorded_end_examples", std::move(examples)},
                    {"cross_queue_completed_count", crossQueueCount}, {"unordered_completed_count", unorderedCount},
                    {"unordered_completed_examples", std::move(unorderedExamples)},
                    {"completion_order_verified", !missing && !unorderedCount}});
                if (missing) {
                    entry["status"] = "missing_query_results";
                    entry["reason"] = "ResolveQueryData execution " + std::to_string(execution) + " (record " + std::to_string(record) +
                        ") lacks " + std::to_string(missing) + " of " + std::to_string(count) + " completed query results from heap " + std::to_string(object);
                }
                if (unorderedCount) {
                    entry["status"] = "unresolved_query_dependency";
                    entry["reason"] = "ResolveQueryData execution " + std::to_string(execution) + " has " +
                        std::to_string(unorderedCount) + " recorded query completions on another queue without a proven Fence dependency";
                }
                entry["destination_resource"] = destinations.describe(destination, record);
                const auto &resource = entry.at("destination_resource");
                require(resource.at("dimension") == D3D12_RESOURCE_DIMENSION_BUFFER, "Query destination must be a buffer");
                validateQueryDestination(type, count, offset, std::stoull(resource.at("width").get<std::string>()));
                entry["result_stride"] = queryResultStride(type);
                entry["result_bytes"] = std::to_string(uint64_t(count) * queryResultStride(type));
            } catch (const OperationCancelled &) { throw; }
            catch (const std::exception &e) { entry["status"] = "invalid_query_data"; entry["reason"] = e.what(); }
            entries_.emplace(execution, std::move(entry));
        }
    }
}
nlohmann::json QueryReplayIndex::details(uint64_t execution) const {
    const auto it = entries_.find(execution); return it == entries_.end() ? Json() : it->second;
}
nlohmann::json QueryReplayIndex::firstIssue(uint64_t execution, bool before) const {
    for (const auto &[id, entry] : entries_) {
        if (id > execution || (before && id == execution)) break;
        if (entry.at("status") != "covered") return entry;
    }
    return nullptr;
}
nlohmann::json QueryReplayIndex::information() const {
    Json entries = Json::array(); for (const auto &[id, entry] : entries_) entries.push_back(entry);
    return {{"scope", "serialized_submission_order"}, {"historical_values_reconstructed", false},
        {"completion_order_scope", "Each observed EndQuery must precede its resolve on the same queue or through captured single-writer monotonic Fence dependencies. This does not certify query-slot reuse, concurrent writers, CPU waits or all resource hazards."},
        {"cross_queue_dependencies_verified", false}, {"resolves_and_issues", std::move(entries)}};
}
}
