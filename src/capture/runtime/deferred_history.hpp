#pragma once
#include "capture_model.hpp"
#include "deferred_bindings.hpp"
#include <deque>
#include <memory>
namespace flora::capture {
struct UpdateSource;
struct DeferredCall {
    CommandKind kind=CommandKind::unsupported;
    std::string api;
    uint64_t sequence=0;
    DWORD thread=0;
    ObjectId operand=0,source=0;
    std::array<uint32_t,5> words{};
    std::array<float,4> floats{};
    std::optional<ResourceBox> update_box;
    std::optional<CopyRegion> copy_region;
    std::optional<DeferredBinding> binding;
    // Effective native bindings before this action, including runtime hazard
    // nulling and Finish(TRUE) retention. No execution-time GPU contents.
    std::shared_ptr<const PipelineBindings> state;
    std::vector<uint8_t> data;
    HRESULT result=S_OK;
    bool completed=true;
};
struct DeferredCommandList {
    ObjectId context=0,device=0;
    uint64_t generation=0;
    uint32_t recording_thread=0;
    bool restore_deferred_state=false,starts_default=true;
    std::vector<DeferredCall> calls;
    DeferredDefinitionGraph definitions;
    std::vector<std::string> gaps;
    size_t owned_bytes=0;
};
struct CommandListExecution {
    ObjectId list=0,source_context=0;
    uint64_t generation=0,context_serial=0;
    uint32_t thread=0;
    bool immediate=false,restore_context_state=false,metadata_available=false;
};
// ObjectRecord::mutex protects this mutable per-context history. It owns no COM
// references; execution entries are CPU call observations, not a GPU fence proof.
struct DeferredHistory {
    static constexpr size_t call_limit=4096,byte_limit=size_t(64)<<20,execution_limit=256;
    DeferredCommandList pending;
    uint64_t finishes=0,failed_finishes=0,execution_serial=0;
    std::deque<CommandListExecution> executions;
    std::optional<size_t> binding_pending;
    // Only live mappings contain borrowed pointers. Unmap removes the entry
    // before copying; immutable command-list histories never contain pointers.
    struct Mapping {
        size_t command=0;
        DWORD thread=0;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        SubresourceData layout;
    };
    std::map<std::pair<ObjectId,UINT>,Mapping> mappings;
};
void observe_deferred_call(ID3D11DeviceContext*,const char*,CommandKind,const std::array<uint32_t,5>&,
    IUnknown*,const float*,const UINT*,IUnknown*,const UpdateSource*,const CopyRegion*) noexcept;
void remember_deferred_finish(ID3D11DeviceContext*,HRESULT,BOOL,ID3D11CommandList*) noexcept;
void remember_deferred_map_result(ID3D11DeviceContext*,ID3D11Resource*,UINT,HRESULT,const D3D11_MAPPED_SUBRESOURCE*) noexcept;
void remember_deferred_unmap(ID3D11DeviceContext*,ID3D11Resource*,UINT) noexcept;
void remember_command_list_execution(ID3D11DeviceContext*,ID3D11CommandList*,BOOL) noexcept;
std::string command_list_gap(IUnknown*);
}
