#pragma once
#include "capture_model.hpp"
#include <span>
namespace flora::capture {
inline constexpr size_t default_resource_byte_limit = size_t(1) << 30;
std::vector<SubresourceData> resource_layout(const ResourceShape& shape, size_t byte_limit=default_resource_byte_limit);
SubresourceData update_layout(const ResourceShape&, std::span<const uint8_t> descriptor, uint32_t subresource, const ResourceBox* box=nullptr);
void copy_pitched(SubresourceData& target, const void* source, size_t row_pitch, size_t slice_pitch);
void validate_indirect_arguments(const D3D11_BUFFER_DESC&, CommandKind, uint32_t offset);
void validate_copy_structure_count(const D3D11_BUFFER_DESC& destination, const D3D11_UNORDERED_ACCESS_VIEW_DESC& view,
    const D3D11_BUFFER_DESC& source, uint32_t offset);
void validate_copy_resource(const ResourceShape& destination, std::span<const uint8_t> descriptor,
    const ResourceShape& source);
void validate_copy_region(const ResourceShape& destination, std::span<const uint8_t> destination_descriptor,
    const ResourceShape& source, std::span<const uint8_t> source_descriptor, const CopyRegion&, bool same_resource);
void validate_resolve(const ResourceShape& destination, std::span<const uint8_t> destination_descriptor,
    const ResourceShape& source, std::span<const uint8_t> source_descriptor,
    uint32_t destination_subresource,uint32_t source_subresource,DXGI_FORMAT format,bool same_resource);
}
