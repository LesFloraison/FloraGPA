#include "StreamArguments.h"
#include <iomanip>
#include <sstream>

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
std::string interfaceId(Reader &r) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(8) << r.read<uint32_t>() << '-'
        << std::setw(4) << r.read<uint16_t>() << '-' << std::setw(4) << r.read<uint16_t>() << '-';
    for (int i = 0; i < 8; ++i) {
        if (i == 2) out << '-';
        out << std::setw(2) << unsigned(r.read<uint8_t>());
    }
    return out.str();
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
    case 0x1d532a4b90915ULL: {
        u64("SwapChain"); u32("BufferIndex"); v["InterfaceId"] = interfaceId(r);
        const auto outputs = pointers(r); u32("HRESULT"); r.end();
        if (outputs.size() > 1) throw std::runtime_error("Invalid swapchain buffer output count");
        if (!outputs.empty()) {
            v["Resource"] = std::to_string(outputs[0]);
            if (outputs[0]) out.resources.push_back(outputs[0]);
        }
        break;
    }
    case 0x1af998824f46bULL:
        u64("SwapChain"); u32("ReturnedIndex"); r.end(); break;
    case 0x1d53218275f72ULL:
        u64("SwapChain"); u32("SyncInterval"); u32("PresentFlags"); u32("HRESULT"); r.end(); break;
    case 0x1000016b94da9ULL: case 0x11fac3bb041bfULL: {
        if (type == 0x11fac3bb041bfULL) u64("DeviceFactory");
        u64("Adapter"); u32("MinimumFeatureLevel"); r.skip(16);
        const auto outputs = pointers(r); u32("HRESULT");
        if (outputs.size() > 1) throw std::runtime_error("Invalid DX12 device output count");
        if (!outputs.empty()) v["Device"] = std::to_string(outputs[0]);
        break;
    }
    case 0x20000000000000ULL:
        u64("CapturedMemory"); u64("ResourceDataDescriptorOffset"); r.end(); break;
    case 0x26000000000000ULL:
        u64("CapturedObject"); u32("ReferenceCount"); r.end(); break;
    case 0x27000000000000ULL:
        u64("EventHandle"); r.end(); break;
    case 0x1ae3c190d8b1eULL:
        u64("CommandList"); u32("VertexCountPerInstance"); u32("InstanceCount");
        u32("StartVertexLocation"); u32("StartInstanceLocation"); break;
    case 0x1ae3c86b1c3b9ULL:
        u64("CommandList"); u32("IndexCountPerInstance"); u32("InstanceCount");
        u32("StartIndexLocation"); v["BaseVertexLocation"] = r.read<int32_t>(); u32("StartInstanceLocation"); break;
    case 0x1ae3cef0e6c17ULL:
        u64("CommandList"); u32("ThreadGroupCountX"); u32("ThreadGroupCountY"); u32("ThreadGroupCountZ"); break;
    case 0x10144f9457a81ULL:
        u64("CommandList"); v["MinDepthBounds"] = r.read<float>(); v["MaxDepthBounds"] = r.read<float>();
        r.end(); break;
    case 0x101407e5f6cc3ULL:
        u64("CommandList"); resource("ShadingRateImage"); r.end(); break;
    case 0x10140788f8c34ULL:
        u64("CommandList"); u32("BaseShadingRate"); v["Combiners"] = nullptr;
        if (r.flag()) {
            if (r.read<uint64_t>() != 2) throw std::runtime_error("Shading rate requires two combiners");
            v["Combiners"] = json::array();
            for (int i = 0; i < 2; ++i) v["Combiners"].push_back(r.read<uint32_t>());
        }
        r.end(); break;
    case 0x1ae3c721c351aULL:
        u64("CommandList"); u64("CommandAllocator"); u64("InitialPipelineState"); u32("HRESULT"); break;
    case 0x1ae3cd6acf667ULL:
        u64("CommandList"); u32("HRESULT"); break;
    case 0x127938b682074ULL: {
        u64("Device"); u32("NodeMask"); u32("Type"); u64("CommandAllocator"); u64("InitialPipelineState");
        r.skip(16); const auto outputs = pointers(r); u32("HRESULT");
        if (outputs.size() > 1) throw std::runtime_error("Invalid DX12 command-list output count");
        if (!outputs.empty()) v["CommandList"] = std::to_string(outputs[0]);
        break;
    }
    case 0x12548dab1bf2dULL: case 0x12548db3bfde2ULL:
        u64("Queue"); u64("Fence"); u64("Value"); u32("HRESULT"); break;
    case 0x114699e3dba70ULL:
        u64("Fence"); u64("Value"); u64("EventHandle"); u32("HRESULT"); r.end(); break;
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
    case 0x1ae3c24e5dabeULL: {
        u64("CommandList");
        const auto count = r.read<uint32_t>();
        v["Barriers"] = json::array();
        if (r.flag()) {
            const auto entries = r.read<uint64_t>();
            if (entries != count || entries > r.remaining() / 32)
                throw std::runtime_error("Invalid resource barrier array");
            for (uint32_t i = 0; i < count; ++i) {
                Reader b(r.take(32));
                const auto kind = b.read<uint32_t>(), flags = b.read<uint32_t>();
                json barrier{{"Type", kind}, {"Flags", flags}};
                auto ref = [&](const char *name) {
                    const auto object = b.read<uint64_t>();
                    barrier[name] = std::to_string(object);
                    if (object)
                        out.resources.push_back(object);
                };
                if (kind == 0) {
                    ref("Resource");
                    barrier["Subresource"] = b.read<uint32_t>();
                    barrier["StateBefore"] = b.read<uint32_t>();
                    barrier["StateAfter"] = b.read<uint32_t>();
                } else if (kind == 1) {
                    ref("ResourceBefore");
                    ref("ResourceAfter");
                } else if (kind == 2) {
                    ref("Resource");
                } else {
                    throw std::runtime_error("Unknown resource barrier type");
                }
                v["Barriers"].push_back(std::move(barrier));
            }
        }
        if (v["Barriers"].size() != count)
            throw std::runtime_error("Missing resource barriers");
        r.end();
        break;
    }
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
