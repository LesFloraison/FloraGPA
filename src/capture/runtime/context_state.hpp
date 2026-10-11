#pragma once
#include "metadata.hpp"
#include <wrl/client.h>
#include <array>
#include <map>

namespace flora::capture {
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
struct RetainedObject {
    std::shared_ptr<ObjectRecord> record;
    Microsoft::WRL::ComPtr<IUnknown> native;
};
struct ContextSnapshot {
    PipelineBindings bindings;
    // Owns canonical native references only for this capture snapshot lifetime.
    // Persistent ObjectRecord itself never owns native objects.
    std::map<ObjectId,RetainedObject> objects;
    std::vector<std::string> gaps;
    void require_supported_bindings() const;
};
// Caller must exclude concurrent context mutation. This only samples bound
// pipeline state: not active queries, command lists, resource bytes or counters.
// Never call from the IPC thread. Does not apply state to the application.
ContextSnapshot snapshot_context(ID3D11DeviceContext* application_context);
}
