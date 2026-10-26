#include "DxbcGpuCompletion.h"
#include <algorithm>
#include <chrono>
#include <d3d11_4.h>
#include <string>
#include <thread>
#include <wrl/client.h>

namespace flora::dxbc_exec {
namespace {
using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;
void checked(HRESULT hr, const char *operation) {
    if (FAILED(hr))
        throw std::runtime_error(std::string(operation) + ": " + std::to_string(hr));
}
bool unavailable(HRESULT hr) {
    return hr == E_NOINTERFACE || hr == E_NOTIMPL || hr == DXGI_ERROR_UNSUPPORTED;
}
} // namespace
struct GpuCompletion::State {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3D11Fence> fence;
    HANDLE event = nullptr;
    CancelCheck cancelled;
    uint32_t timeoutMs;
    uint64_t value = 0;
    Clock::time_point deadline, yieldUntil;
    bool failed = false, submitted = false, waited = false;
    ~State() {
        // No destructor wait: cancellation must not block on unfinished GPU work.
        // Release the private completion interfaces before the owned event handle.
        fence.Reset();
        context4.Reset();
        context.Reset();
        device.Reset();
        if (event)
            CloseHandle(event);
    }
    void usable() const {
        if (failed)
            throw std::runtime_error("Recreate native GPU completion after a failed operation");
    }
    void check() const {
        usable();
        if (!submitted)
            throw std::runtime_error("Native GPU completion has no submitted operation");
        checkCancellation(cancelled);
        if (Clock::now() >= deadline)
            throw std::runtime_error("Native GPU operation timed out");
    }
    void wait() {
        if (!fence || waited)
            return;
        for (;;) {
            check();
            const auto completed = fence->GetCompletedValue();
            if (completed == UINT64_MAX) {
                checked(device->GetDeviceRemovedReason(), "Native GPU completion device removed");
                throw std::runtime_error("Native GPU completion fence was removed");
            }
            if (completed >= value) {
                const auto status = WaitForSingleObject(event, 0);
                if (status == WAIT_OBJECT_0) {
                    waited = true;
                    return;
                }
                if (status != WAIT_TIMEOUT)
                    throw std::runtime_error("Cannot check native GPU completion event");
            }
            const auto remaining =
                std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (remaining <= 0)
                throw std::runtime_error("Native GPU operation timed out");
            const auto status = WaitForSingleObject(event, DWORD((std::min)(int64_t(10), remaining)));
            if (status != WAIT_OBJECT_0 && status != WAIT_TIMEOUT)
                throw std::runtime_error("Cannot wait for native GPU completion event");
        }
    }
    void retry(bool shortYield) const {
        if (Clock::now() >= deadline)
            throw std::runtime_error("Native GPU operation timed out");
        if (shortYield && Clock::now() < yieldUntil)
            std::this_thread::yield();
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
};
GpuCompletion::GpuCompletion(ID3D11Device *device, ID3D11DeviceContext *context, CancelCheck cancelled,
                             uint32_t timeoutMs, GpuCompletionMode mode)
    : state_(std::make_unique<State>()) {
    if (!device || !context || !timeoutMs || timeoutMs > 60000 ||
        (mode != GpuCompletionMode::Automatic && mode != GpuCompletionMode::Poll) ||
        context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        throw std::invalid_argument("Invalid native GPU completion configuration");
    auto &s = *state_;
    ComPtr<ID3D11Device> owner;
    context->GetDevice(&owner);
    if (owner.Get() != device)
        throw std::invalid_argument("Native GPU completion context belongs to a different device");
    s.device = device;
    s.context = context;
    s.cancelled = std::move(cancelled);
    s.timeoutMs = timeoutMs;
    if (mode == GpuCompletionMode::Poll)
        return;
    ComPtr<ID3D11Device5> device5;
    auto hr = s.device.As(&device5);
    if (unavailable(hr))
        return;
    checked(hr, "Query native completion device");
    hr = s.context.As(&s.context4);
    if (unavailable(hr))
        return;
    checked(hr, "Query native completion context");
    hr = device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.fence));
    if (unavailable(hr)) {
        s.context4.Reset();
        return;
    }
    checked(hr, "Create native completion fence");
    s.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!s.event)
        throw std::runtime_error("Cannot create native GPU completion event");
}
GpuCompletion::~GpuCompletion() = default;
bool GpuCompletion::usesFence() const { return bool(state_->fence); }
void GpuCompletion::submit() {
    auto &s = *state_;
    s.usable();
    try {
        // A previous event registration must finish before the event is reused.
        if (s.submitted && !s.waited)
            throw std::runtime_error("Native GPU completion submission is still pending");
        s.deadline = Clock::now() + std::chrono::milliseconds(s.timeoutMs);
        s.yieldUntil = Clock::now() + std::chrono::microseconds(500);
        s.submitted = true;
        s.waited = false;
        if (s.fence) {
            if (s.value == UINT64_MAX - 1)
                throw std::runtime_error("Native GPU completion fence value exhausted");
            if (!ResetEvent(s.event))
                throw std::runtime_error("Cannot reset native GPU completion event");
            checked(s.context4->Signal(s.fence.Get(), ++s.value), "Signal native GPU completion");
            checked(s.fence->SetEventOnCompletion(s.value, s.event), "Register native GPU completion");
        }
        s.context->Flush();
    } catch (...) {
        s.failed = true;
        throw;
    }
}
D3D11_MAPPED_SUBRESOURCE GpuCompletion::map(ID3D11Resource *resource, uint32_t subresource, bool shortYield) {
    auto &s = *state_;
    try {
        s.usable();
        if (!resource)
            throw std::invalid_argument("Missing native GPU readback resource");
        s.wait();
        for (;;) {
            s.check();
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const auto hr =
                s.context->Map(resource, subresource, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
            if (hr == S_OK) {
                s.waited = true;
                return mapped;
            }
            if (hr != DXGI_ERROR_WAS_STILL_DRAWING)
                checked(hr, "Read native GPU result");
            s.retry(shortYield);
        }
    } catch (...) {
        s.failed = true;
        throw;
    }
}
void GpuCompletion::query(ID3D11Asynchronous *query, void *data, uint32_t size) {
    auto &s = *state_;
    try {
        s.usable();
        if (!query || (!data && size))
            throw std::invalid_argument("Invalid native GPU completion query");
        s.wait();
        for (;;) {
            s.check();
            const auto hr = s.context->GetData(query, data, size, D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr == S_OK) {
                s.waited = true;
                return;
            }
            checked(hr, "Read native GPU query");
            s.retry(false);
        }
    } catch (...) {
        s.failed = true;
        throw;
    }
}
} // namespace flora::dxbc_exec
