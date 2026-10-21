#include "NativeReplay.h"
#include "StreamArguments.h"
#include "TileRecipes.h"
#include "NativeDescriptors.h"
#include "NativeCommands.h"
#include "NativePerformance.h"
#include "NativeTimeline.h"
#include "QueryReplayIndex.h"
#include "QueueDependencyIndex.h"
#include "application/RdcEvents.h"
#include "../common/PackedColor.hpp"
#include <wrl/client.h>
#include <dxgi1_4.h>
#include <QCryptographicHash>
#include <QByteArrayView>
#include <chrono>
#include <algorithm>
#include <set>
#include <sstream>
#include <thread>
#include <functional>
#include <QSaveFile>
#include <QImage>
#include <QScopeGuard>

namespace flora::dx12 {
namespace {
using format::Reader;
using Microsoft::WRL::ComPtr;
using nlohmann::json;
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
void check(HRESULT hr, const char *operation) {
    if (FAILED(hr)) { std::ostringstream s; s << operation << " failed: 0x" << std::hex << uint32_t(hr); throw std::runtime_error(s.str()); }
}
uint64_t value(const json &j, const char *key) { return std::stoull(j.at(key).get<std::string>()); }
uint64_t output(Reader &r, REFIID expected) {
    require(r.read<GUID>() == expected, "Unexpected queue object interface");
    require(r.flag() && r.read<uint64_t>() == 1, "Missing queue object output");
    const auto object = r.read<uint64_t>(); check(r.read<HRESULT>(), "Captured queue object creation"); r.end(); return object;
}
struct QueueRecipe { uint64_t record{}, object{}; D3D12_COMMAND_QUEUE_DESC desc{}; };
struct FenceRecipe { uint64_t record{}, object{}, initial{}; D3D12_FENCE_FLAGS flags{}; };
struct AllocatorRecipe { uint64_t record{}, object{}; D3D12_COMMAND_LIST_TYPE type{}; };
QueueRecipe queueRecipe(format::Bytes bytes, uint64_t record) {
    Reader r(bytes); r.skip(8); require(r.flag(), "Missing queue description"); QueueRecipe out{record};
    out.desc = r.read<D3D12_COMMAND_QUEUE_DESC>(); out.object = output(r, __uuidof(ID3D12CommandQueue)); return out;
}
FenceRecipe fenceRecipe(format::Bytes bytes, uint64_t record) {
    Reader r(bytes); r.skip(8); FenceRecipe out{record}; out.initial = r.read<uint64_t>(); out.flags = r.read<D3D12_FENCE_FLAGS>();
    out.object = output(r, __uuidof(ID3D12Fence)); return out;
}
AllocatorRecipe allocatorRecipe(format::Bytes bytes, uint64_t record) {
    Reader r(bytes); r.skip(8); AllocatorRecipe out{record}; out.type = r.read<D3D12_COMMAND_LIST_TYPE>();
    out.object = output(r, __uuidof(ID3D12CommandAllocator)); return out;
}
void submissionExtension(const StreamDirectory &directory, const StreamPacket &packet) {
    const auto bytes = directory.payload(packet); const auto decoded = decodeArguments(packet.type, bytes);
    Reader r(bytes.subspan(decoded.consumed));
    if (r.remaining()) {
        // GPA's ID3D12ExecuteCommandListsUserDataReader: version header followed
        // by a count of three-qword entries. Nonempty remapping data needs the
        // indirect-argument path and cannot be silently skipped.
        require(r.read<UINT>() == 0xfede0000, "Unsupported submission user-data version");
        require(r.read<uint64_t>() == 0, "Submission contains unresolved user-data remappings");
    }
    r.end();
}
bool transfer(uint64_t type) {
    return type == 0x1ae3c24e5dabeULL || type == 0x1ae3cd301bf84ULL || type == 0x1ae3c53697cd3ULL || type == 0x1ae3c1d190092ULL || type == 0x1ae3cba2cc519ULL;
}
struct Location { uint64_t object{}; D3D12_TEXTURE_COPY_LOCATION native{}; };
Location location(Reader &r) {
    require(r.flag(), "Missing texture-copy location"); Location out; out.object = r.read<uint64_t>(); out.native.Type = r.read<D3D12_TEXTURE_COPY_TYPE>();
    const auto data = r.take(32);
    if (out.native.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) memcpy(&out.native.PlacedFootprint, data.data(), 32);
    else if (out.native.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) memcpy(&out.native.SubresourceIndex, data.data(), 4);
    else throw std::runtime_error("Unsupported texture-copy location type");
    return out;
}
std::string hash(format::Bytes bytes) {
    return QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size())), QCryptographicHash::Sha256).toHex().toStdString();
}
}
struct NativeReplay::Impl {
    NativeDevice &device; const StreamDirectory &directory; CancelCheck cancelled;
    SubmissionIndex index; QueryReplayIndex queries; NativeResources resources; NativeDescriptors descriptors;
    bool isolateMissingQueries = false;
    std::map<uint64_t, QueueRecipe> queueRecipes;
    std::map<uint64_t, FenceRecipe> fenceRecipes;
    std::map<uint64_t, AllocatorRecipe> allocatorRecipes;
    std::map<uint64_t, ComPtr<ID3D12CommandQueue>> queues;
    std::map<uint64_t, ComPtr<ID3D12Fence>> fences;
    ComPtr<ID3D12Fence> completion; uint64_t completionValue{};
    json submissions = json::array(); std::map<std::string, uint64_t> recordedCommands, executedCommands;
    uint64_t currentRecord{}; bool initialized = false, attempted = false;
    bool frameAttempted = false, boundaryReached = false, boundaryAfter = false;
    uint64_t selectedExecution{};
    std::set<uint64_t> disabledExecutions, suppressedExecutions;
    std::set<uint64_t> uavBarriersBefore, recordedUavBarriers, executedUavBarriers;
    std::vector<ShaderReplacement> shaderReplacements;
    std::vector<ResourcePatch> resourcePatches;
    std::map<size_t, std::pair<D3D12_RESOURCE_STATES, uint64_t>> executionPatchStates;
    std::vector<ComPtr<ID3D12Resource>> patchUploads;
    json recordedResourcePatches = json::array();
    json appliedResourcePatches = json::array();
    std::map<uint64_t, std::vector<uint64_t>> completedCpuWaits;
    struct PendingSubmission {
        uint64_t record{}, queue{};
        json lists;
        std::map<std::string, uint64_t> counts;
    };
    std::vector<PendingSubmission> pendingSubmissions;
    std::set<uint64_t> dirtyQueues;
    ComPtr<ID3D12Device5> abortDevice;
    uint64_t queuedFrameSubmissions{}, completedFrameSubmissions{}, maxPendingSubmissions{}, frameDrainCount{};
    bool frameAborted{};
    json frameDrains = json::array();
    uint64_t fenceObservations{}, waitedFenceObservations{};
    json firstFenceObservations = json::array();
    using StateActions = std::vector<std::function<void()>>;
    struct FramePlan {
        std::unique_ptr<NativeCommands> commands;
        struct Recording {
            ComPtr<ID3D12CommandAllocator> allocator;
            ComPtr<ID3D12GraphicsCommandList> list;
            Impl::StateActions states;
            size_t count{}, recorded{};
            bool closed = false;
            SubmittedList instance;
            size_t timelineEntry{};
            std::unique_ptr<NativePerformance> statistics;
        };
        using Key = std::pair<size_t, size_t>;
        std::map<Key, Recording> recordings;
        std::map<std::pair<uint64_t, uint64_t>, Key> instances;
        std::map<uint64_t, std::vector<Key>> starts;
        uint64_t execution{}, lastRecord{};
        bool after{}, markers{}, streamEnd{};
        json presents = json::array();
        json queryPolicy;
        std::string queryPolicyMarker;
        bool measure{};
        std::unique_ptr<NativePerformance> performance;
        std::unique_ptr<NativeTimeline> timeline;
        std::vector<CpuWait> cpuWaits;
        std::map<uint64_t, std::string> hostDrains;
        json queueDependencies;
        struct QueueOperation {
            size_t remaining{};
            std::vector<uint64_t> successors;
            bool available{}, issued{};
        };
        std::map<uint64_t, QueueOperation> queueOperations;
        std::set<uint64_t> readyOperations;
        uint64_t pendingOperations{}, deferredOperations{}, issuedOperations{};
        json firstIssuedOperations = json::array();
    };
    std::unique_ptr<FramePlan> frame;
    void recordFrame(uint64_t id);
    void applyResourcePatches(uint64_t submission);
    void recordExecutionPatches(ID3D12GraphicsCommandList *, uint64_t execution, uint64_t record, StateActions &);
    void drainFrame(uint64_t record, const char *reason);
    void enqueueFrameOperation(uint64_t record);
    void executeFrameOperation(uint64_t record);
    void abortFrame() noexcept {
        if (!dirtyQueues.empty() && abortDevice) {
            // Stop this isolated replay device before cancellation/failure
            // releases storage still referenced by queued GPU work.
            frameAborted = true;
            abortDevice->RemoveDevice();
            dirtyQueues.clear();
        }
    }
    struct Tiling {
        UINT total{};
        D3D12_PACKED_MIP_INFO packed{};
        std::vector<D3D12_SUBRESOURCE_TILING> subresources;
    };
    std::map<uint64_t, Tiling> tilings;
    uint64_t tileMappingCalls{};
    Impl(NativeDevice &d, const StreamDirectory &s, uint64_t limit, CancelCheck c)
        : device(d), directory(s), cancelled(std::move(c)), index(s, cancelled), queries(s, index, cancelled),
          resources(d, s, limit, cancelled), descriptors(d, s, resources, cancelled) {
        for (size_t i = 0; i < s.initializationCount(); ++i) {
            if (i % 1024 == 0)
                checkCancellation(cancelled);
            const auto &p = s.packets()[i];
            const auto bytes = s.payload(p);
            if (p.type == 0x127932607a959ULL) { const auto r = queueRecipe(bytes, i + 1); require(queueRecipes.emplace(r.object, r).second, "Ambiguous initialization queue lifetime"); }
            if (p.type == 0x127930cf4f3a0ULL) { const auto r = fenceRecipe(bytes, i + 1); require(fenceRecipes.emplace(r.object, r).second, "Ambiguous initialization fence lifetime"); }
            if (p.type == 0x12793beef5413ULL) { const auto r = allocatorRecipe(bytes, i + 1); require(allocatorRecipes.emplace(r.object, r).second, "Ambiguous initialization allocator lifetime"); }
        }
    }
    ID3D12CommandQueue *queue(uint64_t object) {
        if (auto it = queues.find(object); it != queues.end()) return it->second.Get();
        require(queueRecipes.contains(object), "Missing initialization queue recipe"); ComPtr<ID3D12CommandQueue> native;
        check(device.device()->CreateCommandQueue(&queueRecipes.at(object).desc, IID_PPV_ARGS(&native)), "Create captured command queue");
        auto result = native.Get(); queues.emplace(object, std::move(native)); return result;
    }
    ID3D12Fence *fence(uint64_t object) {
        if (auto it = fences.find(object); it != fences.end()) return it->second.Get();
        require(fenceRecipes.contains(object), "Missing initialization fence recipe"); const auto &r = fenceRecipes.at(object); ComPtr<ID3D12Fence> native;
        check(device.device()->CreateFence(r.initial, r.flags, IID_PPV_ARGS(&native)), "Create captured fence");
        auto result = native.Get(); fences.emplace(object, std::move(native)); return result;
    }
    void awaitFence(ID3D12Fence *fence, uint64_t value) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (fence->GetCompletedValue() < value) {
            checkCancellation(cancelled);
            check(device.device()->GetDeviceRemovedReason(), "Replay GPU completion");
            require(std::chrono::steady_clock::now() < deadline, "Replay GPU completion timed out; retry in a fresh Worker");
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(device.device()->GetDeviceRemovedReason(), "Replay GPU completion"); checkCancellation(cancelled);
    }
    void wait(ID3D12CommandQueue *queue) {
        if (!completion) check(device.device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&completion)), "Create replay completion fence");
        require(completionValue != UINT64_MAX - 1, "Replay completion fence exhausted");
        check(queue->Signal(completion.Get(), ++completionValue), "Signal replay completion");
        awaitFence(completion.Get(), completionValue);
    }
    void checkLocation(const Location &location) {
        const auto desc = location.native.pResource->GetDesc();
        if (location.native.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
            require(desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER, "Texture subresource references a buffer");
            D3D12_FEATURE_DATA_FORMAT_INFO format{desc.Format}; check(device.device()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &format, sizeof(format)), "Query texture planes");
            const uint64_t count = uint64_t(desc.MipLevels) * (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize) * format.PlaneCount;
            require(location.native.SubresourceIndex < count, "Texture copy subresource is out of range");
        } else {
            require(desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER, "Placed texture footprint references a texture");
            const auto &placed = location.native.PlacedFootprint; const auto &f = placed.Footprint;
            require(f.Width && f.Height && f.Depth && f.Depth <= UINT16_MAX && placed.Offset % 512 == 0 && f.RowPitch % 256 == 0, "Invalid texture upload footprint");
            D3D12_RESOURCE_DESC probe{}; probe.Dimension = f.Depth > 1 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            probe.Width = f.Width; probe.Height = f.Height; probe.DepthOrArraySize = UINT16(f.Depth); probe.MipLevels = 1; probe.Format = f.Format; probe.SampleDesc.Count = 1;
            UINT rows{}; UINT64 rowBytes{}, total{}; device.device()->GetCopyableFootprints(&probe, 0, 1, 0, nullptr, &rows, &rowBytes, &total);
            require(total != UINT64_MAX && rows && rowBytes <= f.RowPitch, "Unsupported texture copy footprint format");
            const uint64_t rowCount = uint64_t(rows) * f.Depth;
            require(placed.Offset <= desc.Width && rowBytes <= desc.Width - placed.Offset &&
                rowCount - 1 <= (desc.Width - placed.Offset - rowBytes) / f.RowPitch, "Texture footprint exceeds upload buffer");
        }
    }
    const Tiling &tiling(uint64_t object, uint64_t record) {
        const auto creation = resources.creationRecord(object, record);
        if (auto it = tilings.find(creation); it != tilings.end()) return it->second;
        require(resources.recipe(object, record).kind == ResourceRecipe::Kind::Reserved, "Tiled operation requires a reserved resource");
        auto native = resources.resource(object, record); const auto desc = native->GetDesc();
        UINT planes = 1;
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
            D3D12_FEATURE_DATA_FORMAT_INFO info{desc.Format}; check(device.device()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof(info)), "Query tiled texture planes"); planes = info.PlaneCount;
        }
        const uint64_t count = uint64_t(desc.MipLevels) * (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize) * planes;
        require(count && count <= D3D12_REQ_SUBRESOURCES, "Invalid reserved resource subresource count");
        Tiling result; result.subresources.resize(size_t(count)); UINT available = UINT(count); D3D12_TILE_SHAPE shape{};
        device.device()->GetResourceTiling(native, &result.total, &result.packed, &shape, &available, 0, result.subresources.data());
        require(result.total && available == count, "Device did not return the required resource tiling");
        return tilings.emplace(creation, std::move(result)).first->second;
    }
    uint64_t tileRegion(uint64_t object, uint64_t record, const D3D12_TILED_RESOURCE_COORDINATE &start, const D3D12_TILE_REGION_SIZE &size) {
        const auto &t = tiling(object, record); require(start.Subresource < t.subresources.size(), "Tile subresource is out of range");
        const auto &sub = t.subresources[start.Subresource];
        require(sub.StartTileIndexInOverallResource != D3D12_PACKED_TILE && sub.WidthInTiles && sub.HeightInTiles && sub.DepthInTiles,
            "Packed mip tile coordinates require a separate mapping path");
        require(start.X < sub.WidthInTiles && start.Y < sub.HeightInTiles && start.Z < sub.DepthInTiles, "Tile coordinate is outside the native subresource tiling");
        const uint64_t linear = start.X + uint64_t(sub.WidthInTiles) * (start.Y + uint64_t(sub.HeightInTiles) * start.Z);
        const uint64_t subCount = uint64_t(sub.WidthInTiles) * sub.HeightInTiles * sub.DepthInTiles;
        if (size.UseBox) {
            require(size.UseBox == TRUE && size.Width && size.Height && size.Depth && size.Width <= sub.WidthInTiles - start.X &&
                size.Height <= sub.HeightInTiles - start.Y && size.Depth <= sub.DepthInTiles - start.Z, "Tile box exceeds the native subresource tiling");
            return uint64_t(size.Width) * size.Height * size.Depth;
        }
        require(size.NumTiles && size.NumTiles <= subCount - linear, "Linear tile region crosses an unsupported subresource boundary"); return size.NumTiles;
    }
    void mapTiles(uint64_t record) {
        const auto &p = directory.packets()[size_t(record - 1)]; const auto r = decodeTileMapping(directory.payload(p));
        require(r.regionCount && r.starts.size() == r.regionCount && r.sizes.size() == r.regionCount && r.rangeCount &&
            r.rangeFlags.size() == r.rangeCount && r.heapOffsets.size() == r.rangeCount && r.tileCounts.size() == r.rangeCount,
            "Implicit tile mapping arrays require a separate defaults path");
        require((UINT(r.flags) & ~UINT(D3D12_TILE_MAPPING_FLAG_NO_HAZARD)) == 0, "Unknown tile mapping flags");
        uint64_t regions{}, ranges{};
        for (size_t i = 0; i < r.starts.size(); ++i) regions += tileRegion(r.resource, record, r.starts[i], r.sizes[i]);
        ID3D12Heap *heap = r.heap ? resources.tileHeap(r.heap, record) : nullptr;
        const auto heapTiles = heap ? heap->GetDesc().SizeInBytes / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES : 0;
        for (size_t i = 0; i < r.tileCounts.size(); ++i) {
            require(r.tileCounts[i], "Empty tile range"); ranges += r.tileCounts[i];
            const auto flags = r.rangeFlags[i];
            if (flags == D3D12_TILE_RANGE_FLAG_NONE || flags == D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE) {
                const uint64_t length = flags == D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE ? 1 : r.tileCounts[i];
                require(heap && r.heapOffsets[i] <= heapTiles && length <= heapTiles - r.heapOffsets[i], "Tile range exceeds its captured heap");
            } else require(flags == D3D12_TILE_RANGE_FLAG_NULL || flags == D3D12_TILE_RANGE_FLAG_SKIP, "Unknown tile range flags");
        }
        require(regions == ranges, "Tile region and heap range counts differ");
        queue(r.queue)->UpdateTileMappings(resources.resource(r.resource, record), r.regionCount, r.starts.data(), r.sizes.data(), heap,
            r.rangeCount, r.rangeFlags.data(), r.heapOffsets.data(), r.tileCounts.data(), r.flags);
        ++tileMappingCalls;
    }
    void record(ID3D12GraphicsCommandList *list, uint64_t id, uint64_t expectedList, StateActions *deferred = nullptr) {
        currentRecord = id; checkCancellation(cancelled); const auto &p = directory.packets().at(size_t(id - 1)); Reader r(directory.payload(p));
        require(r.read<uint64_t>() == expectedList, "Command belongs to another captured list");
        const auto action = [&](std::function<void()> f) {
            if (deferred) deferred->push_back([f = std::move(f), id] {
                try { f(); } catch (const std::exception &e) { throw std::runtime_error("Execution source record " + std::to_string(id) + ": " + e.what()); }
            }); else f();
        };
        if (p.type == 0x1ae3cba2cc519ULL) {
            const auto copy = decodeTileCopy(directory.payload(p)); const auto tiles = tileRegion(copy.resource, id, copy.start, copy.size);
            auto tiled = resources.resource(copy.resource, id), buffer = resources.resource(copy.buffer, id); const auto desc = buffer->GetDesc();
            const auto direction = UINT(copy.flags) & ~UINT(D3D12_TILE_COPY_FLAG_NO_HAZARD);
            const bool upload = direction == D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE;
            require(upload || direction == D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER, "Unknown tile copy flags");
            require(desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && copy.offset % D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES == 0 &&
                copy.offset <= desc.Width && tiles <= (desc.Width - copy.offset) / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES, "Tile copy exceeds its linear buffer");
            action([=, this] { resources.requireState(copy.resource, id, copy.start.Subresource, upload ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COPY_SOURCE);
                resources.requireState(copy.buffer, id, 0, upload ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST); });
            list->CopyTiles(tiled, &copy.start, &copy.size, buffer, copy.offset, copy.flags);
        } else if (p.type == 0x1ae3c24e5dabeULL) {
            const auto count = r.read<UINT>(); std::vector<D3D12_RESOURCE_BARRIER> barriers;
            if (r.flag()) {
                const auto entries = r.read<uint64_t>(); require(entries == count && entries <= r.remaining() / 32, "Invalid resource barrier array"); barriers.reserve(count);
                for (UINT i = 0; i < count; ++i) {
                    Reader b(r.take(32)); D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = b.read<D3D12_RESOURCE_BARRIER_TYPE>(); barrier.Flags = b.read<D3D12_RESOURCE_BARRIER_FLAGS>();
                    require(barrier.Flags == D3D12_RESOURCE_BARRIER_FLAG_NONE, "Split barriers require the pending-transition execution path");
                    if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                        const auto object = b.read<uint64_t>(); auto &t = barrier.Transition; t.Subresource = b.read<UINT>(); t.StateBefore = b.read<D3D12_RESOURCE_STATES>(); t.StateAfter = b.read<D3D12_RESOURCE_STATES>();
                        t.pResource = resources.resource(object, id);
                        action([this, object, id, t] { resources.transition(object, id, t.Subresource, t.StateBefore, t.StateAfter); });
                    } else if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
                        const auto before = b.read<uint64_t>(), after = b.read<uint64_t>();
                        barrier.Aliasing.pResourceBefore = before ? resources.resource(before, id) : nullptr; barrier.Aliasing.pResourceAfter = after ? resources.resource(after, id) : nullptr;
                    } else if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
                        const auto object = b.read<uint64_t>(); barrier.UAV.pResource = object ? resources.resource(object, id) : nullptr;
                    } else throw std::runtime_error("Unknown resource barrier type");
                    barriers.push_back(barrier);
                }
            }
            require(barriers.size() == count, "Missing resource barriers"); r.end(); if (count) list->ResourceBarrier(count, barriers.data());
        } else if (p.type == 0x1ae3cd301bf84ULL) {
            const auto destination = r.read<uint64_t>(), destinationOffset = r.read<uint64_t>(), source = r.read<uint64_t>(), sourceOffset = r.read<uint64_t>(), count = r.read<uint64_t>(); r.end();
            auto dst = resources.resource(destination, id), src = resources.resource(source, id); const auto d = dst->GetDesc(), s = src->GetDesc();
            require(d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && s.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && destinationOffset <= d.Width && count <= d.Width - destinationOffset && sourceOffset <= s.Width && count <= s.Width - sourceOffset, "Captured buffer copy is out of range");
            action([=, this] { resources.requireState(source, id, 0, D3D12_RESOURCE_STATE_COPY_SOURCE); resources.requireState(destination, id, 0, D3D12_RESOURCE_STATE_COPY_DEST); });
            list->CopyBufferRegion(dst, destinationOffset, src, sourceOffset, count);
        } else if (p.type == 0x1ae3c53697cd3ULL) {
            auto dst = location(r); const auto x = r.read<UINT>(), y = r.read<UINT>(), z = r.read<UINT>(); auto src = location(r);
            std::optional<D3D12_BOX> box; if (r.flag()) box = r.read<D3D12_BOX>(); r.end();
            dst.native.pResource = resources.resource(dst.object, id); src.native.pResource = resources.resource(src.object, id); checkLocation(dst); checkLocation(src);
            const auto srcSub = src.native.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ? src.native.SubresourceIndex : 0;
            const auto dstSub = dst.native.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ? dst.native.SubresourceIndex : 0;
            action([=, this] { resources.requireState(src.object, id, srcSub, D3D12_RESOURCE_STATE_COPY_SOURCE); resources.requireState(dst.object, id, dstSub, D3D12_RESOURCE_STATE_COPY_DEST); });
            list->CopyTextureRegion(&dst.native, x, y, z, &src.native, box ? &*box : nullptr);
        } else if (p.type == 0x1ae3c1d190092ULL) {
            const auto destination = r.read<uint64_t>(), source = r.read<uint64_t>(); r.end();
            auto dst = resources.resource(destination, id), src = resources.resource(source, id);
            action([=, this] { resources.requireState(source, id, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE); resources.requireState(destination, id, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST); });
            list->CopyResource(dst, src);
        } else throw std::runtime_error("Unsupported native initialization command: " + streamPacketName(p.type));
        ++recordedCommands[streamPacketName(p.type)];
    }
    void submit(uint64_t id) {
        const auto &packet = directory.packets()[size_t(id - 1)]; submissionExtension(directory, packet);
        const auto args = decodeArguments(packet.type, directory.payload(packet)).values; const auto queueId = value(args, "Queue");
        auto nativeQueue = queue(queueId); std::vector<ComPtr<ID3D12CommandAllocator>> allocators; std::vector<ComPtr<ID3D12GraphicsCommandList>> lists; std::vector<ID3D12CommandList *> pointers;
        json executions = json::array();
        for (const auto &instance : index.lists()) {
            if (instance.submission != id) continue;
            require(instance.recording.has_value(), "Submitted list recording is unavailable"); const auto &recording = index.recordings()[*instance.recording];
            require(recording.hasBeginning && recording.closed, "Submitted list recording is incomplete"); const auto &begin = directory.packets()[size_t(recording.begin - 1)];
            require(begin.type == 0x127938b682074ULL, "Initialization Reset requires command-list lifetime restoration");
            const auto create = decodeArguments(begin.type, directory.payload(begin)).values;
            require(value(create, "InitialPipelineState") == 0, "Initialization list needs a graphics/compute pipeline");
            const auto type = D3D12_COMMAND_LIST_TYPE(create.at("Type").get<UINT>()); const auto allocatorId = value(create, "CommandAllocator");
            require(allocatorRecipes.contains(allocatorId) && allocatorRecipes.at(allocatorId).type == type && type == queueRecipes.at(queueId).desc.Type, "Command allocator/list/queue types do not match");
            ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> list;
            check(device.device()->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator)), "Create captured command allocator");
            check(device.device()->CreateCommandList(create.at("NodeMask").get<UINT>(), type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "Create captured command list");
            for (const auto command : recording.records) record(list.Get(), command, instance.list);
            check(list->Close(), "Close captured command list"); pointers.push_back(list.Get()); allocators.push_back(std::move(allocator)); lists.push_back(std::move(list));
            executions.push_back({{"list", std::to_string(instance.list)}, {"ordinal", std::to_string(instance.ordinal)}, {"generation", std::to_string(recording.generation)},
                {"first_execution", std::to_string(instance.firstExecution)}, {"execution_count", std::to_string(instance.count)}});
        }
        require(pointers.size() == args.at("NumCommandLists").get<size_t>(), "Submission instance count mismatch");
        currentRecord = id; checkCancellation(cancelled); if (!pointers.empty()) nativeQueue->ExecuteCommandLists(UINT(pointers.size()), pointers.data()); wait(nativeQueue);
        resources.completeSubmission(queueRecipes.at(queueId).desc.Type);
        for (const auto &instance : index.lists()) if (instance.submission == id)
            for (auto command : index.recordings()[*instance.recording].records)
                ++executedCommands[streamPacketName(directory.packets()[size_t(command - 1)].type)];
        submissions.push_back({{"record", std::to_string(id)}, {"queue", std::to_string(queueId)}, {"lists", std::move(executions)}});
    }
};
NativeReplay::NativeReplay(NativeDevice &d, const StreamDirectory &s, uint64_t limit, const CancelCheck &cancelled)
    : impl_(std::make_unique<Impl>(d, s, limit, cancelled)) {}
