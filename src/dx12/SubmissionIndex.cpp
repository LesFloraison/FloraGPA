#include "SubmissionIndex.h"
#include "StreamArguments.h"
#include <algorithm>
#include <set>
#include <unordered_map>

namespace flora::dx12 {
std::vector<ObservationWait> inspectObservationWaits(const StreamDirectory &directory, uint64_t lastRecord,
                                                     const CancelCheck &cancelled) {
    if (lastRecord > directory.packets().size())
        throw std::runtime_error("Unknown CPU wait inspection boundary");
    std::set<uint64_t> waiting;
    std::vector<ObservationWait> result;
    for (uint64_t i = directory.initializationCount(); i < lastRecord; ++i) {
        if (i % 1024 == 0)
            checkCancellation(cancelled);
        const auto &packet = directory.packets()[size_t(i)];
        const auto thread = directory.files().at(packet.file).thread;
        if (packet.type == 0x27000000000000ULL) {
            format::Reader r(directory.payload(packet));
            const auto handle = r.read<uint64_t>();
            r.end();
            if (!handle)
                throw std::runtime_error("Null captured CPU wait handle");
            waiting.insert(thread);
            result.push_back({i + 1, handle, packet.file});
        } else if (waiting.contains(thread) &&
                   packet.type != 0x11469e993999dULL && // GetCompletedValue: recorded CPU observation.
                   packet.type != 0x114696360f841ULL && // Native fences retain their own references.
                   packet.type != 0x11469e8691d1cULL) {
            throw std::runtime_error("Unresolved captured CPU wait precedes state-changing record " +
                                     std::to_string(i + 1) + " (" + streamPacketName(packet.type) + ")");
        }
    }
    checkCancellation(cancelled);
    return result;
}
SubmissionIndex::SubmissionIndex(const StreamDirectory &directory, const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    std::unordered_map<uint64_t,size_t> current;
    std::unordered_map<uint64_t,uint64_t> generations;
    recordRecording_.resize(directory.packets().size());
    const auto begin = [&](uint64_t list, uint64_t record, bool known) {
        const auto index = recordings_.size();
        recordings_.push_back({list, ++generations[list], record, 0, known, false, {}});
        current[list] = index;
        return index;
    };
    uint64_t record = 0;
    for (const auto &packet : directory.packets()) {
        ++record;
        if (record % 1024 == 0) checkCancellation(cancelled);
        const auto type = packet.type;
        const auto payload = directory.payload(packet);
        if (type == 0x127938b682074ULL) {
            auto args = decodeArguments(type, payload).values;
            if (int32_t(args.at("HRESULT").get<uint32_t>()) >= 0 && args.contains("CommandList"))
                begin(std::stoull(args.at("CommandList").get<std::string>()), record, true);
        } else if (type == 0x125482975df48ULL) {
            auto args = decodeArguments(type, payload).values;
            const auto queue = std::stoull(args.at("Queue").get<std::string>());
            uint64_t ordinal = 0;
            for (const auto &id : args.at("CommandLists")) {
                const auto list = std::stoull(id.get<std::string>());
                SubmittedList instance{record, queue, ordinal++, list, count_ + 1, 0, {}};
                const auto it = current.find(list);
                if (it != current.end() && recordings_[it->second].closed) {
                    instance.recording = it->second;
                    instance.count = recordings_[it->second].records.size();
                    if (instance.count > UINT64_MAX - count_) throw std::runtime_error("DX12 execution index overflow");
                    count_ += instance.count;
                }
                lists_.push_back(instance);
            }
        } else if (type == 0x12548dab1bf2dULL || type == 0x12548db3bfde2ULL) {
            auto args = decodeArguments(type, payload).values;
            if (int32_t(args.at("HRESULT").get<uint32_t>()) >= 0)
                synchronization_.push_back({record, std::stoull(args.at("Queue").get<std::string>()),
                    std::stoull(args.at("Fence").get<std::string>()), std::stoull(args.at("Value").get<std::string>()),
                    type == 0x12548db3bfde2ULL});
        } else {
            const auto interfaceId = (type >> 32) & 0xffff;
            if ((type >> 48) != 1 || (interfaceId != 0xae3c && interfaceId != 0x140 && interfaceId != 0x144)) continue;
            const auto method = uint32_t(type);
            if (method == 0x09e0c150 || method == 0x6360f841 || method == 0xe8691d1c) continue;
            format::Reader reader(payload);
            const auto list = reader.read<uint64_t>();
            if (method == 0x721c351a) {
                auto args = decodeArguments(type, payload).values;
                if (int32_t(args.at("HRESULT").get<uint32_t>()) >= 0) begin(list, record, true);
                continue;
            }
            auto found = current.find(list);
            auto index = found == current.end() ? begin(list, record, false) : found->second;
            if (method == 0xd6acf667) {
                auto args = decodeArguments(type, payload).values;
                if (int32_t(args.at("HRESULT").get<uint32_t>()) >= 0) {
                    recordings_[index].closed = true; recordings_[index].close = record;
                }
                continue;
            }
            // A command after Close cannot change a recording already submitted.
            // Keep an explicitly incomplete generation for malformed/partial data.
            if (recordings_[index].closed) index = begin(list, record, false);
            recordings_[index].records.push_back(record);
            recordRecording_[size_t(record - 1)] = index;
        }
    }
    checkCancellation(cancelled);
}
std::optional<size_t> SubmissionIndex::recordingForRecord(uint64_t record) const {
    if (!record || record > recordRecording_.size()) throw std::runtime_error("Unknown DX12 stream record");
    return recordRecording_[size_t(record - 1)];
}
ExecutionInstance SubmissionIndex::execution(uint64_t id) const {
    if (!id || id > count_) throw std::runtime_error("Unknown DX12 execution instance");
    // Empty/unresolved list invocations deliberately occupy no execution range.
    auto it = std::upper_bound(lists_.begin(), lists_.end(), id,
        [](uint64_t value, const SubmittedList &list) { return value < list.firstExecution; });
    while (it != lists_.begin()) {
        --it;
        if (!it->recording || id < it->firstExecution || id - it->firstExecution >= it->count) continue;
        const auto &recording = recordings_[*it->recording];
        return {id, recording.records[size_t(id - it->firstExecution)], it->submission, it->queue,
                it->list, it->ordinal, recording.generation, recording.hasBeginning && recording.closed};
    }
    throw std::runtime_error("Invalid DX12 execution index");
}
}
