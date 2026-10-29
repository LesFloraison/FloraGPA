#include "DxbcPixelCompletion.h"
#include "DxbcPixelInputs.h"
#include <algorithm>
#include <bit>
#include <set>
namespace flora {
PixelCompletionShader instrumentPixelPrimitiveMask(Bytes original, uint32_t target,
                                                   PixelPrimitiveMask selection, uint64_t maxBytes,
                                                   const CancelCheck &cancelled,
                                                   std::optional<PixelPrimitiveInput> producer) {
    if (!selection.mask || (selection.value & ~selection.mask))
        throw std::invalid_argument("Invalid primitive identity mask");
    // Low bits also remain nonzero when an integer target narrows the marker.
    const std::array<uint32_t, 3> marker{0x3f800001, 0x40000001, 0x40400001};
    auto result = instrumentPixelCompletion(original, target, marker, maxBytes, cancelled);
    const auto inputs = dxbc_exec::pixelShaderInputSignature(original);
    uint32_t input = UINT32_MAX, component = 0;
    std::set<uint32_t> used;
    for (const auto &field : inputs) {
        used.insert(field.reg);
        if (field.system != 7)
            continue;
        if (input != UINT32_MAX || field.type != 1 || !std::has_single_bit(field.mask) || field.reg >= 32 ||
            field.precision)
            throw std::runtime_error("Ambiguous or imprecise primitive identity input");
        input = field.reg;
        component = std::countr_zero(field.mask);
    }
    const bool addedInput = input == UINT32_MAX;
    if (addedInput) {
        if (producer) {
            input = producer->reg;
            component = producer->component;
            if (input >= 32 || component >= 4)
                throw std::runtime_error("Invalid primitive identity producer register");
            for (const auto &field : inputs)
                if (field.reg == input && (field.mask & (1u << component)))
                    throw std::runtime_error("Primitive identity producer overlaps an original input");
        } else
            for (input = 0; input < 32 && used.contains(input); ++input) {
            }
        if (input == 32)
            throw std::runtime_error("No input register for primitive identity observation");
    }
    if (producer && (producer->reg != input || producer->component != component))
        throw std::runtime_error("Primitive identity producer and consumer registers differ");
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> parts;
    size_t codeIndex = SIZE_MAX;
    for (const auto &[tag, bytes] : readDxbcParts(result.bytecode)) {
        checkCancellation(cancelled);
        auto &part = parts.emplace_back(tag, std::vector<uint8_t>(bytes.begin(), bytes.end())).second;
        if (tag == 0x52444853 || tag == 0x58454853)
            codeIndex = parts.size() - 1;
        if (tag != 0x4e475349 && tag != 0x31475349)
            continue;
        auto put = [&](size_t offset, uint32_t value) { std::memcpy(part.data() + offset, &value, 4); };
        const auto count = Reader(bytes).read<uint32_t>();
        const size_t stride = tag == 0x31475349 ? 32 : 24, bias = tag == 0x31475349 ? 4 : 0;
        if (addedInput) {
            const size_t end = 8 + count * stride;
            part.insert(part.begin() + end, stride, 0);
            for (uint32_t i = 0; i < count; ++i) {
                const auto offset = 8 + i * stride + bias;
                put(offset, Reader(bytes.subspan(offset)).read<uint32_t>() + uint32_t(stride));
            }
            put(0, count + 1);
            put(end + bias, uint32_t(part.size()));
            put(end + bias + 8, 7);
            put(end + bias + 12, 1);
            put(end + bias + 16, input);
            part[end + bias + 20] = part[end + bias + 21] = uint8_t(1u << component);
            constexpr char name[] = "SV_PrimitiveID";
            part.insert(part.end(), name, name + sizeof name);
            while (part.size() % 4)
                part.push_back(0);
        } else {
            for (uint32_t i = 0; i < count; ++i) {
                const auto offset = 8 + i * stride + bias;
                if (Reader(bytes.subspan(offset + 8)).read<uint32_t>() == 7)
                    part[offset + 21] |= uint8_t(1u << component);
            }
        }
    }
    auto program = readDxbcProgram(parts.at(codeIndex).second);
    uint32_t temporary = 0;
    bool temps = false, declaredInput = addedInput;
    for (auto &row : program.instructions) {
        const auto opcode = row[0] & 2047;
        if (opcode == 104) {
            if (temps || row.size() != 2 || row[1] >= 4096)
                throw std::runtime_error("Primitive identity temporary register limit");
            temps = true;
            temporary = row[1]++;
        }
        if ((opcode == 99 || opcode == 100) && row.size() >= 4 && row.back() == 7) {
            if (declaredInput || row.size() != 4 || row[2] != input ||
                ((row[1] >> 4) & 15) != (1u << component))
                throw std::runtime_error("Primitive identity declaration differs from signature");
            declaredInput = true;
        }
    }
    if (!declaredInput)
        throw std::runtime_error("Primitive identity input has no declaration");
    if (addedInput) {
        auto position =
            std::find_if(program.instructions.begin(), program.instructions.end(), [](const auto &row) {
                const auto op = row[0] & 2047;
                return op >= 101 && op <= 106;
            });
        program.instructions.insert(position, {0x04000063, 0x00101002 | (1u << (component + 4)), input, 7});
    }
    if (!temps) {
        auto position =
            std::find_if(program.instructions.begin(), program.instructions.end(), [](const auto &row) {
                const auto op = row[0] & 2047;
                return op < 88 && op != 53;
            });
        program.instructions.insert(position, {0x02000068, 1});
    }
    std::vector<std::vector<uint32_t>> rows;
    bool main = true;
    uint32_t rewritten = 0;
    for (size_t i = 0; i < program.instructions.size(); ++i) {
        checkCancellation(cancelled);
        const auto &row = program.instructions[i];
        const auto opcode = row[0] & 2047;
        if (opcode == 44)
            main = false;
        const auto next = i + 1 < program.instructions.size() ? program.instructions[i + 1][0] & 2047 : 0;
        if (!main || (next != 62 && next != 63)) {
            rows.push_back(row);
            continue;
        }
        if (opcode != 54 || row.size() < 8 || row[row.size() - 5] != 0x4002)
            throw std::runtime_error("Primitive identity completion marker differs");
        rows.push_back({0x07000001, 0x00100012, temporary, 0x0010100a | (component << 4), input, 0x00004001,
                        selection.mask});
        rows.push_back({selection.notEqual ? 0x07000027u : 0x07000020u, 0x00100012, temporary, 0x0010000a,
                        temporary, 0x00004001, selection.value});
        std::vector<uint32_t> select{55};
        select.insert(select.end(), row.begin() + 1, row.end() - 5);
        select.insert(select.end(), {0x0010000a, temporary});
        select.insert(select.end(), row.end() - 5, row.end());
        select.insert(select.end(), {0x4002, 0, 0, 0, 0});
        select[0] |= uint32_t(select.size()) << 24;
        rows.push_back(std::move(select));
        ++rewritten;
    }
    if (rewritten != result.identity.at("marker_returns"))
        throw std::runtime_error("Incomplete primitive identity marker rewrite");
    program.instructions = std::move(rows);
    parts[codeIndex].second = writeDxbcProgram(program);
    DxbcParts assembled;
    for (const auto &[tag, bytes] : parts)
        assembled.emplace_back(tag, bytes);
    result.bytecode = makeDxbc(assembled);
    if (result.bytecode.size() > maxBytes)
        throw std::runtime_error("Primitive identity shader payload budget exceeded");
    result.identity.update({{"kind", "native_pixel_primitive_mask_shader"},
                            {"backend", "native-pixel-primitive-mask/1"},
                            {"shader_sha256", sha256(result.bytecode, cancelled)},
                            {"primitive_mask", selection.mask},
                            {"primitive_value", selection.value},
                            {"input_register", input},
                            {"input_component", component},
                            {"extra_input_registers", addedInput && !used.contains(input) ? 1 : 0},
                            {"extra_input_components", addedInput ? 1 : 0},
                            {"extra_temps", 1},
                            {"temporary_register", temporary},
                            {"requires_primitive_id_producer", true},
                            {"nonmatching_rgb_zero", true}});
    if (selection.notEqual) {
        result.identity["backend"] = "native-pixel-primitive-mask/2";
        result.identity["primitive_comparison"] = "not_equal";
    }
    return result;
}
PixelCompletionShader instrumentPixelPrimitivePresence(Bytes original, uint32_t target,
                                                       PixelPrimitiveMask selection, uint64_t maxBytes,
                                                       const CancelCheck &cancelled,
                                                       std::optional<PixelPrimitiveInput> producer) {
    auto result = instrumentPixelPrimitiveMask(original, target, selection, maxBytes, cancelled, producer);
    if (result.identity.at("component_type") != 3)
        throw std::runtime_error("Private primitive presence requires a floating-point shader output");
    result.identity.update({{"kind", "native_pixel_primitive_presence_shader"},
                            {"backend", "native-pixel-primitive-presence-shader/1"},
                            {"requires_private_unblended_target", false},
                            {"requires_private_max_rgb_target", true},
                            {"requires_zero_initial_rgb", true},
                            {"requires_alpha_replace", true}});
    if (selection.notEqual)
        result.identity["backend"] = "native-pixel-primitive-presence-shader/2";
    return result;
}
} // namespace flora
