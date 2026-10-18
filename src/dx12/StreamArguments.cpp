#include "StreamArguments.h"

namespace flora::dx12 {
namespace {
using format::Reader;
using nlohmann::json;
json resourceDesc(Reader &r) {
    if (!r.flag()) return nullptr;
    Reader d(r.take(56));
    json out;
    out["Dimension"] = d.read<uint32_t>(); d.skip(4);
    out["Alignment"] = std::to_string(d.read<uint64_t>());
    out["Width"] = std::to_string(d.read<uint64_t>());
    out["Height"] = d.read<uint32_t>();
    out["DepthOrArraySize"] = d.read<uint16_t>(); out["MipLevels"] = d.read<uint16_t>();
    out["Format"] = d.read<uint32_t>(); out["SampleCount"] = d.read<uint32_t>();
    out["SampleQuality"] = d.read<uint32_t>(); out["Layout"] = d.read<uint32_t>();
    out["Flags"] = d.read<uint32_t>(); d.skip(4);
    return out;
}
std::vector<uint64_t> pointers(Reader &r) {
    std::vector<uint64_t> out;
    if (!r.flag()) return out;
    auto count = r.read<uint64_t>();
    if (count > r.remaining() / 8) throw std::runtime_error("Truncated DX12 pointer array");
    out.reserve(size_t(count));
    for (uint64_t i = 0; i < count; ++i) out.push_back(r.read<uint64_t>());
    return out;
}
}
DecodedArguments decodeArguments(uint64_t type, format::Bytes payload) {
    DecodedArguments out;
    Reader r(payload);
    auto &v = out.values;
    const auto u32 = [&](const char *name) { v[name] = r.read<uint32_t>(); };
    const auto u64 = [&](const char *name) { auto n = r.read<uint64_t>(); v[name] = std::to_string(n); return n; };
    const auto resource = [&](const char *name) { auto n = u64(name); if(n) out.resources.push_back(n); };
    switch (type) {
    case 0x1ae3c190d8b1eULL:
        u64("CommandList"); u32("VertexCountPerInstance"); u32("InstanceCount");
        u32("StartVertexLocation"); u32("StartInstanceLocation"); break;
    case 0x1ae3c86b1c3b9ULL:
        u64("CommandList"); u32("IndexCountPerInstance"); u32("InstanceCount");
        u32("StartIndexLocation"); v["BaseVertexLocation"] = r.read<int32_t>(); u32("StartInstanceLocation"); break;
    case 0x1ae3cef0e6c17ULL:
        u64("CommandList"); u32("ThreadGroupCountX"); u32("ThreadGroupCountY"); u32("ThreadGroupCountZ"); break;
    case 0x1ae3c721c351aULL:
        u64("CommandList"); u64("CommandAllocator"); u64("InitialPipelineState"); u32("HRESULT"); break;
    case 0x1ae3cd6acf667ULL:
        u64("CommandList"); u32("HRESULT"); break;
    case 0x12548dab1bf2dULL: case 0x12548db3bfde2ULL:
        u64("Queue"); u64("Fence"); u64("Value"); u32("HRESULT"); break;
    case 0x125482975df48ULL: {
        u64("Queue"); const auto count = r.read<uint32_t>(); v["NumCommandLists"] = count;
        const auto lists = pointers(r);
        if (lists.size() != count) throw std::runtime_error("DX12 command-list array count mismatch");
        v["CommandLists"] = json::array();
        for (auto list : lists) v["CommandLists"].push_back(std::to_string(list));
        // A separate GPA extension follows some submissions; preserve its size
        // explicitly until its synchronization semantics have been recovered.
        break;
    }
    case 0x1ae3cd301bf84ULL:
        u64("CommandList"); resource("Destination"); u64("DestinationOffset");
        resource("Source"); u64("SourceOffset"); u64("NumBytes"); break;
    case 0x1ae3c1d190092ULL:
        u64("CommandList"); resource("Destination"); resource("Source"); break;
    case 0x12793f58d8f79ULL: case 0x127935d88ccfaULL: case 0x127937ac5a8a9ULL: {
        u64("Device");
        if (type == 0x12793f58d8f79ULL) {
            if (r.flag()) {
                for (const auto *name : {"HeapType", "CPUPageProperty", "MemoryPoolPreference", "CreationNodeMask", "VisibleNodeMask"}) u32(name);
            }
            u32("HeapFlags");
        } else if (type == 0x127935d88ccfaULL) { u64("Heap"); u64("HeapOffset"); }
        v["Description"] = resourceDesc(r); u32("InitialResourceState");
        if (r.flag()) {
            u32("ClearValueFormat");
            // Preserve the union bits; interpretation depends on the format.
            for (const auto *name : {"ClearValueBits0", "ClearValueBits1", "ClearValueBits2", "ClearValueBits3"}) u32(name);
        }
        r.skip(16); const auto outputs = pointers(r); u32("HRESULT");
        if (outputs.size() > 1) throw std::runtime_error("Invalid DX12 resource output count");
        if (!outputs.empty()) {
            v["Resource"] = std::to_string(outputs[0]);
            if (int32_t(v["HRESULT"].get<uint32_t>()) >= 0 && outputs[0]) out.createdResource = outputs[0];
        }
        break;
    }
    default: return out;
    }
    out.supported = true;
    out.consumed = r.position();
    v["UndecodedExtensionBytes"] = r.remaining();
    return out;
}
}
