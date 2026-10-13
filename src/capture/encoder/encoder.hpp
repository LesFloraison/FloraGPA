#pragma once
#include "capture_model.hpp"
#include <filesystem>
#include <functional>
#include <mutex>
#include <atomic>
namespace flora::capture {
struct EncodeOptions {
    std::function<bool()> cancelled;
    size_t msaa_byte_limit=size_t(1)<<30;
    size_t msaa_plane_limit=1024;
    // Optional publication gate. Cancellation takes the same mutex and checks
    // published before claiming cancellation. These pointers outlive encode_frame.
    std::mutex* publication_mutex=nullptr;
    std::atomic<bool>* published=nullptr;
};
// CPU/file-only encoder. Validates before publishing a non-overwriting IGPA v3 file.
void encode_frame(const FrozenFrame&,const std::filesystem::path&,const EncodeOptions& options={});
}
