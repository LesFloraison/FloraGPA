#pragma once
#include <d3d11_3.h>
#include <wrl/client.h>
#include <stdexcept>

// Real application EVENT calls surrounding MSAA work. The private frame-start
// fence must complete the first End; no captured in-frame completion is lowered.
struct MsaaEventFixture {
    Microsoft::WRL::ComPtr<ID3D11Query> query;
    void prepare(ID3D11Device* device, ID3D11DeviceContext* context, bool enabled, bool extended) {
        if (!enabled) return;
        HRESULT hr;
        if (extended) {
            Microsoft::WRL::ComPtr<ID3D11Device3> device3;
            if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device3)))) throw std::runtime_error("Query1 device unavailable");
            D3D11_QUERY_DESC1 desc{D3D11_QUERY_EVENT, 0, D3D11_CONTEXT_TYPE_ALL};
            Microsoft::WRL::ComPtr<ID3D11Query1> query1;
            hr = device3->CreateQuery1(&desc, &query1);
            if (SUCCEEDED(hr)) hr = query1.As(&query);
        } else {
            D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
            hr = device->CreateQuery(&desc, &query);
        }
        if (FAILED(hr)) throw std::runtime_error("Cannot create MSAA EVENT query");
        context->End(query.Get());
    }
    void observe(ID3D11DeviceContext* context) {
        if (!query) return;
        BOOL value = FALSE;
        if (context->GetData(query.Get(), &value, sizeof value, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || value != TRUE ||
            context->GetData(query.Get(), nullptr, 0, 0) != S_OK)
            throw std::runtime_error("Frame-start completion did not preserve native EVENT observations");
    }
    void end(ID3D11DeviceContext* context) { if (query) context->End(query.Get()); }
};
