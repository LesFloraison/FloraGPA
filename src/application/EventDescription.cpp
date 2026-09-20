#include "EventDescription.h"
#include "ApiCommands.h"
namespace flora {
using Json = nlohmann::json;
Json inspectionEvent(const Frame &frame, Id id) {
    if (!isDraw(frame.entry(id).type))
        return inspectCommand(frame, id);
    const auto e = frame.event(id);
    std::vector<const char *> names;
    unsigned slot = 0;
    switch (e.type) {
    case 0x35:
        names = {"x", "y", "z"};
        slot = 41;
        break;
    case 0x36:
        names = {"offset"};
        slot = 42;
        break;
    case 0x37:
        names = {"vertex_count", "start_vertex"};
        slot = 13;
        break;
    case 0x38:
        slot = 13;
        break;
    case 0x39:
        names = {"index_count", "start_index", "base_vertex"};
        slot = 12;
        break;
    case 0x3a:
        names = {"index_count", "instance_count", "start_index", "base_vertex", "start_instance"};
        slot = 20;
        break;
    case 0x3b:
        names = {"offset"};
        slot = 39;
        break;
    case 0x3c:
        names = {"vertex_count", "instance_count", "start_vertex", "start_instance"};
        slot = 21;
        break;
    case 0x3d:
        names = {"offset"};
        slot = 40;
        break;
    }
    Json parameters = Json::object();
    if (e.type == 0x36 || e.type == 0x3b || e.type == 0x3d)
        parameters["argument_buffer"] = e.argumentBuffer;
    for (size_t i = 0; i < names.size(); ++i)
        parameters[names[i]] =
            std::string(names[i]) == "base_vertex" ? Json(int32_t(e.args.at(i))) : Json(e.args.at(i));
    return {{"id", e.id},   {"name", commandName(e.type)}, {"state_id", e.state}, {"context", e.context},
            {"slot", slot}, {"parameters", parameters}};
}
} // namespace flora
