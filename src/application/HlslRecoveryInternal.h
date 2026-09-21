#pragma once
#include "HlslRecovery.h"
#include <set>
#include <tuple>

namespace flora::hlsl {
using Json = nlohmann::json;
using Strings = std::vector<std::string>;
Strings match(const std::string &pattern, const std::string &text, bool full = true);
std::string strip(const std::string &text);
std::string join(const Strings &values, const std::string &separator);
std::string replace(std::string value, const std::string &before, const std::string &after);
uint32_t number(const std::string &value);
[[noreturn]] void fail(const std::string &message);
Strings splitArguments(const std::string &text);
struct Instruction {
    std::string opcode;
    Strings arguments;
    bool precise = false;
};
Instruction parseInstruction(const std::string &line);
struct Lowerer {
    Json info;
    std::string stage;
    Strings lines, globals, declarations;
    std::map<std::string, std::pair<std::string, std::string>> resources;
    std::map<std::string, std::string> samplers;
    std::set<std::string> scalarUavs, inputs;
    std::vector<std::pair<std::string, std::pair<uint32_t, uint32_t>>> indexableTemps;
    uint32_t temps = 0, serial = 0;
    std::optional<std::array<uint32_t, 3>> threads;
    bool skipOptimization = false, instructionPrecise = false, orderedSampleMad = false;
    std::string currentOpcode, returnCode;

    // Memory resource kind and stride, plus reflection-derived hidden counter kinds.
    std::map<std::string, std::pair<std::string, uint32_t>> buffers;
    std::set<std::string> appendUavs, consumeUavs, counterUavs;
    // Graphics interpolation is per physical register lane.
    std::map<std::pair<uint32_t, char>, std::string> interpolation;
    std::string primitive;
    std::optional<uint32_t> maxOutput, inputCount;
    uint32_t instances = 1, currentStream = 0;
    std::map<uint32_t, std::string> streams;
    std::set<std::string> systemInputs;
    std::map<uint32_t, Json> geometryOutputs;

    Lowerer(Json metadata, const std::string &assembly);
    std::string raw(std::string operand, const std::string &kind = "bits") const;
    std::string write(std::string destination, std::string expression, const std::string &kind = "bits",
                      bool saturate = false);
    bool declaration(const std::string &line);
    std::string instruction(const std::string &line);
    static std::string sampleOffset(const std::string &opcode, const std::string &dimension);
    std::string translate();

    bool memoryDeclaration(std::string line);
    Strings prepareMemory(const Strings &body) const;
    std::string memoryWord(const std::string &name, const std::string &index, const std::string &offset,
                           unsigned lane) const;
    std::optional<std::string> memoryInstruction(const std::string &op, const Strings &args);
    bool graphicsDeclaration(const std::string &line);
    bool geometryDeclaration(const std::string &line);
    std::tuple<std::string, std::string, std::string> setupGraphics();
    std::tuple<std::string, std::string, std::string> setupGeometry();
    std::tuple<std::string, std::string, std::string> setupTessellation(unsigned forks, const Strings &body);
    std::optional<std::string> geometryInstruction(const std::string &op, const Strings &args) const;
};
} // namespace flora::hlsl
