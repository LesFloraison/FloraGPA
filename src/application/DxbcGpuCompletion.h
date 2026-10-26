#pragma once
#include "core/Cancellation.h"
#include <cstdint>
#include <memory>
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Resource;
struct ID3D11Asynchronous;
struct D3D11_MAPPED_SUBRESOURCE;

namespace flora::dxbc_exec {
enum class GpuCompletionMode { Automatic, Poll };
// One immediate context, used by one analysis task. Failures poison the instance.
class GpuCompletion {
    struct State;
    std::unique_ptr<State> state_;

  public:
    GpuCompletion(ID3D11Device *, ID3D11DeviceContext *, CancelCheck = {}, uint32_t timeoutMs = 5000,
                  GpuCompletionMode = GpuCompletionMode::Automatic);
    ~GpuCompletion();
    bool usesFence() const;
    // Called after the operation's copies and queries have been queued. All
    // subsequent readbacks share this submission's deadline.
    void submit();
    D3D11_MAPPED_SUBRESOURCE map(ID3D11Resource *, uint32_t subresource = 0, bool shortYield = false);
    void query(ID3D11Asynchronous *, void *, uint32_t size);
};
} // namespace flora::dxbc_exec
