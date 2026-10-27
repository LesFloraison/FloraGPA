#include "NativePixelOutputs.h"
#include "core/Dxbc.h"
#include <algorithm>
#include <cctype>
#include <set>

namespace flora {
namespace {
using namespace dxbc_exec;
using Json = nlohmann::json;
struct Output {
    std::string semantic, kind, numeric;
    uint32_t index, mask, precision;
    Register reg;
};
uint32_t word(Bytes bytes, size_t at) {
    if (at > bytes.size() || bytes.size() - at < sizeof(uint32_t))
        throw std::runtime_error("Truncated pixel output signature word");
    return Reader(bytes.subspan(at)).read<uint32_t>();
}
std::vector<Output> outputs(Bytes bytecode) {
    Bytes signature;
    uint32_t tag = 0;
    for (const auto &[kind, bytes] : readDxbcParts(bytecode))
        if (kind == 0x4e47534f || kind == 0x3547534f || kind == 0x3147534f) {
            if (tag)
                throw std::runtime_error("Ambiguous pixel output signature");
            tag = kind;
            signature = bytes;
        }
    if (!tag)
        throw std::runtime_error("Pixel output signature is missing");
    const auto count = word(signature, 0);
    const uint32_t stride = tag == 0x3147534f ? 32 : tag == 0x3547534f ? 28 : 24;
    if (signature.size() < 8 || count > 128 || uint64_t(count) * stride > signature.size() - 8)
        throw std::runtime_error("Pixel output signature bounds");
    std::vector<Output> result;
    std::map<Register, uint32_t> masks;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t row = 8 + size_t(i) * stride;
        const size_t base = row + (stride > 24 ? 4 : 0);
        const auto name = word(signature, base), index = word(signature, base + 4);
        const auto encodedSystem = word(signature, base + 8), type = word(signature, base + 12);
        const auto reg = word(signature, base + 16);
        const uint32_t mask = signature[base + 20];
        const auto precision = stride == 32 ? word(signature, base + 24) : 0;
        if (name < 8 + count * stride || name >= signature.size() ||
            std::find(signature.begin() + name, signature.end(), uint8_t(0)) == signature.end() ||
            (stride > 24 && word(signature, row)) || !mask || mask > 15 || type < 1 || type > 3 ||
            (precision != 0 && precision != 1 && precision != 2 && precision != 4 && precision != 5))
            throw std::runtime_error("Invalid pixel output signature field");
        Output field{reinterpret_cast<const char *>(signature.data() + name),
                     {},
                     type == 1   ? "uint32"
                     : type == 2 ? "int32"
                                 : "float32",
                     index,
                     mask,
                     precision,
                     {}};
        auto normalized = field.semantic;
        for (auto &c : normalized)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        const std::map<std::string, uint32_t> semantics{{"sv_target", 64},
                                                        {"sv_depth", 65},
                                                        {"sv_coverage", 66},
                                                        {"sv_depthgreaterequal", 67},
                                                        {"sv_depthlessequal", 68}};
        const auto known = semantics.find(normalized);
        if (known == semantics.end() || (encodedSystem != 0 && encodedSystem != known->second))
            throw std::runtime_error("Pixel output signature semantic identity mismatch");
        // FXC may encode output system values as UNDEFINED in OSGN while the
        // reserved semantic name retains the stage-specific output identity.
        const auto system = known->second;
        if (system == 64 && reg < 8 && index == reg) {
            field.kind = "color";
            field.reg = {2, {reg}};
        } else if (reg == UINT32_MAX && mask == 1 && index == 0 && system >= 65 && system <= 68) {
            if ((system == 66 && type != 1) || (system != 66 && type != 3))
                throw std::runtime_error("Pixel depth/coverage output numeric type mismatch");
            field.kind = system == 65   ? "depth"
                         : system == 66 ? "coverage"
                         : system == 67 ? "depth_greater_equal"
                                        : "depth_less_equal";
            field.reg = {system == 65 ? 12u : system == 66 ? 15u : system == 67 ? 38u : 39u, {}};
        } else
            throw std::runtime_error("Unsupported pixel output signature semantic: " + field.semantic +
                                     " system=" + std::to_string(system) +
                                     " register=" + std::to_string(reg) + " mask=" + std::to_string(mask) +
                                     " index=" + std::to_string(index));
        if (masks[field.reg] & mask)
            throw std::runtime_error("Overlapping pixel output signature fields");
        masks[field.reg] |= mask;
        result.push_back(std::move(field));
    }
    return result;
}
} // namespace
Json evaluateNativePixelOutputs(const dxbc_exec::InputSnapshot &snapshot,
                                const dxbc_exec::PixelTraceOptions &selection, const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    if (snapshot.stage != 4 || !snapshot.event || !snapshot.shader || selection.selectedLane >= 4 ||
        selection.activeMask > 15 || !selection.maxSteps || !selection.maxBytes ||
        !selection.inputProvenance.is_object())
        throw std::invalid_argument("Invalid native pixel output request");
    const Program program(snapshot.bytecode);
    if (program.stage() != 0)
        throw std::invalid_argument("Native pixel output evaluation requires a pixel shader");
    const auto fields = outputs(snapshot.bytecode);
    uint64_t bytes = 0;
    auto account = [&](uint64_t amount) {
        if (amount > selection.maxBytes - bytes)
            throw std::runtime_error("Native pixel output private storage budget exceeded");
        bytes += amount;
    };
    // Copy the mutable graph once; all four lanes share its private aliases.
    Inputs common = snapshot.inputs;
    if (common.sharedMemory)
        throw std::runtime_error("Pixel output evaluation cannot accept compute shared memory");
    if (common.memory) {
        for (const auto &[id, buffer] : common.memory->buffers()) {
            account(buffer.bytes.size());
            account(buffer.valid.size());
        }
        common.memory = std::make_shared<Memory>(*common.memory);
    }
    if (common.textures) {
        for (const auto &[id, texture] : common.textures->resources) {
            account(texture.storage.bytes.size());
            account(texture.storage.valid.size());
        }
        common.textures = std::make_shared<TextureInputs>(*common.textures);
    }
    std::array<Inputs, 4> inputs;
    for (uint32_t lane = 0; lane < 4; ++lane) {
        inputs[lane] = common;
        for (const auto &[reg, value] : selection.registers[lane]) {
            if ((reg.kind != 1 && reg.kind != 35) || !inputs[lane].registers.emplace(reg, value).second)
                throw std::runtime_error("Native pixel output input register is invalid or duplicated");
        }
    }
    PixelQuad quad(program, std::move(inputs), selection.activeMask, selection.maxSteps, cancelled);
    uint64_t steps = 0, gpuInstructions = 0, memoryChanges = 0, counterChanges = 0;
    uint8_t discardedMask = 0;
    while (!quad.finished()) {
        checkCancellation(cancelled);
        const auto step = quad.step();
        ++steps;
        gpuInstructions += step.step.instructionGpuAssisted || step.step.memoryGpuAssisted;
        memoryChanges += step.step.memoryChanges.size() + step.step.textureChanges.size();
        counterChanges += step.step.counterChanges.size();
        if (step.step.discarded)
            discardedMask |= uint8_t(1u << step.lane);
    }
    auto lanes = Json::array();
    for (uint32_t lane = 0; lane < 4; ++lane) {
        auto values = Json::array();
        for (const auto &field : fields) {
            const auto value = quad.lane(lane).value(field.reg);
            values.push_back({{"kind", field.kind},
                              {"semantic", field.semantic},
                              {"index", field.index},
                              {"numeric", field.numeric},
                              {"declared_mask", field.mask},
                              {"minimum_precision", field.precision},
                              {"raw_bits", value.bits},
                              {"valid_mask", value.valid & field.mask}});
        }
        lanes.push_back({{"lane", lane},
                         {"initially_covered", (selection.activeMask & (1u << lane)) != 0},
                         {"discarded", (discardedMask & (1u << lane)) != 0},
                         {"shader_active", (quad.activeMask() & (1u << lane)) != 0},
                         {"outputs", values}});
    }
    return {{"schema", 1},
            {"kind", "native_pixel_shader_outputs"},
            {"backend", "native-pixel-outputs/1"},
            {"scope", "one_private_pixel_quad_shader_outputs"},
            {"execution_model", PixelQuad::executionModel},
            {"original_fragment_eligibility", nullptr},
            {"output_merger_executed", false},
            {"captured_gpu_schedule", false},
            {"frame_sha256", snapshot.frameSha256},
            {"shader_sha256", sha256(snapshot.bytecode, cancelled)},
            {"event", snapshot.event},
            {"shader", snapshot.shader},
            {"adapter",
             {{"vendor", snapshot.adapterVendor},
              {"device", snapshot.adapterDevice},
              {"luid", snapshot.adapterLuid}}},
            {"resource_observation", snapshot.resourceObservation},
            {"replay_generation", snapshot.replayGeneration},
            {"input_provenance", selection.inputProvenance},
            {"selected_lane", selection.selectedLane},
            {"initial_active_mask", selection.activeMask},
            {"final_active_mask", quad.activeMask()},
            {"lanes", lanes},
            {"global_steps", steps},
            {"gpu_assisted_steps", gpuInstructions},
            {"derivative_batches", quad.derivativeBatches()},
            {"sampling_batches", quad.samplingBatches()},
            {"private_storage_bytes", bytes},
            {"private_memory_changes", memoryChanges},
            {"private_counter_changes", counterChanges}};
}
} // namespace flora
