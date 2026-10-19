#include "PipelineRecipes.h"
#include "CapturedObjectMarker.h"
#include <wrl/client.h>
#include <QCryptographicHash>
#include <QByteArrayView>
#include <QString>
#include <map>
#include <set>
#include <sstream>

namespace flora::dx12 {
namespace {
using format::Reader;
using format::Bytes;
using nlohmann::json;
using Microsoft::WRL::ComPtr;
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
void check(HRESULT hr, const char *operation) {
    if (FAILED(hr)) { std::ostringstream s; s << operation << " failed: 0x" << std::hex << uint32_t(hr); throw std::runtime_error(s.str()); }
}
Bytes blob(Reader &r) {
    if (!r.flag()) return {};
    const auto size = r.read<uint64_t>();
    require(size <= r.remaining(), "Truncated pipeline blob");
    return r.take(size_t(size));
}
Bytes sizedBlob(Reader &r) {
    const auto bytes = blob(r);
    require(r.read<uint64_t>() == bytes.size(), "Pipeline blob length mismatch");
    return bytes;
}
const char *semantic(Reader &r) {
    const auto bytes = blob(r);
    if (bytes.empty()) return nullptr;
    require(bytes.back() == 0, "Unterminated pipeline semantic name");
    return reinterpret_cast<const char *>(bytes.data());
}
std::string wideName(Reader &r) {
    if (!r.flag()) return {};
    const auto count = r.read<uint64_t>(), width = r.read<uint64_t>();
    require(width == 2 && count && count <= r.remaining() / 2 && count <= 524288, "Invalid or oversized pipeline library name");
    Reader text(r.take(size_t(count * 2))); QString result;
    for (uint64_t i = 0; i < count; ++i) {
        const auto ch = text.read<uint16_t>();
        if (i + 1 == count) require(ch == 0, "Unterminated pipeline library name");
        else result.append(QChar(ch));
    }
    return result.toStdString();
}
uint64_t output(Reader &r, GUID &iid, HRESULT &hr) {
    iid = r.read<GUID>(); uint64_t object{};
    if (r.flag()) { require(r.read<uint64_t>() == 1, "Invalid pipeline output array"); object = r.read<uint64_t>(); }
    hr = r.read<HRESULT>(); r.end();
    return object;
}
D3D12_SHADER_BYTECODE shader(Reader &r, PipelineRecipe &recipe, const char *stage) {
    auto bytes = sizedBlob(r);
    if (!bytes.empty()) recipe.shaders.emplace_back(stage, bytes);
    return {bytes.empty() ? nullptr : bytes.data(), bytes.size()};
}
void inputs(Reader &r, PipelineRecipe &recipe, D3D12_INPUT_LAYOUT_DESC &desc) {
    require(recipe.inputs.empty(), "Duplicate pipeline input layout");
    if (r.flag()) {
        const auto count = r.read<uint64_t>();
        require(count <= r.remaining() / 25 && count <= 32, "Invalid pipeline input element count");
        recipe.inputs.resize(size_t(count));
        for (auto &element : recipe.inputs) {
            element.SemanticName = semantic(r); element.SemanticIndex = r.read<UINT>();
            element.Format = r.read<DXGI_FORMAT>(); element.InputSlot = r.read<UINT>();
            element.AlignedByteOffset = r.read<UINT>(); element.InputSlotClass = r.read<D3D12_INPUT_CLASSIFICATION>();
            element.InstanceDataStepRate = r.read<UINT>();
        }
    }
    desc.NumElements = r.read<UINT>();
    require(desc.NumElements == recipe.inputs.size(), "Pipeline input array count mismatch");
    desc.pInputElementDescs = recipe.inputs.empty() ? nullptr : recipe.inputs.data();
}
void streamOutput(Reader &r, PipelineRecipe &recipe, D3D12_STREAM_OUTPUT_DESC &desc) {
    if (r.flag()) {
        const auto count = r.read<uint64_t>();
        require(count <= r.remaining() / 12 && count <= 4096, "Invalid stream-output declaration count");
        recipe.declarations.resize(size_t(count));
        for (auto &entry : recipe.declarations) {
            entry.Stream = r.read<UINT>(); entry.SemanticName = semantic(r); entry.SemanticIndex = r.read<UINT>();
            entry.StartComponent = r.read<BYTE>(); entry.ComponentCount = r.read<BYTE>(); entry.OutputSlot = r.read<BYTE>();
        }
    }
    desc.NumEntries = r.read<UINT>(); require(desc.NumEntries == recipe.declarations.size(), "Stream-output declaration count mismatch");
    desc.pSODeclaration = recipe.declarations.empty() ? nullptr : recipe.declarations.data();
    if (r.flag()) {
        const auto count = r.read<uint64_t>(); require(count <= 4 && count <= r.remaining() / 4, "Invalid stream-output stride count");
        for (uint64_t i = 0; i < count; ++i) recipe.strides.push_back(r.read<UINT>());
    }
    desc.NumStrides = r.read<UINT>(); require(desc.NumStrides == recipe.strides.size(), "Stream-output stride count mismatch");
    desc.pBufferStrides = recipe.strides.empty() ? nullptr : recipe.strides.data(); desc.RasterizedStream = r.read<UINT>();
}
void cached(Reader &r, PipelineRecipe &recipe, D3D12_CACHED_PIPELINE_STATE &desc) {
    recipe.cachedBytes = sizedBlob(r).size();
    // Driver/adapter-specific caches are only an optimization. Rebuild from the
    // captured shaders and state instead of submitting an incompatible cache.
    desc = {};
}
void graphics(Reader &r, PipelineRecipe &recipe) {
    auto &d = recipe.graphics;
    recipe.rootSignature = r.read<uint64_t>();
    d.VS = shader(r, recipe, "VS"); d.PS = shader(r, recipe, "PS"); d.DS = shader(r, recipe, "DS");
    d.HS = shader(r, recipe, "HS"); d.GS = shader(r, recipe, "GS");
    streamOutput(r, recipe, d.StreamOutput);
    static_assert(sizeof(D3D12_BLEND_DESC) == 328 && sizeof(D3D12_RASTERIZER_DESC) == 44 && sizeof(D3D12_DEPTH_STENCIL_DESC) == 52);
    d.BlendState = r.read<D3D12_BLEND_DESC>(); d.SampleMask = r.read<UINT>();
    d.RasterizerState = r.read<D3D12_RASTERIZER_DESC>(); d.DepthStencilState = r.read<D3D12_DEPTH_STENCIL_DESC>();
    inputs(r, recipe, d.InputLayout);
    d.IBStripCutValue = r.read<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(); d.PrimitiveTopologyType = r.read<D3D12_PRIMITIVE_TOPOLOGY_TYPE>();
    d.NumRenderTargets = r.read<UINT>(); require(d.NumRenderTargets <= 8, "Invalid render-target format count");
    require(r.flag(), "Missing fixed render-target formats");
    for (auto &format : d.RTVFormats) format = r.read<DXGI_FORMAT>();
    d.DSVFormat = r.read<DXGI_FORMAT>(); d.SampleDesc = r.read<DXGI_SAMPLE_DESC>(); d.NodeMask = r.read<UINT>();
    cached(r, recipe, d.CachedPSO); d.Flags = r.read<D3D12_PIPELINE_STATE_FLAGS>();
}
template<class T> void patch(std::vector<uint64_t> &stream, size_t offset, const T &value) {
    require(offset <= stream.size() * 8 && sizeof(T) <= stream.size() * 8 - offset, "Pipeline subobject exceeds stream size");
    memcpy(reinterpret_cast<uint8_t *>(stream.data()) + offset, &value, sizeof(T));
}
void pipelineStream(Reader &r, PipelineRecipe &recipe) {
    const auto size = r.read<uint64_t>(); const auto bytes = blob(r);
    require(size == bytes.size() && size && size % 8 == 0 && size <= 1024 * 1024, "Invalid native pipeline stream size");
    recipe.stream.resize(size_t(size / 8)); memcpy(recipe.stream.data(), bytes.data(), size_t(size));
    Reader stream(bytes); std::set<UINT> seen;
    while (stream.remaining()) {
        const auto offset = stream.position(); const auto type = stream.read<UINT>();
        require(seen.insert(type).second, "Duplicate pipeline stream subobject");
        size_t length{};
        switch (type) {
        case 0: {
            stream.skip(4); recipe.rootSignature = stream.read<uint64_t>(); recipe.rootPointerOffset = offset + 8;
            ID3D12RootSignature *none{}; patch(recipe.stream, offset + 8, none); length = 16; break;
        }
        case 1: case 2: case 3: case 4: case 5: case 6: case 24: case 25: {
            const char *stage = type == 1 ? "VS" : type == 2 ? "PS" : type == 3 ? "DS" : type == 4 ? "HS" :
                type == 5 ? "GS" : type == 6 ? "CS" : type == 24 ? "AS" : "MS";
            patch(recipe.stream, offset + 8, shader(r, recipe, stage));
            recipe.shaderPointerOffsets.emplace(stage, offset + 8); length = 24; break;
        }
        case 7: { D3D12_STREAM_OUTPUT_DESC d{}; streamOutput(r, recipe, d); patch(recipe.stream, offset + 8, d); length = 40; break; }
        case 8: length = 336; break;
        case 9: case 13: case 14: case 16: case 18: case 20: length = 8; break;
        case 10: length = 48; break;
        case 11: length = 56; break;
        case 12: { D3D12_INPUT_LAYOUT_DESC d{}; inputs(r, recipe, d); patch(recipe.stream, offset + 8, d); length = 24; break; }
        case 15: length = 40; break;
        case 17: length = 16; break;
        case 19: { D3D12_CACHED_PIPELINE_STATE d{}; cached(r, recipe, d); patch(recipe.stream, offset + 8, d); length = 24; break; }
        case 21: length = 64; break;
        case 22: {
            D3D12_VIEW_INSTANCING_DESC d{}; d.ViewInstanceCount = r.read<UINT>();
            if (r.flag()) {
                const auto count = r.read<uint64_t>(); require(count <= 4 && count <= r.remaining() / 8, "Invalid view-instance count");
                for (uint64_t i = 0; i < count; ++i) recipe.views.push_back(r.read<D3D12_VIEW_INSTANCE_LOCATION>());
            }
            require(d.ViewInstanceCount == recipe.views.size(), "View-instance count mismatch");
            d.pViewInstanceLocations = recipe.views.empty() ? nullptr : recipe.views.data(); d.Flags = r.read<D3D12_VIEW_INSTANCING_FLAGS>();
            patch(recipe.stream, offset + 8, d); length = 32; break;
        }
        default: throw std::runtime_error("Unsupported pipeline stream subobject " + std::to_string(type));
        }
        require(length >= stream.position() - offset && length <= bytes.size() - offset, "Truncated native pipeline subobject");
        stream.skip(length - (stream.position() - offset));
    }
    require(!(seen.contains(11) && seen.contains(21)), "Duplicate depth/stencil pipeline variants");
}
std::string digest(Bytes bytes) {
    return QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size())), QCryptographicHash::Sha256).toHex().toStdString();
}
std::string shaderFormat(Bytes bytes) {
    if (bytes.size() < 32 || memcmp(bytes.data(), "DXBC", 4)) return "unknown";
    Reader r(bytes); r.skip(24); const auto length = r.read<UINT>(), count = r.read<UINT>();
    require(length == bytes.size() && count <= r.remaining() / 4, "Invalid shader container");
    bool dxil = false;
    for (UINT i = 0; i < count; ++i) {
        const auto offset = r.read<UINT>(); require(offset <= bytes.size() && bytes.size() - offset >= 8, "Invalid shader chunk offset");
        Reader chunk(bytes.subspan(offset)); const auto name = chunk.take(4); const auto size = chunk.read<UINT>();
        require(size <= chunk.remaining(), "Truncated shader container chunk"); if (!memcmp(name.data(), "DXIL", 4)) dxil = true;
    }
    return dxil ? "DXIL" : "DXBC";
}
}
std::string shaderBytecodeStage(Bytes bytes) {
    require(bytes.size() >= 32 && !memcmp(bytes.data(), "DXBC", 4), "Shader replacement must be a DXBC/DXIL container");
    Reader r(bytes); r.skip(24);
    const auto length = r.read<UINT>(), count = r.read<UINT>();
    require(length == bytes.size() && count <= r.remaining() / 4, "Invalid shader replacement container");
    std::map<size_t, size_t> ranges;
    std::optional<UINT> program;
    bool dxil = false;
    for (UINT i = 0; i < count; ++i) {
        const auto offset = r.read<UINT>();
        require(offset >= 32ULL + uint64_t(count) * 4 && offset <= bytes.size() && bytes.size() - offset >= 8,
                "Invalid shader replacement chunk offset");
        Reader chunk(bytes.subspan(offset)); const auto name = chunk.take(4); const auto size = chunk.read<UINT>();
        require(size <= chunk.remaining(), "Truncated shader replacement chunk");
        require(ranges.emplace(offset, size_t(offset) + 8 + size).second, "Duplicate shader replacement chunk");
        const bool isDxil = !memcmp(name.data(), "DXIL", 4);
        if (isDxil || !memcmp(name.data(), "SHEX", 4) || !memcmp(name.data(), "SHDR", 4)) {
            require(!program && size >= 8 && size % 4 == 0, "Invalid or multiple shader programs");
            Reader code(chunk.take(size)); program = code.read<UINT>();
            require(uint64_t(code.read<UINT>()) * 4 == size, "Shader replacement program length mismatch");
            if (isDxil) {
                require(size >= 24 && code.read<UINT>() == 0x4c495844, "Invalid DXIL program header");
                code.skip(4); const auto bitOffset = code.read<UINT>(), bitSize = code.read<UINT>();
                require(bitOffset >= 16 && uint64_t(bitOffset) + 8 <= size && bitSize <= size - bitOffset - 8,
                        "Invalid DXIL bitcode range");
            }
            dxil = isDxil;
        }
    }
    size_t end{};
    for (const auto &[begin, next] : ranges) { require(begin >= end, "Overlapping shader replacement chunks"); end = next; }
    require(program.has_value(), "Shader replacement contains no executable program");
    const auto type = *program >> 16, major = (*program >> 4) & 15;
    require(dxil ? major == 6 : (major >= 4 && major <= 5), "Unsupported replacement shader model");
    static const std::map<UINT, std::string> stages{{0,"PS"},{1,"VS"},{2,"GS"},{3,"HS"},{4,"DS"},{5,"CS"},{13,"MS"},{14,"AS"}};
    require(stages.contains(type) && (dxil || type < 6), "Unsupported replacement shader stage");
    return stages.at(type);
}
json describeShaderBytecode(Bytes bytes) {
    return {{"stage", shaderBytecodeStage(bytes)}, {"format", shaderFormat(bytes)},
            {"bytes", std::to_string(bytes.size())}, {"sha256", digest(bytes)}};
}
bool isPipelineRecipe(uint64_t type) {
    return type == 0x127938a579e5bULL || type == 0x12793a04eeae1ULL || type == 0x1b0ef747d2b24ULL || type == 0x121f7e63b7766ULL;
}
RootSignatureRecipe decodeRootSignature(Bytes bytes) {
    Reader r(bytes); RootSignatureRecipe out;
    out.device = r.read<uint64_t>(); out.nodeMask = r.read<UINT>(); out.bytecode = sizedBlob(r);
    GUID iid{}; out.object = output(r, iid, out.result);
    require(iid == __uuidof(ID3D12RootSignature), "Unsupported root-signature interface"); return out;
}
PipelineRecipe decodePipelineRecipe(uint64_t type, Bytes bytes) {
    require(isPipelineRecipe(type), "Record is not a supported pipeline creation");
    Reader r(bytes); PipelineRecipe out; out.owner = r.read<uint64_t>();
    if (type == 0x1b0ef747d2b24ULL) out.libraryName = wideName(r);
    require(r.flag(), "Missing pipeline descriptor");
    if (type == 0x12793a04eeae1ULL) {
        out.kind = PipelineRecipe::Kind::Compute; out.rootSignature = r.read<uint64_t>();
        out.compute.CS = shader(r, out, "CS"); out.compute.NodeMask = r.read<UINT>();
        cached(r, out, out.compute.CachedPSO); out.compute.Flags = r.read<D3D12_PIPELINE_STATE_FLAGS>();
    } else if (type == 0x121f7e63b7766ULL) { out.kind = PipelineRecipe::Kind::Stream; pipelineStream(r, out); }
    else graphics(r, out);
    out.object = output(r, out.iid, out.result);
    require(out.iid == __uuidof(ID3D12PipelineState), "Unsupported pipeline interface"); return out;
}
json describePipelineRecipe(const PipelineRecipe &r) {
    json shaders = json::array();
    for (const auto &[stage, bytes] : r.shaders)
        shaders.push_back({{"stage", stage}, {"bytes", std::to_string(bytes.size())}, {"format", shaderFormat(bytes)}, {"sha256", digest(bytes)}});
    return {{"owner", std::to_string(r.owner)}, {"captured_object", std::to_string(r.object)}, {"root_signature", std::to_string(r.rootSignature)},
        {"kind", r.kind == PipelineRecipe::Kind::Graphics ? "graphics" : r.kind == PipelineRecipe::Kind::Compute ? "compute" : "stream"},
        {"library_name", r.libraryName}, {"captured_hresult", uint32_t(r.result)}, {"shaders", std::move(shaders)},
        {"input_elements", r.inputs.size()}, {"stream_output_declarations", r.declarations.size()},
        {"cached_pipeline_bytes", std::to_string(r.cachedBytes)}, {"cache_policy", "Rebuild from captured shaders and pipeline state"}};
}

