#pragma once
// Capture-owned CPU data contract. No COM ownership, runtime helpers or replay types.
#include <d3d11_4.h>
#include <optional>
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
namespace flora::capture {
struct DeviceRecordDesc { uint32_t feature_level, creation_flags; };
struct ContextRecordDesc { uint32_t type, flags; };
struct InputElementRecord {
    std::string semantic;
    uint32_t semantic_index, format, slot, offset, slot_class, step_rate;
};
struct StreamElementRecord {
    std::string semantic;
    bool has_semantic;
    uint32_t stream, semantic_index, start_component, component_count, output_slot;
};
struct ResourceShape {
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width=0, height=1, depth=1, mip_levels=1, array_size=1, samples=1, misc_flags=0;
};
struct SubresourceData {
    uint32_t index=0, mip=0, array_slice=0, width=0, height=1, depth=1;
    size_t row_bytes=0, rows=1, slice_bytes=0;
    std::vector<uint8_t> bytes;
};
using ObjectId = uint64_t;
enum class ShaderStage : size_t { vertex, hull, domain, geometry, pixel, compute, count };
struct ShaderBindings {
    ObjectId shader=0;
    std::vector<ObjectId> classes;
    std::array<ObjectId,D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> buffers{};
    std::array<UINT,D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> first_constants{},constant_counts{};
    std::array<ObjectId,D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> resources{};
    std::array<ObjectId,D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> samplers{};
};
// Capture-owned values only. No replay State, Frame or wire state block.
struct PipelineBindings {
    ObjectId context=0,device=0;
    D3D_FEATURE_LEVEL feature_level{};
    bool constant_ranges=false;
    UINT uav_slots=0;
    std::array<ShaderBindings,size_t(ShaderStage::count)> shaders;
    ObjectId input_layout=0,index_buffer=0,rasterizer=0,blend=0,depth_stencil=0,depth_view=0,predicate=0;
    std::array<ObjectId,D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vertices{};
    std::array<UINT,D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> strides{},offsets{};
    DXGI_FORMAT index_format=DXGI_FORMAT_UNKNOWN;
    UINT index_offset=0,stencil_ref=0,sample_mask=0;
    D3D11_PRIMITIVE_TOPOLOGY topology=D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    std::array<float,4> blend_factor{};
    BOOL predicate_value=FALSE;
    std::vector<D3D11_VIEWPORT> viewports;
    std::vector<D3D11_RECT> scissors;
    std::array<ObjectId,D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> render_targets{};
    std::array<ObjectId,D3D11_1_UAV_SLOT_COUNT> om_uavs{},cs_uavs{};
    std::array<ObjectId,D3D11_SO_BUFFER_SLOT_COUNT> stream_targets{};
    // ResourceMinLOD is resource state, not encoded in an SRV descriptor.
    std::map<ObjectId,float> resource_min_lod;
};
enum class CommandKind { unsupported, read, binding, draw, draw_indexed, draw_instanced,
    draw_indexed_instanced, dispatch, clear_rtv, clear_dsv, clear_uav_uint, clear_uav_float };
struct RecordedCommand {
    CommandKind kind=CommandKind::unsupported;
    std::string api;
    uint64_t sequence=0;
    std::array<uint32_t,5> words{};
    std::array<float,4> floats{};
    ObjectId operand=0;
    PipelineBindings bindings;
    std::map<ObjectId,uint32_t> uav_counters;
};
struct FrozenObject {
    ObjectId id=0,device=0,resource=0,class_linkage=0;
    std::string kind;
    std::vector<uint8_t> descriptor,creation_bytes;
    std::vector<InputElementRecord> input_elements;
    std::vector<StreamElementRecord> stream_elements;
    std::vector<UINT> stream_strides;
    UINT rasterized_stream=0;
    bool creation_observed=false;
};
struct ResourceContents { ResourceShape shape; std::vector<SubresourceData> subresources; };
// Native DX11 exposes logical backbuffer zero across presentation. Buffer count
// and swap effect remain the actual application descriptor in the wire record.
inline bool supported_swap_buffers(UINT effect,UINT count){
    if(effect==DXGI_SWAP_EFFECT_DISCARD || effect==DXGI_SWAP_EFFECT_SEQUENTIAL)return count>=1 && count<=16;
    if(effect==DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL || effect==DXGI_SWAP_EFFECT_FLIP_DISCARD)return count>=2 && count<=16;
    return false;
}
struct Presentation {
    DXGI_SWAP_CHAIN_DESC descriptor{};
    UINT sync_interval=0,flags=0;
    bool extended=false;
};
struct FrozenFrame {
    std::map<ObjectId,FrozenObject> objects;
    std::map<ObjectId,ResourceContents> initial_resources;
    std::vector<RecordedCommand> commands;
    ObjectId output=0;
    ResourceContents final_output;
    std::string producer="FloraCapture";
    std::optional<Presentation> presentation;
};
}
