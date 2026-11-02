#include "application/ReplayMesh.h"
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
using Json = nlohmann::json;
using flora::ReplayMesh;
namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
} // namespace
int main(int argc, char **argv) {
    try {
        ReplayMesh mesh;
        mesh.components = 4;
        mesh.positions = {{{2, 4, 6, 2}}, {{-0.0, 1, 2, 0}}, {{3, 2, 1, 1}}};
        mesh.faces = {{{0, 1, 2}}};
        require(mesh.obj().find("v 1.0 2.0 3.0\r\n") != std::string::npos, "Perspective divide failed");
        require(mesh.obj().find("v -0.0 1.0 2.0\r\n") != std::string::npos, "Zero w or signed zero lost");
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