NativeReplay::~NativeReplay() { impl_->abortFrame(); }
void NativeReplay::setIsolateMissingQueries(bool enabled) {
    require(!impl_->frame && !impl_->attempted, "Set query isolation before preparing or initializing replay");
    impl_->isolateMissingQueries = enabled;
}
void NativeReplay::setResourcePatches(std::vector<ResourcePatch> patches) {
    auto &s = *impl_;
    require(!s.frame && !s.attempted, "Set resource patches before preparing or initializing replay");
    decltype(s.executionPatchStates) patchStates;
    for (const auto &patch : patches) {
        require(patch.submission > s.directory.initializationCount() && patch.submission <= s.directory.packets().size() &&
                s.directory.packets()[size_t(patch.submission - 1)].type == 0x125482975df48ULL,
                "Resource patch needs a frame ExecuteCommandLists record");
        require(!patch.bytes.empty() && patch.bytes.size() <= 256ULL * 1024 * 1024, "Resource patch must contain 1 byte to 256 MiB");
        require(s.resources.creationRecord(patch.object, patch.submission) == patch.creation,
                "Resource patch creation is not active at the submission boundary");
        const auto recipe = s.resources.recipe(patch.object, patch.submission);
        require(recipe.kind != ResourceRecipe::Kind::Reserved, "Patching tiled resources needs explicit residency coverage and is not implemented");
        const auto &desc = recipe.description;
        require(patch.texture == (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER), "Resource patch kind does not match the resource");
        if (!patch.texture)
            require(!patch.subresource && patch.offset <= desc.Width && patch.bytes.size() <= desc.Width - patch.offset, "Buffer patch exceeds resource bounds");
        else require(!patch.offset && desc.SampleDesc.Count == 1, "Texture patches require a single-sample full subresource");
        if (patch.afterExecution) {
            require(!patch.texture, "After-execution patches currently require a buffer");
            const auto event = s.index.execution(patch.afterExecution);
            require(event.recordingComplete && event.submission == patch.submission,
                    "Resource patch execution does not belong to its submission");
            const auto type = s.directory.packets().at(size_t(event.record - 1)).type;
            require(streamPacketWork(type) || type == 0x1ae3cd301bf84ULL,
                    "After-execution patches require Draw, Dispatch, ExecuteIndirect or CopyBufferRegion");
            require(s.resources.creationRecord(patch.object, event.record) == patch.creation,
                    "Resource patch creation is not active at the recording boundary");
            std::optional<std::pair<D3D12_RESOURCE_STATES, uint64_t>> known;
            bool inferFollowing = true;
            const auto recording = s.index.recordingForRecord(event.record);
            require(recording.has_value(), "Missing execution recording");
            for (const auto id : s.index.recordings().at(*recording).records) {
                const bool following = id > event.record;
                if (following && (known || !inferFollowing)) break;
                checkCancellation(s.cancelled);
                const auto &packet = s.directory.packets().at(size_t(id - 1));
                if (packet.type != 0x1ae3c24e5dabeULL) continue;
                const auto args = decodeArguments(packet.type, s.directory.payload(packet));
                require(args.supported && args.consumed == packet.size, "Incomplete resource patch barrier evidence");
                for (const auto &barrier : args.values.at("Barriers")) {
                    if (barrier.at("Type") == 0 && value(barrier, "Resource") == patch.object) {
                        require(barrier.at("Subresource") == 0 || barrier.at("Subresource") == UINT32_MAX,
                                "Invalid buffer transition subresource");
                        if (following) {
                            // A buffer cannot change between exclusive write states
                            // without a transition. Before this next barrier it is
                            // therefore in this write state or still COMMON, which
                            // also permits a promotable StateBefore. Read masks can
                            // widen implicitly, so do not infer them backwards.
                            const auto before = barrier.at("StateBefore").get<UINT>();
                            if (barrier.at("Flags") == 0 &&
                                (before == D3D12_RESOURCE_STATE_UNORDERED_ACCESS || before == D3D12_RESOURCE_STATE_COPY_DEST))
                                known = {D3D12_RESOURCE_STATES(before), id};
                            inferFollowing = false;
                            break;
                        }
                        inferFollowing = barrier.at("Flags") == 0;
                        if (!inferFollowing || barrier.at("StateAfter") == 0) known.reset();
                        else known = {D3D12_RESOURCE_STATES(barrier.at("StateAfter").get<UINT>()), id};
                    } else if (barrier.at("Type") == 1 &&
                               (!value(barrier, "ResourceBefore") || !value(barrier, "ResourceAfter") ||
                                value(barrier, "ResourceBefore") == patch.object || value(barrier, "ResourceAfter") == patch.object)) {
                        known.reset();
                        inferFollowing = false;
                        if (following) break;
                    }
                }
            }
            require(known.has_value(), "After-execution buffer patch needs a preceding non-COMMON transition or a following exclusive-write transition in the same command list");
            patchStates.emplace(size_t(&patch - patches.data()), *known);
        }
    }
    s.resourcePatches = std::move(patches);
    s.executionPatchStates = std::move(patchStates);
}
void NativeReplay::setShaderReplacements(std::vector<ShaderReplacement> replacements) {
    auto &s = *impl_;
    require(!s.frame && !s.attempted, "Set shader replacements before preparing or initializing replay");
    s.shaderReplacements = std::move(replacements);
}
void NativeReplay::setDisabledExecutions(std::set<uint64_t> executions) {
    auto &s = *impl_;
    require(!s.attempted && !s.frame, "Configure experiment edits before preparing initialization");
    for (const auto id : executions) {
        const auto execution = s.index.execution(id);
        require(execution.submission > s.directory.initializationCount() && execution.recordingComplete,
                "Disable requires a complete frame execution instance");
        const auto &packet = s.directory.packets().at(size_t(execution.record - 1));
        require(streamPacketCanDisable(packet.type), "Only Draw/Dispatch or work-only ExecuteIndirect suppression is implemented");
        const auto decoded = decodeArguments(packet.type, s.directory.payload(packet));
        require(decoded.supported && decoded.consumed == packet.size, "Invalid disabled command arguments");
    }
    s.disabledExecutions = std::move(executions);
}
void NativeReplay::setUavBarriersBefore(std::set<uint64_t> executions) {
    auto &s = *impl_;
    require(!s.attempted && !s.frame, "Configure UAV barrier experiments before preparing initialization");
    for (const auto id : executions) {
        const auto execution = s.index.execution(id);
        require(execution.submission > s.directory.initializationCount() && execution.recordingComplete,
                "UAV barrier experiment requires a complete frame execution instance");
        const auto &packet = s.directory.packets().at(size_t(execution.record - 1));
        require(streamPacketCanDisable(packet.type), "UAV barrier experiments require Draw/Dispatch or ExecuteIndirect");
        const auto decoded = decodeArguments(packet.type, s.directory.payload(packet));
        require(decoded.supported && decoded.consumed == packet.size, "Invalid UAV barrier target arguments");
    }
    s.uavBarriersBefore = std::move(executions);
}
void NativeReplay::initializeTransfers() {
    auto &s = *impl_; require(!s.attempted, "Use a fresh replay context for initialization"); s.attempted = true;
    std::set<uint64_t> queues;
    for (const auto &instance : s.index.lists()) {
        checkCancellation(s.cancelled);
        if (instance.submission > s.directory.initializationCount()) continue; queues.insert(instance.queue);
        require(instance.recording.has_value(), "Missing initialization list recording");
        for (const auto id : s.index.recordings()[*instance.recording].records) {
            const auto type = s.directory.packets()[size_t(id - 1)].type;
            if (!transfer(type)) throw std::runtime_error("Initialization record " + std::to_string(id) + " needs " + streamPacketName(type));
        }
    }
    for (size_t i = 0; i < s.directory.initializationCount(); ++i) {
        if (i % 1024 == 0) checkCancellation(s.cancelled); const auto &p = s.directory.packets()[i];
        if (p.type == 0x125482264c67fULL) queues.insert(decodeTileMapping(s.directory.payload(p)).queue);
    }
    require(queues.size() == 1, "Multi-queue initialization requires dependency scheduling");
    for (size_t i = 0; i < s.directory.initializationCount(); ++i) {
        s.currentRecord = i + 1; if (i % 256 == 0) checkCancellation(s.cancelled); const auto &p = s.directory.packets()[i];
        try {
            if (s.frame) s.recordFrame(i + 1);
            if (p.type == 0x20000000000000ULL) s.resources.restoreMappedData(i + 1);
            else if (s.descriptors.apply(i + 1)) {}
            else if (p.type == 0x14538e8691d1cULL) s.resources.releaseCompleted(i + 1);
            else if (p.type == 0x125482975df48ULL) s.submit(i + 1);
            else if (p.type == 0x12548dab1bf2dULL || p.type == 0x12548db3bfde2ULL) {
                Reader r(s.directory.payload(p)); const auto queue = r.read<uint64_t>(), fence = r.read<uint64_t>(), value = r.read<uint64_t>();
                check(r.read<HRESULT>(), "Captured queue synchronization"); r.end(); auto nativeQueue = s.queue(queue); auto nativeFence = s.fence(fence);
                if (p.type == 0x12548dab1bf2dULL) check(nativeQueue->Signal(nativeFence, value), "Replay captured queue signal");
                else {
                    require(nativeFence->GetCompletedValue() >= value, "Initialization wait has an unresolved producer");
                    check(nativeQueue->Wait(nativeFence, value), "Replay captured queue wait");
                }
            } else if (p.type == 0x125482264c67fULL) s.mapTiles(i + 1);
        } catch (const OperationCancelled &) { throw; }
        catch (const std::exception &e) { throw std::runtime_error("Initialization record " + std::to_string(s.currentRecord) + ": " + e.what()); }
    }
    for (const auto &[id, queue] : s.queues)
        s.wait(queue.Get());
    s.initialized = true;
}
void NativeReplay::prepareExecution(uint64_t execution, bool after, bool analysisMarkers, bool performance) {
    prepare(execution, after, analysisMarkers, performance, false);
}
void NativeReplay::prepareFrame(bool analysisMarkers, bool performance) {
    prepare(impl_->index.executionCount(), true, analysisMarkers, performance, true);
}
void NativeReplay::prepare(uint64_t execution, bool after, bool analysisMarkers, bool performance, bool streamEnd) {
    auto &s = *impl_;
    require(!s.frameAttempted && !s.frame, "Execution plan is already prepared or attempted");
    const auto target = s.index.execution(execution);
    require(target.submission > s.directory.initializationCount() && target.recordingComplete,
            "Select a complete frame execution instance");
    if (s.isolateMissingQueries) {
        // NativePerformance/NativeTimeline own their query heaps and readbacks.
        // Captured query outputs remain isolated and unavailable to consumers.
        // Overlay edits preserve Begin/End/Resolve query commands. Resource and
        // descriptor access still reject isolated READBACK allocations, including
        // experiment patches; an edit cannot turn absent query bytes into data.
        for (const auto creation : s.queries.isolatedReadbacks(execution, !after)) s.resources.isolateQueryReadback(creation);
    } else if (const auto issue = s.queries.firstIssue(execution, !after); !issue.is_null())
        throw std::runtime_error(issue.at("reason").get<std::string>());
    auto frame = std::make_unique<Impl::FramePlan>();
    frame->streamEnd = streamEnd;
    frame->lastRecord = streamEnd ? s.directory.packets().size() : target.submission;
    frame->cpuWaits = planCpuWaits(s.directory, frame->lastRecord, s.cancelled);
    const QueueDependencyIndex dependencies(s.directory, frame->lastRecord, s.cancelled);
    frame->queueDependencies = dependencies.information();
    require(frame->queueDependencies.at("unresolved_queue_operations") == "0",
            "Frame queue dependencies are missing, ambiguous or cyclic; inspect command-lists queue_dependencies");
    for (const auto &operation : dependencies.frameOperations()) {
        checkCancellation(s.cancelled);
        frame->queueOperations.try_emplace(operation.record);
    }
    for (const auto &operation : dependencies.frameOperations()) {
        checkCancellation(s.cancelled);
        for (const auto predecessor : operation.predecessors)
            if (predecessor > s.directory.initializationCount()) {
                frame->queueOperations.at(predecessor).successors.push_back(operation.record);
                ++frame->queueOperations.at(operation.record).remaining;
            }
    }
    for (const auto &wait : frame->cpuWaits) frame->hostDrains[wait.record] = "captured_cpu_wait";
    for (uint64_t id = s.directory.initializationCount() + 1; id <= frame->lastRecord; ++id) {
        if (id % 1024 == 0) checkCancellation(s.cancelled);
        const auto type = s.directory.packets()[size_t(id - 1)].type;
        if (type == 0x20000000000000ULL) frame->hostDrains[id] = "mapped_data_write";
        else if (isDescriptorWrite(type) || type == 0x12793300ba04eULL || type == 0x1279345e6c880ULL)
            frame->hostDrains[id] = "descriptor_update";
        else if (type == 0x125482264c67fULL) frame->hostDrains[id] = "tile_mapping";
        else if (streamEnd && type == 0x1d53218275f72ULL) frame->hostDrains[id] = "offscreen_present_boundary";
        else if (type == 0x11469e993999dULL) {
            Reader r(s.directory.payload(s.directory.packets()[size_t(id - 1)]));
            const auto fence = r.read<uint64_t>(), observed = r.read<uint64_t>(); r.end();
            const auto dependency = dependencies.observationDependency(fence, observed, id);
            if (dependency.at("status") != "resolved")
                throw std::runtime_error("Fence observation record " + std::to_string(id) + ": " + dependency.at("reason").get<std::string>());
        }
    }
    for (const auto &patch : s.resourcePatches)
        if (!patch.afterExecution && patch.submission <= frame->lastRecord) frame->hostDrains[patch.submission] = "resource_experiment";
    for (const auto &wait : frame->queueDependencies.at("waits")) {
        if (wait.value("producer_relation", "") != "later_cpu_record") continue;
        const auto record = value(wait, "record"), producer = value(wait, "producer_record");
        require(record > s.directory.initializationCount(), "Initialization queue wait requires a later producer");
        const auto drain = frame->hostDrains.lower_bound(record);
        if (drain != frame->hostDrains.end() && drain->first < producer)
            throw std::runtime_error("Queue wait record " + std::to_string(record) + " needs Signal " +
                std::to_string(producer) + " beyond host drain " + std::to_string(drain->first));
    }
    check(s.device.device()->QueryInterface(IID_PPV_ARGS(&s.abortDevice)),
          "Frame queue scheduling requires device removal support for interrupted in-flight work");
    frame->execution = execution;
    frame->after = after;
    frame->markers = analysisMarkers;
    if (s.isolateMissingQueries && analysisMarkers) {
        frame->queryPolicy = {{"schema", 1}, {"policy", "isolate_readback_outputs"}, {"frame_complete", false},
            {"selected_execution", std::to_string(execution)}, {"after", after},
            {"resources", json::array()}, {"resolves", json::array()}};
        const auto resourceInfo = s.resources.information();
        for (const auto &resource : resourceInfo.at("isolated_query_readbacks")) {
            const auto creation = std::stoull(resource.at("creation").get<std::string>());
            const auto &packet = s.directory.packets().at(size_t(creation - 1));
            const auto recipe = decodeResourceRecipe(packet.type, s.directory.payload(packet));
            frame->queryPolicy["resources"].push_back({{"creation", resource.at("creation")},
                {"width", resource.at("width")}, {"object", std::to_string(recipe.object)}});
        }
        const auto coverage = s.queries.information();
        for (const auto &entry : coverage.at("resolves_and_issues")) {
            const auto id = std::stoull(entry.at("execution").get<std::string>());
            if (id > execution || (id == execution && !after)) break;
            if (entry.at("status") != "missing_query_results") continue;
            frame->queryPolicy["resolves"].push_back({{"execution", entry.at("execution")},
                {"source_record", entry.at("source_record")}, {"submission", entry.at("submission")}, {"queue", entry.at("queue")},
                {"creation", entry.at("destination_resource").at("creation")}, {"offset", entry.at("destination_offset")},
                {"bytes", entry.at("result_bytes")}, {"ranges", entry.at("destination_byte_ranges")}});
        }
        frame->queryPolicyMarker = rdcQueryIsolationMarker(frame->queryPolicy);
    }
    require(!performance || after, "Performance queries require an after-event replay boundary");
    require(!performance || streamEnd || !s.disabledExecutions.contains(execution), "Selected execution is disabled by the experiment");
    require(!performance || !analysisMarkers, "Native performance queries cannot be combined with RenderDoc capture");
    frame->measure = performance;
    frame->commands =
        std::make_unique<NativeCommands>(s.device, s.directory, s.resources, s.descriptors, s.cancelled);
    frame->commands->pipelines().setShaderReplacements(s.shaderReplacements);
    auto &recordings = frame->recordings;
    auto &instances = frame->instances;
    auto &starts = frame->starts;
    uint64_t nativeCommandCount{};
    for (const auto &instance : s.index.lists()) {
        if (instance.submission <= s.directory.initializationCount() ||
            (!streamEnd && (instance.submission > target.submission ||
                            (instance.submission == target.submission && instance.ordinal > target.ordinal))))
            continue;
        require(instance.recording.has_value(), "Missing frame command-list recording");
        const auto &original = s.index.recordings()[*instance.recording];
        require(original.hasBeginning && original.closed &&
                    (!s.initialized || original.begin > s.directory.initializationCount()),
                "Prepare the execution before initialization to restore keyframe recording state");
        const auto count = !streamEnd && instance.submission == target.submission && instance.ordinal == target.ordinal
                               ? size_t(execution - instance.firstExecution + (after ? 1 : 0))
                               : original.records.size();
        require(count <= 4000000 - nativeCommandCount,
                "Native frame recording exceeds the four-million command limit");
        nativeCommandCount += count;
        // Each submission instance gets its own native recording, including
        // repeated submissions of one generation. This makes execution markers
        // unambiguous without splitting a native ExecuteCommandLists batch.
        const Impl::FramePlan::Key key{*instance.recording, instances.size()};
        auto &native = recordings.try_emplace(key).first->second;
        native.count = count;
        native.instance = instance;
        instances.emplace(std::pair{instance.submission, instance.ordinal}, key);
    }
    for (const auto &[key, native] : recordings)
        starts[s.index.recordings()[key.first].begin].push_back(key);
    if (performance) {
        const auto waits = std::count_if(s.index.synchronization().begin(), s.index.synchronization().end(),
            [](const auto &operation) { return operation.wait; });
        frame->timeline = std::make_unique<NativeTimeline>(s.device.device(), recordings.size(), size_t(waits));
    }
    s.frame = std::move(frame);
}
void NativeReplay::Impl::recordFrame(uint64_t id) {
    auto &s = *this;
    auto &starts = frame->starts;
    auto &recordings = frame->recordings;
    auto &commands = frame->commands;
    const auto analysisMarkers = frame->markers;
    const auto &p = directory.packets().at(size_t(id - 1));
    if (auto it = starts.find(id); it != starts.end())
        for (const auto &key : it->second) {
            auto &native = recordings.at(key);
            const auto args = decodeArguments(p.type, s.directory.payload(p)).values;
            const auto allocatorId = value(args, "CommandAllocator");
            require(s.allocatorRecipes.contains(allocatorId), "Missing frame allocator recipe");
            const auto type = s.allocatorRecipes.at(allocatorId).type;
            if (frame->measure && !frame->streamEnd && native.instance.firstExecution <= frame->execution &&
                frame->execution - native.instance.firstExecution < native.count) {
                require(!frame->performance, "Ambiguous performance execution instance");
                frame->performance = std::make_unique<NativePerformance>(s.device.device(), type);
            }
            require(type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_COPY ||
                        type == D3D12_COMMAND_LIST_TYPE_COMPUTE,
                    "Unsupported frame command-list type");
            if (args.contains("Type"))
                require(args.at("Type").get<UINT>() == UINT(type), "Frame list and allocator types disagree");
            const auto pso = value(args, "InitialPipelineState");
            auto initial = pso ? commands->pipelines().pipeline(pso, id) : nullptr;
            check(s.device.device()->CreateCommandAllocator(type, IID_PPV_ARGS(&native.allocator)),
                  "Create frame allocator");
            check(s.device.device()->CreateCommandList(args.value("NodeMask", UINT(0)), type,
                                                       native.allocator.Get(), initial,
                                                       IID_PPV_ARGS(&native.list)),
                  "Create frame command list");
            if (frame->timeline) {
                const auto &instance = native.instance;
                native.timelineEntry = frame->timeline->begin(native.list.Get(),
                    {{"submission", std::to_string(instance.submission)}, {"queue", std::to_string(instance.queue)},
                     {"command_list", std::to_string(instance.list)}, {"generation", std::to_string(s.index.recordings()[key.first].generation)},
                     {"list_ordinal", std::to_string(instance.ordinal)}, {"first_execution", native.count ? json(std::to_string(instance.firstExecution)) : json(nullptr)},
                     {"execution_count", std::to_string(native.count)}});
            }
            if (frame->measure && frame->streamEnd && type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
                native.statistics = std::make_unique<NativePerformance>(s.device.device(), type);
                native.statistics->begin(native.list.Get());
            }
            if (!native.count) {
                if (native.statistics) native.statistics->end(native.list.Get());
                if (frame->timeline) frame->timeline->end(native.list.Get(), native.timelineEntry);
                check(native.list->Close(), "Close empty frame prefix");
                native.closed = true;
            }
        }
    if (const auto recording = s.index.recordingForRecord(id)) {
        for (auto &[key, native] : recordings) {
            if (key.first != *recording || native.closed)
                continue;
            require(native.list && native.recorded < native.count, "Invalid native recording boundary");
            const auto &original = s.index.recordings()[key.first];
            require(original.records[native.recorded] == id, "Native recording order mismatch");
            const auto executionId = native.instance.firstExecution + native.recorded;
            const bool disabled = s.disabledExecutions.contains(executionId);
            if (s.uavBarriersBefore.contains(executionId)) {
                // A sibling marker keeps synthetic synchronization outside the
                // captured event's API range and original execution identity.
                if (analysisMarkers) {
                    const auto marker = "Flora DX12 experiment UAV barrier before execution " + std::to_string(executionId);
                    native.list->BeginEvent(1, marker.c_str(), UINT(marker.size() + 1));
                }
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                native.list->ResourceBarrier(1, &barrier);
                if (analysisMarkers) native.list->EndEvent();
                s.recordedUavBarriers.insert(executionId);
            }
            if (analysisMarkers) {
                const auto &instance = native.instance;
                const auto marker =
                    std::string(disabled ? "Flora DX12 disabled execution " : "Flora DX12 execution ") + std::to_string(executionId) +
                    " record " + std::to_string(id) + " submission " + std::to_string(instance.submission) +
                    " queue " + std::to_string(instance.queue) + " list " + std::to_string(instance.list) +
                    " generation " + std::to_string(original.generation) + " ordinal " +
                    std::to_string(instance.ordinal) + ": " + streamPacketName(p.type);
                native.list->BeginEvent(1, marker.c_str(), UINT(marker.size() + 1));
            }
            const bool measured = frame->measure && !frame->streamEnd &&
                                  native.instance.firstExecution + native.recorded == frame->execution;
            if (measured) frame->performance->begin(native.list.Get());
            if (disabled) {
                // Validate the signature even when suppressing indirect work.
                // Unsupported state-bearing signatures must not silently pass.
                if (p.type == 0x1ae3ccddf9f8cULL)
                    commands->record(native.list.Get(), id, original.list, native.states, nullptr, true);
            }
            else if (transfer(p.type))
                s.record(native.list.Get(), id, original.list, &native.states);
            else {
                commands->record(native.list.Get(), id, original.list, native.states,
                    s.isolateMissingQueries && p.type == 0x1ae3cdc411c2dULL ? s.queries.details(executionId) : json());
                ++s.recordedCommands[streamPacketName(p.type)];
            }
            if (analysisMarkers)
                native.list->EndEvent();
            if (measured) frame->performance->end(native.list.Get());
            s.recordExecutionPatches(native.list.Get(), executionId, id, native.states);
            if (++native.recorded == native.count) {
                if (native.statistics) native.statistics->end(native.list.Get());
                if (frame->timeline) frame->timeline->end(native.list.Get(), native.timelineEntry);
                check(native.list->Close(), "Close frame execution prefix");
                native.closed = true;
            }
        }
        return;
    }
}
void NativeReplay::Impl::recordExecutionPatches(ID3D12GraphicsCommandList *list, uint64_t execution,
                                                uint64_t record, StateActions &states) {
    for (size_t ordinal = 0; ordinal < resourcePatches.size(); ++ordinal) {
        const auto &patch = resourcePatches[ordinal];
        if (!patch.afterExecution || patch.afterExecution != execution) continue;
        checkCancellation(cancelled);
        auto destination = resources.resource(patch.object, record);
        D3D12_HEAP_PROPERTIES heap{}; D3D12_HEAP_FLAGS flags{};
        check(destination->GetHeapProperties(&heap, &flags), "Read execution patch heap properties");
        require(heap.Type == D3D12_HEAP_TYPE_DEFAULT, "After-execution buffer patches require a default heap");
        D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = patch.bytes.size(); desc.Height = 1; desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const auto allocation = device.device()->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        resources.reserveRetainedAllocation(allocation, record);
        D3D12_HEAP_PROPERTIES uploadHeap{}; uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        ComPtr<ID3D12Resource> upload;
        check(device.device()->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)), "Create execution patch staging");
        void *mapped{}; D3D12_RANGE none{0, 0};
        check(upload->Map(0, &none, &mapped), "Map execution patch staging");
        memcpy(mapped, patch.bytes.data(), patch.bytes.size());
        upload->Unmap(0, nullptr);
        const auto [before, barrierRecord] = executionPatchStates.at(ordinal);
        states.push_back([this, object = patch.object, record, before] {
            const auto current = resources.state(object, record, 0);
            require(current == D3D12_RESOURCE_STATE_COMMON || current == before,
                    "Execution patch state evidence disagrees with submitted resource state");
            resources.requireState(object, record, 0, before);
        });
        const auto marker = "Flora DX12 resource experiment " + std::to_string(ordinal) +
                            " after execution " + std::to_string(execution);
        list->BeginEvent(1, marker.c_str(), UINT(marker.size() + 1));
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {destination, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, D3D12_RESOURCE_STATE_COPY_DEST};
        const bool transition = before != D3D12_RESOURCE_STATE_COPY_DEST;
        if (transition) list->ResourceBarrier(1, &barrier);
        list->CopyBufferRegion(destination, patch.offset, upload.Get(), 0, patch.bytes.size());
        if (transition) { std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter); list->ResourceBarrier(1, &barrier); }
        list->EndEvent();
        patchUploads.push_back(std::move(upload));
        recordedResourcePatches.push_back({{"patch_ordinal", std::to_string(ordinal)},
            {"submission", std::to_string(patch.submission)}, {"execution", std::to_string(execution)},
            {"creation_record", std::to_string(patch.creation)}, {"offset", std::to_string(patch.offset)}, {"subresource", "0"},
            {"bytes", std::to_string(patch.bytes.size())}, {"sha256", hash(patch.bytes)}, {"method", "recorded_buffer_copy"},
            {"scope", "after_execution"}, {"state", uint32_t(before)}, {"state_source_record", std::to_string(barrierRecord)},
            {"state_evidence", barrierRecord > record ? "following_exclusive_write_transition" : "preceding_transition"},
            {"later_commands_may_overwrite", true}});
    }
}
void NativeReplay::Impl::applyResourcePatches(uint64_t submission) {
    for (size_t ordinal = 0; ordinal < resourcePatches.size(); ++ordinal) {
        const auto &patch = resourcePatches[ordinal];
        if (patch.afterExecution || patch.submission != submission) continue;
        checkCancellation(cancelled);
        // Captured queues may reference the same allocation. Complete prior
        // submissions before a CPU map or auxiliary copy modifies it.
        drainFrame(submission, "resource_experiment");
        require(resources.creationRecord(patch.object, submission) == patch.creation, "Resource patch lifetime changed before application");
        auto destination = resources.resource(patch.object, submission); const auto desc = destination->GetDesc();
        D3D12_HEAP_PROPERTIES heap{}; D3D12_HEAP_FLAGS flags{};
        check(destination->GetHeapProperties(&heap, &flags), "Read resource patch heap properties");
        std::string method;
        if (!patch.texture && (heap.Type == D3D12_HEAP_TYPE_UPLOAD || heap.Type == D3D12_HEAP_TYPE_GPU_UPLOAD)) {
            void *mapped{}; D3D12_RANGE none{0, 0}; check(destination->Map(0, &none, &mapped), "Map experiment buffer");
            memcpy(static_cast<uint8_t *>(mapped) + patch.offset, patch.bytes.data(), patch.bytes.size());
            D3D12_RANGE written{SIZE_T(patch.offset), SIZE_T(patch.offset + patch.bytes.size())}; destination->Unmap(0, &written);
            method = "mapped_buffer";
        } else {
            require(heap.Type == D3D12_HEAP_TYPE_DEFAULT, "Resource patches support default or upload heaps");
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT rows{}; UINT64 rowBytes{}, total = patch.bytes.size();
            if (patch.texture) {
                D3D12_FEATURE_DATA_FORMAT_INFO format{desc.Format};
                check(device.device()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &format, sizeof(format)), "Read patch format planes");
                require(patch.subresource < UINT(desc.MipLevels) * (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize) * format.PlaneCount,
                        "Texture patch subresource is out of range");
                device.device()->GetCopyableFootprints(&desc, patch.subresource, 1, 0, &footprint, &rows, &rowBytes, &total);
                require(total && total != UINT64_MAX && rows && rowBytes && footprint.Footprint.Depth &&
                        rowBytes <= patch.bytes.size() / rows / footprint.Footprint.Depth &&
                        rowBytes * rows * footprint.Footprint.Depth == patch.bytes.size(),
                        "Texture patch must contain tightly packed rows for the complete subresource");
            }
            D3D12_RESOURCE_DESC uploadDesc{}; uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            uploadDesc.Width = total; uploadDesc.Height = 1; uploadDesc.DepthOrArraySize = uploadDesc.MipLevels = 1;
            uploadDesc.SampleDesc.Count = 1; uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            const auto allocation = device.device()->GetResourceAllocationInfo(0, 1, &uploadDesc).SizeInBytes;
            require(allocation != UINT64_MAX && allocation <= resources.remainingAllocationBudget(), "Resource patch staging exceeds allocation budget");
            D3D12_HEAP_PROPERTIES uploadHeap{}; uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
            ComPtr<ID3D12Resource> upload;
            check(device.device()->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
                    nullptr, IID_PPV_ARGS(&upload)), "Create resource patch staging buffer");
            void *mapped{}; D3D12_RANGE none{0, 0}; check(upload->Map(0, &none, &mapped), "Map resource patch staging");
            if (patch.texture) {
                for (UINT z = 0; z < footprint.Footprint.Depth; ++z)
                    for (UINT row = 0; row < rows; ++row)
                        memcpy(static_cast<uint8_t *>(mapped) + footprint.Offset + (uint64_t(z) * rows + row) * footprint.Footprint.RowPitch,
                               patch.bytes.data() + (uint64_t(z) * rows + row) * rowBytes, size_t(rowBytes));
            } else memcpy(mapped, patch.bytes.data(), patch.bytes.size());
            upload->Unmap(0, nullptr);
            ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> list;
            check(device.device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create resource patch allocator");
            check(device.device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "Create resource patch commands");
            const auto subresource = patch.texture ? patch.subresource : D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            const auto before = resources.state(patch.object, submission, subresource);
            D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition = {destination, subresource, before, D3D12_RESOURCE_STATE_COPY_DEST};
            const bool transition = before != D3D12_RESOURCE_STATE_COPY_DEST;
            const auto marker = "Flora DX12 resource experiment " + std::to_string(ordinal) + " before submission " + std::to_string(submission);
            list->BeginEvent(1, marker.c_str(), UINT(marker.size() + 1));
            if (transition) list->ResourceBarrier(1, &barrier);
            if (patch.texture) {
                D3D12_TEXTURE_COPY_LOCATION source{}, target{};
                source.pResource = upload.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; source.PlacedFootprint = footprint;
                target.pResource = destination; target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; target.SubresourceIndex = patch.subresource;
                list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
            } else list->CopyBufferRegion(destination, patch.offset, upload.Get(), 0, patch.bytes.size());
            if (transition) { std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter); list->ResourceBarrier(1, &barrier); }
            list->EndEvent(); check(list->Close(), "Close resource patch commands");
            ID3D12CommandQueue *queue{};
            for (const auto &[object, native] : queues) if (queueRecipes.at(object).desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) { queue = native.Get(); break; }
            if (!queue) for (const auto &[object, recipe] : queueRecipes) if (recipe.desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) { queue = this->queue(object); break; }
            require(queue, "Resource patches require a direct replay queue");
            ID3D12CommandList *lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists); wait(queue);
            // The copy restored the legacy state explicitly. Account for
            // buffer/simultaneous-access decay at this auxiliary submission.
            resources.transition(patch.object, submission, subresource, before, before);
            resources.completeSubmission(D3D12_COMMAND_LIST_TYPE_DIRECT);
            method = patch.texture ? "texture_copy" : "buffer_copy";
        }
        appliedResourcePatches.push_back({{"patch_ordinal", std::to_string(ordinal)}, {"submission", std::to_string(submission)}, {"creation_record", std::to_string(patch.creation)},
            {"offset", std::to_string(patch.offset)}, {"subresource", std::to_string(patch.subresource)},
            {"bytes", std::to_string(patch.bytes.size())}, {"sha256", hash(patch.bytes)}, {"method", method},
            {"scope", "before_submission"}, {"later_commands_may_overwrite", true}});
    }
}
void NativeReplay::Impl::executeFrameOperation(uint64_t id) {
    auto &s = *this;
    const auto &p = directory.packets().at(size_t(id - 1));
    auto &recordings = frame->recordings;
    auto &instances = frame->instances;
    if (p.type == 0x125482975df48ULL) {
        submissionExtension(s.directory, p);
        const auto args = decodeArguments(p.type, s.directory.payload(p)).values;
        const auto queueId = value(args, "Queue");
        s.applyResourcePatches(id);
        auto queue = s.queue(queueId);
        std::vector<ID3D12CommandList *> lists;
        json executed = json::array();
        std::map<std::string, uint64_t> pendingCounts;
        for (const auto &instance : s.index.lists()) {
            if (instance.submission != id)
                continue;
            const auto selected = instances.find({id, instance.ordinal});
            if (selected == instances.end())
                continue;
            auto &native = recordings.at(selected->second);
            require(native.closed, "Submitted frame recording is not closed");
            require(native.list->GetType() == s.queueRecipes.at(queueId).desc.Type,
                    "Frame queue/list types disagree");
            for (auto &action : native.states)
                action();
            lists.push_back(native.list.Get());
            const auto &original = s.index.recordings()[selected->second.first];
            for (size_t i = 0; i < native.count; ++i) {
                if (s.disabledExecutions.contains(instance.firstExecution + i))
                    continue;
                ++pendingCounts[streamPacketName(
                    s.directory.packets()[size_t(original.records[i] - 1)].type)];
            }
            executed.push_back({{"list", std::to_string(instance.list)},
                                {"ordinal", std::to_string(instance.ordinal)},
                                {"generation", std::to_string(original.generation)},
                                {"first_execution", std::to_string(instance.firstExecution)},
                                {"execution_count", std::to_string(native.count)}});
        }
        if (s.frame->timeline)
            s.frame->timeline->beforeSubmit(id, queue);
        s.dirtyQueues.insert(queueId);
        if (!lists.empty())
            queue->ExecuteCommandLists(UINT(lists.size()), lists.data());
        s.pendingSubmissions.push_back({id, queueId, std::move(executed), std::move(pendingCounts)});
        ++s.queuedFrameSubmissions;
        s.maxPendingSubmissions = (std::max)(s.maxPendingSubmissions, uint64_t(s.pendingSubmissions.size()));
        // Logical state decay belongs to a submission boundary. It is
        // independent of host completion/accounting of that submission.
        s.resources.completeSubmission(s.queueRecipes.at(queueId).desc.Type);
    } else if (p.type == 0x12548dab1bf2dULL || p.type == 0x12548db3bfde2ULL) {
        Reader r(s.directory.payload(p));
        const auto queue = r.read<uint64_t>(), fence = r.read<uint64_t>(), value = r.read<uint64_t>();
        check(r.read<HRESULT>(), "Captured frame synchronization");
        r.end();
        auto nativeQueue = s.queue(queue);
        auto nativeFence = s.fence(fence);
        s.dirtyQueues.insert(queue);
        if (p.type == 0x12548dab1bf2dULL)
            check(nativeQueue->Signal(nativeFence, value), "Replay frame signal");
        else {
            if (s.frame->timeline) s.frame->timeline->beforeWait(id, queue, nativeQueue);
            check(nativeQueue->Wait(nativeFence, value), "Replay frame wait");
            if (s.frame->timeline) s.frame->timeline->afterWait(id, nativeQueue);
        }
        if (s.frame->timeline)
            s.frame->timeline->synchronization(id, queue, fence, value, p.type == 0x12548db3bfde2ULL);
    }
}
void NativeReplay::Impl::enqueueFrameOperation(uint64_t id) {
    // CPU recording may encounter a consumer before its GPU producer. Keep
    // both native submission and its state checks behind the captured edges.
    // No operation moves ahead of its CPU record; host mutation drains prevent
    // deferred lists from observing later descriptor or mapped-data changes.
    auto &operation = frame->queueOperations.at(id);
    require(!operation.available, "Frame queue operation was already scheduled");
    operation.available = true;
    ++frame->pendingOperations;
    if (operation.remaining)
        ++frame->deferredOperations;
    else
        frame->readyOperations.insert(id);
    while (!frame->readyOperations.empty()) {
        checkCancellation(cancelled);
        const auto next = *frame->readyOperations.begin();
        frame->readyOperations.erase(frame->readyOperations.begin());
        auto &ready = frame->queueOperations.at(next);
        require(ready.available && !ready.remaining && !ready.issued, "Invalid ready queue operation");
        try {
            executeFrameOperation(next);
        } catch (const OperationCancelled &) {
            throw;
        } catch (const std::exception &e) {
            throw std::runtime_error("Scheduled queue record " + std::to_string(next) + ": " + e.what());
        }
        ready.issued = true;
        --frame->pendingOperations;
        ++frame->issuedOperations;
        if (frame->firstIssuedOperations.size() < 128)
            frame->firstIssuedOperations.push_back(
                {{"record", std::to_string(next)}, {"cpu_boundary", std::to_string(id)}});
        for (const auto successor : ready.successors) {
            auto &dependent = frame->queueOperations.at(successor);
            require(dependent.remaining != 0, "Queue dependency was already satisfied");
            if (!--dependent.remaining && dependent.available)
                frame->readyOperations.insert(successor);
        }
    }
}
void NativeReplay::Impl::drainFrame(uint64_t record, const char *reason) {
    if (dirtyQueues.empty())
        return;
    const auto pending = pendingSubmissions.size();
    for (const auto queueId : dirtyQueues) {
        wait(queue(queueId));
        if (frame->timeline) frame->timeline->afterQueueComplete(queueId, queue(queueId));
    }
    for (auto &submission : pendingSubmissions) {
        if (frame->timeline) frame->timeline->afterComplete(submission.record, queue(submission.queue));
        for (const auto &list : submission.lists) {
            const auto first = value(list, "first_execution"), count = value(list, "execution_count");
            for (auto id = uavBarriersBefore.lower_bound(first); id != uavBarriersBefore.end() && *id - first < count; ++id)
                executedUavBarriers.insert(*id);
            for (auto id = disabledExecutions.lower_bound(first); id != disabledExecutions.end() && *id - first < count; ++id)
                suppressedExecutions.insert(*id);
            for (const auto &patch : recordedResourcePatches) {
                const auto execution = value(patch, "execution");
                if (value(patch, "submission") == submission.record && execution >= first && execution - first < count)
                    appliedResourcePatches.push_back(patch);
            }
        }
        for (const auto &[name, count] : submission.counts) executedCommands[name] += count;
        submissions.push_back({{"record", std::to_string(submission.record)}, {"queue", std::to_string(submission.queue)},
                               {"lists", std::move(submission.lists)}});
        ++completedFrameSubmissions;
    }
    ++frameDrainCount;
    if (frameDrains.size() < 128) frameDrains.push_back({{"record", std::to_string(record)}, {"reason", reason},
        {"queues", std::to_string(dirtyQueues.size())}, {"completed_submissions", std::to_string(pending)}});
    pendingSubmissions.clear(); dirtyQueues.clear();
}
void NativeReplay::replayTo(uint64_t execution, bool after, bool analysisMarkers) {
    replay(execution, after, analysisMarkers, false);
}
void NativeReplay::replayFrame(bool analysisMarkers) {
    replay(impl_->index.executionCount(), true, analysisMarkers, true);
}
void NativeReplay::replay(uint64_t execution, bool after, bool analysisMarkers, bool streamEnd) {
    auto &s = *impl_;
    require(s.initialized && !s.frameAttempted, "Frame replay requires a fresh initialized context");
    if (!s.frame)
        prepare(execution, after, analysisMarkers, false, streamEnd);
    s.frameAttempted = true;
    s.selectedExecution = execution;
    s.boundaryAfter = after;
    const auto target = s.index.execution(execution);
    require(target.submission > s.directory.initializationCount() && target.recordingComplete,
            "Select a complete frame execution instance");
    require(s.frame && s.frame->execution == execution && s.frame->after == after &&
                s.frame->markers == analysisMarkers && s.frame->streamEnd == streamEnd,
            "Prepared execution does not match replay request");
    const auto abortOnFailure = qScopeGuard([&] { if (!s.boundaryReached) s.abortFrame(); });
    if (!s.frame->queryPolicyMarker.empty())
        s.queue(target.queue)->SetMarker(1, s.frame->queryPolicyMarker.c_str(), UINT(s.frame->queryPolicyMarker.size() + 1));
    for (uint64_t id = s.directory.initializationCount() + 1; id <= s.frame->lastRecord; ++id) {
        s.currentRecord = id; checkCancellation(s.cancelled); const auto &p = s.directory.packets()[size_t(id - 1)];
        try {
            if (const auto drain = s.frame->hostDrains.find(id); drain != s.frame->hostDrains.end()) {
                require(s.frame->pendingOperations == 0, "Host mutation precedes a deferred queue dependency");
                s.drainFrame(id, drain->second.c_str());
            }
            const auto &waits = s.frame->cpuWaits;
            const auto wait = std::lower_bound(waits.begin(), waits.end(), id,
                                              [](const CpuWait &entry, uint64_t record) { return entry.record < record; });
            if (wait != waits.end() && wait->record == id) {
                // CPU application code is not re-executed: its API arguments and
                // mapped writes are captured. Serialize outstanding GPU work at
                // this boundary; do not invent absent Win32 event/reset state.
                // GPU queue Wait packets retain their separate producer checks.
                std::vector<uint64_t> values;
                for (const auto &dependency : wait->registrations) {
                    const auto completed = s.fence(dependency.fence)->GetCompletedValue();
                    check(s.device.device()->GetDeviceRemovedReason(), "Replay CPU Fence observation");
                    require(completed != UINT64_MAX, "Device removed while draining CPU wait queues");
                    if (!wait->handle)
                        require(completed >= dependency.value,
                                "Synchronous CPU Fence wait requires an unscheduled producer");
                    values.push_back(completed);
                }
                s.completedCpuWaits.emplace(id, std::move(values));
            }
            s.recordFrame(id);
            if (s.index.recordingForRecord(id)) continue;
            if (p.type == 0x20000000000000ULL) s.resources.restoreMappedData(id);
            else if (s.descriptors.apply(id)) {}
            else if (p.type == 0x125482975df48ULL || p.type == 0x12548dab1bf2dULL || p.type == 0x12548db3bfde2ULL)
                s.enqueueFrameOperation(id);
            else if (p.type == 0x125482264c67fULL) {
                s.dirtyQueues.insert(decodeTileMapping(s.directory.payload(p)).queue);
                s.mapTiles(id);
            }
            else if (isResourceRecipe(p.type) || isPipelineRecipe(p.type) || p.type==0x12793fa7d46faULL) {} // Validated indexed recipes, materialized on use.
            else switch (p.type) {
                case 0x1d53218275f72ULL: {
                    Reader r(s.directory.payload(p));
                    const auto swapchain = r.read<uint64_t>();
                    const auto interval = r.read<UINT>(), flags = r.read<UINT>();
                    const auto result = r.read<HRESULT>();
                    r.end();
                    require(swapchain != 0, "Null captured Present swapchain");
                    check(result, "Captured Present");
                    s.frame->presents.push_back({{"record", std::to_string(id)},
                        {"swapchain", std::to_string(swapchain)}, {"sync_interval", interval},
                        {"flags", flags}, {"captured_hresult", uint32_t(result)}, {"displayed", false}});
                    break;
                }
                case 0x12793beef5413ULL: {
                    const auto recipe = allocatorRecipe(s.directory.payload(p), id);
                    require(recipe.object != 0, "Null frame command allocator");
                    require(s.allocatorRecipes.emplace(recipe.object, recipe).second,
                            "Ambiguous frame allocator lifetime");
                    // Native allocators belong to individual submitted recordings;
                    // publish the captured recipe only at its CPU creation boundary.
                    break;
                }
                case 0x27000000000000ULL:
                    // All previously submitted queues were drained above. This
                    // is explicit serialized replay, not Win32 event emulation.
                    break;
                case 0x114699e3dba70ULL: {
                    const auto event = decodeFenceEvent(s.directory.payload(p));
                    (void)s.fence(event.fence);
                    // Retain registrations for diagnostics at the next wait;
                    // replay never uses a captured HANDLE as an OS handle.
                    break;
                }
                case 0x1cb0a721c351aULL: {
                    Reader r(s.directory.payload(p));
                    require(s.allocatorRecipes.contains(r.read<uint64_t>()), "Reset references an unknown allocator");
                    check(r.read<HRESULT>(), "Captured command allocator reset");
                    r.end();
                    // Every submitted recording owns a fresh native allocator.
                    // Retain them until their submitted instance has completed.
                    break;
                }
                case 0x11469e993999dULL: {
                    Reader r(s.directory.payload(p));
                    const auto fence = r.read<uint64_t>();
                    require(s.fenceRecipes.contains(fence),
                            "Completion observation references an unknown fence");
                    const auto observed = r.read<uint64_t>();
                    r.end();
                    auto native = s.fence(fence);
                    const bool waited = native->GetCompletedValue() < observed;
                    s.awaitFence(native, observed);
                    ++s.fenceObservations;
                    if (waited) ++s.waitedFenceObservations;
                    if (s.firstFenceObservations.size() < 128)
                        s.firstFenceObservations.push_back({{"record", std::to_string(id)}, {"fence", std::to_string(fence)},
                            {"observed_value", std::to_string(observed)}, {"native_completed_value", std::to_string(native->GetCompletedValue())},
                            {"waited_for_value", waited}});
                    break;
                }
                case 0x1254809e0c150ULL:
                case 0x1279309e0c150ULL:
                case 0x1ae3c09e0c150ULL: {
                    Reader r(s.directory.payload(p));
                    const auto object = r.read<uint64_t>();
                    require(object != 0, "Null captured interface query");
                    if (p.type == 0x1254809e0c150ULL)
                        require(s.queueRecipes.contains(object), "Interface query references an unknown queue");
                    (void)r.read<GUID>();
                    uint64_t returned{};
                    if (r.flag()) {
                        require(r.read<uint64_t>() == 1, "Invalid captured interface output");
                        returned = r.read<uint64_t>();
                    }
                    const auto hr = r.read<HRESULT>();
                    r.end();
                    require((hr == E_NOINTERFACE && !returned) || (SUCCEEDED(hr) && returned == object),
                            "Captured interface query introduces an unresolved alias or failure");
                    break;
                }
                case 0x1e65015a11fecULL: {
                    Reader r(s.directory.payload(p));
                    (void)s.resources.creationRecord(r.read<uint64_t>(), id);
                    if (r.flag())
                        (void)r.read<D3D12_RESOURCE_DESC>();
                    r.end();
                    break;
                }
                case 0x1e6506360f841ULL: {
                    Reader r(s.directory.payload(p));
                    (void)s.resources.creationRecord(r.read<uint64_t>(), id);
                    require(r.read<ULONG>() != 0, "Invalid captured resource AddRef result");
                    r.end();
                    // Captured reference counts do not own replay COM references.
                    break;
                }
                case 0x1991015a11fecULL: {
                    // StreamPlayback reader RVA 0x5c2960: object and optional
                    // 48-byte heap description, without an HRESULT.
                    Reader r(s.directory.payload(p));
                    require(r.read<uint64_t>() != 0, "Null captured heap description query");
                    if (r.flag()) (void)r.read<D3D12_HEAP_DESC>();
                    r.end();
                    break;
                }
                case 0x152c75a22c40dULL: {
                    Reader r(s.directory.payload(p));
                    require(r.read<uint64_t>() != 0, "Null device-child query");
                    const auto device = output(r, __uuidof(ID3D12Device));
                    bool known = false;
                    for (const auto &[object, recipe] : s.queueRecipes) {
                        Reader creator(
                            s.directory.payload(s.directory.packets().at(size_t(recipe.record - 1))));
                        known |= creator.read<uint64_t>() == device;
                    }
                    require(known, "Device-child query introduces an unresolved device alias");
                    break;
                }
                case 0x1ea7e4eeffcebULL: {
                    Reader r(s.directory.payload(p));
                    require(r.read<uint64_t>() != 0, "Null command-list type query");
                    require(r.read<UINT>() <= D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE,
                            "Unknown captured command-list type");
                    r.end();
                    break;
                }
                case 0x1254841e17bd5ULL: {
                    Reader r(s.directory.payload(p));
                    require(s.queueRecipes.contains(r.read<uint64_t>()),
                            "Timestamp query references an unknown queue");
                    if (r.flag())
                        (void)r.read<uint64_t>();
                    check(r.read<HRESULT>(), "Captured queue timestamp frequency query");
                    r.end();
                    // This is a recorded CPU observation. Native measurements
                    // must query the replay device's frequency independently.
                    break;
                }
                case 0x114696360f841ULL:
                case 0x11469e8691d1cULL:
                case 0x18f056360f841ULL:
                case 0x18f05e8691d1cULL:
                case 0x129496360f841ULL:
                case 0x12949e8691d1cULL:
                case 0x12793e8691d1cULL:
                case 0x12548e8691d1cULL:
                case 0x15a3e6360f841ULL:
                case 0x15a3ee8691d1cULL:
                case 0x1d532e8691d1cULL: {
                    Reader r(s.directory.payload(p));
                    require(r.read<uint64_t>() != 0, "Null captured COM reference");
                    (void)r.read<ULONG>();
                    r.end();
                    // Replay-owned fences, PSOs and root signatures retain their
                    // native references through the selected execution boundary.
                    break;
                }
                case 0x1d53209e0c150ULL: {
                    Reader r(s.directory.payload(p));
                    const auto object = r.read<uint64_t>();
                    require(r.read<GUID>() == __uuidof(IDXGISwapChain3),
                            "Unimplemented swapchain interface query");
                    require(r.flag() && r.read<uint64_t>() == 1 && r.read<uint64_t>() == object,
                            "Swapchain query introduces an unresolved object alias");
                    check(r.read<HRESULT>(), "Captured swapchain interface query");
                    r.end();
                    break;
                }
                case 0x18972bf306bb9ULL: {
                    Reader r(s.directory.payload(p));
                    r.skip(8 + 4 + 4);
                    if (r.flag())
                        r.skip(sizeof(DXGI_QUERY_VIDEO_MEMORY_INFO));
                    check(r.read<HRESULT>(), "Captured video memory query");
                    r.end();
                    break;
                }
                case 0x1d53215a11fecULL: {
                    Reader r(s.directory.payload(p));
                    r.skip(8);
                    if (r.flag())
                        r.skip(64); // Captured 64-byte body in BF1 record 364425; not used for replay state.
                    check(r.read<HRESULT>(), "Captured swapchain description query");
                    r.end();
                    break;
                }
                case 0x1d5320e2ce403ULL: {
                    // StreamPlayback reader RVA 0x5e3e80: object, optional BOOL,
                    // optional counted output interface, then HRESULT.
                    Reader r(s.directory.payload(p));
                    require(r.read<uint64_t>() != 0, "Null swapchain fullscreen query");
                    if (r.flag()) (void)r.read<BOOL>();
                    if (r.flag()) {
                        require(r.read<uint64_t>() == 1, "Invalid fullscreen output count");
                        require(r.read<uint64_t>() == 0, "Fullscreen query introduces an unresolved output interface");
                    }
                    check(r.read<HRESULT>(), "Captured fullscreen state query");
                    r.end();
                    // A CPU observation does not change our offscreen backbuffers.
                    break;
                }
                case 0x1279364bf60fbULL: {
                    Reader r(s.directory.payload(p));
                    r.skip(8);
                    check(r.read<HRESULT>(), "Captured device removal status");
                    r.end();
                    break;
                }
                // These APIs carry no additional GPU commands. Map/Unmap data is
                // restored by CopyOperation, and native allocations outlive all
                // recordings in this bounded execution context.
                case 0x127938b682074ULL:
                case 0x1ae3c721c351aULL:
                case 0x1ae3cd6acf667ULL:
                case 0x1ae3ce8691d1cULL:
                case 0x1ae3c6360f841ULL:
                case 0x1e6503e2ff325ULL:
                case 0x1e650cfcd4902ULL:
                case 0x1e650e8691d1cULL:
                case 0x14538e8691d1cULL:
                case 0x1279331d76605ULL:
                case 0x1e6508ef58ff0ULL:
                case 0x15a3eb3b8e877ULL:
                case 0x15a3edf3ce2bbULL:
                case 0x1af998824f46bULL:
                case 0x28000000000000ULL:
                    break;
                default:
                    throw std::runtime_error("Unsupported frame CPU operation: " + streamPacketName(p.type));
            }
        } catch (const OperationCancelled &) { throw; }
        catch (const std::exception &e) { throw std::runtime_error("Frame record " + std::to_string(id) + ": " + e.what()); }
    }
    require(s.frame->pendingOperations == 0 && s.frame->issuedOperations == s.frame->queueOperations.size(),
            "Selected boundary has unscheduled queue operations");
    s.drainFrame(s.frame->lastRecord, streamEnd ? "frame_stream_end" : "selected_execution_boundary");
    const auto validation = s.device.information();
    require(validation.value("validation_error_count", std::string("0")) == "0", "D3D12 validation reported errors; execution boundary is not accepted");
    s.boundaryReached = true;
    if (!streamEnd && analysisMarkers) {
        const auto marker = rdcResourceBoundaryMarker({{"schema", 1}, {"execution", std::to_string(execution)},
            {"boundary", after ? "after" : "before"}, {"isolate_missing_queries", s.isolateMissingQueries}});
        s.queue(target.queue)->SetMarker(1, marker.c_str(), UINT(marker.size() + 1));
    }
    if (streamEnd && analysisMarkers) {
        const auto marker = rdcFrameEndMarker({{"schema", 1}, {"scope", "frame_stream"},
            {"last_record", std::to_string(s.frame->lastRecord)}, {"last_execution", std::to_string(execution)},
            {"missing_query_policy", s.isolateMissingQueries ? "isolate_readback_outputs" : "fail"}});
        s.queue(target.queue)->SetMarker(1, marker.c_str(), UINT(marker.size() + 1));
    }
}
json NativeReplay::inspectBuffer(uint64_t creation, uint64_t offset, uint64_t length, const std::filesystem::path &output) {
    auto &s = *impl_; require(s.initialized, "Initialization submissions have not completed");
    require(!s.frameAttempted || s.boundaryReached, "Selected execution failed before buffer inspection");
    const auto at = s.boundaryReached ? s.currentRecord : uint64_t(s.directory.initializationCount());
    require(creation && creation <= at, "Buffer creation is outside the inspection boundary");
    const auto &packet = s.directory.packets()[size_t(creation - 1)]; const auto recipe = decodeResourceRecipe(packet.type, s.directory.payload(packet));
    require(creation == s.resources.creationRecord(recipe.object, at), "Selected buffer lifetime is no longer active at the inspection boundary");
    auto source = s.resources.readableBuffer(recipe.object, at, offset, length); const auto desc = source->GetDesc();
    require(desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && length && offset <= desc.Width && length <= desc.Width - offset, "Invalid buffer inspection range");
    require(length <= 256ULL * 1024 * 1024, "Buffer inspection is limited to 256 MiB per request");
    const auto publish = [&](ID3D12Resource *resource, uint64_t mappedOffset) {
        void *mapped{}; D3D12_RANGE read{SIZE_T(mappedOffset), SIZE_T(mappedOffset + length)};
        check(resource->Map(0, &read, &mapped), "Map buffer inspection");
        struct Unmap { ID3D12Resource *resource; ~Unmap() { D3D12_RANGE none{0, 0}; resource->Unmap(0, &none); } } unmap{resource};
        const auto data = static_cast<const uint8_t *>(mapped) + mappedOffset;
        const auto preview = QByteArray(reinterpret_cast<const char *>(data), int(std::min<uint64_t>(length, 256))).toHex().toStdString();
        QCryptographicHash digest(QCryptographicHash::Sha256);
        QSaveFile file(QString::fromStdWString(output.wstring()));
        if (!output.empty()) require(file.open(QIODevice::WriteOnly), "Cannot open buffer output");
        for (uint64_t at = 0; at < length;) {
            checkCancellation(s.cancelled);
            const auto count = qint64(std::min<uint64_t>(1024 * 1024, length - at));
            const auto chunk = reinterpret_cast<const char *>(data + at);
            digest.addData(QByteArrayView(chunk, count));
            if (!output.empty()) require(file.write(chunk, count) == count, "Cannot write buffer output");
            at += uint64_t(count);
        }
        checkCancellation(s.cancelled);
        require(s.device.information().value("validation_error_count", std::string("0")) == "0", "D3D12 validation reported buffer readback errors");
        if (!output.empty()) require(file.commit(), "Cannot publish buffer output");
        return json{{"creation_record", std::to_string(creation)}, {"object", std::to_string(recipe.object)}, {"offset", std::to_string(offset)},
            {"placement", s.resources.placement(creation)},
            {"length", std::to_string(length)}, {"sha256", digest.result().toHex().toStdString()}, {"preview_hex", preview},
            {"source", "native_gpu_readback"}, {"boundary", s.boundaryReached ? (s.boundaryAfter ? "after_event" : "before_event") : "after_initialization_submissions"},
            {"execution", s.boundaryReached ? json(std::to_string(s.selectedExecution)) : json(nullptr)}, {"frame_replayed", false}};
    };
    D3D12_HEAP_PROPERTIES heap{}; D3D12_HEAP_FLAGS flags{};
    check(source->GetHeapProperties(&heap, &flags), "Read buffer heap properties");
    if (heap.Type == D3D12_HEAP_TYPE_READBACK) return publish(source, offset);
    auto readDesc = desc; readDesc.Width = length; readDesc.Alignment = 0; readDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    const auto allocation = s.device.device()->GetResourceAllocationInfo(0, 1, &readDesc).SizeInBytes;
    require(allocation <= s.resources.remainingAllocationBudget(), "Readback exceeds the resource allocation budget");
    D3D12_HEAP_PROPERTIES properties{}; properties.Type = D3D12_HEAP_TYPE_READBACK; ComPtr<ID3D12Resource> readback;
    check(s.device.device()->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &readDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "Create initialized buffer readback");
    ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> list;
    check(s.device.device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create inspection allocator");
    check(s.device.device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "Create inspection list");
    const auto before = s.resources.state(recipe.object, at);
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; barrier.Transition = {source, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, D3D12_RESOURCE_STATE_COPY_SOURCE};
    const bool transition = !(before & D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (transition) list->ResourceBarrier(1, &barrier); list->CopyBufferRegion(readback.Get(), 0, source, offset, length);
    if (transition) { std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter); list->ResourceBarrier(1, &barrier); }
    check(list->Close(), "Close buffer inspection");
    ID3D12CommandQueue *queue{}; for (const auto &[id, native] : s.queues) if (s.queueRecipes.at(id).desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) { queue = native.Get(); break; }
    require(queue, "Buffer inspection needs a direct replay queue"); ID3D12CommandList *lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists); s.wait(queue);
    if (transition) s.resources.transition(recipe.object, at, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, before);
    s.resources.completeSubmission(D3D12_COMMAND_LIST_TYPE_DIRECT);
    return publish(readback.Get(), 0);
}
json NativeReplay::verifyInitializedTexture(uint64_t copyRecord) {
    auto &s = *impl_; require(s.initialized, "Initialization submissions have not completed");
    require(copyRecord && copyRecord <= s.directory.initializationCount(), "Texture copy is outside initialization");
    const auto &packet = s.directory.packets()[size_t(copyRecord - 1)];
    require(packet.type == 0x1ae3c53697cd3ULL, "Selected record is not CopyTextureRegion");
    Reader r(s.directory.payload(packet)); r.skip(8); auto destination = location(r);
    const auto x = r.read<UINT>(), y = r.read<UINT>(), z = r.read<UINT>(); auto source = location(r);
    require(!r.flag() && !x && !y && !z, "Verification requires a complete subresource upload"); r.end();
    require(destination.native.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && source.native.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
        "Verification requires a placed-buffer to texture copy");
    uint64_t submission{};
    for (const auto &instance : s.index.lists()) {
        if (instance.submission > s.directory.initializationCount() || !instance.recording) continue;
        const auto &recording = s.index.recordings()[*instance.recording];
        if (std::find(recording.records.begin(), recording.records.end(), copyRecord) == recording.records.end()) continue;
        require(!submission, "Select a texture copy with one initialization execution instance"); submission = instance.submission;
    }
    require(submission, "Selected texture copy was not executed during initialization");
    uint64_t uploadRecord{}, dataOffset{};
    const auto sourceCreation = s.resources.creationRecord(source.object, copyRecord);
    for (size_t i = 0; i < submission; ++i) {
        const auto &p = s.directory.packets()[i]; if (p.type != 0x20000000000000ULL) continue;
        Reader data(s.directory.payload(p)); if (data.read<uint64_t>() != source.object) continue;
        if (s.resources.creationRecord(source.object, i + 1) != sourceCreation) continue;
        dataOffset = data.read<uint64_t>(); uploadRecord = i + 1;
    }
    require(uploadRecord, "Captured texture upload bytes are missing"); const auto captured = s.directory.resourceData(dataOffset);
    require(s.resources.creationRecord(destination.object, copyRecord) == s.resources.creationRecord(destination.object, s.directory.initializationCount()),
        "Selected texture lifetime is no longer active at the initialization boundary");
    auto texture = s.resources.resource(destination.object, s.directory.initializationCount()); const auto desc = texture->GetDesc();
    const auto subresource = destination.native.SubresourceIndex; D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows{}; UINT64 rowBytes{}, total{};
    s.device.device()->GetCopyableFootprints(&desc, subresource, 1, 0, &footprint, &rows, &rowBytes, &total);
    const auto &original = source.native.PlacedFootprint;
    require(total && total != UINT64_MAX && rows && rowBytes && footprint.Footprint.Width == original.Footprint.Width &&
        footprint.Footprint.Height == original.Footprint.Height && footprint.Footprint.Depth == original.Footprint.Depth &&
        footprint.Footprint.Format == original.Footprint.Format, "Upload footprint does not match the selected full subresource");
    const uint64_t rowCount = uint64_t(rows) * footprint.Footprint.Depth;
    require(rowBytes <= original.Footprint.RowPitch && original.Offset <= captured.bytes.size() && rowBytes <= captured.bytes.size() - original.Offset &&
        rowCount - 1 <= (captured.bytes.size() - original.Offset - rowBytes) / original.Footprint.RowPitch, "Captured upload rows are out of range");
    D3D12_RESOURCE_DESC readDesc{}; readDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; readDesc.Width = total;
    readDesc.Height = 1; readDesc.DepthOrArraySize = 1; readDesc.MipLevels = 1; readDesc.SampleDesc.Count = 1; readDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const auto allocation = s.device.device()->GetResourceAllocationInfo(0, 1, &readDesc).SizeInBytes;
    require(allocation <= s.resources.remainingAllocationBudget(), "Texture readback exceeds the resource allocation budget");
    D3D12_HEAP_PROPERTIES properties{}; properties.Type = D3D12_HEAP_TYPE_READBACK; ComPtr<ID3D12Resource> readback;
    check(s.device.device()->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &readDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "Create initialized texture readback");
    ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> list;
    check(s.device.device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create texture inspection allocator");
    check(s.device.device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "Create texture inspection list");
    const auto before = s.resources.state(destination.object, s.directory.initializationCount(), subresource);
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {texture, subresource, before, D3D12_RESOURCE_STATE_COPY_SOURCE}; const bool transition = !(before & D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (transition) list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION nativeSource{}; nativeSource.pResource = texture; nativeSource.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; nativeSource.SubresourceIndex = subresource;
    D3D12_TEXTURE_COPY_LOCATION nativeDestination{}; nativeDestination.pResource = readback.Get(); nativeDestination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; nativeDestination.PlacedFootprint = footprint;
    list->CopyTextureRegion(&nativeDestination, 0, 0, 0, &nativeSource, nullptr);
    if (transition) { std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter); list->ResourceBarrier(1, &barrier); }
    check(list->Close(), "Close texture inspection"); ID3D12CommandQueue *queue{};
    for (const auto &[id, native] : s.queues) if (s.queueRecipes.at(id).desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) { queue = native.Get(); break; }
    require(queue, "Texture inspection needs a direct replay queue"); ID3D12CommandList *lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists); s.wait(queue);
    void *mapped{}; D3D12_RANGE read{0, SIZE_T(total)}; check(readback->Map(0, &read, &mapped), "Map initialized texture readback");
    QCryptographicHash expected(QCryptographicHash::Sha256), actual(QCryptographicHash::Sha256); uint64_t different{};
    for (uint64_t row = 0; row < rowCount; ++row) {
        const auto a = captured.bytes.data() + original.Offset + row * original.Footprint.RowPitch;
        const auto b = static_cast<const uint8_t *>(mapped) + footprint.Offset + row * footprint.Footprint.RowPitch;
        expected.addData(QByteArrayView(reinterpret_cast<const char *>(a), qsizetype(rowBytes)));
        actual.addData(QByteArrayView(reinterpret_cast<const char *>(b), qsizetype(rowBytes)));
        for (uint64_t byte = 0; byte < rowBytes; ++byte) different += a[byte] != b[byte];
    }
    D3D12_RANGE none{0, 0}; readback->Unmap(0, &none);
    return {{"copy_record", std::to_string(copyRecord)}, {"submission_record", std::to_string(submission)}, {"upload_record", std::to_string(uploadRecord)},
        {"creation_record", std::to_string(s.resources.creationRecord(destination.object, copyRecord))}, {"object", std::to_string(destination.object)}, {"subresource", subresource},
        {"width", footprint.Footprint.Width}, {"height", footprint.Footprint.Height}, {"depth", footprint.Footprint.Depth}, {"format", uint32_t(footprint.Footprint.Format)},
        {"compared_bytes", std::to_string(rowBytes * rowCount)}, {"different_bytes", std::to_string(different)}, {"matched", different == 0},
        {"expected_sha256", expected.result().toHex().toStdString()}, {"actual_sha256", actual.result().toHex().toStdString()},
        {"boundary", "after_initialization_submissions"}, {"row_padding_compared", false}, {"frame_replayed", false}};
}
json NativeReplay::readTexture(uint64_t object, UINT subresource, const std::filesystem::path &output, uint64_t expectedCreation) {
    auto &s = *impl_; require(s.boundaryReached, "Frame execution boundary has not completed");
    require(!expectedCreation || s.resources.creationRecord(object,s.currentRecord)==expectedCreation,
        "Selected resource creation does not match the lifetime at this execution boundary");
    auto texture = s.resources.resource(object, s.currentRecord); const auto desc = texture->GetDesc();
    require(desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER && desc.SampleDesc.Count == 1, "Texture readback requires a single-sample texture");
    D3D12_FEATURE_DATA_FORMAT_INFO format{desc.Format}; check(s.device.device()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &format, sizeof(format)), "Query readback planes");
    require(subresource < UINT(desc.MipLevels) * (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize) * format.PlaneCount, "Readback subresource is out of range");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT rows{}; UINT64 rowBytes{}, total{};
    s.device.device()->GetCopyableFootprints(&desc, subresource, 1, 0, &footprint, &rows, &rowBytes, &total);
    require(total && total != UINT64_MAX && rows && rowBytes, "Unsupported texture readback footprint");
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = total; buffer.Height = 1;
    buffer.DepthOrArraySize = buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const auto allocation = s.device.device()->GetResourceAllocationInfo(0, 1, &buffer).SizeInBytes;
    require(allocation <= s.resources.remainingAllocationBudget(), "Frame readback exceeds the allocation budget");
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK; ComPtr<ID3D12Resource> readback;
    check(s.device.device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "Create frame texture readback");
    ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> list;
    check(s.device.device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create frame readback allocator");
    check(s.device.device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "Create frame readback list");
    const auto before = s.resources.state(object, s.currentRecord, subresource);
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {texture, subresource, before, D3D12_RESOURCE_STATE_COPY_SOURCE}; const bool transition = !(before & D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (transition) list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource = texture; source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; source.SubresourceIndex = subresource;
    D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource = readback.Get(); destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; destination.PlacedFootprint = footprint;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    if (transition) { std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter); list->ResourceBarrier(1, &barrier); }
    check(list->Close(), "Close frame readback"); ID3D12CommandQueue *queue{};
    for (const auto &[id, native] : s.queues) if (s.queueRecipes.at(id).desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) { queue = native.Get(); break; }
    require(queue, "Frame texture readback requires a direct queue"); ID3D12CommandList *lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists); s.wait(queue);
    QSaveFile file(QString::fromStdWString(output.wstring())); require(file.open(QIODevice::WriteOnly), "Cannot open frame texture output");
    void *mapped{}; D3D12_RANGE read{0, SIZE_T(total)}; check(readback->Map(0, &read, &mapped), "Map frame texture readback");
    QCryptographicHash digest(QCryptographicHash::Sha256); uint64_t nonzero{}; std::string imageFile, displayInterpretation="unavailable";
    const uint64_t rowCount = uint64_t(rows) * footprint.Footprint.Depth;
    try {
        for (uint64_t row = 0; row < rowCount; ++row) {
            checkCancellation(s.cancelled); const auto data = static_cast<const uint8_t *>(mapped) + footprint.Offset + row * footprint.Footprint.RowPitch;
            digest.addData(QByteArrayView(reinterpret_cast<const char *>(data), qsizetype(rowBytes)));
            for (uint64_t i = 0; i < rowBytes; ++i) nonzero += data[i] != 0;
            require(file.write(reinterpret_cast<const char *>(data), qint64(rowBytes)) == qint64(rowBytes), "Cannot write frame texture output");
        }
        const auto pixelFormat = footprint.Footprint.Format;
        const bool typelessBgra = pixelFormat == DXGI_FORMAT_B8G8R8A8_TYPELESS;
        const bool bgra = pixelFormat == DXGI_FORMAT_B8G8R8A8_UNORM || pixelFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || typelessBgra;
        const bool rgba = pixelFormat == DXGI_FORMAT_R8G8B8A8_UNORM || pixelFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        if ((bgra || rgba) && footprint.Footprint.Depth == 1) {
            // A typeless resource has no inherent numeric or color-space view.
            // Preview its storage bytes explicitly as UNORM, preserving raw data.
            displayInterpretation = typelessBgra ? "typeless_bgra8_storage_as_unorm_no_color_space_transform"
                                                 : "stored_rgba8_components";
            const QImage image(static_cast<const uchar *>(mapped) + footprint.Offset, int(footprint.Footprint.Width), int(footprint.Footprint.Height),
                qsizetype(footprint.Footprint.RowPitch), bgra ? QImage::Format_ARGB32 : QImage::Format_RGBA8888);
            auto imagePath = output; imagePath.replace_extension(L".png"); QSaveFile png(QString::fromStdWString(imagePath.wstring()));
            require(png.open(QIODevice::WriteOnly) && image.save(&png, "PNG") && png.commit(), "Cannot publish GPU texture PNG"); imageFile = imagePath.filename().string();
        } else if ((pixelFormat == DXGI_FORMAT_R10G10B10A2_UNORM || pixelFormat == DXGI_FORMAT_R11G11B10_FLOAT) && footprint.Footprint.Depth == 1) {
            const bool packedFloat = pixelFormat == DXGI_FORMAT_R11G11B10_FLOAT;
            displayInterpretation = packedFloat ? "unsigned_float_rgb_linear_clamp_0_to_1_nan_zero_inf_one_alpha_one_no_color_space_transform"
                                                : "unorm10_rgb_unorm2_alpha_to_rgba8_no_color_space_transform";
            QImage image(int(footprint.Footprint.Width), int(footprint.Footprint.Height), QImage::Format_RGBA8888);
            require(!image.isNull(), "Cannot allocate packed-color display image");
            for (int y = 0; y < image.height(); ++y) {
                checkCancellation(s.cancelled);
                const auto row = static_cast<const uint8_t *>(mapped) + footprint.Offset + uint64_t(y) * footprint.Footprint.RowPitch;
                auto pixels = image.scanLine(y);
                for (int x = 0; x < image.width(); ++x) {
                    uint32_t packed{};
                    memcpy(&packed, row + size_t(x) * 4, 4);
                    const auto components = packedFloat ? display_r11g11b10(packed) : unpack_rgb10a2(packed);
                    std::copy(components.begin(), components.end(), pixels + size_t(x) * 4);
                }
            }
            auto imagePath = output; imagePath.replace_extension(L".png");
            QSaveFile png(QString::fromStdWString(imagePath.wstring()));
            require(png.open(QIODevice::WriteOnly) && image.save(&png, "PNG") && png.commit(), "Cannot publish packed-color display PNG");
            imageFile = imagePath.filename().string();
        } else if (footprint.Footprint.Depth == 1 && (pixelFormat == DXGI_FORMAT_R16_UNORM || pixelFormat == DXGI_FORMAT_D16_UNORM ||
            (pixelFormat == DXGI_FORMAT_R16_TYPELESS && (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)))) {
            displayInterpretation="unorm16_grayscale_0_to_1";
            QImage image(int(footprint.Footprint.Width),int(footprint.Footprint.Height),QImage::Format_RGBA8888);
            require(!image.isNull(),"Cannot allocate D16 display image");
            for(int y=0;y<image.height();++y) {
                checkCancellation(s.cancelled);
                const auto row=static_cast<const uint8_t *>(mapped)+footprint.Offset+uint64_t(y)*footprint.Footprint.RowPitch;
                auto pixels=image.scanLine(y);
                for(int x=0;x<image.width();++x) {
                    uint16_t value{}; memcpy(&value,row+size_t(x)*2,2);
                    const auto gray=uint8_t((uint32_t(value)*255+32767)/65535);
                    pixels[x*4]=pixels[x*4+1]=pixels[x*4+2]=gray; pixels[x*4+3]=255;
                }
            }
            auto imagePath=output; imagePath.replace_extension(L".png"); QSaveFile png(QString::fromStdWString(imagePath.wstring()));
            require(png.open(QIODevice::WriteOnly)&&image.save(&png,"PNG")&&png.commit(),"Cannot publish D16 display PNG"); imageFile=imagePath.filename().string();
        }
    } catch (...) { D3D12_RANGE none{0, 0}; readback->Unmap(0, &none); throw; }
    D3D12_RANGE none{0, 0}; readback->Unmap(0, &none); require(file.commit(), "Cannot publish frame texture output");
    const UINT arrays=desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize;
    return {{"object", std::to_string(object)}, {"creation_record", std::to_string(s.resources.creationRecord(object, s.currentRecord))}, {"subresource", subresource},
        {"placement", s.resources.placement(s.resources.creationRecord(object, s.currentRecord))},
        {"mip",subresource%desc.MipLevels},{"layer",(subresource/desc.MipLevels)%arrays},
        {"plane",subresource/(UINT(desc.MipLevels)*arrays)},{"samples",desc.SampleDesc.Count},{"dimension",int(desc.Dimension)},
        {"width", footprint.Footprint.Width}, {"height", footprint.Footprint.Height}, {"depth", footprint.Footprint.Depth}, {"format", UINT(footprint.Footprint.Format)},
        {"row_bytes", std::to_string(rowBytes)}, {"rows", rows}, {"bytes", std::to_string(rowBytes * rowCount)}, {"nonzero_bytes", std::to_string(nonzero)},
        {"sha256", digest.result().toHex().toStdString()}, {"execution", std::to_string(s.selectedExecution)}, {"boundary", s.frame->streamEnd ? "frame_end" : s.boundaryAfter ? "after" : "before"},
        {"file", output.filename().string()}, {"image", imageFile}, {"display_interpretation",displayInterpretation}, {"source", "native_gpu_readback"}, {"row_padding_included", false}};
}
json NativeReplay::performanceResult() const {
    auto &s = *impl_;
    require(s.boundaryReached && s.boundaryAfter && s.frame && s.frame->measure &&
                s.selectedExecution == s.frame->execution,
            "Performance results require a completed measured execution");
    if (s.frame->streamEnd) {
        auto timeline = s.frame->timeline->result();
        timeline["scope"] = "frame_stream_submitted_lists";
        timeline["last_record"] = std::to_string(s.frame->lastRecord);
        auto lists = json::array();
        for (const auto &entry : timeline.at("entries")) {
            auto row = entry;
            const auto submission = value(entry, "submission"), ordinal = value(entry, "list_ordinal");
            const auto &recording = s.frame->recordings.at(s.frame->instances.at({submission, ordinal}));
            row["pipeline_statistics"] = nullptr;
            row["pipeline_statistics_status"] = "not_sampled_on_compute_or_copy_list";
            row["pipeline_statistics_query_type"] = nullptr;
            row["mesh_pipeline_statistics"] = {{"status", "not_sampled_on_compute_or_copy_list"}};
            if (recording.statistics) {
                const auto statistics = recording.statistics->result(s.queue(value(entry, "queue")));
                row["pipeline_statistics"] = statistics.at("pipeline_statistics");
                row["pipeline_statistics_status"] = statistics.at("pipeline_statistics_status");
                row["pipeline_statistics_query_type"] = statistics.at("pipeline_statistics_query_type");
                row["mesh_pipeline_statistics"] = statistics.at("mesh_pipeline_statistics");
            }
            lists.push_back(std::move(row));
        }
        return {{"source", "native_d3d12_queries"}, {"scope", "frame_stream"},
            {"execution", std::to_string(s.selectedExecution)}, {"last_record", std::to_string(s.frame->lastRecord)},
            {"frame_complete", false}, {"sample_count", 1}, {"warmup_count", 0}, {"stable_power_state", false},
            {"missing_query_policy", s.isolateMissingQueries ? "isolate_readback_outputs" : "fail"},
            {"query_outputs_incomplete", s.isolateMissingQueries}, {"timeline", std::move(timeline)},
            {"list_measurements", std::move(lists)},
            {"limitations", {"One instrumented full-stream replay sample; clocks are not stabilized. Not original-application timing.",
                "Durations bracket submitted command-list instances, including query instrumentation; they are not individual Draw/Dispatch durations.",
                "Pipeline statistics cover direct lists only. Initialization and experiment helper submissions are excluded.",
                "Cross-queue durations are not summed into a frame time. No vendor hardware counters or GPU Fence stall measurements.",
                s.isolateMissingQueries ? "Historical query outputs remain unavailable; this measures incomplete query-isolated replay."
                                        : "Original-frame fidelity remains unverified."}}};
    }
    require(bool(s.frame->performance), "Selected execution performance queries are missing");
    const auto execution = s.index.execution(s.selectedExecution);
    auto result = s.frame->performance->result(s.queue(execution.queue));
    result["scope"] = "selected_execution";
    if (s.frame->timeline) result["timeline"] = s.frame->timeline->result();
    if (s.isolateMissingQueries) {
        result["missing_query_policy"] = "isolate_readback_outputs";
        result["frame_complete"] = false;
        result["query_outputs_incomplete"] = true;
        result["limitations"].push_back(
            "Measures this incomplete query-isolated replay using independent instrumentation queries. "
            "Historical query values remain unavailable; these timings and statistics are not captured original-frame results.");
    }
    result.update({{"execution", std::to_string(execution.id)}, {"source_record", std::to_string(execution.record)},
                   {"queue", std::to_string(execution.queue)}, {"submission", std::to_string(execution.submission)},
                   {"command_list", std::to_string(execution.list)}, {"generation", std::to_string(execution.generation)},
                   {"list_ordinal", std::to_string(execution.ordinal)},
                   {"command", streamPacketName(s.directory.packets().at(size_t(execution.record - 1)).type)}});
    return result;
}
json NativeReplay::information() const {
    const auto &s = *impl_;
    auto disabled = json::array(), suppressed = json::array();
    for (const auto id : s.disabledExecutions) disabled.push_back(std::to_string(id));
    for (const auto id : s.suppressedExecutions) suppressed.push_back(std::to_string(id));
    auto barrierRequests = json::array(), barrierRecords = json::array(), barrierExecutions = json::array();
    for (const auto id : s.uavBarriersBefore) barrierRequests.push_back(std::to_string(id));
    for (const auto id : s.recordedUavBarriers) barrierRecords.push_back(std::to_string(id));
    for (const auto id : s.executedUavBarriers) barrierExecutions.push_back(std::to_string(id));
    auto patches = json::array();
    for (size_t i = 0; i < s.resourcePatches.size(); ++i) {
        const auto &patch = s.resourcePatches[i];
        const auto ordinal = std::to_string(i);
        const bool applied = std::any_of(s.appliedResourcePatches.begin(), s.appliedResourcePatches.end(),
            [&](const auto &entry) { return entry.at("patch_ordinal") == ordinal; });
        patches.push_back({{"patch_ordinal", ordinal}, {"submission", std::to_string(patch.submission)},
            {"execution", patch.afterExecution ? json(std::to_string(patch.afterExecution)) : json(nullptr)},
            {"creation_record", std::to_string(patch.creation)}, {"offset", std::to_string(patch.offset)},
            {"subresource", std::to_string(patch.subresource)}, {"bytes", std::to_string(patch.bytes.size())},
            {"scope", patch.afterExecution ? "after_execution" : "before_submission"}, {"applied", applied}});
    }
    auto waits = json::array();
    if (s.frame) for (const auto &entry : s.frame->cpuWaits) {
        if (waits.size() == 32) break;
        json wait{{"record", std::to_string(entry.record)}, {"handle", std::to_string(entry.handle)},
                  {"file", s.directory.files().at(entry.file).storage->path().filename().string()},
                  {"kind", entry.handle ? "submitted_queue_drain" : "synchronous_fence_completion"},
                  {"drained", s.completedCpuWaits.contains(entry.record)}};
        auto conditions = json::array();
        for (size_t i = 0; i < entry.registrations.size(); ++i) {
            const auto &event = entry.registrations[i];
            json condition{{"registration", std::to_string(event.registration)}, {"fence", std::to_string(event.fence)},
                           {"value", std::to_string(event.value)}, {"native_completed_value", nullptr},
                           {"condition_reached", nullptr}};
            if (const auto completed = s.completedCpuWaits.find(entry.record); completed != s.completedCpuWaits.end()) {
                condition["native_completed_value"] = std::to_string(completed->second.at(i));
                condition["condition_reached"] = completed->second.at(i) >= event.value;
            }
            conditions.push_back(std::move(condition));
        }
        wait["registrations_since_previous_wait"] = std::move(conditions);
        waits.push_back(std::move(wait));
    }
    return {{"status", s.initialized ? "initialization_transfers_executed" : "incomplete"},
        {"frame_queue_schedule", {{"mode", "captured_queue_fences_with_host_mutation_drains"},
            {"dependencies", s.frame ? s.frame->queueDependencies : json()},
            {"operation_order", "dependency_ready_after_captured_cpu_boundary"},
            {"issued_queue_operations", std::to_string(s.frame ? s.frame->issuedOperations : 0)},
            {"deferred_queue_operations", std::to_string(s.frame ? s.frame->deferredOperations : 0)},
            {"pending_queue_operations", std::to_string(s.frame ? s.frame->pendingOperations : 0)},
            {"first_issued_queue_operations", s.frame ? s.frame->firstIssuedOperations : json::array()},
            {"queued_submissions", std::to_string(s.queuedFrameSubmissions)},
            {"completed_submissions", std::to_string(s.completedFrameSubmissions)},
            {"pending_submissions", std::to_string(s.pendingSubmissions.size())},
            {"maximum_host_pending_submissions", std::to_string(s.maxPendingSubmissions)},
            {"host_drains", std::to_string(s.frameDrainCount)}, {"first_host_drains", s.frameDrains},
            {"aborted_device", s.frameAborted},
            {"fence_observations", std::to_string(s.fenceObservations)},
            {"waited_fence_observations", std::to_string(s.waitedFenceObservations)}, {"first_fence_observations", s.firstFenceObservations},
            {"scope", "Queue operations and resource-state checks follow dependency-ready order, retaining captured identities. "
                      "Signal/Wait execute on the GPU. Fence observations wait only for the captured value. Host drains protect captured CPU waits, "
                      "mapped writes, descriptors, tile mappings, experiments and final readback. Pending counts do not prove "
                      "GPU overlap. Initialization is serialized; complete original CPU concurrency is not restored."}}},
        {"query_replay_coverage", s.queries.information()},
        {"missing_query_policy", s.isolateMissingQueries ? "isolate_readback_outputs" : "fail"},
        {"analysis_query_isolation", s.frame ? s.frame->queryPolicy : json()},
        {"query_readback_availability_valid", s.boundaryReached},
        {"query_command_count_scope", "Original execution instances; an isolated ResolveQueryData records one native call per available contiguous range"},
        {"disabled_executions", std::move(disabled)}, {"suppressed_executions", std::move(suppressed)},
        {"uav_barrier_experiment", {{"before_executions", std::move(barrierRequests)},
            {"recorded_before_executions", std::move(barrierRecords)}, {"executed_before_executions", std::move(barrierExecutions)},
            {"scope", "Additional global UAV barriers before selected execution instances, including disabled ones. "
                      "This changes synchronization and performance; it does not establish original-capture fidelity."}}},
        {"shader_replacements", s.frame ? s.frame->commands->pipelines().shaderReplacementInformation() : json::array()},
        {"resource_patches", std::move(patches)}, {"applied_resource_patches", s.appliedResourcePatches},
        {"last_record", std::to_string(s.currentRecord)}, {"submissions", s.submissions}, {"recorded_commands", s.recordedCommands}, {"executed_commands", s.executedCommands}, {"resources", s.resources.information()},
        {"tile_mapping_calls", std::to_string(s.tileMappingCalls)},
        {"descriptors", s.descriptors.information()},
        {"cpu_waits", {{"scope", "serialized_submitted_queue_barriers"},
                       {"full_event_semantics", false},
                       {"win32_event_state", "creation_reset_and_timeout_results_not_recorded"},
                       {"drained_waits", std::to_string(s.completedCpuWaits.size())},
                       {"count", std::to_string(s.frame ? s.frame->cpuWaits.size() : 0)},
                       {"first_records", std::move(waits)}}},
        {"selected_execution", std::to_string(s.selectedExecution)}, {"boundary", s.boundaryAfter ? "after" : "before"}, {"execution_boundary_reached", s.boundaryReached},
        {"frame_stream_end_reached", s.boundaryReached && s.frame && s.frame->streamEnd},
        {"planned_last_record", s.frame ? json(std::to_string(s.frame->lastRecord)) : json(nullptr)},
        {"offscreen_presents", s.frame ? s.frame->presents : json::array()},
        {"frame_replayed", false}, {"note", "Stream/selected execution coverage does not certify original-frame fidelity; Present observations do not display a swapchain"}};
}
}
