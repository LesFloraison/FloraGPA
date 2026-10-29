#include "NativeMesh.h"

namespace flora {
NativeMesh readNativeMesh(const nlohmann::json &job, const nlohmann::json &result, const QString &directory,
                          uint64_t maximumBytes, const CancelCheck &cancelled) {
    validateNativeAnalysisResult(job, result);
    return decodeNativeMesh(job, result,
                            readNativeArtifacts(directory, result.at("artifacts"), maximumBytes, cancelled),
                            cancelled);
}
NativeMesh decodeNativeMesh(const nlohmann::json &job, const nlohmann::json &result,
                            NativeArtifactBytes artifacts, const CancelCheck &cancelled) {
    validateNativeAnalysisResult(job, result);
    if (job.at("action") != "mesh")
        throw std::runtime_error("Expected a native mesh analysis");
    NativeMesh mesh;
    mesh.geometry.report = result.at("data");
    const auto &report = mesh.geometry.report;
    const auto &options = job.at("options");
    if (report.at("requested_stage") != options.value("stage", std::string("final")) ||
        report.at("event").at("id") != options.at("gpa_event") ||
        report.at("stream") != options.value("stream", 0u))
        throw std::runtime_error("Native mesh belongs to another selection");
    if (report.at("requested_stage") == "hs")
        throw std::runtime_error("Hull phase results require the hull inspection reader");
    if (options.contains("instance") &&
        report.at("instance_selection").at("instance") != options.at("instance"))
        throw std::runtime_error("Native mesh belongs to another instance");
    mesh.artifacts = std::move(artifacts);
    if (!mesh.artifacts.contains("vertices.bin"))
        throw std::runtime_error("Native mesh vertex storage is missing");
    const auto &raw = mesh.artifacts["vertices.bin"];
    mesh.geometry.bytes.assign(reinterpret_cast<const uint8_t *>(raw.constData()),
                               reinterpret_cast<const uint8_t *>(raw.constData()) + raw.size());
    if (report.at("sha256") != sha256(mesh.geometry.bytes))
        throw std::runtime_error("Native mesh vertex digest mismatch");
    if (report.contains("vertex_identity")) {
        if (!mesh.artifacts.contains("identities.json"))
            throw std::runtime_error("Native mesh identities are missing");
        const auto identities = nlohmann::json::parse(mesh.artifacts["identities.json"].toStdString());
        if (!identities.is_array() || identities.size() != report.at("vertices").get<uint64_t>())
            throw std::runtime_error("Native mesh identity count mismatch");
        for (const auto &identity : identities) {
            if (!identity.is_array() || identity.size() != 3)
                throw std::runtime_error("Invalid native mesh identity");
            for (const auto index : {0, 2})
                if (!identity[index].is_number_integer() ||
                    (!identity[index].is_number_unsigned() && identity[index].get<int64_t>() < 0) ||
                    identity[index].get<uint64_t>() > UINT32_MAX)
                    throw std::runtime_error("Invalid native vertex system value");
            if (!identity[1].is_number_integer() ||
                (identity[1].is_number_unsigned() && identity[1].get<uint64_t>() > uint64_t(INT64_MAX)))
                throw std::runtime_error("Invalid native input vertex index");
            mesh.geometry.identities.push_back(
                {identity[0].get<uint32_t>(), identity[1].get<int64_t>(), identity[2].get<uint32_t>()});
        }
    }
    checkCancellation(cancelled);
    const bool remaining = options.value("vertex_storage", std::string("selected")) == "remaining";
    if (remaining != report.contains("vertex_storage") ||
        remaining != mesh.artifacts.contains("remaining_vertices.bin") ||
        remaining != mesh.artifacts.contains("remaining_identities.json"))
        throw std::runtime_error("Native vertex storage scope disagrees with its request");
    if (remaining) {
        const auto &storage = report.at("vertex_storage"), &selection = report.at("instance_selection");
        const auto stride = report.at("stride").get<uint64_t>();
        const auto selected = report.at("vertices").get<uint64_t>();
        const auto total = storage.at("vertices").get<uint64_t>();
        const auto full = storage.at("full_vertices").get<uint64_t>();
        const auto first = storage.at("first_vertex").get<uint64_t>();
        const auto &extra = mesh.artifacts["remaining_vertices.bin"];
        if (!stride || storage.at("schema") != 1 || storage.at("scope") != "remaining" ||
            storage.at("stride") != stride || storage.at("selected_vertices") != selected ||
            selected > total || first > full || total != full - first ||
            total > options.value("max_bytes", uint64_t(256) * 1024 * 1024) / stride ||
            mesh.geometry.bytes.size() / stride != selected || mesh.geometry.bytes.size() % stride ||
            uint64_t(extra.size()) % stride || uint64_t(extra.size()) / stride != total - selected ||
            selection.at("full_capture").at("vertices") != full ||
            selection.at("full_capture").at("sha256") != storage.at("full_sha256") ||
            (!first && storage.at("full_sha256") != storage.at("sha256")))
            throw std::runtime_error("Native remaining storage bounds or provenance mismatch");
        uint64_t expectedFirst = 0;
        if (selection.contains("first_vertex"))
            expectedFirst = selection.at("first_vertex").get<uint64_t>();
        else {
            const auto instance = options.at("instance").get<uint32_t>();
            if (report.at("requested_stage") != "vs-inputs" || (selected && instance > full / selected))
                throw std::runtime_error("Native remaining storage lacks a selected range");
            expectedFirst = selected * instance;
        }
        if (first != expectedFirst)
            throw std::runtime_error("Native remaining storage starts outside the selected instance");
        mesh.geometry.remainingBytes.assign(reinterpret_cast<const uint8_t *>(extra.constData()),
                                            reinterpret_cast<const uint8_t *>(extra.constData()) +
                                                extra.size());
        auto combined = mesh.geometry.bytes;
        combined.insert(combined.end(), mesh.geometry.remainingBytes.begin(),
                        mesh.geometry.remainingBytes.end());
        if (storage.at("sha256") != sha256(combined) ||
            storage.at("remaining_sha256") != sha256(mesh.geometry.remainingBytes))
            throw std::runtime_error("Native remaining vertex digest mismatch");
        const auto identities =
            nlohmann::json::parse(mesh.artifacts["remaining_identities.json"].toStdString());
        const auto expected = report.contains("vertex_identity") ? total - selected : 0;
        if (!identities.is_array() || identities.size() != expected)
            throw std::runtime_error("Native remaining identity count mismatch");
        const auto instance = options.at("instance").get<uint32_t>();
        for (const auto &id : mesh.geometry.identities)
            if (id.instance != instance)
                throw std::runtime_error("Native selected storage contains another instance");
        uint32_t previous = instance;
        for (const auto &id : identities) {
            if (!id.is_array() || id.size() != 3 || !id[0].is_number_integer() || id[0] < 0 ||
                id[0] > UINT32_MAX || !id[1].is_number_integer() ||
                (id[1].is_number_unsigned() && id[1].get<uint64_t>() > uint64_t(INT64_MAX)) ||
                !id[2].is_number_integer() || id[2] < 0 || id[2] > UINT32_MAX)
                throw std::runtime_error("Invalid native remaining identity");
            const auto owner = id[0].get<uint32_t>();
            if (owner <= instance || owner < previous ||
                owner >= selection.at("instance_count").get<uint32_t>())
                throw std::runtime_error("Native remaining identities are outside the storage scope");
            previous = owner;
            mesh.geometry.remainingIdentities.push_back({owner, id[1].get<int64_t>(), id[2].get<uint32_t>()});
        }
    }
    mesh.tables = postTransformTables(mesh.geometry);
    if (report.at("primitives").get<uint64_t>() !=
        report.at("vertices").get<uint64_t>() / report.at("vertices_per_primitive").get<uint32_t>())
        throw std::runtime_error("Native mesh primitive count mismatch");
    for (const auto key : {"vertex_references", "obj_vertices", "obj_faces", "obj_lines", "obj_points",
                           "obj_unavailable_reason"})
        if (report.at(key) != mesh.tables.at(key))
            throw std::runtime_error("Native mesh metadata disagrees with vertex storage");
    if (report.contains("vertex_identity"))
        for (const auto key : {"unique_vertices", "unique_identities", "conflicting_identities"})
            if (report.at(key) != mesh.tables.at(key))
                throw std::runtime_error("Native mesh identity metadata mismatch");
    const auto canonical = postTransformFiles(mesh.geometry);
    for (auto it = canonical.cbegin(); it != canonical.cend(); ++it)
        if (!mesh.artifacts.contains(it.key()) || mesh.artifacts[it.key()] != it.value())
            throw std::runtime_error("Native mesh exports disagree with vertex storage: " +
                                     it.key().toStdString());
    if (!canonical.contains("geometry.obj") && mesh.artifacts.contains("geometry.obj"))
        throw std::runtime_error("Native mesh contains an invalid position preview");
    checkCancellation(cancelled);
    return mesh;
}
} // namespace flora
