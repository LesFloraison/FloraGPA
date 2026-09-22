#include "MetricIterations.h"
#include "data/FrameApiTypes.h"
#include <algorithm>
namespace flora {
uint32_t metricApiKind(uint16_t type) {
    switch (type) {
    case 2:
    case 9:
    case 0x20:
    case 0x21:
    case 0x31:
    case 0x32:
    case 0x33:
    case 0x34:
    case 0x257:
        return 0;
    case 0x35:
    case 0x36:
        return 2;
    case 0x3e:
    case 0x3f:
    case 0x40:
    case 0x42:
    case 0x245:
    case 0x256:
        return 6;
    case 0x41:
        return 0x12;
    case 0x101:
    case 0x327b:
        return 0x18;
    case 0x102:
    case 0x327c:
        return 0x19;
    case 0x103:
        return 0x14;
    default:
        if ((type >= 3 && type <= 8) || (type >= 10 && type <= 15) || (type >= 0x37 && type <= 0x3d))
            return 1;
        if (type >= 0x10 && type <= 0x15)
            return 6;
        return 0xf;
    }
}
nlohmann::json buildFrameMetricIndex(const Frame &frame) {
    using Json = nlohmann::json;
    Json ergs = Json::array(), ranges = Json::array(), excluded = Json::array();
    size_t start = 0;
    for (const auto &[id, entry] : frame.entries()) {
        if (entry.category != 7)
            continue;
        if (!std::binary_search(frameApiTypes.begin(), frameApiTypes.end(), entry.type)) {
            excluded.push_back({{"id", id}, {"reason", "unsupported_decoder"}});
            continue;
        }
        const auto kind = metricApiKind(entry.type);
        if (kind == 0xf) {
            const auto data = frame.payload(id);
            if (data.size() < 16)
                throw std::invalid_argument("API " + std::to_string(id) + " has a truncated object header");
            Reader reader(data);
            reader.skip(8);
            const auto target = reader.read<uint32_t>();
            const auto resource = frame.entries().find(target);
            if (resource == frame.entries().end() ||
                (resource->second.type != 0x99 && resource->second.type != 0x10b &&
                 resource->second.type != 0x121 && resource->second.type != 0x123 &&
                 resource->second.type != 0x127)) {
                excluded.push_back({{"id", id}, {"reason", "non_context"}, {"target", target}});
                continue;
            }
        }
        if (id > UINT32_MAX)
            throw std::invalid_argument("Internal API ID exceeds uint32");
        const auto index = ergs.size();
        ergs.push_back(id);
        if (kind < 13) {
            ranges.push_back(Json::array({2, start, index}));
            start = index + 1;
        }
    }
    return {{"ergs", ergs}, {"ranges", {{"2", ranges}}}, {"fallback", Json::array()}, {"excluded", excluded}};
}
} // namespace flora
