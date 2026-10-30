#include "DxbcCheckpointModel.h"
#include "DxbcPixelCompletion.h"
#include <set>
namespace flora {
namespace {
bool declaration(uint32_t opcode) {
    return (opcode >= 88 && opcode <= 106) || (opcode >= 143 && opcode <= 162) || opcode == 53 ||
           opcode == 206;
}
} // namespace
PixelCompletionShader normalizeSingleSamplePixelCoverage(Bytes original, uint64_t maxBytes,
                                                         const CancelCheck &cancelled) {
    if (!maxBytes || original.size() > maxBytes)
        throw std::runtime_error("Pixel coverage shader payload budget exceeded");
    checkCancellation(cancelled);
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> parts;
    std::set<uint32_t> tags;
    size_t code = SIZE_MAX;
    for (const auto &[tag, bytes] : readDxbcParts(original)) {
        if (!tags.insert(tag).second)
            throw std::runtime_error("Duplicate pixel coverage DXBC chunk");
        if (tag == 0x52444853 || tag == 0x58454853) {
            if (code != SIZE_MAX)
                throw std::runtime_error("Conflicting pixel coverage executables");
            code = parts.size();
        }
        parts.emplace_back(tag, std::vector<uint8_t>(bytes.begin(), bytes.end()));
    }
    if (code == SIZE_MAX)
        throw std::runtime_error("Missing pixel coverage executable");
    auto program = readDxbcProgram(parts[code].second);
    if (program.header[0] != 0x40 && program.header[0] != 0x41 && program.header[0] != 0x50)
        throw std::runtime_error("Pixel coverage requires an SM4/SM5 pixel shader");
    size_t temporaryDeclaration = SIZE_MAX;
    uint32_t temporary = 0, declarations = 0;
    for (size_t i = 0; i < program.instructions.size(); ++i) {
        checkCancellation(cancelled);
        const auto &row = program.instructions[i];
        const auto opcode = row[0] & 2047;
        if (opcode == 101 && row.size() >= 2 && ((row[1] >> 12) & 255) == 15) {
            if (row != std::vector<uint32_t>{0x02000065, 0x0000f000} || ++declarations != 1)
                throw std::runtime_error("Unsupported pixel coverage declaration");
        }
        if (opcode == 104) {
            if (row.size() != 2 || temporaryDeclaration != SIZE_MAX)
                throw std::runtime_error("Invalid pixel coverage temporary declaration");
            temporaryDeclaration = i;
            temporary = row[1];
        }
    }
    PixelCompletionShader result;
    result.identity = {{"schema", 1},
                       {"backend", "native-single-sample-coverage/1"},
                       {"source_sha256", sha256(original, cancelled)},
                       {"changed", bool(declarations)},
                       {"required_samples", 1},
                       {"coverage_declaration_retained", bool(declarations)},
                       {"extra_temps", declarations ? 1 : 0},
                       {"extra_uav_slots", 0},
                       {"original_fragment_eligibility", nullptr}};
    if (!declarations) {
        result.bytecode.assign(original.begin(), original.end());
        result.identity["shader_sha256"] = result.identity["source_sha256"];
        return result;
    }
    if (temporary >= 4096)
        throw std::runtime_error("Pixel coverage requires one available temporary register");
    std::vector<std::vector<uint32_t>> rows;
    uint64_t bytes = 8;
    auto append = [&](std::vector<uint32_t> row) {
        const auto size = row.size() * sizeof(uint32_t);
        if (bytes > maxBytes || size > maxBytes - bytes)
            throw std::runtime_error("Pixel coverage instruction budget exceeded");
        bytes += size;
        rows.push_back(std::move(row));
    };
    bool main = true, inserted = temporaryDeclaration != SIZE_MAX;
    uint32_t returns = 0, writes = 0;
    for (size_t i = 0; i < program.instructions.size(); ++i) {
        checkCancellation(cancelled);
        auto row = program.instructions[i];
        const auto opcode = row[0] & 2047;
        if (!declaration(opcode) && !inserted) {
            append({0x02000068, temporary + 1});
            inserted = true;
        }
        if (i == temporaryDeclaration)
            row[1] = temporary + 1;
        if (opcode == 44)
            main = false;
        // Interface calls have a function-index prefix and no output operands.
        if (!declaration(opcode) && opcode != 120) {
            const auto operands = checkpoint::instructionOperands(row);
            for (auto it = operands.ranges.rbegin(); it != operands.ranges.rend(); ++it) {
                const auto [begin, end] = *it;
                if (((row[begin] >> 12) & 255) != 15)
                    continue;
                if (end != begin + 1 || row[begin] != 0x0000f001)
                    throw std::runtime_error("Unsupported pixel coverage operand");
                row[begin] = 0x00100012;
                row.insert(row.begin() + begin + 1, temporary);
                ++writes;
            }
            if (row.size() > 127)
                throw std::runtime_error("Pixel coverage instruction exceeds token limit");
            row[0] = (row[0] & ~0x7f000000u) | (uint32_t(row.size()) << 24);
        }
        if (main && (opcode == 62 || opcode == 63)) {
            append({0x07000001, 0x00100022, temporary, 0x0010000a, temporary, 0x00004001, 1});
            if (opcode == 63) {
                const auto operands = checkpoint::instructionOperands(row);
                const auto [begin, end] = operands.ranges.at(0);
                std::vector<uint32_t> select{55, 0x00100022, temporary};
                select.insert(select.end(), row.begin() + begin, row.begin() + end);
                const std::vector<uint32_t> coverage{0x0010001a, temporary}, covered{0x00004001, 1};
                const auto &yes = (row[0] & 0x40000) ? coverage : covered;
                const auto &no = (row[0] & 0x40000) ? covered : coverage;
                select.insert(select.end(), yes.begin(), yes.end());
                select.insert(select.end(), no.begin(), no.end());
                if (select.size() > 127)
                    throw std::runtime_error("Pixel coverage conditional return exceeds token limit");
                select[0] |= uint32_t(select.size()) << 24;
                append(std::move(select));
            }
            // Keep the original return and flow depth. An untaken RETC cannot
            // discard a lane, and its shadow coverage remains available later.
            append({0x0300000d, 0x0010001a, temporary});
            append({0x04000036, 0x0000f001, 0x00004001, UINT32_MAX});
            append(std::move(row));
            ++returns;
        } else {
            append(std::move(row));
        }
    }
    if (!returns)
        throw std::runtime_error("Pixel coverage has no main entry return");
    program.instructions = std::move(rows);
    parts[code].second = writeDxbcProgram(program);
    const std::set<uint32_t> stale{0x54415453, 0x47424453, 0x42445053, 0x42444c49, 0x49435253, 0x4e444c49};
    DxbcParts assembled;
    uint64_t total = 32;
    for (const auto &[tag, data] : parts)
        if (!stale.contains(tag)) {
            if (total > maxBytes || uint64_t(data.size()) + 12 > maxBytes - total)
                throw std::runtime_error("Pixel coverage container budget exceeded");
            total += data.size() + 12;
            assembled.emplace_back(tag, data);
        }
    result.bytecode = makeDxbc(assembled);
    if (result.bytecode.size() > maxBytes)
        throw std::runtime_error("Pixel coverage container exceeds payload budget");
    result.identity["shader_sha256"] = sha256(result.bytecode, cancelled);
    result.identity["main_returns"] = returns;
    result.identity["coverage_writes"] = writes;
    result.identity["temporary_register"] = temporary;
    return result;
}
} // namespace flora
