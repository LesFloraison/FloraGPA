#pragma once
// Opt-in, test-process-only tracking of allocations through ucrtbase's heap imports.
#include <Windows.h>
#include <QDir>
#include <QFile>
#include <nlohmann/json.hpp>
#include <array>
#include <cstdlib>
#include <map>
#include <vector>

namespace flora::testing::heapProbe {
struct Record {
    void *pointer{};
    size_t bytes{};
    unsigned epoch{};
    USHORT depth{};
    std::array<void *, 16> stack{};
};
inline constexpr size_t capacity = 1 << 19;
inline Record *records{};
inline SRWLOCK lock = SRWLOCK_INIT;
inline thread_local bool inside = false;
inline bool enabled = false;
inline unsigned epoch = 0;
inline uint64_t dropped = 0;
inline decltype(&HeapAlloc) allocate{};
inline decltype(&HeapReAlloc) reallocate{};
inline decltype(&HeapFree) release{};
inline std::array<void **, 3> importSlots{};
inline std::array<void *, 3> original{};
inline size_t index(void *pointer) { return (reinterpret_cast<uintptr_t>(pointer) >> 4) * 11400714819323198485ull % capacity; }
inline void erase(void *pointer) {
    if (!pointer) return;
    for (size_t n = 0, i = index(pointer); n < capacity; ++n, i = (i + 1) % capacity) {
        if (!records[i].pointer) return;
        if (records[i].pointer == pointer) { records[i].pointer = reinterpret_cast<void *>(1); return; }
    }
}
inline void insert(void *pointer, size_t size) {
    if (!pointer) return;
    for (size_t n = 0, i = index(pointer); n < capacity; ++n, i = (i + 1) % capacity)
        if (reinterpret_cast<uintptr_t>(records[i].pointer) <= 1 || records[i].pointer == pointer) {
            auto &r = records[i]; r.pointer = pointer; r.bytes = size; r.epoch = epoch;
            r.depth = CaptureStackBackTrace(2, USHORT(r.stack.size()), r.stack.data(), nullptr);
            return;
        }
    ++dropped;
}
inline void *WINAPI onAllocate(HANDLE heap, DWORD flags, SIZE_T bytes) {
    if (inside) return allocate(heap, flags, bytes);
    inside = true; AcquireSRWLockExclusive(&lock);
    auto p = allocate(heap, flags, bytes); const auto error = GetLastError();
    if (enabled) insert(p, bytes);
    ReleaseSRWLockExclusive(&lock); inside = false; SetLastError(error); return p;
}
inline void *WINAPI onReallocate(HANDLE heap, DWORD flags, void *old, SIZE_T bytes) {
    if (inside) return reallocate(heap, flags, old, bytes);
    inside = true; AcquireSRWLockExclusive(&lock);
    auto p = reallocate(heap, flags, old, bytes); const auto error = GetLastError();
    if (enabled && p) { erase(old); insert(p, bytes); }
    ReleaseSRWLockExclusive(&lock); inside = false; SetLastError(error); return p;
}
inline BOOL WINAPI onFree(HANDLE heap, DWORD flags, void *p) {
    if (inside) return release(heap, flags, p);
    inside = true; AcquireSRWLockExclusive(&lock);
    const auto ok = release(heap, flags, p); const auto error = GetLastError();
    if (enabled && ok) erase(p);
    ReleaseSRWLockExclusive(&lock); inside = false; SetLastError(error); return ok;
}
inline void writeSlot(void **slot, void *value) {
    DWORD protection{}, ignored{};
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &protection))
        throw std::runtime_error("Cannot change test import protection");
    InterlockedExchangePointer(slot, value);
    VirtualProtect(slot, sizeof(void *), protection, &ignored);
}
inline void start() {
    auto base = reinterpret_cast<uint8_t *>(GetModuleHandleW(L"ucrtbase.dll"));
    if (!base) throw std::runtime_error("No UCRT module for heap probe");
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    auto imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    const char *names[]{"HeapAlloc", "HeapReAlloc", "HeapFree"};
    for (; imports->Name; ++imports) {
        if (!imports->OriginalFirstThunk) continue;
        auto name = reinterpret_cast<IMAGE_THUNK_DATA64 *>(base + imports->OriginalFirstThunk);
        auto thunk = reinterpret_cast<IMAGE_THUNK_DATA64 *>(base + imports->FirstThunk);
        for (; name->u1.AddressOfData; ++name, ++thunk) {
            if (IMAGE_SNAP_BY_ORDINAL64(name->u1.Ordinal)) continue;
            auto symbol = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + name->u1.AddressOfData);
            for (unsigned i = 0; i < 3; ++i)
                if (!strcmp(reinterpret_cast<const char *>(symbol->Name), names[i]))
                    importSlots[i] = reinterpret_cast<void **>(&thunk->u1.Function);
        }
    }
    for (unsigned i = 0; i < 3; ++i) {
        if (!importSlots[i]) throw std::runtime_error("Missing UCRT heap import");
        original[i] = *importSlots[i];
    }
    allocate = reinterpret_cast<decltype(allocate)>(original[0]);
    reallocate = reinterpret_cast<decltype(reallocate)>(original[1]);
    release = reinterpret_cast<decltype(release)>(original[2]);
    records = static_cast<Record *>(VirtualAlloc(nullptr, sizeof(Record) * capacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!records) throw std::runtime_error("Cannot allocate test tracking storage");
    writeSlot(importSlots[0], reinterpret_cast<void *>(onAllocate));
    writeSlot(importSlots[1], reinterpret_cast<void *>(onReallocate));
    writeSlot(importSlots[2], reinterpret_cast<void *>(onFree));
    AcquireSRWLockExclusive(&lock); enabled = true; ReleaseSRWLockExclusive(&lock);
}
inline bool contains(void *pointer, size_t bytes) {
    bool found = false;
    AcquireSRWLockShared(&lock);
    for (size_t n = 0, i = index(pointer); n < capacity; ++n, i = (i + 1) % capacity) {
        if (!records[i].pointer) break;
        if (records[i].pointer == pointer) { found = records[i].bytes == bytes; break; }
    }
    ReleaseSRWLockShared(&lock);
    return found;
}
inline bool selfCheck() {
    auto p = std::malloc(137);
    if (!p) return false;
    bool ok = contains(p, 137);
    auto next = std::realloc(p, 911);
    if (!next) { std::free(p); return false; }
    ok = ok && contains(next, 911) && (next == p || !contains(p, 137));
    std::free(next);
    return ok && !contains(next, 911);
}
inline void mark(unsigned value) {
    AcquireSRWLockExclusive(&lock); epoch = value; ReleaseSRWLockExclusive(&lock);
}
inline void snapshot(const QString &directory, unsigned number) {
    inside = true;
    struct Reset { ~Reset() { inside = false; } } reset;
    std::vector<Record> copy(capacity);
    size_t count = 0; uint64_t lost = 0;
    AcquireSRWLockExclusive(&lock);
    for (size_t i = 0; i < capacity; ++i)
        if (reinterpret_cast<uintptr_t>(records[i].pointer) > 1) copy[count++] = records[i];
    lost = dropped;
    ReleaseSRWLockExclusive(&lock);
    using Key = std::pair<unsigned, std::array<void *, 16>>;
    std::map<Key, std::pair<uint64_t, uint64_t>> groups;
    for (size_t i = 0; i < count; ++i) {
        auto stack = copy[i].stack;
        for (size_t j = copy[i].depth; j < stack.size(); ++j) stack[j] = nullptr;
        auto &g = groups[{copy[i].epoch, stack}]; ++g.first; g.second += copy[i].bytes;
    }
    nlohmann::json rows = nlohmann::json::array();
    for (const auto &[key, value] : groups) {
        nlohmann::json stack = nlohmann::json::array();
        for (auto address : key.second) {
            if (!address) break;
            HMODULE module{}; wchar_t path[32768]{};
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(address), &module);
            if (module) GetModuleFileNameW(module, path, DWORD(std::size(path)));
            stack.push_back({{"module", QString::fromWCharArray(path).toStdString()},
                {"rva", reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module)}});
        }
        rows.push_back({{"epoch", key.first}, {"blocks", value.first}, {"bytes", value.second}, {"stack", stack}});
    }
    QDir().mkpath(directory);
    QFile file(directory + QString("/heap-%1.json").arg(number));
    const auto output = nlohmann::json({{"epoch", number}, {"dropped", lost}, {"blocks", count}, {"groups", rows}}).dump(2);
    if (!file.open(QIODevice::WriteOnly) || file.write(output.data(), output.size()) != qsizetype(output.size()))
        throw std::runtime_error("Cannot save test allocation stacks");
}
inline void stop() {
    AcquireSRWLockExclusive(&lock); enabled = false; ReleaseSRWLockExclusive(&lock);
    for (unsigned i = 0; i < 3; ++i) writeSlot(importSlots[i], original[i]);
    // Keep the fixed tracking storage until process exit; an in-flight observer
    // can still reach it while imports are being restored.
}
} // namespace flora::testing::heapProbe
