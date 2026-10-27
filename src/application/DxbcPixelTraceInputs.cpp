#include "DxbcPixelTraceInputs.h"
#include "core/Dxbc.h"

namespace flora::dxbc_exec {
PixelTraceOptions pixelTraceInputs(Bytes original, const std::map<uint32_t, PixelRasterInputs> &observed) {
    const Program program(original);
    const auto shaderHash = sha256(original);
    if (program.stage() != 0 || observed.empty())
        throw std::runtime_error("Pixel trace requires observed pixel inputs");
    std::map<uint32_t, uint32_t> masks;
    bool coverageInput = false, sampleFrequency = false;
    for (const auto &instruction : program.instructions()) {
        const auto &row = instruction.words;
        const auto op = instruction.opcode;
        if (op != 95 && (op < 98 || op > 100))
            continue;
        if (row.size() < 2)
            throw std::runtime_error("Truncated pixel input declaration");
        const auto kind = (row[1] >> 12) & 255;
        if (kind == 35 && op == 95) {
            coverageInput = true;
            continue;
        }
        if (kind != 1 || op == 95)
            throw std::runtime_error("Unobserved special pixel input");
        const auto index = (row[1] & 0x80000000u) ? 3u : 2u;
        if (row.size() <= index || row[index] >= 32)
            throw std::runtime_error("Invalid pixel input register");
        masks[row[index]] |= (row[1] >> 4) & 15;
        const auto interpolation = (row[0] >> 11) & 15;
        sampleFrequency |= interpolation == 6 || interpolation == 7 || (op != 98 && row.back() == 10);
    }
    const bool coverageOnly = masks.empty();
    if (coverageOnly)
        masks.emplace(UINT32_MAX, 0);
    if (masks.size() != observed.size())
        throw std::runtime_error("Pixel trace observation register set differs");
    PixelTraceOptions result;
    const auto &first = observed.begin()->second;
    if (first.records.size() != 1)
        throw std::runtime_error("Pixel trace requires a unique observed invocation");
    const auto &anchor = first.records[0].words;
    if (anchor[4][2] > 1 || anchor[4][3] > 1 || anchor[8][1] >= 32 || anchor[8][3] > 1)
        throw std::runtime_error("Pixel trace observation has invalid quad/sample identity");
    result.selectedLane = anchor[4][2] + 2 * anchor[4][3];
    result.activeMask = 0;
    for (unsigned lane = 0; lane < 4; ++lane) {
        const bool active =
            sampleFrequency ? (anchor[6][lane] & (1u << anchor[8][1])) != 0 : anchor[6][lane] != 0;
        if (active)
            result.activeMask |= uint8_t(1u << lane);
        if (coverageInput)
            result.registers[lane][{35, {}}] = {{anchor[6][lane], 0, 0, 0}, 1};
    }
    if (!(result.activeMask & (1u << result.selectedLane)))
        throw std::runtime_error("Selected pixel trace lane has no observed coverage");
    auto passes = nlohmann::json::array();
    for (const auto &[reg, mask] : masks) {
        const auto found = observed.find(reg);
        if (found == observed.end())
            throw std::runtime_error("Pixel trace input register was not observed");
        const auto &pass = found->second;
        const auto &meta = pass.identity;
        if (pass.records.size() != 1 || meta.at("records") != 1 || meta.at("input_register") != reg ||
            meta.at("backend") != "dxbc-pixel-raster-inputs/2" || meta.at("shader_sha256") != shaderHash ||
            meta.at("valid_mask") != mask || meta.at("sample_frequency") != sampleFrequency ||
            meta.at("scope") != "isolated_primitive_pre_depth_inputs" ||
            meta.at("original_fragment_eligibility") != false)
            throw std::runtime_error("Pixel trace input identity or multiplicity differs");
        if (meta.at("x") != anchor[4][0] || meta.at("y") != anchor[4][1] ||
            meta.at("sample") != anchor[8][1] || meta.at("layer") != anchor[8][0])
            throw std::runtime_error("Pixel trace record and selection metadata differ");
        for (const auto *key :
             {"adapter_luid", "adapter_vendor", "adapter_device", "feature_level", "warp", "x", "y", "sample",
              "layer", "raster_samples", "target_samples", "raster_state"})
            if (meta.at(key) != first.identity.at(key))
                throw std::runtime_error("Pixel trace observation contexts differ");
        for (const auto *key : {"primitive_ordinal", "vertices_per_primitive", "source_stride"})
            if (meta.at("geometry").at(key) != first.identity.at("geometry").at(key))
                throw std::runtime_error("Pixel trace geometry identities differ");
        for (unsigned i = 4; i < 9; ++i)
            if (pass.records[0].words[i] != anchor[i])
                throw std::runtime_error("Pixel trace observed quad identities differ across registers");
        if (!coverageOnly)
            for (unsigned lane = 0; lane < 4; ++lane)
                result.registers[lane][{1, {reg}}] = {pass.records[0].words[lane], uint8_t(mask)};
        passes.push_back(meta);
    }
    result.inputProvenance = {{"source", "unique-private-raster-quad/1"},
                              {"scope", "isolated_primitive_pre_depth_inputs"},
                              {"original_fragment_eligibility", false},
                              {"sample_frequency", sampleFrequency},
                              {"coverage_only", coverageOnly},
                              {"selected_raster",
                               {{"depth_bits", anchor[8][2]},
                                {"front_face", anchor[8][3] != 0},
                                {"coverage_mask", anchor[6][result.selectedLane]},
                                {"sample", anchor[8][1]},
                                {"lane", result.selectedLane}}},
                              {"passes", passes}};
    return result;
}
} // namespace flora::dxbc_exec
