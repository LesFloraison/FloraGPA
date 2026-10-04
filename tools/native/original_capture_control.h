#pragma once
// Development-only control of the unmodified, pinned GPA 2025 R1 shim.
// Reproduce its own active-swap-chain selector before CaptureNextFrame when
// automatic primary selection has not run. Never link this into FloraGPA.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <array>
#include <bcrypt.h>
#include <cstring>
#include <dxgi.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace flora::research {
inline bool selectOriginalPrimarySwapChain(HMODULE module, IDXGISwapChain *swap) {
    if (!module || !swap)
        throw std::runtime_error("Original capture control requires a shim and swap chain");
    wchar_t path[32768]{};
    auto length = GetModuleFileNameW(module, path, DWORD(std::size(path)));
    if (!length || length == std::size(path))
        throw std::runtime_error("Cannot identify loaded capture shim");
    std::ifstream file(std::filesystem::path(path), std::ios::binary | std::ios::ate);
    auto size = file.tellg();
    if (!file || size <= 0 || size > 128 * 1024 * 1024)
        throw std::runtime_error("Invalid original capture shim file");
    std::vector<uint8_t> bytes(size_t(size), uint8_t{});
    file.seekg(0);
    if (!file.read(reinterpret_cast<char *>(bytes.data()), size))
        throw std::runtime_error("Cannot read original capture shim");
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Cannot initialize capture shim hash");
    std::array<uint8_t, 32> hash{};
    auto status =
        BCryptHash(algorithm, nullptr, 0, bytes.data(), ULONG(bytes.size()), hash.data(), ULONG(hash.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    constexpr std::array<uint8_t, 32> expected{
        0xcb, 0x0b, 0x99, 0xd1, 0x13, 0x51, 0x1c, 0xbf, 0x7c, 0xa9, 0xf9, 0x49, 0xd9, 0x7a, 0xa0, 0x2e,
        0x4a, 0x1f, 0x8f, 0x11, 0x0b, 0x1f, 0x3a, 0xd3, 0x16, 0x5d, 0x8a, 0x81, 0x03, 0x5d, 0xee, 0x31};
    if (status < 0 || hash != expected)
        throw std::runtime_error("Original primary selection requires the pinned GPA 2025 R1 shim");
    auto base = reinterpret_cast<uint8_t *>(module);
    constexpr uint8_t prologue[]{0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x56, 0x57,
                                 0x41, 0x56, 0x48, 0x83, 0xec, 0x20, 0x48, 0x83, 0x79, 0x18, 0x00, 0x48};
    if (std::memcmp(base + 0x3265f0, prologue, sizeof(prologue)) ||
        reinterpret_cast<void *>(GetProcAddress(module, "CaptureNextFrame")) != base + 0x2ede80)
        throw std::runtime_error("Original capture entry point does not match pinned code");
    // Ghidra: SwapChain registry +0x30 is its recursive lock; +0x60 is the
    // primary entry pointer. The original selector is called under this lock.
    auto registry = base + 0x65d918;
    auto lock = registry + 0x30;
    auto table = *reinterpret_cast<void ***>(lock);
    using Lock = void (*)(void *);
    using Select = uintptr_t (*)(void *);
    struct Unlock {
        void *object;
        Lock release;
        ~Unlock() { release(object); }
    };
    reinterpret_cast<Lock>(table[0])(lock);
    const Unlock unlock{lock, reinterpret_cast<Lock>(table[1])};
    auto primary = [&] {
        auto entry = *reinterpret_cast<uintptr_t **>(registry + 0x60);
        return entry ? entry[0] : uintptr_t{};
    };
    if (primary() == uintptr_t(swap))
        return false;
    reinterpret_cast<Select>(base + 0x3265f0)(registry);
    if (primary() != uintptr_t(swap))
        throw std::runtime_error("Original selector did not choose this probe's active swap chain");
    return true;
}
} // namespace flora::research
