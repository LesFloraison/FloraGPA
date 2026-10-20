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
        require(mesh.firstReferences == std::vector<uint32_t>{0, 1, 2}, "First indexed references are not Draw ordinals");
        auto sparse = m; sparse["numIndices"] = 3;
        const auto unreferenced = ReplayMesh::decode(sparse, vertices, std::vector<uint8_t>{1, 1, 3});
        require(unreferenced.firstReferences == std::vector<uint32_t>{0, UINT32_MAX, 2}, "Unreferenced row was assigned an invocation");
        require(mesh.obj().find("v 1.0 2.0 3.0\r\n") != std::string::npos, "Perspective divide failed");
        require(mesh.obj().find("v -0.0 1.0 2.0\r\n") != std::string::npos, "Zero w or signed zero lost");
        m["topology"] = 6;
        m["numIndices"] = 5;
        mesh = ReplayMesh::decode(m, vertices, std::vector<uint8_t>{1, 2, 3, 1, 1});
        require(mesh.candidateFaceCount == 2 && mesh.faces.size() == 2, "Strip degenerates changed");
        require(mesh.faces[1] == std::array<uint64_t, 3>{2, 1, 0}, "Strip winding changed");
        {
            auto strip = m; strip["baseVertex"] = -5; strip["allowRestart"] = true;
            strip["numIndices"] = 12; strip["indexByteStride"] = 2; strip["restartIndex"] = 65535;
            const uint16_t cutIndices[]{65535, 5, 6, 7, 5, 65535, 65535, 5, 6, 7, 65535, 65535};
            std::vector<uint8_t> raw(sizeof cutIndices); memcpy(raw.data(), cutIndices, sizeof cutIndices);
            const auto restarted = ReplayMesh::decode(strip, vertices, raw);
            require(restarted.faces == std::vector<std::array<uint64_t,3>>{{0,1,2},{2,1,0},{0,1,2}},
                    "Strip cut did not reset winding or created a cross-segment triangle");
            require(restarted.firstReferences == std::vector<uint32_t>{1,2,3}, "Cut index shifted original invocation ordinals");
            strip["numIndices"] = 2;
            require(ReplayMesh::decode(strip, vertices, std::vector<uint8_t>{255,255,255,255}).faces.empty(),
                    "All-cut strip created geometry");
            strip["allowRestart"] = false; strip["numIndices"] = 3; strip["baseVertex"] = 0;
            strip["restartIndex"] = 0;
            const auto ordinary = ReplayMesh::decode(strip, vertices, std::vector<uint8_t>{0,0,1,0,2,0});
            require(ordinary.faces.size() == 1 && ordinary.firstReferences[0] == 0, "Disabled restart consumed an index");
            strip["allowRestart"] = true; strip["topology"] = 5;
            require(ReplayMesh::decode(strip, vertices, std::vector<uint8_t>{0,0,1,0,2,0}).faces.size() == 1,
                    "Triangle-list index was interpreted as a cut");
            strip["topology"] = 6; strip["indexByteStride"] = 4; strip["restartIndex"] = UINT32_MAX;
            const uint32_t wide[]{0,1,2,UINT32_MAX,0,1,2}; raw.resize(sizeof wide); memcpy(raw.data(), wide, sizeof wide);
            strip["numIndices"] = 7;
            require(ReplayMesh::decode(strip, vertices, raw).faces == std::vector<std::array<uint64_t,3>>{{0,1,2},{0,1,2}},
                    "32-bit strip cut failed");
        }
        m["indexResourceId"] = "ResourceId::0";
        m["baseVertex"] = 100;
        m["numIndices"] = 3;
        mesh = ReplayMesh::decode(m, vertices, {});
        require(mesh.faces.size() == 1, "Nonindexed draw must ignore base vertex");
        require(mesh.firstReferences == std::vector<uint32_t>{0, 1, 2}, "Nonindexed ordinals are incorrect");
        auto input = m; input["perspective_divide"] = false; input["input"] = {{"first_vertex", "9007199254740993"}};
        auto inputMesh = ReplayMesh::decode(input, vertices, {});
        require(inputMesh.obj().find("v 2.0 4.0 6.0\r\n") != std::string::npos, "Input attribute must not be perspective divided");
        require(inputMesh.csv().find("9007199254740993,2.0,4.0,6.0,2.0") != std::string::npos, "Input source vertex identity lost");
        Json job = {{"mesh", m}, {"vertices", vertices}, {"indices", Json::array()}};
        job["mesh"] = input;
        for (const auto invalid : {"-1", "1x", "01", "18446744073709551616", "18446744073709551615"}) {
            job["mesh"]["input"]["first_vertex"] = invalid;
            require(!evaluate(job)["ok"].get<bool>(), "Invalid or overflowing input vertex identity accepted");
        }
        job["mesh"] = m;
        job["mesh"]["vertexByteStride"] = 0;
        require(!evaluate(job)["ok"].get<bool>(), "Zero stride accepted");
        job["mesh"] = m;
        job["mesh"]["indexResourceId"] = "ResourceId::7";
        require(!evaluate(job)["ok"].get<bool>(), "Missing indices accepted");
        auto typed = m; typed["attribute_byte_offset"] = 4; typed["format"]["compCount"] = 3;
        typed["format"]["compType"] = 4; typed["perspective_divide"] = false;
        uint32_t words[]{0, UINT32_MAX, 0x80000000, 16777217};
        std::vector<uint8_t> typedBytes(sizeof words); memcpy(typedBytes.data(), words, sizeof words);
        const auto unsignedOutput = ReplayMesh::decode(typed, typedBytes, {});
        require(unsignedOutput.positions[0] == std::array<double,4>{4294967295.,2147483648.,16777217.,0},
                "Integer output attribute lost bits through float32 conversion");
        typed["format"]["compType"] = 5;
        const auto signedOutput = ReplayMesh::decode(typed, typedBytes, {});
        require(signedOutput.positions[0] == std::array<double,4>{-1.,-2147483648.,16777217.,0}, "Signed output interpretation failed");
        job = {{"mesh", typed}, {"vertices", typedBytes}, {"indices", Json::array()}};
        for (unsigned normalized : {2u, 3u}) {
            job["mesh"]["format"]["compType"] = normalized;
            require(!evaluate(job)["ok"].get<bool>(), "Normalized output was decoded as an integer");
        }
        job["mesh"] = typed;
        job["mesh"]["attribute_byte_offset"] = 8;
        require(!evaluate(job)["ok"].get<bool>(), "Attribute crossed vertex stride");
        job["mesh"] = typed; typedBytes.pop_back(); job["vertices"] = typedBytes;
        require(!evaluate(job)["ok"].get<bool>(), "Partial output vertex silently accepted");
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
