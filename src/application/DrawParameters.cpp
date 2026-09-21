#include "DrawParameters.h"
#include "EventDescription.h"
#include "StreamOutputInspector.h"
#include <QByteArray>
namespace flora {
ResolvedDraw resolveDrawParameters(Replay &replay, const Event &event) {
    ResolvedDraw result{event, inspectionEvent(replay.frame(), event.id).at("parameters"), nullptr};
    auto &direct = result.direct;
    auto &parameters = result.parameters;
    if (event.type == 0x38) {
        const auto automatic = replay.drawAutoParameters(event.id);
        direct.type = 0x37;
        direct.args = {automatic.vertexCount, 0};
        parameters = {{"vertex_count", automatic.vertexCount}, {"start_vertex", 0}};
        result.indirect = drawAutoJson(automatic);
    } else if (event.argumentBuffer) {
        const auto storage = replay.readBuffer(event.argumentBuffer);
        const auto offset = event.args.at(0), size = event.type == 0x3b ? 20u : 16u;
        if (offset % 4 || offset > storage.size() || size > storage.size() - offset)
            throw std::runtime_error("Indirect draw arguments are unaligned or outside their buffer");
        Reader reader(Bytes(storage).subspan(offset, size));
        direct.args.clear();
        while (reader.remaining())
            direct.args.push_back(reader.read<uint32_t>());
        direct.type = event.type == 0x3b ? 0x3a : 0x3c;
        direct.argumentBuffer = 0;
        parameters = {{direct.type == 0x3a ? "index_count" : "vertex_count", direct.args[0]},
                      {"instance_count", direct.args[1]},
                      {direct.type == 0x3a ? "start_index" : "start_vertex", direct.args[2]},
                      {"start_instance", direct.args.back()}};
        if (direct.type == 0x3a)
            parameters["base_vertex"] = int32_t(direct.args[3]);
        result.indirect = {
            {"resource", event.argumentBuffer},
            {"offset", offset},
            {"size", size},
            {"raw_hex", QByteArray(reinterpret_cast<const char *>(storage.data() + offset), size)
                            .toHex()
                            .toStdString()}};
    }
    return result;
}
} // namespace flora
