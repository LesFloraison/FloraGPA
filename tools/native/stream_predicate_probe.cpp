// Development-only original captures and independent byte/image oracles.
#include "texture_probe_helpers.h"
#include "original_capture_control.h"

int wmain(int argc, wchar_t **argv) {
    if (argc != 3 && argc != 5) return 2;
    output = argv[1];
    if (fs::exists(output)) return 2;
    fs::create_directories(output);
    ComPtr<ID3D11InfoQueue> diagnostics;
    try {
        const unsigned mode = unsigned(std::stoul(argv[2]));
        if (mode >= 48) throw std::runtime_error("Invalid stream predicate mode");
        const unsigned stream = mode % 4, scenario = (mode / 4) % 3;
        const bool comparison = (mode / 12) % 2, frameCreation = mode >= 24;
        const int overflowStream = scenario == 0 ? -1 : int((stream + (scenario == 2)) % 4);
        const bool expectedQuery = scenario == 1;
        const bool expectedPredicate = overflowStream >= 0;
        if (argc == 5 && !LoadLibraryExW(argv[4], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))
            throw std::runtime_error("Cannot load development shim");
        WNDCLASSW wc{};
        wc.lpfnWndProc = windowProc; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"FloraPresentCaptureProbe";
        if (!RegisterClassW(&wc)) throw std::runtime_error("RegisterClass failed");
        const bool warp = GetEnvironmentVariableW(L"FLORA_STREAM_PREDICATE_WARP", nullptr, 0) != 0;
        const bool debug = GetEnvironmentVariableW(L"FLORA_STREAM_PREDICATE_DEBUG", nullptr, 0) != 0;
        ComPtr<ID3D11Device> d; ComPtr<ID3D11DeviceContext> c;
        checked(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
            nullptr, debug ? D3D11_CREATE_DEVICE_DEBUG : 0, nullptr, 0, D3D11_SDK_VERSION, &d, nullptr, &c));
        if(debug)checked(d.As(&diagnostics));
        ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter; ComPtr<IDXGIFactory> factory;
        checked(d.As(&dxgi)); checked(dxgi->GetAdapter(&adapter)); checked(adapter->GetParent(IID_PPV_ARGS(&factory)));
        Chain chain; createChain(chain, d.Get(), factory.Get(), DXGI_SWAP_EFFECT_DISCARD);
        auto compile = [&](const std::string &source, const char *profile) {
            ComPtr<ID3DBlob> code, error;
            const auto hr = D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr,
                "main", profile, 0, 0, &code, &error);
            if (FAILED(hr) && error) throw std::runtime_error(static_cast<const char *>(error->GetBufferPointer()));
            checked(hr); return code;
        };
        auto vb = compile("float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}", "vs_5_0");
        auto pb = compile("float4 main():SV_Target{return float4(0,1,0,1);}", "ps_5_0");
        std::string source = "struct O{float4 v:DATA;};[maxvertexcount(8)]void main(point float4 p[1]:SV_Position,";
        for (unsigned s = 0; s < 4; ++s) source += (s ? "," : "") + std::string("inout PointStream<O> s") + std::to_string(s);
        source += "){O o;";
        for (unsigned s = 0; s < 4; ++s) for (unsigned v = 0; v < 2; ++v)
            source += "o.v=float4(" + std::to_string(s+1) + "," + std::to_string(v+10) + ",20,30);s" + std::to_string(s) + ".Append(o);";
        source += "}";
        auto gb = compile(source, "gs_5_0");
        ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps; ComPtr<ID3D11GeometryShader> gs;
        checked(d->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &vs));
        checked(d->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &ps));
        D3D11_SO_DECLARATION_ENTRY decl[4]{}; UINT strides[]{16,16,16,16};
        for (unsigned s = 0; s < 4; ++s) decl[s] = {s, "DATA", 0, 0, 4, BYTE(s)};
        checked(d->CreateGeometryShaderWithStreamOutput(gb->GetBufferPointer(), gb->GetBufferSize(),
            decl, 4, strides, 4, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &gs));
        D3D11_RASTERIZER_DESC rd{}; rd.FillMode=D3D11_FILL_SOLID; rd.CullMode=D3D11_CULL_NONE; rd.DepthClipEnable=TRUE;
        ComPtr<ID3D11RasterizerState> rs; checked(d->CreateRasterizerState(&rd,&rs));
        std::array<ComPtr<ID3D11Buffer>,4> buffers,stages;
        std::array<std::vector<uint8_t>,4> expectedBuffers;
        for (unsigned s = 0; s < 4; ++s) {
            const UINT count = int(s)==overflowStream ? 1 : 2;
            auto &bytes=expectedBuffers[s]; bytes.resize(count*16);
            for (unsigned v=0;v<count;++v) { const float value[]{float(s+1),float(v+10),20,30};std::memcpy(bytes.data()+v*16,value,16); }
            D3D11_BUFFER_DESC bd{count*16,D3D11_USAGE_DEFAULT,D3D11_BIND_STREAM_OUTPUT,0,0,0};
            checked(d->CreateBuffer(&bd,nullptr,&buffers[s]));
            bd.Usage=D3D11_USAGE_STAGING;bd.BindFlags=0;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            checked(d->CreateBuffer(&bd,nullptr,&stages[s]));
            save(output/("expected-stream"+std::to_string(s)+".bin"),bytes);
        }
        Tex screen;screen.width=screen.height=8;screen.mips=screen.layers=1;
        auto staging=create(d.Get(),screen,true);auto expected=storage(screen,true)[0].bytes;
        if (expectedPredicate==comparison) for(size_t i=0;i<expected.size();i+=4){expected[i]=expected[i+2]=255;expected[i+1]=0;}
        save(output/L"expected.rgba",expected);
        D3D11_QUERY_DESC desc{D3D11_QUERY(9+2*stream),0},predicateDesc{D3D11_QUERY_SO_OVERFLOW_PREDICATE,0};
        ComPtr<ID3D11Predicate> predicate;ComPtr<ID3D11Query> query;
        if(!frameCreation){checked(d->CreatePredicate(&predicateDesc,&predicate));checked(d->CreateQuery(&desc,&query));}
        DXGI_ADAPTER_DESC ad{};checked(adapter->GetDesc(&ad));
        std::ofstream report(output/L"oracle.json");report.exceptions(std::ios::badbit|std::ios::failbit);
        report<<"{\"mode\":"<<mode<<",\"query_type\":"<<unsigned(desc.Query)<<",\"overflow_stream\":"<<overflowStream
              <<",\"frame_creation\":"<<(frameCreation?"true":"false")<<",\"warp\":"<<(warp?"true":"false")
              <<",\"debug\":"<<(debug?"true":"false")<<",\"capture_requested\":"<<(argc==5?"true":"false")
              <<",\"vendor_id\":"<<ad.VendorId<<",\"device_id\":"<<ad.DeviceId<<",\"frames\":[";
        for(unsigned frame=0;frame<12;++frame){
            MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
            if(argc==5&&frame==5){
                auto shim=GetModuleHandleW(L"shimd3d64.dll");
                flora::research::selectOriginalPrimarySwapChain(shim,chain.swap.Get());
                auto req=reinterpret_cast<void(WINAPI *)(const wchar_t *,void(WINAPI *)(const wchar_t *))>(GetProcAddress(shim,"CaptureNextFrame"));
                if(!req)throw std::runtime_error("CaptureNextFrame unavailable");req(argv[3],captured);
            }
            c->ClearState();
            if(frameCreation){predicate.Reset();query.Reset();checked(d->CreatePredicate(&predicateDesc,&predicate));checked(d->CreateQuery(&desc,&query));}
            D3D11_QUERY_DESC actual{};predicate->GetDesc(&actual);
            if(actual.Query!=predicateDesc.Query||actual.MiscFlags||predicate->GetDataSize()!=4)throw std::runtime_error("Predicate descriptor mismatch");
            query->GetDesc(&actual);
            if(actual.Query!=desc.Query||actual.MiscFlags||query->GetDataSize()!=4)throw std::runtime_error("Query descriptor mismatch");
            auto target=chain.rtv.Get();c->OMSetRenderTargets(1,&target,nullptr);
            D3D11_VIEWPORT vp{0,0,8,8,0,1};c->RSSetViewports(1,&vp);c->RSSetState(rs.Get());
            c->VSSetShader(vs.Get(),nullptr,0);c->PSSetShader(nullptr,nullptr,0);c->GSSetShader(gs.Get(),nullptr,0);
            c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
            ID3D11Buffer *targets[]{buffers[0].Get(),buffers[1].Get(),buffers[2].Get(),buffers[3].Get()};UINT offsets[4]{};
            c->SOSetTargets(4,targets,offsets);c->Begin(predicate.Get());c->Begin(query.Get());c->Draw(1,0);c->End(query.Get());c->End(predicate.Get());c->SOSetTargets(0,nullptr,nullptr);
            BOOL measured=FALSE;HRESULT hr;auto until=GetTickCount64()+5000;
            while((hr=c->GetData(predicate.Get(),&measured,sizeof measured,0))==S_FALSE){if(GetTickCount64()>until)throw std::runtime_error("Predicate timeout");Sleep(1);}
            checked(hr);if(bool(measured)!=expectedPredicate)throw std::runtime_error("Wrong aggregate overflow result");
            BOOL streamMeasured=FALSE;until=GetTickCount64()+5000;
            while((hr=c->GetData(query.Get(),&streamMeasured,sizeof streamMeasured,0))==S_FALSE){if(GetTickCount64()>until)throw std::runtime_error("Stream Query timeout");Sleep(1);}
            checked(hr);if(bool(streamMeasured)!=expectedQuery)throw std::runtime_error("Wrong per-stream overflow result");
            c->GSSetShader(nullptr,nullptr,0);c->PSSetShader(ps.Get(),nullptr,0);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const float magenta[]{1,0,1,1};c->ClearRenderTargetView(chain.rtv.Get(),magenta);
            c->SetPredication(predicate.Get(),BOOL(comparison));c->Draw(3,0);c->SetPredication(nullptr,FALSE);
            for(unsigned s=0;s<4;++s){
                c->CopyResource(stages[s].Get(),buffers[s].Get());D3D11_MAPPED_SUBRESOURCE mapped{};
                checked(c->Map(stages[s].Get(),0,D3D11_MAP_READ,0,&mapped));
                const auto begin=static_cast<const uint8_t *>(mapped.pData);std::vector<uint8_t> actualBytes(begin,begin+expectedBuffers[s].size());
                c->Unmap(stages[s].Get(),0);
                save(output/("frame"+std::to_string(frame)+"-stream"+std::to_string(s)+".bin"),actualBytes);
                const bool equal=actualBytes==expectedBuffers[s];
                if(!equal)throw std::runtime_error("Stream-output byte mismatch");
            }
            c->CopyResource(staging.Get(),chain.buffer.Get());auto rgba=read(c.Get(),staging.Get(),storage(screen,true));
            if(rgba!=expected)throw std::runtime_error("Conditional image mismatch");save(output/("frame"+std::to_string(frame)+".rgba"),rgba);
            checked(chain.swap->Present(0,0));
            if(frame)report<<',';report<<"{\"frame\":"<<frame<<",\"query_value\":"<<(streamMeasured?"true":"false")<<",\"predicate_value\":"<<(measured?"true":"false")
                <<",\"comparison\":"<<(comparison?"true":"false")<<",\"image_verified\":true,\"buffers_verified\":4}";
            Sleep(25);
        }
        if(debug){ComPtr<ID3D11InfoQueue> queue;checked(d.As(&queue));
            const auto count=queue->GetNumStoredMessagesAllowedByRetrievalFilter();
            std::ofstream messages(output/L"debug.txt");
            for(UINT64 i=0;i<count;++i){SIZE_T size=0;checked(queue->GetMessage(i,nullptr,&size));std::vector<uint8_t> bytes(size);
                auto message=reinterpret_cast<D3D11_MESSAGE *>(bytes.data());checked(queue->GetMessage(i,message,&size));messages<<message->pDescription<<'\n';}
            if(count)throw std::runtime_error("D3D11 debug messages present");
        }
        report<<"],\"completed\":true}\n";return 0;
    }catch(const std::exception &error){
        std::ofstream(output/L"error.txt")<<error.what();
        if(diagnostics){std::ofstream messages(output/L"failure-debug.txt");
            for(UINT64 i=0;i<diagnostics->GetNumStoredMessagesAllowedByRetrievalFilter();++i){
                SIZE_T size=0;diagnostics->GetMessage(i,nullptr,&size);std::vector<uint8_t> bytes(size);
                auto message=reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
                if(SUCCEEDED(diagnostics->GetMessage(i,message,&size)))messages<<message->pDescription<<'\n';
            }
        }
        return 1;
    }
}
