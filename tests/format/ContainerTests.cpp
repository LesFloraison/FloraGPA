#include "format/Container.h"
#include "format/Writer.h"
#include <fstream>
#include <iostream>

using namespace flora::format;
namespace {
void require(bool ok) { if (!ok) throw std::runtime_error("Container assertion failed"); }
template<class F> void rejected(F action) {
    bool failed = false;
    try { action(); } catch (const std::runtime_error &) { failed = true; }
    require(failed);
}
}
int main(int argc, char **argv) {
    try {
        require(argc == 2);
        const std::filesystem::path root(argv[1]);
        std::filesystem::create_directories(root);
        const auto original = root / "valid.gpa_frame";
        std::filesystem::remove(original);
        Header h{};
        std::memcpy(h.data(), "IGPA", 4);
        wire_write<uint32_t>(h.data() + 4, HEADER_SIZE);
        wire_write<uint32_t>(h.data() + 8, VERSION);
        std::memcpy(h.data() + API_OFFSET, "DX11", 4);
        wire_write<uint32_t>(h.data() + DIMENSIONS_OFFSET, 8);
        const std::vector<uint8_t> payload{1, 2, 3};
        { ContainerWriter writer(original, h);
          writer.append({20, 9, 1, payload.data(), payload.size()});
          writer.append({30, 9, 1, payload.data(), payload.size()}); writer.finish(); }
        std::vector<uint8_t> bytes;
        { Container valid(original);
          require(valid.entries().size() == 2 && valid.width() == 8);
          require(valid.payload(20, 9, 1).size() == 3);
          rejected([&] { valid.payload(21); });
          rejected([&] { valid.payload(20, 5); });
          bytes.assign(valid.bytes().begin(), valid.bytes().end()); }
        const auto damaged = root / "invalid.gpa_frame";
        auto invalid = [&](auto modify) {
            auto copy = bytes; modify(copy);
            { std::ofstream out(damaged, std::ios::binary | std::ios::trunc);
              out.write(reinterpret_cast<const char *>(copy.data()), copy.size()); }
            rejected([&] { Container frame(damaged); });
            // Failed construction must release all Windows file handles.
            require(std::filesystem::remove(damaged));
        };
        invalid([](auto &b) { b.resize(5); });
        invalid([](auto &b) { b[0] = 0; });
        invalid([](auto &b) { b[API_OFFSET] = 'X'; });
        invalid([](auto &b) { wire_write<uint32_t>(b.data() + ENTRY_COUNT_OFFSET, MAX_ENTRIES + 1); });
        invalid([](auto &b) { wire_write<uint64_t>(b.data() + INDEX_OFFSET, UINT64_MAX); });
        invalid([](auto &b) { b.pop_back(); });
        const auto table = wire_read<uint64_t>(bytes.data() + INDEX_OFFSET);
        invalid([&](auto &b) { wire_write<uint64_t>(b.data() + table + ENTRY_SIZE, 20); });
        invalid([&](auto &b) { wire_write<uint64_t>(b.data() + table + 8, table); });
        invalid([&](auto &b) { wire_write<uint32_t>(b.data() + table + 16, UINT32_MAX); });
        unsigned checkpoints = 0;
        rejected([&] { Container frame(original, [&] { if (++checkpoints == 2) throw std::runtime_error("cancel"); }); });
        require(checkpoints == 2);
        require(std::filesystem::remove(original));
        std::cout << "Checked container bounds, identities, cancellation and cleanup passed\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
