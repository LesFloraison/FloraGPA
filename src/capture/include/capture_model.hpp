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
// MSAA contents are separate from ordinary GenData/subresources. Each plane
// owns exactly one array layer/sample, never resolved pixels or COM references.
struct MsaaSamplePlane {
    uint32_t array_slice=0, sample=0;
    size_t row_bytes=0;
    std::vector<uint8_t> bytes;
};
enum class MsaaSampleEncoding { color_bits, depth16, depth32, depth24_stencil8, depth32_stencil8 };
struct MsaaSamples {
    ObjectId resource=0;
    ResourceShape shape;
    uint32_t quality=0;
    // D32S8 uses depth32 followed by stencil in a zero-filled uint32. Unused
    // padding is canonical zero and does not claim to preserve source bits.
    MsaaSampleEncoding encoding=MsaaSampleEncoding::color_bits;
    std::vector<MsaaSamplePlane> planes;
};
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
    draw_indexed_instanced, dispatch, clear_rtv, clear_dsv, clear_uav_uint, clear_uav_float, map, unmap, copy_resource,
    query_end, query_get_data, update_subresource, copy_region };
// CPU observation history, not a cross-thread/context GPU ordering proof.
struct QueryEndStamp {
    uint64_t generation=0;
    ObjectId context=0;
    uint32_t thread=0;
    bool immediate=false;
    uint64_t context_serial=0; // Application End order on this context only.
};
struct FrameStartCompletion {
    ObjectId context=0,device=0;
    uint32_t thread=0;
    uint64_t last_end_serial=0,elapsed_ms=0;
    int32_t result=E_FAIL;
    uint32_t value=0;
};
struct QueryObservation {
    QueryEndStamp last_end;
    std::optional<uint64_t> end_sequence; // Only a matching End in this recording.
    bool has_output=false;
    std::optional<uint32_t> value; // EVENT BOOL, owned only after native S_OK.
};
using ResourceBox = std::array<uint32_t,6>; // left, top, front, right, bottom, back
struct CopyRegion {
    uint32_t destination_subresource=0,x=0,y=0,z=0,source_subresource=0,flags=0;
    bool extended=false;
    std::optional<ResourceBox> box;
};
struct DeferredOrigin {
    ObjectId list=0,source_context=0;
    uint64_t generation=0,execution_ordinal=0,call_sequence=0;
    bool restore_context_state=false;
};
struct RecordedCommand {
    CommandKind kind=CommandKind::unsupported;
    std::string api;
    uint64_t sequence=0;
    std::array<uint32_t,5> words{};
    std::array<float,4> floats{};
    ObjectId operand=0;
    ObjectId source=0; // CopyResource: operand is destination; no native pointer survives.
    std::optional<QueryObservation> query;
    std::optional<ResourceBox> update_box;
    std::optional<CopyRegion> copy_region;
    std::optional<DeferredOrigin> deferred;
    // Update uses words {subresource, original row pitch, depth pitch, extended, flags}; packed owned bytes.
    // Map uses words {subresource, type, flags}; bytes own the completed CPU write.
    int32_t result=0;
    bool completed=false;
    std::vector<uint8_t> data;
    std::vector<uint8_t> before_data; // NO_OVERWRITE baseline for sparse byte patches.
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
    std::optional<FrameStartCompletion> start_completion;
    std::map<ObjectId,FrozenObject> objects;
    std::map<ObjectId,ResourceContents> initial_resources;
    std::map<ObjectId,MsaaSamples> initial_msaa;
    std::vector<RecordedCommand> commands;
    ObjectId output=0;
    ResourceContents final_output;
    std::string producer="FloraCapture";
    std::optional<Presentation> presentation;
};
}
