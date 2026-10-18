#pragma once
#include "../format/Reader.h"
#include <string>
#include <vector>

namespace flora::dx12 {
// Applies to every use of one captured PSO creation, including repeated lists.
// The original shader digest prevents silently targeting another stage/recipe.
struct ShaderReplacement {
    uint64_t pipeline{};
    std::string stage, originalSha256;
    std::vector<uint8_t> bytecode;
};
std::string shaderBytecodeStage(format::Bytes);
}
