#include "application/ReplayMesh.h"
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
using Json = nlohmann::json;
using flora::ReplayMesh;
namespace {
Json evaluate(const Json &job) {
    try {
        const auto vertices = job.at("vertices").get<std::vector<uint8_t>>();
        const auto indices = job.at("indices").get<std::vector<uint8_t>>();
        auto mesh = ReplayMesh::decode(job.at("mesh"), vertices, indices);
        return {{"ok", true},
                {"vertex_count", mesh.positions.size()},
                {"index_count", mesh.indexCount},
                {"face_count", mesh.candidateFaceCount},
                {"csv", mesh.csv()},
                {"obj", mesh.obj()}};
    } catch (const std::exception &e) {
        return {{"ok", false}, {"error", e.what()}};
    }
}
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc == 4 && std::string(argv[1]) == "--probe") {
            std::ifstream input(argv[2]);
            const auto jobs = Json::parse(input);
            Json results = Json::array();
            for (const auto &job : jobs)
                results.push_back(evaluate(job));
            std::ofstream output(argv[3], std::ios::binary);
            output << results.dump(2);
            if (!output)
                throw std::runtime_error("Cannot write probe output");
            return 0;
        }
        Json m = {{"format", {{"compType", 1}, {"compByteWidth", 4}, {"compCount", 4}}},
                  {"vertexByteStride", 16},
                  {"indexResourceId", "ResourceId::7"},
                  {"indexByteStride", 1},
                  {"baseVertex", -1},
                  {"numIndices", 9},
                  {"topology", 5}};
        const float positions[] = {2, 4, 6, 2, -0.f, 1, 2, 0, 3, 2, 1, 1};
        std::vector<uint8_t> vertices(sizeof positions);
        std::memcpy(vertices.data(), positions, sizeof positions);
        auto mesh = ReplayMesh::decode(m, vertices, std::vector<uint8_t>{1, 2, 3, 0, 1, 2, 1, 1, 1});
        require(mesh.candidateFaceCount == 3 && mesh.faces.size() == 2,
                "Candidate face count or list degenerates changed");
        require(mesh.obj().find("v 1.0 2.0 3.0\r\n") != std::string::npos, "Perspective divide failed");
        require(mesh.obj().find("v -0.0 1.0 2.0\r\n") != std::string::npos, "Zero w or signed zero lost");
        m["topology"] = 6;
        m["numIndices"] = 5;
        mesh = ReplayMesh::decode(m, vertices, std::vector<uint8_t>{1, 2, 3, 1, 1});
        require(mesh.candidateFaceCount == 2 && mesh.faces.size() == 2, "Strip degenerates changed");
        require(mesh.faces[1] == std::array<uint64_t, 3>{2, 1, 0}, "Strip winding changed");
        m["indexResourceId"] = "ResourceId::0";
        m["baseVertex"] = 100;
        m["numIndices"] = 3;
        mesh = ReplayMesh::decode(m, vertices, {});
        require(mesh.faces.size() == 1, "Nonindexed draw must ignore base vertex");
        Json job = {{"mesh", m}, {"vertices", vertices}, {"indices", Json::array()}};
        job["mesh"]["vertexByteStride"] = 0;
        require(!evaluate(job)["ok"].get<bool>(), "Zero stride accepted");
        job["mesh"] = m;
        job["mesh"]["indexResourceId"] = "ResourceId::7";
        require(!evaluate(job)["ok"].get<bool>(), "Missing indices accepted");
        require(flora::replayMeshFloat(1e-5) == "1e-05" && flora::replayMeshFloat(1e-4) == "0.0001" &&
                    flora::replayMeshFloat(1e16) == "1e+16",
                "Python float thresholds changed");
        std::cout << "Replay mesh boundaries passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
