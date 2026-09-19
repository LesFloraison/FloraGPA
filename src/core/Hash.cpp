#include "Frame.h"
#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <bcrypt.h>

namespace flora {
std::string sha256(Bytes bytes) {
    struct Algorithm {
        BCRYPT_ALG_HANDLE value{};
        ~Algorithm() {
            if (value)
                BCryptCloseAlgorithmProvider(value, 0);
        }
    } algorithm;
    struct Hash {
        BCRYPT_HASH_HANDLE value{};
        ~Hash() {
            if (value)
                BCryptDestroyHash(value);
        }
    } hash;
    auto check = [](NTSTATUS status) {
        if (status < 0)
            throw std::runtime_error("SHA-256 operation failed");
    };
    check(BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
    check(BCryptCreateHash(algorithm.value, &hash.value, nullptr, 0, nullptr, 0, 0));
    while (!bytes.empty()) {
        auto size = ULONG(std::min<size_t>(bytes.size(), 16 * 1024 * 1024));
        check(BCryptHashData(hash.value, const_cast<PUCHAR>(bytes.data()), size, 0));
        bytes = bytes.subspan(size);
    }
    std::array<uint8_t, 32> digest{};
    check(BCryptFinishHash(hash.value, digest.data(), ULONG(digest.size()), 0));
    std::string result;
    constexpr char hex[] = "0123456789abcdef";
    for (auto x : digest) {
        result.push_back(hex[x >> 4]);
        result.push_back(hex[x & 15]);
    }
    return result;
}
const std::string &Frame::sha256() const {
    std::call_once(hashOnce_, [this] { hash_ = flora::sha256({data_, size_t(size_)}); });
    return hash_;
}
} // namespace flora
