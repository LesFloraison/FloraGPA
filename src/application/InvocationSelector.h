#pragma once
#include "DxbcCheckpointModel.h"
#include <filesystem>

namespace flora::checkpoint {
using InputKey = std::pair<std::string, uint32_t>;
std::vector<InputKey> inputKeys(const Json &inputSlots, const Json &known);
Json validateSelector(const Json &selector, Bytes shader, const std::string &stage, const Json &inputSlots,
                      const Json &known, const Json &phase = nullptr);
Json selectorFromSnapshot(const Json &result, const Json &registers, const Json &row,
                          const std::string &policy = "unique");
Json readSelector(const std::filesystem::path &path);
void writeSelector(const std::filesystem::path &path, const Json &selector);
void verifySelectorRecords(Bytes data, const Json &metadata);
} // namespace flora::checkpoint
