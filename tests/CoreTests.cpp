#include "core/Frame.h"
#include <fstream>
#include <iostream>
using namespace flora;
void require(bool value) {
    if (!value)
        throw std::runtime_error("Check failed");
}
template <class F> void rejects(F f) {
    bool rejected = false;
    try {
        f();
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected);
}
template <class T> void put(std::vector<uint8_t> &b, size_t p, T value) {
    std::memcpy(b.data() + p, &value, sizeof value);
}
int main() {
    auto path = std::filesystem::temp_directory_path() / L"flora-core-test.gpa_frame";
    try {
        std::array<uint8_t, 3> tiny{};
        rejects([&] { Reader(tiny).read<uint64_t>(); });
        require(pitches(5, 7, 71) == std::pair<uint32_t, uint32_t>(16, 2));
        rejects([] { pitches(3, 3, 103); });
        rejects([] { pitches(UINT32_MAX, 1, 2); });
        std::vector<uint8_t> b(0x128 + 8 + 24);
        put(b, 0, 0x41504749u);
        put(b, 4, 0x128u);
        put(b, 8, 3u);
        put(b, 12, 1u);
        std::memcpy(b.data() + 0x44, "DX11", 4);
        put(b, 0xf4, uint64_t(0x130));
        put(b, 0x120, 640u);
        put(b, 0x124, 480u);
        put(b, 0x128, 4u);
        put(b, 0x12c, 0x12345678u);
        constexpr Id large = 0xfedcba9876543210ull;
        put(b, 0x130, large);
        put(b, 0x138, uint64_t(0x128));
        put(b, 0x140, 8u);
        b[0x145] = 9;
        put(b, 0x146, uint16_t(1));
        auto save = [&] {
            std::ofstream file(path, std::ios::binary);
            file.write(reinterpret_cast<const char *>(b.data()), b.size());
        };
        save();
        {
            Frame f(path);
            require(f.width() == 640 && f.height() == 480);
            require(f.entry(large).id == large);
            require(Reader(f.data(large)).read<uint32_t>() == 0x12345678);
        }
        put(b, 0x138, UINT64_MAX);
        save();
        rejects([&] { Frame f(path); });
        put(b, 0xf4, UINT64_MAX);
        save();
        rejects([&] { Frame f(path); });
        std::filesystem::remove(path);
        std::cout << "Core checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::filesystem::remove(path);
        std::cerr << e.what() << '\n';
        return 1;
    }
}
