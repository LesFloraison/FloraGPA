#pragma once
#include "DxbcInspection.h"
#include <functional>
#include <optional>

namespace flora::checkpoint {
using Json = nlohmann::json;
using Words = std::vector<uint32_t>;
using Rows = std::vector<Words>;

struct ArrayDeclaration {
    uint32_t array, elements, components, mask;
};
using Arrays = std::vector<ArrayDeclaration>; // Declaration order is observable in capture metadata.
struct ArrayAccess {
    uint32_t array, offset;
    std::optional<Words> relative;
};
struct ArrayDestination {
    ArrayAccess access;
    uint32_t mask;
};
Arrays indexableDeclarations(const Rows &rows);
ArrayDestination indexableDestination(const Words &words, const Arrays &arrays);
std::vector<ArrayAccess> indexableAccesses(const Words &words, const Arrays &arrays);
bool changesIndex(const Words &destination, const std::optional<Words> &relative);
Words rewriteIndices(const Words &words, const std::function<Words(const ArrayAccess &)> &replace);
Words indexableOperand(uint32_t array, uint32_t index, uint32_t mask = 15,
                       std::optional<uint32_t> swizzle = {}, std::optional<uint32_t> component = {},
                       const std::optional<Words> &relative = {});

Json hullPhases(const Rows &rows);
struct DeclaredInputs {
    Json inputSlots = Json::array();
    Json known = Json::object();
    Json identity = Json::array();
    std::string domain;
};
DeclaredInputs hullInputs(const Rows &globals, const Rows &rows, const Json &phase);
DeclaredInputs domainInputs(const Rows &rows);
Words inputOperand(const Json &record, std::optional<uint32_t> component = {});

struct InstructionOperands {
    Words prefix;
    std::vector<std::pair<size_t, size_t>> ranges;
};
InstructionOperands instructionOperands(const Words &row);
bool dependentResult(const Words &row, const Arrays &arrays);
struct CallGraph {
    std::map<size_t, std::optional<uint32_t>> owners;
    std::map<size_t, uint32_t> targets;
    std::map<uint32_t, size_t> labels;
    uint32_t depth = 0;
};
CallGraph subroutines(const Rows &rows, size_t split);
struct Program {
    DxbcProgram code;
    uint32_t tag;
    std::string profile;
    size_t split;
    Json catalog;
};
Program program(Bytes raw, const std::string &stage);
} // namespace flora::checkpoint
