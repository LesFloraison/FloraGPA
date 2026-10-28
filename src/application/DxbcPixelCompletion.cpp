#include "DxbcPixelCompletion.h"
#include <algorithm>
#include <cctype>
#include <set>
namespace flora {
namespace {
uint32_t word(Bytes bytes, size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("Pixel completion signature bounds");
    return Reader(bytes.subspan(offset)).read<uint32_t>();
}
} // namespace
PixelCompletionShader instrumentPixelCompletion(Bytes original, uint32_t target,
                                                std::array<uint32_t, 3> markerBits, uint64_t maxBytes,
                                                const CancelCheck &cancelled) {
    constexpr uint32_t SHDR = 0x52444853, SHEX = 0x58454853, OSGN = 0x4e47534f, OSG5 = 0x3547534f,
                       OSG1 = 0x3147534f;
    if (target >= 8)
        throw std::invalid_argument("Pixel completion target exceeds the RTV limit");
    checkCancellation(cancelled);
    if (!maxBytes || original.size() > maxBytes)
        throw std::runtime_error("Pixel completion shader payload budget exceeded");
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> parts;
    std::set<uint32_t> tags;
    size_t codePart = SIZE_MAX, signaturePart = SIZE_MAX, signatureRow = SIZE_MAX;
    uint32_t componentType = 0, originalMask = 0;
    for (const auto &[tag, bytes] : readDxbcParts(original)) {
        checkCancellation(cancelled);
        if (!tags.insert(tag).second)
            throw std::runtime_error("Duplicate pixel completion DXBC chunk");
        if (tag == SHDR || tag == SHEX) {
            if (codePart != SIZE_MAX)
                throw std::runtime_error("Conflicting pixel completion executables");
            codePart = parts.size();
        }
        if (tag == OSGN || tag == OSG5 || tag == OSG1) {
            if (signaturePart != SIZE_MAX)
                throw std::runtime_error("Conflicting pixel completion output signatures");
            signaturePart = parts.size();
            const auto count = word(bytes, 0);
            const size_t stride = tag == OSGN ? 24 : tag == OSG5 ? 28 : 32;
            if (bytes.size() < 8 || count > 128 || uint64_t(count) * stride > bytes.size() - 8)
                throw std::runtime_error("Pixel completion output signature size");
            for (uint32_t i = 0; i < count; ++i) {
                const auto row = 8 + size_t(i) * stride + (tag == OSGN ? 0 : 4);
                const auto name = word(bytes, row), reg = word(bytes, row + 16);
                if (name < 8 + count * stride || name >= bytes.size() ||
                    std::find(bytes.begin() + name, bytes.end(), 0) == bytes.end())
                    throw std::runtime_error("Pixel completion semantic name bounds");
                if (reg != target)
                    continue;
                std::string semantic(reinterpret_cast<const char *>(bytes.data() + name));
                for (auto &c : semantic)
                    c = char(std::tolower(static_cast<unsigned char>(c)));
                const auto system = word(bytes, row + 8);
                if (signatureRow != SIZE_MAX || semantic != "sv_target" || (system != 0 && system != 64) ||
                    word(bytes, row + 4) != target || (tag != OSGN && word(bytes, row - 4)))
                    throw std::runtime_error("Pixel completion target is not a unique color output");
                signatureRow = row;
                componentType = word(bytes, row + 12);
                originalMask = bytes[row + 20];
                if (componentType < 1 || componentType > 3 || !originalMask || originalMask > 15)
                    throw std::runtime_error("Invalid pixel completion color output");
            }
        }
        parts.emplace_back(tag, std::vector<uint8_t>(bytes.begin(), bytes.end()));
    }
    if (codePart == SIZE_MAX || signatureRow == SIZE_MAX)
        throw std::runtime_error("Pixel completion requires an executable and an existing color target");
    auto program = readDxbcProgram(parts[codePart].second);
    if (program.header[0] != 0x40 && program.header[0] != 0x41 && program.header[0] != 0x50)
        throw std::runtime_error("Pixel completion requires an SM4/SM5 pixel shader");
    auto &signature = parts[signaturePart].second;
    signature[signatureRow + 20] |= 7;
    signature[signatureRow + 21] &= ~uint8_t(7);
    size_t selectedDeclaration = SIZE_MAX;
    std::vector<uint32_t> markerDestination;
    for (size_t i = 0; i < program.instructions.size(); ++i) {
        checkCancellation(cancelled);
        const auto &row = program.instructions[i];
        if ((row[0] & 2047) != 101 || row.size() < 3 || ((row[1] >> 12) & 255) != 2)
            continue;
        if ((row[1] & ~uint32_t(0x800000f0)) != 0x00102002)
            throw std::runtime_error("Unsupported pixel completion output declaration");
        size_t reg = 2;
        for (auto extended = row[1] >> 31; extended;)
            extended = row.at(reg++) >> 31;
        if (reg + 1 != row.size())
            throw std::runtime_error("Invalid pixel completion output declaration length");
        if (row[reg] != target)
            continue;
        if (selectedDeclaration != SIZE_MAX)
            throw std::runtime_error("Repeated pixel completion target declaration");
        selectedDeclaration = i;
        markerDestination.assign(row.begin() + 1, row.end());
        markerDestination[0] = (markerDestination[0] & ~0xf0u) | (7u << 4);
    }
    if (selectedDeclaration == SIZE_MAX)
        throw std::runtime_error("Pixel completion has no target declaration");
    uint32_t returns = 0;
    std::vector<std::vector<uint32_t>> rows;
    std::vector<uint32_t> marker{54};
    marker.insert(marker.end(), markerDestination.begin(), markerDestination.end());
    marker.insert(marker.end(), {0x00004002, markerBits[0], markerBits[1], markerBits[2], 0});
    if (marker.size() > 127)
        throw std::runtime_error("Pixel completion marker instruction exceeds token limit");
    marker[0] |= uint32_t(marker.size()) << 24;
    bool mainBody = true;
    uint64_t addedBytes = 0;
    for (size_t i = 0; i < program.instructions.size(); ++i) {
        checkCancellation(cancelled);
        auto row = program.instructions[i];
        const auto opcode = row[0] & 2047;
        if (opcode == 44)
            mainBody = false;
        if (i == selectedDeclaration)
            row[1] |= 7u << 4;
        // An untaken RETC only changes the diagnostic RGB output; eventual
        // return rewrites it, while DISCARD still suppresses the target write.
        if (mainBody && (opcode == 62 || opcode == 63)) {
            if (maxBytes - original.size() - addedBytes < marker.size() * sizeof(uint32_t))
                throw std::runtime_error("Pixel completion marker payload budget exceeded");
            addedBytes += marker.size() * sizeof(uint32_t);
            rows.push_back(marker);
            ++returns;
        }
        rows.push_back(std::move(row));
    }
    if (!returns)
        throw std::runtime_error("Pixel completion has no main entry return");
    program.instructions = std::move(rows);
    parts[codePart].second = writeDxbcProgram(program);
    const std::set<uint32_t> stale{0x54415453, 0x47424453, 0x42445053, 0x42444c49, 0x49435253, 0x4e444c49};
    DxbcParts assembled;
    for (const auto &[tag, bytes] : parts)
        if (!stale.contains(tag))
            assembled.emplace_back(tag, bytes);
    PixelCompletionShader result;
    result.bytecode = makeDxbc(assembled);
    checkCancellation(cancelled);
    result.identity = {{"schema", 1},
                       {"kind", "native_pixel_completion_shader"},
                       {"backend", "native-pixel-completion/1"},
                       {"source_sha256", sha256(original, cancelled)},
                       {"shader_sha256", sha256(result.bytecode, cancelled)},
                       {"target", target},
                       {"component_type", componentType},
                       {"original_output_mask", originalMask},
                       {"marker_bits", markerBits},
                       {"marker_returns", returns},
                       {"extra_uav_slots", 0},
                       {"extra_input_registers", 0},
                       {"extra_temps", 0},
                       {"alpha_preserved", true},
                       {"requires_private_unblended_target", true},
                       {"original_fragment_eligibility", nullptr}};
    return result;
}
} // namespace flora
