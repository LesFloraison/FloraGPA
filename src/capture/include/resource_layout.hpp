#pragma once
#include "capture_model.hpp"
#include <span>
namespace flora::capture {
inline constexpr size_t default_resource_byte_limit = size_t(1) << 30;
std::vector<SubresourceData> resource_layout(const ResourceShape& shape, size_t byte_limit=default_resource_byte_limit);
SubresourceData update_layout(const ResourceShape&, std::span<const uint8_t> descriptor, uint32_t subresource, const ResourceBox* box=nullptr);
void copy_pitched(SubresourceData& target, const void* source, size_t row_pitch, size_t slice_pitch);
void validate_copy_resource(const ResourceShape& destination, std::span<const uint8_t> descriptor,
    const ResourceShape& source);
}
