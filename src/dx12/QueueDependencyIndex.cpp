#include "QueueDependencyIndex.h"
#include "StreamArguments.h"
#include <algorithm>
#include <d3d12.h>
#include <deque>
#include <map>
#include <set>

namespace flora::dx12 {
namespace {
using Json = nlohmann::json;
constexpr uint64_t createFence = 0x127930cf4f3a0ULL, submit = 0x125482975df48ULL, signal = 0x12548dab1bf2dULL,
                   wait = 0x12548db3bfde2ULL;
struct Node {
    uint64_t record{}, queue{}, fence{}, value{}, type{};
    std::vector<size_t> successors;
    size_t incoming{};
    bool externalBlock{};
};
struct Fence {
    uint64_t creation{}, initial{};
    bool ambiguous{};
    std::vector<size_t> signals;
};
void require(bool valid, const char *message) {
    if (!valid)
        throw std::runtime_error(message);
}
} // namespace
QueueDependencyIndex::QueueDependencyIndex(const StreamDirectory &directory, uint64_t lastRecord,
                                           const CancelCheck &cancelled) {
    if (!lastRecord)
        lastRecord = directory.packets().size();
    require(lastRecord <= directory.packets().size(), "Unknown queue dependency boundary");
    initializationCount_ = directory.initializationCount();
    std::map<uint64_t, Fence> fences;
    std::vector<Node> nodes;
    std::map<uint64_t, size_t> previous;
    Json invalid = Json::array();
    uint64_t invalidCount{};
    for (uint64_t i = 0; i < lastRecord; ++i) {
        if (i % 1024 == 0)
            checkCancellation(cancelled);
        const auto &packet = directory.packets().at(size_t(i));
        if (packet.type == createFence) {
            try {
                format::Reader r(directory.payload(packet));
                r.skip(8);
                const auto initial = r.read<uint64_t>();
                r.skip(4);
                require(r.read<GUID>() == __uuidof(ID3D12Fence), "Unexpected Fence creation interface");
                require(r.flag() && r.read<uint64_t>() == 1, "Missing Fence creation output");
                const auto object = r.read<uint64_t>();
                require(object && r.read<int32_t>() >= 0, "Invalid captured Fence creation");
                r.end();
                require(fences.size() < 1000000, "Queue dependency index exceeds one million Fence objects");
                auto [it, inserted] =
                    fences.try_emplace(object, Fence{i + 1, initial, initial == UINT64_MAX});
                if (!inserted)
                    it->second.ambiguous = true;
            } catch (const std::exception &e) {
                ++invalidCount;
                if (invalid.size() < 32)
                    invalid.push_back({{"record", std::to_string(i + 1)}, {"reason", e.what()}});
            }
        } else if (packet.type == submit || packet.type == signal || packet.type == wait) {
            require(nodes.size() < 1000000, "Queue dependency index exceeds one million queue operations");
            const auto decoded = decodeArguments(packet.type, directory.payload(packet));
            const auto &args = decoded.values;
            if (packet.type != submit) {
                require(decoded.supported && decoded.consumed == packet.size,
                        "Malformed queue synchronization");
                require(int32_t(args.at("HRESULT").get<uint32_t>()) >= 0,
                        "Captured queue synchronization failed");
            }
            const auto queue = std::stoull(args.at("Queue").get<std::string>());
            require(queue, "Null captured queue identity");
            Node node;
            node.record = i + 1;
            node.queue = queue;
            node.type = packet.type;
            if (packet.type != submit) {
                node.fence = std::stoull(args.at("Fence").get<std::string>());
                node.value = std::stoull(args.at("Value").get<std::string>());
            }
            const auto n = nodes.size();
            nodes.push_back(std::move(node));
            if (const auto prior = previous.find(queue); prior != previous.end()) {
                nodes[prior->second].successors.push_back(n);
                ++nodes[n].incoming;
            }
            previous[queue] = n;
        }
    }
    // Collect producers after all recipes: metadata can contain unique forward
    // references. Multiple lifetimes or writers need additional ordering proof.
    for (size_t n = 0; n < nodes.size(); ++n) {
        if (n % 1024 == 0)
            checkCancellation(cancelled);
        if (nodes[n].type == signal && fences.contains(nodes[n].fence))
            fences.at(nodes[n].fence).signals.push_back(n);
    }
    size_t visited{};
    for (auto &[object, fence] : fences) {
        if (++visited % 1024 == 0)
            checkCancellation(cancelled);
        uint64_t value = fence.initial, writer{};
        for (const auto n : fence.signals) {
            if (++visited % 1024 == 0)
                checkCancellation(cancelled);
            const auto &node = nodes[n];
            if ((writer && writer != node.queue) || node.value < value || node.value == UINT64_MAX ||
                (fence.creation > node.record && fence.creation > directory.initializationCount()))
                fence.ambiguous = true;
            value = node.value;
            writer = node.queue;
        }
    }
    for (auto &node : nodes)
        if (node.type == signal) {
            const auto found = fences.find(node.fence);
            node.externalBlock = found == fences.end() || found->second.ambiguous;
        }
    Json waits = Json::array();
    std::vector<size_t> waitNodes;
    size_t initialCount{}, priorCount{}, futureCount{};
    for (size_t n = 0; n < nodes.size(); ++n) {
        if (n % 1024 == 0)
            checkCancellation(cancelled);
        auto &node = nodes[n];
        if (node.type != wait)
            continue;
        Json row{{"record", std::to_string(node.record)},
                 {"queue", std::to_string(node.queue)},
                 {"fence", std::to_string(node.fence)},
                 {"value", std::to_string(node.value)},
                 {"phase", node.record <= directory.initializationCount() ? "initialization" : "frame"},
                 {"condition", "unresolved"}};
        const auto found = fences.find(node.fence);
        if (found == fences.end()) {
            row["reason"] = "Missing valid Fence creation";
            node.externalBlock = true;
        } else {
            const auto &fence = found->second;
            row["fence_creation"] = std::to_string(fence.creation);
            row["initial_value"] = std::to_string(fence.initial);
            if (fence.ambiguous) {
                row["reason"] = "Multiple Fence lifetimes/writers, rewinding or reserved values require "
                                "additional ordering proof";
                node.externalBlock = true;
            } else if (fence.creation > node.record && fence.creation > directory.initializationCount()) {
                row["reason"] = "Queue wait precedes frame Fence creation";
                node.externalBlock = true;
            } else if (fence.initial >= node.value) {
                row["condition"] = "initial_value";
                ++initialCount;
            } else {
                const auto producer = std::lower_bound(
                    fence.signals.begin(), fence.signals.end(), node.value,
                    [&](size_t entry, uint64_t value) { return nodes[entry].value < value; });
                if (producer == fence.signals.end()) {
                    row["reason"] = "No sufficient Signal exists within the inspected prefix";
                    node.externalBlock = true;
                } else {
                    const auto &source = nodes[*producer];
                    row["condition"] = "queue_signal";
                    row["producer_record"] = std::to_string(source.record);
                    row["producer_queue"] = std::to_string(source.queue);
                    row["producer_value"] = std::to_string(source.value);
                    row["producer_relation"] =
                        source.record < node.record ? "earlier_cpu_record" : "later_cpu_record";
                    if (source.record < node.record)
                        ++priorCount;
                    else
                        ++futureCount;
                    nodes[*producer].successors.push_back(n);
                    ++node.incoming;
                }
            }
        }
        waits.push_back(std::move(row));
        waitNodes.push_back(n);
    }
    std::vector<uint64_t> completionRecords(nodes.size());
    const auto visit = [&](bool honorMissing) {
        std::vector<size_t> incoming;
        incoming.reserve(nodes.size());
        std::vector<bool> reached(nodes.size());
        std::deque<size_t> ready;
        for (size_t n = 0; n < nodes.size(); ++n) {
            if (n % 1024 == 0)
                checkCancellation(cancelled);
            incoming.push_back(nodes[n].incoming);
            if (!incoming.back() && (!honorMissing || !nodes[n].externalBlock))
                ready.push_back(n);
        }
        size_t count{};
        while (!ready.empty()) {
            if (++count % 1024 == 0)
                checkCancellation(cancelled);
            const auto n = ready.front();
            ready.pop_front();
            reached[n] = true;
            if (honorMissing)
                completionRecords[n] = (std::max)(completionRecords[n], nodes[n].record);
            for (const auto next : nodes[n].successors) {
                if (honorMissing)
                    completionRecords[next] = (std::max)(completionRecords[next], completionRecords[n]);
                if (!--incoming[next] && (!honorMissing || !nodes[next].externalBlock))
                    ready.push_back(next);
            }
        }
        return reached;
    };
    const auto acyclic = visit(false), resolved = visit(true);
    for (const auto &[object, fence] : fences) {
        FenceSources sources{fence.creation, fence.initial, fence.ambiguous};
        for (const auto n : fence.signals)
            sources.producers.push_back(
                {nodes[n].value, nodes[n].record, nodes[n].queue, completionRecords[n], resolved[n]});
        fenceSources_.emplace(object, std::move(sources));
    }
    size_t blocked{}, cycles{};
    for (size_t i = 0; i < waitNodes.size(); ++i) {
        const auto n = waitNodes[i];
        waits[i]["dependency_status"] = !acyclic[n]   ? "cycle_or_downstream"
                                        : resolved[n] ? "resolved"
                                                      : "unresolved_or_upstream";
        if (!resolved[n])
            ++blocked;
        if (!acyclic[n])
            ++cycles;
    }
    Json blockedOperations = Json::array();
    for (size_t n = 0; n < nodes.size() && blockedOperations.size() < 32; ++n)
        if (!resolved[n])
            blockedOperations.push_back({{"record", std::to_string(nodes[n].record)},
                                         {"queue", std::to_string(nodes[n].queue)},
                                         {"external_block", nodes[n].externalBlock},
                                         {"cycle_or_downstream", !acyclic[n]}});
    checkCancellation(cancelled);
    report_ = {
        {"scope", "captured_queue_order_and_single_writer_monotonic_fences"},
        {"last_record", std::to_string(lastRecord)},
        {"queue_operations", std::to_string(nodes.size())},
        {"queues", std::to_string(previous.size())},
        {"waits", std::move(waits)},
        {"initial_value_waits", std::to_string(initialCount)},
        {"prior_signal_waits", std::to_string(priorCount)},
        {"later_signal_waits", std::to_string(futureCount)},
        {"unresolved_waits", std::to_string(blocked)},
        {"unresolved_queue_operations", std::to_string(std::count(resolved.begin(), resolved.end(), false))},
        {"first_unresolved_operations", std::move(blockedOperations)},
        {"cyclic_or_downstream_waits", std::to_string(cycles)},
        {"invalid_fence_creations", std::move(invalid)},
        {"invalid_fence_creation_count", std::to_string(invalidCount)},
        {"limitations",
         "This is a dependency index, not an executed schedule. CPU event/timeout semantics, "
         "object aliases, memory hazards and complete command-list coverage are not certified. "
         "An initial-value wait does not order work on another queue."}};
}
Json QueueDependencyIndex::observationDependency(uint64_t fence, uint64_t value, uint64_t record) const {
    Json out{{"record", std::to_string(record)},
             {"fence", std::to_string(fence)},
             {"observed_value", std::to_string(value)},
             {"status", "unresolved"}};
    const auto found = fenceSources_.find(fence);
    if (record <= initializationCount_ || record > std::stoull(report_.at("last_record").get<std::string>()))
        out["reason"] = "Completion observation is outside the inspected frame prefix";
    else if (found == fenceSources_.end() || found->second.ambiguous || value == UINT64_MAX)
        out["reason"] = "Completion observation has a missing, ambiguous or reserved-value Fence";
    else {
        const auto &source = found->second;
        if (source.creation > record)
            out["reason"] = "Completion observation precedes Fence creation";
        else if (source.initial >= value)
            out.update({{"status", "resolved"},
                        {"source", "initial_value"},
                        {"required_record", std::to_string(source.creation)}});
        else {
            const auto producer = std::lower_bound(
                source.producers.begin(), source.producers.end(), value,
                [](const CompletionSource &entry, uint64_t value) { return entry.value < value; });
            if (producer == source.producers.end())
                out["reason"] = "No sufficient Signal for the observed value";
            else {
                out.update({{"source", "queue_signal"},
                            {"producer_record", std::to_string(producer->record)},
                            {"producer_queue", std::to_string(producer->queue)},
                            {"required_record", std::to_string(producer->requiredRecord)}});
                if (!producer->resolved)
                    out["reason"] = "Observed producer has unresolved dependencies";
                else if (producer->requiredRecord > record)
                    out["reason"] =
                        "Observed value depends on GPU work not yet submitted at this CPU boundary";
                else
                    out["status"] = "resolved";
            }
        }
    }
    return out;
}
} // namespace flora::dx12
