#pragma once
#include <array>
#include <string_view>
namespace flora {
inline bool usesNativeReplayDevice(std::string_view command) {
    constexpr std::array commands{"replay", "buffer", "texture", "statistics", "texture-storage",
                                  "geometry", "post-geometry", "replay-pipeline", "predicate",
                                  "shader-checkpoint", "draw-resources", "coverage", "quad", "timings"};
    for (const auto value : commands)
        if (command == value) return true;
    return false;
}
} // namespace flora
