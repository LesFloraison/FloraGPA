#include "NativeReplayMesh.h"
#include "Geometry.h"
#include "NativeMesh.h"
#include <cmath>
#include <cstring>
namespace flora {
using Json = nlohmann::json;
NativeReplayMesh nativeReplayMesh(const Json &job, const Json &result, const NativeArtifactBytes &artifacts,
                                  const CancelCheck &cancelled) {
    auto validated = decodeNativeMesh(job, result, artifacts, cancelled);
    const auto &geometry = validated.geometry;
    NativeReplayMesh output;
    output.contextId = result.at("context_id").get<std::string>();
    output.tables = std::move(validated.tables);
    const auto &report = geometry.report;
    const auto count = report.at("vertices").get<uint64_t>(), stride = report.at("stride").get<uint64_t>();
    std::optional<uint64_t> offset;
    uint32_t components = 0;
    for (const auto &field : report.at("attributes"))
        if (field.at("system_value") == 1 && field.at("component_type") == 3 && !offset) {
            const auto &channels = field.at("components");
            if (channels.empty() || channels.size() > 4)
                throw std::runtime_error("Invalid native position components");
            for (size_t i = 0; i < channels.size(); ++i)
                if (channels[i] != i)
                    throw std::runtime_error("Native position output has noncontiguous components");
            offset = field.at("offset").get<uint64_t>();
            components = uint32_t(channels.size());
        }
    if (!offset || *offset > stride || 4 * components > stride - *offset)
        throw std::runtime_error("Native replay mesh has no bounded float position output");
    auto &mesh = output.positions;
    mesh.components = components;
    mesh.indexCount = count;
    for (uint64_t i = 0; i < count; ++i) {
        if (!(i % 4096))
            checkCancellation(cancelled);
        std::array<float, 4> value{};
        std::memcpy(value.data(), geometry.bytes.data() + i * stride + *offset, 4 * components);
        mesh.positions.push_back({value[0], value[1], value[2], value[3]});
    }
    Json primitives = {{"faces", Json::array()}, {"lines", Json::array()}, {"points", Json::array()}};
    auto append = [&](const Json &part) {
        for (auto key : {"faces", "lines", "points"})
            for (const auto &row : part.at(key))
                primitives[key].push_back(row);
    };
    if (output.tables.at("tables").contains("input_references")) {
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
            stream.push_back(row[3] == true ? 0 : row[4].get<int64_t>() + 1);
        }
        append(meshPrimitives(stream, topology));
    } else {
        const auto factor = report.at("vertices_per_primitive").get<uint32_t>();
        std::vector<int64_t> stream;
        for (uint64_t i = 0; i < count; ++i)
            stream.push_back(int64_t(i + 1));
        append(meshPrimitives(stream, factor == 3 ? 4 : factor == 2 ? 2 : 1));
    }
    for (const auto &face : primitives.at("faces"))
        mesh.faces.push_back(
            {face[0].get<uint64_t>() - 1, face[1].get<uint64_t>() - 1, face[2].get<uint64_t>() - 1});
    mesh.candidateFaceCount = mesh.faces.size();
    output.preview = primitives;
    output.preview["positions"] = Json::array();
    bool finite = components >= 3;
    for (const auto &p : mesh.positions) {
        const auto w = p[3] == 0 ? 1 : p[3];
        finite &= std::isfinite(p[0] / w) && std::isfinite(p[1] / w) && std::isfinite(p[2] / w);
        output.preview["positions"].push_back({p[0] / w, p[1] / w, p[2] / w});
    }
    if (!finite)
        output.preview = Json::object();
    output.files = artifacts;
    output.files["post_vertices.csv"] = QByteArray::fromStdString(mesh.csv());
    std::string obj = mesh.obj();
    for (const auto &line : primitives.at("lines"))
        obj += "l " + std::to_string(line[0].get<uint64_t>()) + " " +
               std::to_string(line[1].get<uint64_t>()) + "\r\n";
    for (const auto &point : primitives.at("points"))
        obj += "p " + std::to_string(point.get<uint64_t>()) + "\r\n";
    output.files["post_geometry.obj"] = QByteArray::fromStdString(obj);
    checkCancellation(cancelled);
    return output;
}
} // namespace flora
