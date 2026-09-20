#include "core/StreamOutput.h"
#include "Replay.h"
#include "core/Dxbc.h"
#include <algorithm>
#include <chrono>
#include <regex>
#include <sstream>
#include <thread>
namespace flora {
Com<ID3D11GeometryShader> Replay::createStreamOutputShader(Bytes bytes, Id id, ID3D11ClassLinkage *linkage) {
    const auto decl = readStreamOutputDeclaration(frame_, id);
    std::vector<D3D11_SO_DECLARATION_ENTRY> entries;
    for (const auto &e : decl.entries)
        entries.push_back({e.stream, e.semantic ? e.semantic->c_str() : nullptr, e.index, BYTE(e.start),
                           BYTE(e.count), BYTE(e.slot)});
    // Preserve output-only signatures. This runtime requires an empty ISGN chunk.
    auto program = addEmptyInputSignature(bytes);
    Com<ID3D11GeometryShader> shader;
    check(device_->CreateGeometryShaderWithStreamOutput(
              program.data(), program.size(), entries.empty() ? nullptr : entries.data(),
              UINT(entries.size()), decl.strides.empty() ? nullptr : decl.strides.data(),
              UINT(decl.strides.size()), decl.rasterizedStream, linkage, &shader),
          "CreateGeometryShaderWithStreamOutput");
    return shader;
}
void Replay::resetStreamOutputBindings() {
    soSignature_.reset();
    soPendingAppend_.clear();
}
void Replay::unbindStreamOutput() {
    if (!options_.warp) {
        // The tested hardware can retain stale destinations after a zero-count unbind.
        // Replacing them with full private buffers prevents writes to user resources.
        for (auto &sink : soSinks_)
            if (!sink) {
                D3D11_BUFFER_DESC desc{4, D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT, 0, 0, 0};
                check(device_->CreateBuffer(&desc, nullptr, &sink), "Create SO unbind sink");
            }
        ID3D11Buffer *targets[]{soSinks_[0].Get(), soSinks_[1].Get(), soSinks_[2].Get(), soSinks_[3].Get()};
        UINT offsets[]{4, 4, 4, 4};
        context_->SOSetTargets(4, targets, offsets);
    }
    context_->SOSetTargets(0, nullptr, nullptr);
}
void Replay::markStreamOutputOffsets(std::span<ID3D11Buffer *const> objects,
                                     std::span<const uint32_t> offsets) {
    std::array<ID3D11Buffer *, 4> actual{};
    context_->SOGetTargets(UINT(objects.size()), actual.data());
    for (size_t i = 0; i < objects.size(); ++i) {
        Com<ID3D11Buffer> observed;
        observed.Attach(actual[i]);
        if (objects[i] && objects[i] == observed.Get() && offsets[i] != UINT32_MAX) {
            soVertexCounts_[objects[i]] = 0;
            soByteCursors_[objects[i]] = offsets[i];
        }
    }
}
void Replay::bindStreamOutput(const State &state) {
    if (state.soCount > 4)
        throw std::runtime_error("Stream-output target count exceeds four");
    auto ids = std::span(state.so).first(state.soCount);
    auto capturedOffsets = std::span(state.soOffsets).first(state.soCount);
    validateStreamOutputBindings(frame_, ids, capturedOffsets);
    std::vector<std::pair<Id, uint32_t>> signature;
    std::vector<ID3D11Buffer *> objects;
    for (size_t i = 0; i < ids.size(); ++i) {
        signature.emplace_back(ids[i], capturedOffsets[i]);
        objects.push_back(get<ID3D11Buffer>(ids[i]));
    }
    bool changed = !soSignature_ || *soSignature_ != signature || (state.mask[20] & 0x80000000);
    std::vector<uint32_t> offsets;
    for (size_t i = 0; i < ids.size(); ++i) {
        auto pending = soPendingAppend_.find(uint32_t(i));
        bool continuing =
            !changed || (objects[i] && pending != soPendingAppend_.end() && pending->second == objects[i]);
        offsets.push_back(continuing ? UINT32_MAX : capturedOffsets[i]);
    }
    if (!objects.empty()) {
        context_->SOSetTargets(UINT(objects.size()), objects.data(), offsets.data());
        markStreamOutputOffsets(objects, offsets);
    }
    soSignature_ = std::move(signature);
    soPendingAppend_.clear();
}
void Replay::applyStreamOutput(Bytes payload) {
    const auto command = readStreamOutputTargets(payload);
    immediate(command.context);
    if (command.count && !command.buffers)
        throw std::runtime_error("SOSetTargets requires a buffer array for nonzero count");
    auto ids = command.buffers.value_or(std::vector<Id>{});
    auto offsets = command.offsets.value_or(std::vector<uint32_t>(command.count, UINT32_MAX));
    validateStreamOutputBindings(frame_, ids, offsets);
    std::vector<ID3D11Buffer *> objects;
    for (auto id : ids)
        objects.push_back(get<ID3D11Buffer>(id));
    if (command.count)
        context_->SOSetTargets(command.count, objects.data(), command.offsets ? offsets.data() : nullptr);
    else
        unbindStreamOutput();
    if (command.offsets)
        markStreamOutputOffsets(objects, offsets);
    soPendingAppend_.clear();
    for (uint32_t i = 0; i < objects.size(); ++i)
        if (objects[i] && offsets[i] == UINT32_MAX)
            soPendingAppend_[i] = objects[i];
}
static std::map<uint32_t, uint32_t> outputTopologies(Bytes bytecode) {
    bool program = false;
    for (const auto &[tag, bytes] : readDxbcParts(bytecode))
        program |= tag == 0x52444853 || tag == 0x58454853;
    if (!program)
        return {};
    std::map<uint32_t, uint32_t> out;
    std::istringstream text(disassemble(bytecode));
    std::string line;
    uint32_t stream = 0;
    const std::regex streams(R"(^\s*dcl_stream m([0-3])\s*$)");
    const std::regex topologies(R"(^\s*dcl_outputtopology (pointlist|linestrip|trianglestrip)\s*$)");
    const std::regex tessellation(R"(^\s*dcl_tessellator_output_primitive output_(\w+)\s*$)");
    while (std::getline(text, line)) {
        std::smatch match;
        if (std::regex_match(line, match, streams))
            stream = uint32_t(std::stoul(match[1]));
        if (std::regex_match(line, match, topologies))
            out[stream] = match[1] == "pointlist" ? 1 : match[1] == "linestrip" ? 2 : 3;
        if (std::regex_match(line, match, tessellation))
            out[0] = match[1] == "point"                                       ? 1
                     : match[1] == "line"                                      ? 2
                     : match[1] == "triangle_cw" || match[1] == "triangle_ccw" ? 3
                                                                               : 0;
    }
    return out;
}
std::vector<Replay::ActiveStream> Replay::beginStreamOutput(const State &state) {
    auto gs = state.stages[3].shader;
    auto declId = gs ? shaderStreamOutput(frame_, gs) : 0;
    if (!soCountEnabled_ || !declId)
        return {};
    auto bytecode = [&](Id id) -> Bytes {
        if (auto it = options_.shaders.find(id); it != options_.shaders.end())
            return it->second;
        return frame_.shader(frame_.resource(id).data);
    };
    auto decl = readStreamOutputDeclaration(frame_, declId);
    auto factors = outputTopologies(bytecode(gs));
    if (factors.empty()) {
        auto hs = state.stages[1].shader;
        if (hs && state.stages[2].shader)
            factors = outputTopologies(bytecode(hs));
        else {
            switch (state.topology) {
            case 1:
                factors[0] = 1;
                break;
            case 2:
            case 3:
            case 10:
            case 11:
                factors[0] = 2;
                break;
            case 4:
            case 5:
            case 12:
            case 13:
                factors[0] = 3;
                break;
            }
        }
    }
    std::array<uint32_t, 4> strides{};
    for (const auto &e : decl.entries)
        strides[e.slot] += e.count * 4;
    std::copy(decl.strides.begin(), decl.strides.end(), strides.begin());
    std::array<ID3D11Buffer *, 4> targets{};
    context_->SOGetTargets(4, targets.data());
    std::array<Com<ID3D11Buffer>, 4> held;
    for (size_t i = 0; i < 4; ++i)
        held[i].Attach(targets[i]);
    std::map<uint32_t, std::map<ID3D11Buffer *, uint32_t>> streams;
    for (const auto &e : decl.entries)
        if (targets[e.slot])
            streams[e.stream][targets[e.slot]] = strides[e.slot];
    std::vector<ActiveStream> active;
    for (const auto &[stream, objects] : streams) {
        auto factor = factors[stream];
        if (factor < 1 || factor > 3)
            throw std::runtime_error("Cannot reconstruct DrawAuto count: unknown SO output topology");
        auto &query = soQueries_[stream];
        if (!query) {
            D3D11_QUERY_DESC desc{D3D11_QUERY(8 + 2 * stream), 0};
            check(device_->CreateQuery(&desc, &query), "Create SO count query");
        }
        active.push_back({stream, factor, objects, query});
    }
    for (const auto &s : active)
        context_->Begin(s.query.Get());
    return active;
}
void Replay::endStreamOutput(Id event, const std::vector<ActiveStream> &active) {
    for (const auto &s : active)
        context_->End(s.query.Get());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (const auto &s : active) {
        D3D11_QUERY_DATA_SO_STATISTICS result{};
        for (;;) {
            auto hr = context_->GetData(s.query.Get(), &result, sizeof result, 0);
            check(hr, "Read SO statistics");
            if (hr == S_OK)
                break;
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("SO statistics query timeout");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        auto vertices = result.NumPrimitivesWritten * s.factor;
        for (const auto &[obj, stride] : s.strides) {
            if (auto it = soVertexCounts_.find(obj); it != soVertexCounts_.end())
                it->second += uint32_t(vertices);
            if (auto it = soByteCursors_.find(obj); it != soByteCursors_.end())
                it->second += vertices * stride;
        }
        streamOutputHistory.push_back(
            {event, s.stream, s.factor, result.NumPrimitivesWritten, result.PrimitivesStorageNeeded});
    }
}
DrawAutoParameters Replay::drawAutoParameters(Id id) {
    if (auto it = soAutoResults_.find(id); it != soAutoResults_.end())
        return it->second;
    auto event = frame_.event(id);
    if (event.type != 0x38)
        throw std::runtime_error("DrawAuto parameters require a DrawAuto event");
    auto state = frame_.state(event.state);
    DrawAutoParameters result;
    result.shader = state.stages[3].shader;
    result.declaration = result.shader ? shaderStreamOutput(frame_, result.shader) : 0;
    auto stream =
        result.declaration ? readStreamOutputDeclaration(frame_, result.declaration).rasterizedStream : 0;
    result.noRasterization = stream == UINT32_MAX;
    result.stream = result.noRasterization ? 0 : stream;
    result.capturedCount = result.vertexCount = state.soCounts[result.stream];
    result.resource = state.vb[0];
    auto obj = get<ID3D11Buffer>(result.resource);
    auto count = soVertexCounts_.find(obj);
    auto cursor = soByteCursors_.find(obj);
    if (cursor != soByteCursors_.end()) {
        result.iaStride = state.strides[0];
        result.iaOffset = state.offsets[0];
        if (!result.iaStride)
            throw std::runtime_error("Cannot reconstruct DrawAuto count: first IA stride is zero");
        auto vertices =
            cursor->second > result.iaOffset ? (cursor->second - result.iaOffset) / result.iaStride : 0;
        if (vertices > UINT32_MAX)
            throw std::runtime_error("Reconstructed DrawAuto count exceeds uint32");
        result.vertexCount = uint32_t(vertices);
        result.filledBytes = cursor->second;
        result.verified = true;
    } else if (count != soVertexCounts_.end()) {
        result.vertexCount = count->second;
        result.verified = true;
    } else if (soCountRequiresKnown_)
        throw std::runtime_error(
            "Cannot reconstruct DrawAuto count: IA buffer has no known SO vertex history");
    soAutoResults_[id] = result;
    return result;
}
} // namespace flora
