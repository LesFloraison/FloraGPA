#include "NativeReplayMesh.h"
#include "Geometry.h"
#include "NativeMesh.h"
#include <cmath>
#include <cstring>
#include <map>
#include <numeric>
#include <tuple>
namespace flora {
using Json = nlohmann::json;
Json nativeReplayMeshPresentationStatus(const Json &report) {
    Json reason = nullptr;
    if (report.at("requested_stage") == "hs")
        reason = "Hull phase output uses the hull inspection representation";
    else {
        bool position = false;
        for (const auto &field : report.at("attributes"))
            position |= field.at("system_value") == 1 && field.at("component_type") == 3;
        if (!position)
            reason = "No float SV_Position output; raw attributes remain available";
    }
    return {{"schema", 1}, {"available", reason.is_null()}, {"reason", reason}};
}
NativeReplayMesh presentNativeReplayMesh(const PostTransformGeometry &geometry,
                                         const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    NativeReplayMesh output;
    output.tables = postTransformTables(geometry);
    const auto &report = geometry.report;
    const auto count = report.at("vertices").get<uint64_t>(), stride = report.at("stride").get<uint64_t>();
    std::optional<uint64_t> offset;
    uint32_t components = 0;
    auto &mesh = output.positions;
    for (const auto &field : report.at("attributes"))
        if (field.at("system_value") == 1 && field.at("component_type") == 3 && !offset) {
            const auto &channels = field.at("components");
            if (channels.empty() || channels.size() > 4)
                throw std::runtime_error("Invalid native position components");
            for (size_t i = 0; i < channels.size(); ++i) {
                const auto &channel = channels[i];
                if ((!channel.is_number_integer() && !channel.is_number_unsigned()) || channel < 0 ||
                    channel > 3)
                    throw std::runtime_error("Invalid native position channel");
                mesh.componentChannels[i] = channel.get<uint32_t>();
            }
            offset = field.at("offset").get<uint64_t>();
            components = uint32_t(channels.size());
        }
    if (!offset || *offset > stride || 4 * components > stride - *offset)
        throw std::runtime_error("Native replay mesh has no bounded float position output");
    mesh.components = components;
    mesh.componentOffsets();
    mesh.indexCount = count;
    std::vector<uint8_t> combinedBytes;
    std::span<const uint8_t> storageBytes = geometry.bytes;
    if (!geometry.remainingBytes.empty()) {
        combinedBytes = geometry.bytes;
        combinedBytes.insert(combinedBytes.end(), geometry.remainingBytes.begin(),
                             geometry.remainingBytes.end());
        storageBytes = combinedBytes;
    }
    std::vector<PostTransformGeometry::Identity> combinedIdentities;
    std::span<const PostTransformGeometry::Identity> storageIdentities = geometry.identities;
    if (!geometry.remainingIdentities.empty()) {
        combinedIdentities = geometry.identities;
        combinedIdentities.insert(combinedIdentities.end(), geometry.remainingIdentities.begin(),
                                  geometry.remainingIdentities.end());
        storageIdentities = combinedIdentities;
    }
    const auto storedCount = storageBytes.size() / stride;
    std::vector<uint64_t> sources(storedCount), references(count);
    std::iota(sources.begin(), sources.end(), 0);
    std::iota(references.begin(), references.end(), 0);
    const auto &tables = output.tables.at("tables");
    const bool identified = tables.contains("unique_vertices");
    if (identified) {
        if (storageIdentities.size() != storedCount)
            throw std::runtime_error("Native presentation identity count mismatch");
        using Owner = std::pair<uint32_t, int64_t>;
        using Key = std::tuple<uint32_t, int64_t, uint64_t>;
        std::map<Owner, std::map<std::string, uint64_t>> variants;
        std::map<Key, uint64_t> unique;
        std::vector<Key> selectedKeys;
        for (size_t i = 0; i < storedCount; ++i) {
            if (!(i % 4096))
                checkCancellation(cancelled);
            const auto &identity = storageIdentities[i];
            auto &values = variants[{identity.instance, identity.vertexIndex}];
            std::string bytes(reinterpret_cast<const char *>(storageBytes.data() + i * stride), stride);
            const auto found = values.try_emplace(std::move(bytes), values.size()).first;
            const Key key{identity.instance, identity.vertexIndex, found->second};
            unique.try_emplace(key, i);
            if (i < count)
                selectedKeys.push_back(key);
        }
        sources.clear();
        std::map<Key, uint64_t> remap;
        for (const auto &[key, source] : unique) {
            remap.emplace(key, sources.size());
            sources.push_back(source);
        }
        for (size_t i = 0; i < selectedKeys.size(); ++i)
            references[i] = remap.at(selectedKeys[i]);
    }
    QByteArray compact;
    for (uint64_t i = 0; i < sources.size(); ++i) {
        if (!(i % 4096))
            checkCancellation(cancelled);
        std::array<float, 4> value{};
        const auto begin = storageBytes.data() + sources[i] * stride;
        std::memcpy(value.data(), begin + *offset, 4 * components);
        mesh.positions.push_back({value[0], value[1], value[2], value[3]});
        compact.append(reinterpret_cast<const char *>(begin), qsizetype(stride));
    }
    Json primitives = {{"faces", Json::array()}, {"lines", Json::array()}, {"points", Json::array()}};
    auto append = [&](const Json &part) {
        for (auto key : {"faces", "lines", "points"})
            for (const auto &row : part.at(key))
                primitives[key].push_back(row);
    };
    if (output.tables.at("tables").contains("input_references")) {
        mesh.indexCount = tables.at("input_references").at("rows").size();
        std::vector<int64_t> stream;
        std::optional<uint32_t> instance;
        auto topology = report.at("input_topology").get<uint32_t>();
        if (topology >= 33 && topology <= 64)
            topology = 1;
        for (const auto &row : output.tables.at("tables").at("input_references").at("rows")) {
            const auto current = row[0].get<uint32_t>();
            if (instance && current != *instance) {
                append(meshPrimitives(stream, topology));
                stream.clear();
            }
            instance = current;
            stream.push_back(row[3] == true ? 0 : int64_t(references.at(row[4].get<size_t>()) + 1));
        }
        append(meshPrimitives(stream, topology));
    } else {
        const auto factor = report.at("vertices_per_primitive").get<uint32_t>();
        std::vector<int64_t> stream;
        for (uint64_t i = 0; i < count; ++i)
            stream.push_back(int64_t(references[i] + 1));
        append(meshPrimitives(stream, factor == 3 ? 4 : factor == 2 ? 2 : 1));
    }
    for (const auto &face : primitives.at("faces"))
        mesh.faces.push_back(
            {face[0].get<uint64_t>() - 1, face[1].get<uint64_t>() - 1, face[2].get<uint64_t>() - 1});
    mesh.candidateFaceCount = mesh.faces.size();
    if (report.contains("vertex_storage"))
        output.tables["vertex_storage"] = report.at("vertex_storage");
    output.preview = primitives;
    output.preview["positions"] = Json::array();
    const auto axes = mesh.componentOffsets();
    bool finite = axes[0] >= 0 && axes[1] >= 0 && axes[2] >= 0;
    for (const auto &p : mesh.positions) {
        const auto projected = mesh.projectedPosition(p);
        if (!projected) {
            finite = false;
            break;
        }
        output.preview["positions"].push_back(*projected);
    }
    if (!finite)
        output.preview = Json::object();
    output.tables["replay_mesh"] = {
        {"schema", 1},
        {"strategy", identified ? "sorted_input_identities_and_output_variants" : "expanded_outputs"},
        {"vertex_sources", sources},
        {"reference_vertices", references},
        {"vertex_stride", stride},
        {"index_count", mesh.indexCount},
        {"position_components",
         std::vector<uint32_t>(mesh.componentChannels.begin(), mesh.componentChannels.begin() + components)},
        {"obj_unavailable_reason",
         mesh.canExportObj() ? Json(nullptr)
                             : Json("Sparse positions lack X, Y or Z; raw components remain available")}};
    output.files["post_vertices.bin"] = std::move(compact);
    output.files["post_vertices.csv"] = QByteArray::fromStdString(mesh.csv());
    output.files.remove("post_geometry.obj");
    if (mesh.canExportObj()) {
        std::string obj = mesh.obj();
        for (const auto &line : primitives.at("lines"))
            obj += "l " + std::to_string(line[0].get<uint64_t>()) + " " +
                   std::to_string(line[1].get<uint64_t>()) + "\r\n";
        for (const auto &point : primitives.at("points"))
            obj += "p " + std::to_string(point.get<uint64_t>()) + "\r\n";
        output.files["post_geometry.obj"] = QByteArray::fromStdString(obj);
    }
    output.files["presentation.json"] = QByteArray::fromStdString(output.tables.dump(2));
    checkCancellation(cancelled);
    return output;
}
NativeReplayMesh nativeReplayMesh(const Json &job, const Json &result, const NativeArtifactBytes &artifacts,
                                  const CancelCheck &cancelled) {
    auto validated = decodeNativeMesh(job, result, artifacts, cancelled);
    const auto status = nativeReplayMeshPresentationStatus(validated.geometry.report);
    if (result.at("mesh_presentation") != status || status.at("available") != true)
        throw std::runtime_error("Native mesh presentation status disagrees with captured outputs");
    auto output = presentNativeReplayMesh(validated.geometry, cancelled);
    for (const auto name :
         {"post_vertices.bin", "post_vertices.csv", "post_geometry.obj", "presentation.json"})
        if (output.files.contains(name) != artifacts.contains(name) ||
            (output.files.contains(name) && output.files[name] != artifacts[name]))
            throw std::runtime_error("Native mesh presentation disagrees with vertex storage: " +
                                     std::string(name));
    output.contextId = result.at("context_id").get<std::string>();
    for (auto it = artifacts.cbegin(); it != artifacts.cend(); ++it)
        if (!output.files.contains(it.key()))
            output.files.insert(it.key(), it.value());
    return output;
}
} // namespace flora
