#include "InitializationCopy.h"
#include "StreamArguments.h"
#include <wrl/client.h>
#include <QCryptographicHash>
#include <QByteArrayView>
#include <chrono>
#include <thread>
#include <sstream>

namespace flora::dx12 {
namespace {
using nlohmann::json;
using Microsoft::WRL::ComPtr;
uint64_t number(const json &value, const char *name) { return std::stoull(value.at(name).get<std::string>()); }
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
void check(HRESULT hr, const char *operation) {
    if (FAILED(hr)) { std::ostringstream text; text << operation << " failed: 0x" << std::hex << uint32_t(hr); throw std::runtime_error(text.str()); }
}
D3D12_RESOURCE_DESC desc(const json &v) {
    require(!v.is_null(), "Missing resource description");
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION(v.at("Dimension").get<UINT>());
    d.Alignment = number(v, "Alignment"); d.Width = number(v, "Width");
    d.Height = v.at("Height"); d.DepthOrArraySize = v.at("DepthOrArraySize"); d.MipLevels = v.at("MipLevels");
    d.Format = DXGI_FORMAT(v.at("Format").get<UINT>()); d.SampleDesc.Count = v.at("SampleCount"); d.SampleDesc.Quality = v.at("SampleQuality");
    d.Layout = D3D12_TEXTURE_LAYOUT(v.at("Layout").get<UINT>()); d.Flags = D3D12_RESOURCE_FLAGS(v.at("Flags").get<UINT>());
    require(d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && d.Width, "Initialization copy check currently requires buffers");
    return d;
}
D3D12_HEAP_PROPERTIES properties(const json &v) {
    D3D12_HEAP_PROPERTIES p{};
    p.Type = D3D12_HEAP_TYPE(v.at("HeapType").get<UINT>());
    p.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY(v.at("CPUPageProperty").get<UINT>());
    p.MemoryPoolPreference = D3D12_MEMORY_POOL(v.at("MemoryPoolPreference").get<UINT>());
    p.CreationNodeMask = v.at("CreationNodeMask"); p.VisibleNodeMask = v.at("VisibleNodeMask"); return p;
}
std::string hash(format::Bytes bytes) {
    return QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size())), QCryptographicHash::Sha256).toHex().toStdString();
}
}
InitializationCopy prepareInitializationCopy(const StreamDirectory &directory, uint64_t record,
    uint64_t memoryLimit, const CancelCheck &cancelled) {
    require(record && record <= directory.initializationCount(), "Copy record must belong to initialization");
    const auto &packets = directory.packets(); const auto &packet = packets[size_t(record - 1)];
    require(packet.type == 0x1ae3cd301bf84ULL, "Selected record is not CopyBufferRegion");
    auto args = decodeArguments(packet.type, directory.payload(packet)).values;
    require(args.at("UndecodedExtensionBytes") == 0, "Copy has unsupported extension data");
    InitializationCopy plan;
    plan.record = record; plan.memoryLimit = memoryLimit; plan.source = number(args, "Source"); plan.destination = number(args, "Destination");
    plan.sourceOffset = number(args, "SourceOffset"); plan.destinationOffset = number(args, "DestinationOffset"); plan.count = number(args, "NumBytes");
    require(plan.count && plan.count <= memoryLimit, "Copy size exceeds the requested memory budget or is zero");
    json source, destination;
    // Keyframe dependency definitions can follow their users. Resolve recipes
    // over the whole keyframe, retaining exact record provenance and ambiguity.
    for (size_t i = 0; i < directory.initializationCount(); ++i) {
        if (i % 1024 == 0) checkCancellation(cancelled);
        const auto &p = packets[i];
        if (p.type != 0x12793f58d8f79ULL && p.type != 0x127935d88ccfaULL) continue;
        auto a = decodeArguments(p.type, directory.payload(p));
        if (!a.createdResource) continue;
        if (*a.createdResource == plan.source) {
            require(!plan.sourceRecord, "Source object has ambiguous initialization lifetimes");
            require(p.type == 0x12793f58d8f79ULL, "Source must be a committed upload buffer");
            plan.sourceRecord = i + 1; source = std::move(a.values);
        } else if (*a.createdResource == plan.destination) {
            require(!plan.destinationRecord, "Destination object has ambiguous initialization lifetimes");
            plan.destinationRecord = i + 1; plan.placed = p.type == 0x127935d88ccfaULL; destination = std::move(a.values);
        }
    }
    require(plan.sourceRecord && plan.destinationRecord, "Copy resource creation recipe is missing");
    require(source.at("UndecodedExtensionBytes") == 0 && destination.at("UndecodedExtensionBytes") == 0, "Resource recipe has unsupported extension data");
    plan.sourceDesc = desc(source.at("Description")); plan.destinationDesc = desc(destination.at("Description"));
    plan.sourceProperties = properties(source); plan.sourceFlags = D3D12_HEAP_FLAGS(source.at("HeapFlags").get<UINT>());
    plan.sourceState = D3D12_RESOURCE_STATES(source.at("InitialResourceState").get<UINT>());
    plan.destinationState = D3D12_RESOURCE_STATES(destination.at("InitialResourceState").get<UINT>());
    require(plan.sourceProperties.Type == D3D12_HEAP_TYPE_UPLOAD && plan.sourceState == D3D12_RESOURCE_STATE_GENERIC_READ,
        "Source requires an unsupported initialization path");
    require(plan.destinationState == D3D12_RESOURCE_STATE_COMMON || plan.destinationState == D3D12_RESOURCE_STATE_COPY_DEST,
        "Destination initial state requires preceding commands");
    require(plan.sourceOffset <= plan.sourceDesc.Width && plan.count <= plan.sourceDesc.Width - plan.sourceOffset &&
        plan.destinationOffset <= plan.destinationDesc.Width && plan.count <= plan.destinationDesc.Width - plan.destinationOffset, "Captured copy exceeds buffer bounds");
    uint64_t allocation = plan.sourceDesc.Width;
    const auto budget = [&](uint64_t bytes) { require(allocation <= memoryLimit && bytes <= memoryLimit - allocation, "Resource restoration exceeds the requested memory budget"); allocation += bytes; };
    budget(plan.count);
    if (plan.placed) {
        const auto heap = number(destination, "Heap"); plan.heapOffset = number(destination, "HeapOffset");
        for (size_t i = 0; i < directory.initializationCount(); ++i) {
            if (i % 1024 == 0) checkCancellation(cancelled);
            const auto &p = packets[i]; if (p.type != 0x1279335a5747bULL) continue;
            format::Reader r(directory.payload(p)); r.skip(8);
            require(r.flag(), "Missing heap description");
            static_assert(sizeof(D3D12_HEAP_DESC) == 48); const auto d = r.read<D3D12_HEAP_DESC>(); r.skip(16);
            require(r.flag() && r.read<uint64_t>() == 1, "Invalid heap output array"); const auto output = r.read<uint64_t>();
            const auto hr = r.read<HRESULT>(); if (FAILED(hr) || output != heap) continue;
            r.end(); require(!plan.heapRecord, "Heap object has ambiguous initialization lifetimes"); plan.heapRecord = i + 1; plan.heapDesc = d;
        }
        require(plan.heapRecord, "Placed resource heap recipe is missing");
        require(plan.heapDesc.Properties.Type == D3D12_HEAP_TYPE_DEFAULT, "Destination heap is not DEFAULT");
        budget(plan.heapDesc.SizeInBytes);
    } else {
        plan.destinationProperties = properties(destination); plan.destinationFlags = D3D12_HEAP_FLAGS(destination.at("HeapFlags").get<UINT>());
        require(plan.destinationProperties.Type == D3D12_HEAP_TYPE_DEFAULT, "Destination heap is not DEFAULT"); budget(plan.destinationDesc.Width);
    }
    // The recovered helper-buffer sequence is Create / Map / CopyOperation /
    // Unmap. Validate every field instead of treating an arbitrary data block as
    // a complete resource snapshot.
    const auto start = size_t(plan.sourceRecord);
    require(start + 2 < directory.initializationCount() && start + 3 < record, "Source upload sequence is incomplete or follows the selected copy");
    require(packets[start].type == 0x1e6503e2ff325ULL && packets[start + 1].type == 0x20000000000000ULL && packets[start + 2].type == 0x1e650cfcd4902ULL,
        "Source does not have the recovered contiguous upload sequence");
    format::Reader map(directory.payload(packets[start]));
    require(map.read<uint64_t>() == plan.source && map.read<UINT>() == 0, "Upload map object/subresource mismatch");
    require(!map.flag(), "Upload map has an unsupported read range"); require(map.flag(), "Upload map has no output pointer"); map.skip(8);
    check(map.read<HRESULT>(), "Captured upload Map"); map.end();
    format::Reader copy(directory.payload(packets[start + 1])); require(copy.read<uint64_t>() == plan.source, "Upload data object mismatch");
    plan.sourceData = directory.resourceData(copy.read<uint64_t>()); copy.end();
    require(plan.sourceData.capturedObject == plan.source && plan.sourceData.size == plan.sourceDesc.Width, "Upload descriptor does not contain the complete source buffer");
    const auto raw = directory.resources().bytes(plan.sourceData.descriptorOffset, 64);
    format::Reader descriptor(raw); descriptor.skip(8); require(descriptor.read<uint64_t>() == 0, "Unsupported upload subresource");
    descriptor.skip(4); require(descriptor.read<uint32_t>() == 0, "Unsupported upload descriptor flags"); descriptor.skip(8);
    require(descriptor.read<uint64_t>() == 0, "Unsupported upload destination offset"); descriptor.skip(16);
    require(descriptor.read<uint64_t>() == 0, "Unsupported upload descriptor extension"); descriptor.end();
    format::Reader unmap(directory.payload(packets[start + 2]));
    require(unmap.read<uint64_t>() == plan.source && unmap.read<UINT>() == 0 && !unmap.flag(), "Unsupported upload unmap range"); unmap.end();
    return plan;
}

