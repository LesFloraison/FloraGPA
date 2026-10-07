#pragma once
#include <Windows.h>
#include <Psapi.h>
#include <QFileInfo>
#include <QSaveFile>
#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <vector>
#include <nlohmann/json.hpp>

namespace flora::testing {
// Test-only address/heap metadata. Never reads page contents or changes page
// protection. The walks are sequential, not an atomic process snapshot.
class ProcessMemorySnapshot {
    struct Region {
        MEMORY_BASIC_INFORMATION memory{};
        uint64_t busyBytes{}, busyBlocks{};
    };
    std::vector<Region> regions_;
    size_t capacity_;
    Region *regionAt(uintptr_t address) {
        const auto end = std::upper_bound(regions_.begin(), regions_.end(), address,
            [](uintptr_t value, const Region &row) { return value < uintptr_t(row.memory.BaseAddress); });
        if (end == regions_.begin()) return nullptr;
        auto &row = *std::prev(end);
        return address - uintptr_t(row.memory.BaseAddress) < row.memory.RegionSize ? &row : nullptr;
    }
  public:
    explicit ProcessMemorySnapshot(size_t capacity = 16384) : capacity_(capacity) {
        regions_.reserve(capacity); // Reused, allocated before any observations.
    }
    nlohmann::json save(const QString &path) {
        using Json = nlohmann::json;
        if (QFileInfo::exists(path)) throw std::runtime_error("Memory snapshot already exists");
        regions_.clear();
        PROCESS_MEMORY_COUNTERS_EX before{}, after{};
        if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&before), sizeof before))
            throw std::runtime_error("Cannot sample private commit");
        SYSTEM_INFO system{}; GetSystemInfo(&system);
        const auto maximum = uintptr_t(system.lpMaximumApplicationAddress);
        uintptr_t address = 0;
        bool complete = true;
        DWORD queryError = 0;
        while (address <= maximum) {
            MEMORY_BASIC_INFORMATION memory{};
            if (VirtualQuery(reinterpret_cast<void *>(address), &memory, sizeof memory) != sizeof memory) {
                complete = false; queryError = GetLastError(); break;
            }
            const auto base = uintptr_t(memory.BaseAddress);
            if (regions_.size() == capacity_ || !memory.RegionSize || base > address ||
                memory.RegionSize > std::numeric_limits<uintptr_t>::max() - base ||
                base + memory.RegionSize <= address) {
                complete = false; break;
            }
            regions_.push_back({memory}); // Capacity was reserved before measuring.
            address = base + memory.RegionSize;
        }
        std::array<HANDLE, 256> heaps{};
        const auto heapCount = GetProcessHeaps(DWORD(heaps.size()), heaps.data());
        bool heapComplete = complete && heapCount && heapCount <= heaps.size();
        uint64_t busyBytes = 0, busyBlocks = 0, unmappedBlocks = 0;
        if (heapComplete) for (DWORD i = 0; i < heapCount; ++i) {
            if (!HeapLock(heaps[i])) { heapComplete = false; continue; }
            PROCESS_HEAP_ENTRY entry{};
            // No allocations, Qt calls or I/O while a process heap is locked.
            while (HeapWalk(heaps[i], &entry)) if (entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) {
                busyBytes += entry.cbData; ++busyBlocks;
                auto row = regionAt(uintptr_t(entry.lpData));
                if (row && row->memory.State == MEM_COMMIT) {
                    row->busyBytes += entry.cbData; ++row->busyBlocks;
                } else ++unmappedBlocks; // Address map and heap walk are not atomic.
            }
            const auto error = GetLastError();
            HeapUnlock(heaps[i]);
            heapComplete = heapComplete && error == ERROR_NO_MORE_ITEMS;
        }
        if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&after), sizeof after))
            throw std::runtime_error("Cannot resample private commit");
        // Serialization happens after all locks are released. Its allocator
        // footprint can affect a later snapshot, so a warm-up file is retained.
        struct Allocation { uint64_t committed{}, reserved{}, privateCommit{}, busyBytes{}, busyBlocks{}; };
        std::map<uintptr_t, Allocation> allocations;
        uint64_t privateCommit = 0, mappedCommit = 0, imageCommit = 0;
        Json rows = Json::array();
        for (const auto &row : regions_) {
            const auto &m = row.memory;
            if (m.State == MEM_FREE) continue;
            auto &a = allocations[uintptr_t(m.AllocationBase)];
            if (m.State == MEM_COMMIT) {
                a.committed += m.RegionSize;
                if (m.Type == MEM_PRIVATE) { privateCommit += m.RegionSize; a.privateCommit += m.RegionSize; }
                else if (m.Type == MEM_MAPPED) mappedCommit += m.RegionSize;
                else if (m.Type == MEM_IMAGE) imageCommit += m.RegionSize;
            } else if (m.State == MEM_RESERVE) a.reserved += m.RegionSize;
            a.busyBytes += row.busyBytes; a.busyBlocks += row.busyBlocks;
            rows.push_back({{"base",uintptr_t(m.BaseAddress)}, {"allocation_base",uintptr_t(m.AllocationBase)},
                {"bytes",m.RegionSize}, {"state",m.State}, {"type",m.Type}, {"protect",m.Protect},
                {"heap_busy_bytes",row.busyBytes}, {"heap_busy_blocks",row.busyBlocks}});
        }
        Json groups = Json::array();
        uint64_t heapAssociatedCommit = 0;
        for (const auto &[base,a] : allocations) {
            if (a.busyBlocks) heapAssociatedCommit += a.privateCommit;
            groups.push_back({{"base",base}, {"committed",a.committed}, {"reserved",a.reserved},
                {"private_committed",a.privateCommit}, {"heap_busy_bytes",a.busyBytes}, {"heap_busy_blocks",a.busyBlocks}});
        }
        Json summary{{"address_walk_complete",complete}, {"query_error",queryError},
            {"heap_walk_complete",heapComplete}, {"unmapped_heap_blocks",unmappedBlocks},
            {"heap_count",heapCount}, {"heap_busy_bytes",busyBytes}, {"heap_busy_blocks",busyBlocks},
            {"private_bytes_before",before.PrivateUsage}, {"private_bytes_after",after.PrivateUsage},
            {"private_committed",privateCommit}, {"mapped_committed",mappedCommit}, {"image_committed",imageCommit},
            {"private_committed_in_heap_allocations",heapAssociatedCommit},
            {"private_committed_in_other_allocations",privateCommit-heapAssociatedCommit},
            {"regions",regions_.size()}, {"region_capacity",capacity_}};
        Json report{{"schema","FloraGPA process address and heap snapshot 1"}, {"summary",summary},
            {"regions",std::move(rows)}, {"allocations",std::move(groups)},
            {"scope","Current test process, sequential metadata walks; no page contents. Heap association means the allocation contains observed busy heap blocks. PrivateUsage includes commit not fully classified by VirtualQuery, including copy-on-write pages."}};
        const auto text = report.dump(2);
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(text.data(), qint64(text.size())) != qint64(text.size()) || !file.commit())
            throw std::runtime_error("Cannot save complete memory snapshot");
        return summary;
    }
};
} // namespace flora::testing
