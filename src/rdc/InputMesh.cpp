#include "InputMesh.h"
#include "Assets.h"
#include <qfloat16.h>
#include <QCryptographicHash>
#include <QByteArrayView>
#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
namespace flora {
namespace {
using Json = nlohmann::json;
void require(bool test, const char *reason) { if (!test) throw std::runtime_error(reason); }
std::string name(ResourceId id) { uint64_t value{}; memcpy(&value, &id, 8); return "ResourceId::" + std::to_string(value); }
uint32_t size(const ResourceFormat &f) {
    require(f.type == ResourceFormatType::Regular && f.compCount >= 1 && f.compCount <= 4 &&
            (f.compByteWidth == 1 || f.compByteWidth == 2 || f.compByteWidth == 4),
            "Input mesh attribute needs a regular 8/16/32-bit format; packed formats are not implemented");
    return f.compCount * f.compByteWidth;
}
float component(const uint8_t *data, const ResourceFormat &f) {
    uint32_t u{}; memcpy(&u, data, f.compByteWidth);
    int32_t s{};
    if (f.compByteWidth == 1) s = int8_t(u);
    else if (f.compByteWidth == 2) s = int16_t(u);
    else memcpy(&s, &u, 4);
    switch (f.compType) {
    case CompType::Float:
        if (f.compByteWidth == 2) { qfloat16 value; memcpy(&value, data, 2); return float(value); }
        if (f.compByteWidth == 4) { float value; memcpy(&value, data, 4); return value; }
        break;
    case CompType::UInt: return float(u);
    case CompType::SInt: return float(s);
    case CompType::UNorm: return float(double(u) / double(f.compByteWidth == 4 ? UINT32_MAX : (1U << (f.compByteWidth * 8)) - 1));
    case CompType::SNorm: return std::max(-1.0f, float(double(s) / double(f.compByteWidth == 4 ? INT32_MAX : (1U << (f.compByteWidth * 8 - 1)) - 1)));
    default: break;
    }
    throw std::runtime_error("Unsupported input attribute component interpretation");
}
}
InputMeshData readDx12InputMesh(IReplayController &controller, const ActionDescription &draw,
                               uint32_t instance, uint32_t attribute) {
    const auto *pipeline = controller.GetD3D12PipelineState(); require(pipeline, "DX12 pipeline state is unavailable");
    const auto &ia = pipeline->inputAssembly;
    require(instance < draw.numInstances, "Input instance is outside the draw");
    require(attribute < ia.layouts.size(), "Input attribute index is outside the layout");
    require(draw.numIndices && draw.numIndices <= 4000000, "Input mesh requires 1 to four million vertex/index references");
    const auto &layout = ia.layouts[attribute]; const auto bytesPerElement = size(layout.format);
    require(layout.inputSlot < ia.vertexBuffers.size(), "Input attribute vertex buffer slot is unbound");
    const auto &vb = ia.vertexBuffers[layout.inputSlot];
    require(vb.resourceId != ResourceId::Null(), "Input vertex buffer is null");
    Json layouts = Json::array(), bindings = Json::array();
    std::map<uint32_t, uint32_t> nextOffset; std::set<uint32_t> unknownOffset; uint32_t attributeOffset{};
    for (uint32_t i = 0; i < ia.layouts.size(); ++i) {
        const auto &entry = ia.layouts[i];
        uint32_t offset = entry.byteOffset;
        if (offset == D3D12Pipe::Layout::TightlyPacked) {
            require(!unknownOffset.contains(entry.inputSlot), "Append-aligned layout follows an unsupported packed format");
            offset = nextOffset[entry.inputSlot];
        }
        try {
            const auto width = size(entry.format); require(offset <= UINT32_MAX - width, "Input layout offset overflow");
            nextOffset[entry.inputSlot] = offset + width; unknownOffset.erase(entry.inputSlot);
        } catch (const std::exception &) { unknownOffset.insert(entry.inputSlot); }
        layouts.push_back({{"index", i}, {"semantic", std::string(entry.semanticName.c_str())}, {"semantic_index", entry.semanticIndex},
            {"slot", entry.inputSlot}, {"byte_offset", offset}, {"format", rdcResourceFormat(entry.format)},
            {"per_instance", entry.perInstance}, {"instance_step_rate", entry.instanceDataStepRate}});
        if (i == attribute) attributeOffset = offset;
    }
    for (uint32_t i = 0; i < ia.vertexBuffers.size(); ++i) {
        const auto &b = ia.vertexBuffers[i];
        bindings.push_back({{"slot", i}, {"resource", name(b.resourceId)}, {"offset", std::to_string(b.byteOffset)},
            {"size", std::to_string(b.byteSize)}, {"stride", b.byteStride}});
    }
    const bool indexed = bool(draw.flags & ActionFlags::Indexed);
    InputMeshData result; std::vector<uint32_t> references(draw.numIndices);
    uint32_t minimum = UINT32_MAX, maximum{};
    uint64_t indexOffset{};
    if (indexed) {
        const auto &ib = ia.indexBuffer;
        require(ib.resourceId != ResourceId::Null() && (ib.byteStride == 2 || ib.byteStride == 4), "Input index buffer must contain 16/32-bit indices");
        const uint64_t relative = uint64_t(draw.indexOffset) * ib.byteStride, length = uint64_t(draw.numIndices) * ib.byteStride;
        require(relative <= ib.byteSize && length <= ib.byteSize - relative && ib.byteOffset <= UINT64_MAX - relative, "Input index range exceeds binding");
        indexOffset = ib.byteOffset + relative;
        const auto bytes = controller.GetBufferData(ib.resourceId, indexOffset, length);
        require(bytes.size() == length, "Truncated input index data"); result.rawIndices.assign(bytes.begin(), bytes.end());
        for (uint32_t i = 0; i < draw.numIndices; ++i) {
            uint32_t index{}; memcpy(&index, bytes.data() + uint64_t(i) * ib.byteStride, ib.byteStride);
            require(!ia.indexStripCutValue || index != ia.indexStripCutValue, "Input mesh primitive restart is not implemented");
            const int64_t vertex = int64_t(index) + draw.baseVertex;
            require(vertex >= 0 && vertex <= UINT32_MAX, "Input index plus base vertex is out of range");
            references[i] = uint32_t(vertex); minimum = std::min(minimum, references[i]); maximum = std::max(maximum, references[i]);
        }
    } else {
        require(uint64_t(draw.vertexOffset) + draw.numIndices <= uint64_t(UINT32_MAX) + 1, "Input vertex range overflows");
        for (uint32_t i = 0; i < draw.numIndices; ++i) references[i] = draw.vertexOffset + i;
        minimum = references.front(); maximum = references.back();
    }
    const uint64_t vertexCount = uint64_t(maximum) - minimum + 1;
    require(vertexCount <= 4000000, "Input vertex span exceeds four million vertices");
    const uint64_t element = layout.perInstance ? uint64_t(draw.instanceOffset) + (layout.instanceDataStepRate ? instance / layout.instanceDataStepRate : 0) : minimum;
    const uint64_t count = layout.perInstance ? 1 : vertexCount;
    require(!vb.byteStride || element <= (UINT64_MAX - attributeOffset) / vb.byteStride, "Input vertex offset overflow");
    const uint64_t relative = element * vb.byteStride + attributeOffset;
    const uint64_t length = (count - 1) * vb.byteStride + bytesPerElement;
    require(relative <= vb.byteSize && length <= vb.byteSize - relative && length <= 256ULL * 1024 * 1024 && vb.byteOffset <= UINT64_MAX - relative,
            "Input vertex span exceeds binding or 256 MiB readback limit");
    const auto raw = controller.GetBufferData(vb.resourceId, vb.byteOffset + relative, length);
    require(raw.size() == length, "Truncated input vertex data"); result.rawVertices.assign(raw.begin(), raw.end());
    result.vertices.resize(size_t(vertexCount) * 16); result.indices.resize(size_t(draw.numIndices) * 4);
    for (uint64_t i = 0; i < vertexCount; ++i) {
        float value[4]{0, 0, 0, 1};
        const auto source = raw.data() + (layout.perInstance ? 0 : i * vb.byteStride);
        for (uint32_t c = 0; c < layout.format.compCount; ++c) value[c] = component(source + c * layout.format.compByteWidth, layout.format);
        if (layout.format.BGRAOrder()) std::swap(value[0], value[2]);
        memcpy(result.vertices.data() + i * 16, value, 16);
    }
    for (size_t i = 0; i < references.size(); ++i) { const auto index = references[i] - minimum; memcpy(result.indices.data() + i * 4, &index, 4); }
    const auto digest = [](const auto &bytes) {
        return QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size())), QCryptographicHash::Sha256).toHex().toStdString();
    };
    result.metadata = {{"vertexResourceId", name(vb.resourceId)}, {"vertexByteOffset", 0}, {"vertexByteStride", 16},
        {"indexResourceId", "input-remapped-indices"}, {"indexByteOffset", 0}, {"indexByteStride", 4}, {"baseVertex", 0},
        {"numIndices", draw.numIndices}, {"topology", uint32_t(ia.topology)},
        {"format", {{"compType", 1}, {"compCount", 4}, {"compByteWidth", 4}}}, {"perspective_divide", false},
        {"input", {{"attribute", attribute}, {"layouts", layouts}, {"vertex_buffers", bindings},
            {"first_vertex", std::to_string(minimum)}, {"vertex_span", std::to_string(vertexCount)},
            {"raw_vertex_offset", std::to_string(vb.byteOffset + relative)}, {"raw_vertex_bytes", std::to_string(length)},
            {"raw_index_offset", std::to_string(indexOffset)}, {"index_resource", name(ia.indexBuffer.resourceId)},
            {"raw_index_bytes", std::to_string(result.rawIndices.size())},
            {"raw_vertex_sha256", digest(result.rawVertices)}, {"raw_index_sha256", digest(result.rawIndices)},
            {"index_width", indexed ? ia.indexBuffer.byteStride : 0}, {"indexed", indexed},
            {"instance", instance}, {"instance_count", draw.numInstances}, {"start_instance", draw.instanceOffset},
            {"start_index", draw.indexOffset}, {"start_vertex", draw.vertexOffset}, {"base_vertex", draw.baseVertex},
            {"interpretation", "Selected input attribute converted to float32; exact original bytes retained separately"}}}};
    return result;
}
}
