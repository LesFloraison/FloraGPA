#pragma once
#include "core/Frame.h"
#include <nlohmann/json.hpp>
namespace flora {
std::string viewKind(uint16_t type);
std::vector<std::string> viewKeys(const std::string &kind, unsigned dimension);
nlohmann::json normalizeView(const std::string &kind, const nlohmann::json &patch);
nlohmann::json unpackView(const std::string &kind, Bytes bytes);
std::vector<uint8_t> packView(const std::string &kind, const nlohmann::json &value);
nlohmann::json describeView(const Frame &frame, Id view);
nlohmann::json mergeView(const std::string &kind, nlohmann::json previous, const nlohmann::json &patch);
std::vector<uint8_t> editedViewPayload(const Frame &frame, Id view, const nlohmann::json &descriptor);
} // namespace flora