json executeInitializationCopy(NativeDevice &native, const InitializationCopy &plan, const CancelCheck &cancelled) {
    checkCancellation(cancelled); auto device = native.device();
    D3D12_RESOURCE_DESC readDesc{}; readDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; readDesc.Width = plan.count;
    readDesc.Height = 1; readDesc.DepthOrArraySize = 1; readDesc.MipLevels = 1; readDesc.SampleDesc.Count = 1; readDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    uint64_t allocated = 0;
    const auto reserve = [&](uint64_t size) {
        require(allocated <= plan.memoryLimit && size <= plan.memoryLimit - allocated, "Device allocation requirements exceed the requested memory budget"); allocated += size;
    };
    reserve(device->GetResourceAllocationInfo(0, 1, &plan.sourceDesc).SizeInBytes);
    reserve(device->GetResourceAllocationInfo(0, 1, &readDesc).SizeInBytes);
    reserve(plan.placed ? plan.heapDesc.SizeInBytes : device->GetResourceAllocationInfo(0, 1, &plan.destinationDesc).SizeInBytes);
    ComPtr<ID3D12Heap> heap; ComPtr<ID3D12Resource> source, destination, readback;
    check(device->CreateCommittedResource(&plan.sourceProperties, plan.sourceFlags, &plan.sourceDesc, plan.sourceState, nullptr, IID_PPV_ARGS(&source)), "Create captured upload buffer");
    if (plan.placed) {
        const auto allocation = device->GetResourceAllocationInfo(0, 1, &plan.destinationDesc);
        require(allocation.SizeInBytes != UINT64_MAX && plan.heapOffset <= plan.heapDesc.SizeInBytes && allocation.SizeInBytes <= plan.heapDesc.SizeInBytes - plan.heapOffset,
            "Captured heap placement is incompatible with this device");
        check(device->CreateHeap(&plan.heapDesc, IID_PPV_ARGS(&heap)), "Create captured heap");
        check(device->CreatePlacedResource(heap.Get(), plan.heapOffset, &plan.destinationDesc, plan.destinationState, nullptr, IID_PPV_ARGS(&destination)), "Create captured placed buffer");
    } else check(device->CreateCommittedResource(&plan.destinationProperties, plan.destinationFlags, &plan.destinationDesc, plan.destinationState, nullptr, IID_PPV_ARGS(&destination)), "Create captured destination buffer");
    D3D12_HEAP_PROPERTIES readProperties{}; readProperties.Type = D3D12_HEAP_TYPE_READBACK;
    check(device->CreateCommittedResource(&readProperties, D3D12_HEAP_FLAG_NONE, &readDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "Create comparison readback buffer");
    void *mapped{}; D3D12_RANGE noRead{0, 0}; check(source->Map(0, &noRead, &mapped), "Map restored upload buffer");
    for (size_t offset = 0; offset < plan.sourceData.bytes.size();) {
        checkCancellation(cancelled);
        const auto bytes = (std::min)(size_t(8 * 1024 * 1024), plan.sourceData.bytes.size() - offset);
        memcpy(static_cast<uint8_t *>(mapped) + offset, plan.sourceData.bytes.data() + offset, bytes); offset += bytes;
    }
    D3D12_RANGE written{0, SIZE_T(plan.sourceData.size)}; source->Unmap(0, &written);
    ComPtr<ID3D12CommandQueue> queue; ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> list; ComPtr<ID3D12Fence> fence;
    D3D12_COMMAND_QUEUE_DESC queueDesc{}; queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "Create initialization validation queue");
    check(device->CreateCommandAllocator(queueDesc.Type, IID_PPV_ARGS(&allocator)), "Create initialization allocator");
    check(device->CreateCommandList(0, queueDesc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "Create initialization list");
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = destination.Get(); barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = plan.destinationState; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    if (plan.destinationState != D3D12_RESOURCE_STATE_COPY_DEST) list->ResourceBarrier(1, &barrier);
    list->CopyBufferRegion(destination.Get(), plan.destinationOffset, source.Get(), plan.sourceOffset, plan.count);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE; list->ResourceBarrier(1, &barrier);
    list->CopyBufferRegion(readback.Get(), 0, destination.Get(), plan.destinationOffset, plan.count);
    check(list->Close(), "Close initialization list"); check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Create initialization completion fence");
    checkCancellation(cancelled); ID3D12CommandList *submitted[] = {list.Get()}; queue->ExecuteCommandLists(1, submitted); check(queue->Signal(fence.Get(), 1), "Signal initialization completion");
    // After submission, retain all allocations until completion. The Worker
    // supplies process-level cancellation for hung/device-lost GPU work.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (fence->GetCompletedValue() < 1) {
        check(device->GetDeviceRemovedReason(), "Initialization GPU execution");
        require(std::chrono::steady_clock::now() < deadline, "Initialization GPU copy timed out; retry in a fresh Worker");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    check(device->GetDeviceRemovedReason(), "Initialization GPU execution"); checkCancellation(cancelled);
    D3D12_RANGE readRange{0, SIZE_T(plan.count)}; check(readback->Map(0, &readRange, &mapped), "Map initialization readback");
    const format::Bytes actual(static_cast<const uint8_t *>(mapped), size_t(plan.count));
    const auto expected = plan.sourceData.bytes.subspan(size_t(plan.sourceOffset), size_t(plan.count));
    const bool equal = std::equal(actual.begin(), actual.end(), expected.begin()); const auto actualHash = hash(actual);
    readback->Unmap(0, &noRead);
    return {{"status", equal ? "copy_verified" : "comparison_failed"}, {"record", std::to_string(plan.record)},
        {"source_creation_record", std::to_string(plan.sourceRecord)}, {"destination_creation_record", std::to_string(plan.destinationRecord)},
        {"heap_creation_record", std::to_string(plan.heapRecord)}, {"source", std::to_string(plan.source)}, {"destination", std::to_string(plan.destination)},
        {"source_offset", std::to_string(plan.sourceOffset)}, {"destination_offset", std::to_string(plan.destinationOffset)}, {"bytes", std::to_string(plan.count)},
        {"resource_data_offset", std::to_string(plan.sourceData.dataOffset)}, {"expected_sha256", hash(expected)}, {"readback_sha256", actualHash},
        {"exact_match", equal}, {"allocation_bytes", std::to_string(allocated)}, {"frame_replayed", false}, {"note", "Isolated initialization CopyBufferRegion with captured resource/heap descriptions; original queue dependencies and full resource history have not been replayed"}};
}
}
