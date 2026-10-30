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
PixelCompletionShader addPixelCompletionTarget(Bytes original, uint32_t target, uint64_t maxBytes,
                                               const CancelCheck &cancelled) {
    if (target >= 8 || !maxBytes || original.size() > maxBytes)
        throw std::invalid_argument("Invalid additional pixel completion target or budget");
    checkCancellation(cancelled);
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> parts;
    size_t code = SIZE_MAX, signature = SIZE_MAX;
    std::set<uint32_t> tags;
    for (const auto &[tag, bytes] : readDxbcParts(original)) {
        checkCancellation(cancelled);
        if (!tags.insert(tag).second)
            throw std::runtime_error("Duplicate additional pixel target DXBC chunk");
        if (tag == 0x52444853 || tag == 0x58454853) {
            if (code != SIZE_MAX)
                throw std::runtime_error("Conflicting additional pixel target executables");
            code = parts.size();
        }
        if (tag == 0x4e47534f || tag == 0x3547534f || tag == 0x3147534f) {
            if (signature != SIZE_MAX)
                throw std::runtime_error("Conflicting additional pixel target signatures");
            signature = parts.size();
        }
        parts.emplace_back(tag, std::vector<uint8_t>(bytes.begin(), bytes.end()));
    }
    if (code == SIZE_MAX || signature == SIZE_MAX)
        throw std::runtime_error("Additional pixel target requires code and output signature");
    auto program = readDxbcProgram(parts[code].second);
    if (program.header[0] != 0x40 && program.header[0] != 0x41 && program.header[0] != 0x50)
        throw std::runtime_error("Additional pixel target requires an SM4/SM5 pixel shader");
    for (const auto &row : program.instructions) {
        checkCancellation(cancelled);
        if ((row[0] & 2047) == 101 && row.size() >= 2 && ((row[1] >> 12) & 255) == 2)
            throw std::runtime_error("Additional pixel target requires a shader without color outputs");
    }
    auto &[tag, output] = parts[signature];
    const uint32_t count = word(output, 0);
    const size_t stride = tag == 0x4e47534f ? 24 : tag == 0x3547534f ? 28 : 32;
    const size_t bias = tag == 0x4e47534f ? 0 : 4, end = 8 + size_t(count) * stride;
    if (count >= 128 || end > output.size())
        throw std::runtime_error("Additional pixel target signature bounds");
    auto put = [&](size_t offset, uint32_t value) { std::memcpy(output.data() + offset, &value, 4); };
    for (uint32_t i = 0; i < count; ++i) {
        const auto row = 8 + size_t(i) * stride + bias;
        const auto name = word(output, row);
        if (name < end || name >= output.size() ||
            std::find(output.begin() + name, output.end(), 0) == output.end() || word(output, row + 16) < 8)
            throw std::runtime_error("Additional pixel target conflicts with an output signature");
    }
    // Color registers precede the special depth/coverage signature entries.
    output.insert(output.begin() + 8, stride, 0);
    for (uint32_t i = 0; i < count; ++i) {
        const auto row = 8 + size_t(i + 1) * stride + bias;
        put(row, word(output, row) + uint32_t(stride));
    }
    put(0, count + 1);
    put(8 + bias, uint32_t(output.size()));
    put(8 + bias + 4, target);
    put(8 + bias + 8, 64);
    put(8 + bias + 12, 3);
    put(8 + bias + 16, target);
    output[8 + bias + 20] = 7;
    constexpr char name[] = "SV_Target";
    output.insert(output.end(), name, name + sizeof name);
    while (output.size() % 4)
        output.push_back(0);
    const auto position =
        std::find_if(program.instructions.begin(), program.instructions.end(), [](const auto &row) {
            const auto op = row[0] & 2047;
            return op == 101 ||
                   !((op >= 88 && op <= 106) || (op >= 143 && op <= 162) || op == 53 || op == 206);
        });
    program.instructions.insert(position, {0x03000065, 0x00102072, target});
    parts[code].second = writeDxbcProgram(program);
    const std::set<uint32_t> stale{0x54415453, 0x47424453, 0x42445053, 0x42444c49, 0x49435253, 0x4e444c49};
    DxbcParts assembled;
    for (const auto &[partTag, bytes] : parts)
        if (!stale.contains(partTag))
            assembled.emplace_back(partTag, bytes);
    PixelCompletionShader result;
    result.bytecode = makeDxbc(assembled);
    if (result.bytecode.size() > maxBytes)
        throw std::runtime_error("Additional pixel target exceeds shader budget");
    result.identity = {{"schema", 1},
                       {"kind", "native_pixel_additional_target"},
                       {"source_sha256", sha256(original, cancelled)},
                       {"shader_sha256", sha256(result.bytecode, cancelled)},
                       {"target", target},
                       {"requires_return_instrumentation", true},
                       {"original_alpha_available", false},
                       {"original_fragment_eligibility", nullptr}};
    return result;
}
static PixelCompletionShader markPixelCompletion(Bytes original, uint32_t target,
                                                 std::array<uint32_t, 3> markerBits, uint64_t maxBytes,
                                                 const CancelCheck &cancelled, bool fullPrecisionMarker) {
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
    // Only the new RGB operation uses full precision. Original declarations and
    // instructions, including integer alpha precision, retain their own operands.
    if (fullPrecisionMarker)
        markerDestination = {0x00102072, target};
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
    if (fullPrecisionMarker)
        result.identity["coverage_output_declared"] =
            std::any_of(program.instructions.begin(), program.instructions.end(),
                        [](const auto &row) { return row == std::vector<uint32_t>{0x02000065, 0x0000f000}; });
    return result;
}
PixelCompletionShader instrumentPixelCompletion(Bytes original, uint32_t target,
                                                std::array<uint32_t, 3> markerBits, uint64_t maxBytes,
                                                const CancelCheck &cancelled) {
    return markPixelCompletion(original, target, markerBits, maxBytes, cancelled, false);
}
PixelCompletionShader instrumentPixelReturnCount(Bytes original, uint32_t target, uint64_t maxBytes,
                                                 const CancelCheck &cancelled) {
    auto result = markPixelCompletion(original, target, {0x3f800000, 0x3f800000, 0x3f800000}, maxBytes,
                                      cancelled, true);
    result.identity["kind"] = "native_pixel_return_count_shader";
    result.identity["backend"] = "native-pixel-return-count-shader/1";
    result.identity["marker_precision"] = "full";
    result.identity["requires_private_unblended_target"] = false;
    result.identity["requires_additive_rgb_float32_target"] = true;
    result.identity["alpha_storage_is_original_bits"] = false;
    return result;
}

} // namespace flora
