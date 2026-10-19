#pragma once
#include <charconv>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace flora::dx12 {
enum class CapturedObjectKind { Pipeline, RootSignature };
struct CapturedObjectMarker {
    CapturedObjectKind kind;
    uint64_t object{}, creation{};
};
inline std::string capturedObjectMarker(CapturedObjectKind kind, uint64_t object, uint64_t creation) {
    const auto prefix = kind == CapturedObjectKind::Pipeline ? "Flora DX12 pipeline "
                                                           : "Flora DX12 root signature ";
    return prefix + std::to_string(object) + " creation " + std::to_string(creation);
}
inline std::optional<CapturedObjectMarker> parseCapturedObjectMarker(std::string_view name) {
    for (const auto kind : {CapturedObjectKind::Pipeline, CapturedObjectKind::RootSignature}) {
        const std::string_view prefix = kind == CapturedObjectKind::Pipeline ? "Flora DX12 pipeline "
                                                                           : "Flora DX12 root signature ";
        if (!name.starts_with(prefix)) continue;
        const auto tail = name.substr(prefix.size());
        const auto split = tail.find(" creation ");
        if (split == std::string_view::npos) return std::nullopt;
        CapturedObjectMarker result{kind};
        const auto read = [](std::string_view text, uint64_t &value) {
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            return value && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
                   std::to_string(value) == text;
        };
        if (read(tail.substr(0, split), result.object) && read(tail.substr(split + 10), result.creation))
            return result;
        return std::nullopt;
    }
    return std::nullopt;
}
} // namespace flora::dx12
