// Development-only merged Command Lists: two child contexts plus a parent,
// retained buffer versions and independent restore flags at each boundary.
#include "original_capture_control.h"
#include "texture_probe_helpers.h"
int wmain(int argc, wchar_t **argv) {
    if (argc != 3 && argc != 5)
        return 2;
    output = argv[1];
    if (fs::exists(output))
        return 2;
    fs::create_directories(output);
    try {
        const int mode = std::stoi(argv[2]);
        if (mode < 0 || mode >= 64)
            throw std::runtime_error("Invalid deferred merge mode");
        const bool childFinishRestore = mode & 1, mergeRestore = mode & 2, parentFinishRestore = mode & 4,
                   executeRestore = mode & 8, each = mode & 16, immutableChildren = mode & 32;
        if (argc == 5 && !LoadLibraryExW(argv[4], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))
            throw std::runtime_error("Cannot load development shim");
        WNDCLASSW wc{};
        wc.lpfnWndProc = windowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"FloraPresentCaptureProbe";
        if (!RegisterClassW(&wc))
            throw std::runtime_error("RegisterClass failed");
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> immediate, contexts[3];
        checked(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                  D3D11_SDK_VERSION, &device, nullptr, &immediate));
        for (auto &context : contexts)
            checked(device->CreateDeferredContext(0, &context));
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory> factory;
        checked(device.As(&dxgi));
        checked(dxgi->GetAdapter(&adapter));
        checked(adapter->GetParent(IID_PPV_ARGS(&factory)));
        Chain chain;
        createChain(chain, device.Get(), factory.Get(), DXGI_SWAP_EFFECT_DISCARD);
        DXGI_ADAPTER_DESC adapterDesc{};
        checked(adapter->GetDesc(&adapterDesc));
        auto compile = [&](const char *source, const char *profile) {
            ComPtr<ID3DBlob> code, errors;
            checked(D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, "main", profile, 0, 0,
                               &code, &errors));
            return code;
        };
        auto csCode = compile("cbuffer C:register(b0){uint factor;uint tag;uint2 pad;}"
                              "Buffer<uint> src:register(t0);RWBuffer<uint> dst:register(u0);"
                              "[numthreads(1,1,1)]void main(){dst[0]=dst[0]*factor+src[0]+tag;"
                              "dst[1]++;dst[2]=src[0];dst[3]=factor;}",
                              "cs_5_0");
        auto vsCode = compile("float4 main(uint i:SV_VertexID):SV_Position{"
                              "return float4(i==2?3:-1,i==1?3:-1,0,1);}",
                              "vs_5_0");
        auto psCode = compile("Buffer<uint> result:register(t0);float4 main():SV_Target{"
                              "bool ok=result[0]==126996&&result[1]==9&&result[2]==11&&result[3]==3;"
                              "return float4(!ok,ok,0,1);}",
                              "ps_5_0");
        ComPtr<ID3D11ComputeShader> cs;
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        checked(
            device->CreateComputeShader(csCode->GetBufferPointer(), csCode->GetBufferSize(), nullptr, &cs));
        checked(
            device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs));
        checked(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps));
        auto buffer = [&](UINT bytes, UINT bind, D3D11_USAGE usage, UINT cpu) {
            D3D11_BUFFER_DESC desc{bytes, usage, bind, cpu, 0, 0};
            ComPtr<ID3D11Buffer> value;
            checked(device->CreateBuffer(&desc, nullptr, &value));
            return value;
        };
        auto constants = buffer(16, D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
        ComPtr<ID3D11Buffer> childConstants[2];
        if (immutableChildren) {
            for (unsigned child = 0; child < 2; ++child) {
                const UINT values[]{child ? 5u : 3u, child ? 2u : 1u, 0, 0};
                D3D11_BUFFER_DESC desc{16, D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
                D3D11_SUBRESOURCE_DATA data{values, 0, 0};
                checked(device->CreateBuffer(&desc, &data, &childConstants[child]));
            }
        }
        auto baseline = buffer(16, D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DEFAULT, 0);
        auto input = buffer(4, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0);
        auto result =
            buffer(16, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, D3D11_USAGE_DEFAULT, 0);
        auto staging = buffer(16, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
        auto srv = [&](ID3D11Buffer *storage, UINT count) {
            D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.Format = DXGI_FORMAT_R32_UINT;
            desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            desc.Buffer.NumElements = count;
            ComPtr<ID3D11ShaderResourceView> value;
            checked(device->CreateShaderResourceView(storage, &desc, &value));
            return value;
        };
        auto inputView = srv(input.Get(), 1), resultView = srv(result.Get(), 4);
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_UINT;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = 4;
        ComPtr<ID3D11UnorderedAccessView> uav;
        checked(device->CreateUnorderedAccessView(result.Get(), &ud, &uav));
        unsigned finishChecks = 0, finishFailures = 0, executeChecks = 0, executeFailures = 0;
        unsigned mergeChecks = 0, mergeFailures = 0, parentFinishChecks = 0, parentFinishFailures = 0;
        auto uploadConstants = [&](ID3D11DeviceContext *ctx, UINT factor, UINT tag) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            checked(ctx->Map(constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
            const UINT values[]{factor, tag, 0, 0};
            memcpy(mapped.pData, values, sizeof(values));
            ctx->Unmap(constants.Get(), 0);
        };
        auto emit = [&](ID3D11DeviceContext *ctx, unsigned index) {
            if (!immutableChildren)
                uploadConstants(ctx, index ? 5 : 3, index ? 2 : 1);
            auto cb = immutableChildren ? childConstants[index].Get() : constants.Get();
            auto source = inputView.Get();
            auto target = uav.Get();
            ctx->CSSetConstantBuffers(0, 1, &cb);
            ctx->CSSetShaderResources(0, 1, &source);
            ctx->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
            ctx->CSSetShader(cs.Get(), nullptr, 0);
            ctx->Dispatch(1, 1, 1);
        };
        auto checkBindings = [&](ID3D11DeviceContext *ctx, ID3D11ComputeShader *expectedShader,
                                 ID3D11Buffer *expectedBuffer) {
            ComPtr<ID3D11ComputeShader> shader;
            ComPtr<ID3D11Buffer> cb;
            ctx->CSGetShader(&shader, nullptr, nullptr);
            ctx->CSGetConstantBuffers(0, 1, &cb);
            return shader.Get() == expectedShader && cb.Get() == expectedBuffer;
        };
        ComPtr<ID3D11CommandList> lists[2], merged;
        auto record = [&] {
            for (unsigned i = 0; i < 2; ++i) {
                contexts[i]->ClearState();
                emit(contexts[i].Get(), i);
                lists[i].Reset();
                checked(contexts[i]->FinishCommandList(childFinishRestore, &lists[i]));
                if (!lists[i])
                    throw std::runtime_error("Finish returned no real native list");
                ++finishChecks;
                if (!checkBindings(contexts[i].Get(), childFinishRestore ? cs.Get() : nullptr,
                                   childFinishRestore
                                       ? (immutableChildren ? childConstants[i].Get() : constants.Get())
                                       : nullptr))
                    ++finishFailures;
            }
            auto *parent = contexts[2].Get();
            parent->ClearState();
            for (unsigned child : {0u, 1u, 0u}) {
                parent->ClearState();
                auto cb = baseline.Get();
                parent->CSSetConstantBuffers(0, 1, &cb);
                parent->ExecuteCommandList(lists[child].Get(), mergeRestore);
                ++mergeChecks;
                if (!checkBindings(parent, nullptr, mergeRestore ? baseline.Get() : nullptr))
                    ++mergeFailures;
            }
            merged.Reset();
            checked(parent->FinishCommandList(parentFinishRestore, &merged));
            if (!merged)
                throw std::runtime_error("Parent Finish returned no real native list");
            ++parentFinishChecks;
            if (!checkBindings(parent, nullptr,
                               parentFinishRestore && mergeRestore ? baseline.Get() : nullptr))
                ++parentFinishFailures;
        };
        if (!each)
            record();
        D3D11_TEXTURE2D_DESC td{};
        chain.buffer->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> imageStaging;
        checked(device->CreateTexture2D(&td, nullptr, &imageStaging));
        std::ofstream report(output / L"oracle.json");
        report.exceptions(std::ios::badbit | std::ios::failbit);
        report << "{\"mode\":" << mode << ",\"vendor\":" << adapterDesc.VendorId
               << ",\"device\":" << adapterDesc.DeviceId << ",\"child_finish_restore\":" << childFinishRestore
               << ",\"merge_restore\":" << mergeRestore
               << ",\"parent_finish_restore\":" << parentFinishRestore
               << ",\"execute_restore\":" << executeRestore << ",\"record_each_frame\":" << each
               << ",\"immutable_children\":" << immutableChildren << ",\"frames\":[";
        unsigned storageFailures = 0, imageFailures = 0;
        for (unsigned frame = 0; frame < 12; ++frame) {
            if (argc == 5 && frame == 5) {
                auto module = GetModuleHandleW(L"shimd3d64.dll");
                flora::research::selectOriginalPrimarySwapChain(module, chain.swap.Get());
                auto request =
                    reinterpret_cast<void(WINAPI *)(const wchar_t *, void(WINAPI *)(const wchar_t *))>(
                        GetProcAddress(module, "CaptureNextFrame"));
                request(argv[3], captured);
            }
            const auto finishBefore = finishFailures, executeBefore = executeFailures,
                       mergeBefore = mergeFailures, parentFinishBefore = parentFinishFailures;
            immediate->ClearState();
            {
                // Real application writes/readback retain the sentinel descriptor and bytes.
                const std::array<UINT, 4> sentinel{101, 103, 107, 109};
                immediate->UpdateSubresource(baseline.Get(), 0, nullptr, sentinel.data(), 0, 0);
                immediate->CopyResource(staging.Get(), baseline.Get());
                D3D11_MAPPED_SUBRESOURCE mapped{};
                checked(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
                std::array<UINT, 4> actual{};
                memcpy(actual.data(), mapped.pData, sizeof(actual));
                immediate->Unmap(staging.Get(), 0);
                save(output / ("frame-" + std::to_string(frame) + "-baseline.bin"),
                     std::vector<uint8_t>(reinterpret_cast<uint8_t *>(actual.data()),
                                          reinterpret_cast<uint8_t *>(actual.data() + actual.size())));
                if (actual != sentinel)
                    throw std::runtime_error("Baseline sentinel readback mismatch");
            }
            const UINT zero[4]{};
            immediate->ClearUnorderedAccessViewUint(uav.Get(), zero);
            const float black[]{0, 0, 0, 1};
            immediate->ClearRenderTargetView(chain.rtv.Get(), black);
            if (each)
                record();
            std::array<UINT, 4> expected{};
            if (frame)
                report << ',';
            report << "{\"frame\":" << frame << ",\"steps\":[";
            const UINT inputs[]{2, 5, 11};
            for (unsigned step = 0; step < 3; ++step) {
                immediate->UpdateSubresource(input.Get(), 0, nullptr, &inputs[step], 0, 0);
                uploadConstants(immediate.Get(), 17, 99); // Must not replace a retained list's version.
                immediate->ClearState();
                auto cb = baseline.Get();
                immediate->CSSetConstantBuffers(0, 1, &cb);
                immediate->ExecuteCommandList(merged.Get(), executeRestore);
                ++executeChecks;
                if (!checkBindings(immediate.Get(), nullptr, executeRestore ? baseline.Get() : nullptr))
                    ++executeFailures;
                for (unsigned child : {0u, 1u, 0u})
                    expected = {expected[0] * (child ? 5u : 3u) + inputs[step] + (child ? 2u : 1u),
                                expected[1] + 1, inputs[step], child ? 5u : 3u};
                immediate->CopyResource(staging.Get(), result.Get());
                D3D11_MAPPED_SUBRESOURCE mapped{};
                checked(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
                std::array<UINT, 4> actual{};
                memcpy(actual.data(), mapped.pData, sizeof(actual));
                immediate->Unmap(staging.Get(), 0);
                const bool correct = actual == expected;
                storageFailures += !correct;
                if (step)
                    report << ',';
                report << "{\"step\":" << step << ",\"verified\":" << (correct ? "true" : "false")
                       << ",\"actual\":[" << actual[0] << ',' << actual[1] << ',' << actual[2] << ','
                       << actual[3] << "],\"expected\":[" << expected[0] << ',' << expected[1] << ','
                       << expected[2] << ',' << expected[3] << "]}";
                save(output / ("frame-" + std::to_string(frame) + "-step-" + std::to_string(step) + ".bin"),
                     std::vector<uint8_t>(reinterpret_cast<uint8_t *>(actual.data()),
                                          reinterpret_cast<uint8_t *>(actual.data() + actual.size())));
            }
            immediate->ClearState();
            auto target = chain.rtv.Get();
            auto source = resultView.Get();
            const D3D11_VIEWPORT viewport{0, 0, 8, 8, 0, 1};
            immediate->OMSetRenderTargets(1, &target, nullptr);
            immediate->RSSetViewports(1, &viewport);
            immediate->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            immediate->VSSetShader(vs.Get(), nullptr, 0);
            immediate->PSSetShader(ps.Get(), nullptr, 0);
            immediate->PSSetShaderResources(0, 1, &source);
            immediate->Draw(3, 0);
            immediate->CopyResource(imageStaging.Get(), chain.buffer.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            checked(immediate->Map(imageStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
            std::vector<uint8_t> rgba(256), golden(256);
            for (unsigned y = 0; y < 8; ++y)
                memcpy(rgba.data() + y * 32, static_cast<uint8_t *>(mapped.pData) + y * mapped.RowPitch, 32);
            immediate->Unmap(imageStaging.Get(), 0);
            for (unsigned i = 0; i < 256; i += 4)
                golden[i + 1] = golden[i + 3] = 255;
            imageFailures += rgba != golden;
            save(output / L"frame.rgba", rgba);
            save(output / ("frame-" + std::to_string(frame) + ".rgba"), rgba);
            save(output / L"expected.rgba", golden);
            checked(chain.swap->Present(0, 0));
            report << "],\"finish_state_failures\":" << finishFailures - finishBefore
                   << ",\"execute_state_failures\":" << executeFailures - executeBefore
                   << ",\"merge_state_failures\":" << mergeFailures - mergeBefore
                   << ",\"parent_finish_state_failures\":" << parentFinishFailures - parentFinishBefore
                   << ",\"image_verified\":" << (rgba == golden ? "true" : "false") << '}';
            report.flush();
            Sleep(50);
        }
        if (argc == 5) {
            for (unsigned i = 0; i < 100 && !fs::exists(argv[3]); ++i)
                Sleep(50);
            if (!fs::exists(argv[3]))
                throw std::runtime_error("No original capture delivered");
        }
        report << "],\"finish_checks\":" << finishChecks << ",\"finish_state_failures\":" << finishFailures
               << ",\"execute_checks\":" << executeChecks << ",\"execute_state_failures\":" << executeFailures
               << ",\"merge_checks\":" << mergeChecks << ",\"merge_state_failures\":" << mergeFailures
               << ",\"parent_finish_checks\":" << parentFinishChecks
               << ",\"parent_finish_state_failures\":" << parentFinishFailures
               << ",\"storage_failures\":" << storageFailures << ",\"image_failures\":" << imageFailures
               << ",\"completed\":true}\n";
        return storageFailures || imageFailures ||
                       (argc == 3 &&
                        (finishFailures || executeFailures || mergeFailures || parentFinishFailures))
                   ? 1
                   : 0;
    } catch (const std::exception &error) {
        std::ofstream(output / L"error.txt") << error.what();
        return 1;
    }
}
