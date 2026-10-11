#pragma once
#include "capture_model.hpp"
#include <filesystem>
#include <functional>
namespace flora::capture {
struct EncodeOptions { std::function<bool()> cancelled; };
// CPU/file-only encoder. Validates before publishing a non-overwriting IGPA v3 file.
void encode_frame(const FrozenFrame&,const std::filesystem::path&,const EncodeOptions& options={});
}