struct NativePipelines::Impl {
    NativeDevice &device;
    const StreamDirectory &directory;
    CancelCheck cancelled;
    std::map<uint64_t, std::vector<uint64_t>> rootRecords;
    std::map<uint64_t, std::vector<uint64_t>> pipelineRecords;
    std::map<uint64_t, ComPtr<ID3D12RootSignature>> roots;
    std::map<uint64_t, ComPtr<ID3D12PipelineState>> pipelines;
    std::map<uint64_t, json> reports;
    std::map<std::pair<uint64_t, std::string>, ShaderReplacement> replacements;
    Impl(NativeDevice &d, const StreamDirectory &s, CancelCheck c) : device(d), directory(s), cancelled(std::move(c)) {}
    std::pair<ID3D12RootSignature *, uint64_t> root(uint64_t object, uint64_t use) {
        if (!object) return {nullptr, 0};
        const auto it = rootRecords.find(object); require(it != rootRecords.end(), "Pipeline root-signature recipe is missing");
        const auto &records = it->second;
        uint64_t selected{};
        // Initialization permits forward definitions. Ambiguous initialization
        // lifetimes are rejected; frame creation is selected at its CPU boundary.
        if (use <= directory.initializationCount()) {
            for (const auto id : records) if (id <= directory.initializationCount()) {
                require(!selected, "Ambiguous initialization root-signature lifetime"); selected = id;
            }
        } else for (const auto id : records) if (id <= use) selected = id;
        require(selected, "Root signature is not defined at this creation boundary");
        if (auto cached = roots.find(selected); cached != roots.end()) return {cached->second.Get(), selected};
        const auto &packet = directory.packets().at(size_t(selected - 1)); const auto recipe = decodeRootSignature(directory.payload(packet));
        ComPtr<ID3D12RootSignature> native;
        check(device.device()->CreateRootSignature(recipe.nodeMask, recipe.bytecode.data(), recipe.bytecode.size(), IID_PPV_ARGS(&native)), "Recreate captured root signature");
        const auto marker = capturedObjectMarker(CapturedObjectKind::RootSignature, object, selected);
        check(native->SetName(std::wstring(marker.begin(), marker.end()).c_str()), "Name captured root signature");
        auto result = native.Get(); roots.emplace(selected, std::move(native)); return {result, selected};
    }
};
NativePipelines::NativePipelines(NativeDevice &device, const StreamDirectory &directory, const CancelCheck &cancelled)
    : impl_(std::make_unique<Impl>(device, directory, cancelled)) {
    for (size_t i = 0; i < directory.packets().size(); ++i) {
        if (i % 1024 == 0) checkCancellation(cancelled);
        const auto &p = directory.packets()[i];
        if (isPipelineRecipe(p.type)) {
            const auto r = decodePipelineRecipe(p.type, directory.payload(p));
            if (SUCCEEDED(r.result) && r.object) impl_->pipelineRecords[r.object].push_back(i + 1);
        }
        if (p.type != 0x127938051b445ULL) continue;
        const auto r = decodeRootSignature(directory.payload(p));
        if (SUCCEEDED(r.result) && r.object) impl_->rootRecords[r.object].push_back(i + 1);
    }
}
NativePipelines::~NativePipelines() = default;
void NativePipelines::setShaderReplacements(std::vector<ShaderReplacement> replacements) {
    auto &s = *impl_;
    require(s.pipelines.empty() && s.replacements.empty(), "Set shader replacements before creating pipelines");
    std::map<std::pair<uint64_t, std::string>, ShaderReplacement> validated;
    for (auto &replacement : replacements) {
        checkCancellation(s.cancelled);
        require(replacement.pipeline && replacement.pipeline <= s.directory.packets().size(), "Unknown replacement pipeline record");
        const auto &p = s.directory.packets()[size_t(replacement.pipeline - 1)];
        const auto recipe = decodePipelineRecipe(p.type, s.directory.payload(p));
        require(SUCCEEDED(recipe.result) && recipe.object, "Replacement pipeline was not successfully captured");
        require(recipe.rootSignature, "Shader replacement requires an explicit captured root signature; embedded-only roots are not implemented");
        require(shaderBytecodeStage(replacement.bytecode) == replacement.stage, "Replacement shader stage mismatch");
        bool found = false;
        for (const auto &[stage, bytes] : recipe.shaders) if (stage == replacement.stage) {
            require(digest(bytes) == replacement.originalSha256, "Original pipeline shader digest mismatch"); found = true;
        }
        require(found, "Captured pipeline does not contain the replacement stage");
        const auto key = std::make_pair(replacement.pipeline, replacement.stage);
        require(validated.emplace(key, std::move(replacement)).second, "Duplicate pipeline shader replacement");
    }
    s.replacements = std::move(validated);
    for (const auto &[key, replacement] : s.replacements) {
        try { create(key.first); }
        catch (const OperationCancelled &) { throw; }
        catch (const std::exception &e) {
            auto reason = "Shader replacement pipeline " + std::to_string(key.first) + ": " + e.what();
            const auto device = s.device.information();
            if (device.contains("validation_messages")) {
                size_t count{};
                for (const auto &message : device.at("validation_messages"))
                    if (message.at("severity").get<unsigned>() <= 1 && count++ < 4)
                        reason += "\n" + message.at("description").get<std::string>();
            }
            throw std::runtime_error(reason);
        }
    }
}
json NativePipelines::shaderReplacementInformation() const {
    json result = json::array();
    for (const auto &[key, replacement] : impl_->replacements)
        result.push_back({{"pipeline_creation", std::to_string(key.first)}, {"stage", key.second},
            {"original_sha256", replacement.originalSha256}, {"replacement_sha256", digest(replacement.bytecode)},
            {"scope", "pipeline_lifetime"}, {"pso_created", impl_->pipelines.contains(key.first)}});
    return result;
}
ID3D12RootSignature *NativePipelines::rootSignature(uint64_t object, uint64_t atRecord) { return impl_->root(object, atRecord).first; }
ID3D12PipelineState *NativePipelines::pipeline(uint64_t object, uint64_t atRecord) {
    if (!object) return nullptr;
    const auto it = impl_->pipelineRecords.find(object); require(it != impl_->pipelineRecords.end(), "Captured pipeline recipe is missing");
    uint64_t selected{}; for (const auto id : it->second) if (id <= atRecord) selected = id;
    if (!selected && atRecord <= impl_->directory.initializationCount()) for (const auto id : it->second) if (id <= impl_->directory.initializationCount()) {
        require(!selected, "Ambiguous forward pipeline lifetime"); selected = id;
    }
    require(selected, "Pipeline is not defined at this boundary"); return create(selected);
}
ID3D12PipelineState *NativePipelines::create(uint64_t record) {
    auto &s = *impl_; checkCancellation(s.cancelled);
    if (auto cached = s.pipelines.find(record); cached != s.pipelines.end()) return cached->second.Get();
    require(record && record <= s.directory.packets().size(), "Unknown pipeline creation record");
    const auto &packet = s.directory.packets()[size_t(record - 1)]; auto recipe = decodePipelineRecipe(packet.type, s.directory.payload(packet));
    check(recipe.result, "Captured pipeline creation"); require(recipe.object, "Captured pipeline creation has no output object");
    for (auto &[stage, bytes] : recipe.shaders) {
        const auto it = s.replacements.find({record, stage});
        if (it == s.replacements.end()) continue;
        bytes = it->second.bytecode;
        D3D12_SHADER_BYTECODE shader{bytes.data(), bytes.size()};
        if (recipe.kind == PipelineRecipe::Kind::Stream) patch(recipe.stream, recipe.shaderPointerOffsets.at(stage), shader);
        else if (stage == "CS") recipe.compute.CS = shader;
        else if (stage == "VS") recipe.graphics.VS = shader;
        else if (stage == "PS") recipe.graphics.PS = shader;
        else if (stage == "GS") recipe.graphics.GS = shader;
        else if (stage == "HS") recipe.graphics.HS = shader;
        else if (stage == "DS") recipe.graphics.DS = shader;
        else throw std::runtime_error("Shader replacement stage is absent from the native descriptor");
    }
    auto [root, rootRecord] = s.root(recipe.rootSignature, record); ComPtr<ID3D12PipelineState> native;
    if (recipe.kind == PipelineRecipe::Kind::Graphics) {
        recipe.graphics.pRootSignature = root;
        check(s.device.device()->CreateGraphicsPipelineState(&recipe.graphics, IID_PPV_ARGS(&native)), "Recreate captured graphics pipeline");
    } else if (recipe.kind == PipelineRecipe::Kind::Compute) {
        recipe.compute.pRootSignature = root;
        check(s.device.device()->CreateComputePipelineState(&recipe.compute, IID_PPV_ARGS(&native)), "Recreate captured compute pipeline");
    } else {
        if (recipe.rootPointerOffset) patch(recipe.stream, *recipe.rootPointerOffset, root);
        ComPtr<ID3D12Device2> device2; check(s.device.device()->QueryInterface(IID_PPV_ARGS(&device2)), "Query pipeline stream device");
        D3D12_PIPELINE_STATE_STREAM_DESC desc{recipe.stream.size() * 8, recipe.stream.data()};
        check(device2->CreatePipelineState(&desc, IID_PPV_ARGS(&native)), "Recreate captured pipeline stream");
    }
    checkCancellation(s.cancelled);
    const auto marker = capturedObjectMarker(CapturedObjectKind::Pipeline, recipe.object, record);
    check(native->SetName(std::wstring(marker.begin(), marker.end()).c_str()), "Name captured pipeline");
    auto report = describePipelineRecipe(recipe);
    report["record"] = std::to_string(record); report["root_creation_record"] = std::to_string(rootRecord);
    report["status"] = "pipeline_created"; report["gpu_commands_executed"] = false;
    report["note"] = "Native pipeline reconstruction; captured draw/dispatch execution and output are not yet verified";
    auto result = native.Get(); s.pipelines.emplace(record, std::move(native)); s.reports.emplace(record, std::move(report)); return result;
}
json NativePipelines::information(uint64_t record) const { return impl_->reports.at(record); }
}
