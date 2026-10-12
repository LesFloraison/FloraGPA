#pragma once
#include "capture_model.hpp"
#include <memory>
#include <wrl/client.h>
namespace flora::capture {
// Native ownership is separate from the immutable CPU history. A native
// context/list lifetime entry owns this map, never ObjectRecord or FrozenFrame.
using DeferredResourceLeases=std::map<ObjectId,Microsoft::WRL::ComPtr<ID3D11Resource>>;
void retain_deferred_resource(ID3D11DeviceContext* application_context,IUnknown* native_operand);
void finish_deferred_resources(ID3D11DeviceContext* application_context,HRESULT result,ID3D11CommandList* application_list);
// Caller owns the list for this lookup; the returned lease pins resources for
// readback without promoting any weak native address from the registry.
std::shared_ptr<const DeferredResourceLeases> command_list_resources(IUnknown* application_list);
}
