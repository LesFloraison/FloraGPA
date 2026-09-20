#pragma once
#include "replay/Replay.h"
#include <nlohmann/json.hpp>
namespace flora {
struct FrameDisplayOptions {
    std::string channel = "rgba";
    double low = 0, high = 1;
    std::optional<uint32_t> layer, sample;
    // Explicit subresource selection for the existing CLI resource override.
    std::optional<uint32_t> mip;
};
struct FrameOutput {
    Image image;
    std::vector<uint8_t> storage;
    nlohmann::json display, msaa;
};
std::vector<uint8_t> displayOutput(Bytes data, uint32_t format, const FrameDisplayOptions &options = {},
                                   const std::string &aspect = {});
nlohmann::json outputSubresource(const Resource &resource, Bytes descriptor,
                                 std::optional<uint32_t> layer = {}, bool depth = false);
FrameOutput readFrameOutput(Replay &replay, Id resource, const FrameDisplayOptions &options = {},
                            std::optional<Id> view = {}, const std::string &aspect = {});
std::string parseOutputTarget(const std::string &target);
nlohmann::json presentationInventory(const Frame &frame);
nlohmann::json selectFrameOutput(const Replay &replay, const std::string &target = "auto");
} // namespace flora
