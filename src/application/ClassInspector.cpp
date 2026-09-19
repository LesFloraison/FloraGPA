#include "ClassInspector.h"
namespace flora {
nlohmann::json inspectClass(const Frame &frame, Id id) {
    const auto record = readClassRecord(frame, id);
    if (!record.instance)
        return {{"resource_kind", "class_linkage"}};
    static constexpr const char *fields[]{
        "instance_id",    "instance_index", "type_id", "constant_buffer", "constant_vector_offset",
        "texture_offset", "sampler_offset", "created"};
    nlohmann::json desc = nlohmann::json::object();
    for (size_t i = 0; i < 8; ++i)
        desc[fields[i]] = record.desc[i];
    return {{"resource_kind", "class_instance"},
            {"class_linkage_id", record.linkage},
            {"desc", desc},
            {"names_data_id", record.namesData},
            {"instance_name", record.instanceName},
            {"type_name", record.typeName},
            {"creation_method", record.desc[7] ? "CreateClassInstance" : "GetClassInstance"}};
}
} // namespace flora
