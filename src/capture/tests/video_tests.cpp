#include "objects.hpp"
#include "video_forwarding.hpp"
#include <iostream>
#include <cstring>
using namespace flora::capture;
using Microsoft::WRL::ComPtr;
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void check(HRESULT hr,const char* message){require(SUCCEEDED(hr),message);}
bool same(IUnknown* a,IUnknown* b){ComPtr<IUnknown> x,y;check(a->QueryInterface(IID_PPV_ARGS(&x)),"Identity A");check(b->QueryInterface(IID_PPV_ARGS(&y)),"Identity B");return x==y;}
void shared_resource(ID3D11Device* device){
    D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=16;desc.MipLevels=desc.ArraySize=1;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;desc.MiscFlags=D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    ComPtr<ID3D11Texture2D> texture;check(device->CreateTexture2D(&desc,nullptr,&texture),"Shared texture");
    ComPtr<IDXGIResource> resource;check(texture.As(&resource),"Shared resource QI");HANDLE shared=nullptr;check(resource->GetSharedHandle(&shared),"Shared handle");
    ComPtr<ID3D11Device> peer;check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&peer,nullptr,nullptr),"Shared peer device");
    ComPtr<ID3D11Texture2D> imported;check(peer->OpenSharedResource(shared,IID_PPV_ARGS(&imported)),"Open shared resource");
    ComPtr<IDXGIKeyedMutex> local,remote;check(texture.As(&local),"Local keyed mutex");check(imported.As(&remote),"Remote keyed mutex");
    require(same(local.Get(),texture.Get()),"Keyed mutex identity escaped");
    ComPtr<ID3D11Device> parent;check(local->GetDevice(IID_PPV_ARGS(&parent)),"Mutex parent");require(same(parent.Get(),device),"Mutex device identity escaped");
    require(local->AcquireSync(0,1000)==S_OK,"Acquire initial shared key");check(local->ReleaseSync(1),"Release shared key");
    require(remote->AcquireSync(1,1000)==S_OK,"Peer did not receive shared key");check(remote->ReleaseSync(2),"Release peer key");
    require(local->AcquireSync(2,1000)==S_OK,"Local did not receive peer key");check(local->ReleaseSync(0),"Restore shared key");
}
std::vector<uint8_t> exercise(ID3D11Device* device,ID3D11DeviceContext* context){
    shared_resource(device);
    ComPtr<ID3D11VideoDevice> video;check(device->QueryInterface(IID_PPV_ARGS(&video)),"Video device QI");
    ComPtr<ID3D11VideoContext> processing;check(context->QueryInterface(IID_PPV_ARGS(&processing)),"Video context QI");
    require(same(device,video.Get()),"Video device identity escaped");
    require(same(context,processing.Get()),"Video context identity escaped");
    GUID profile{};bool found=false;
    for(UINT i=0;i<video->GetVideoDecoderProfileCount();++i){BOOL supported=FALSE;check(video->GetVideoDecoderProfile(i,&profile),"Decoder profile");check(video->CheckVideoDecoderFormat(&profile,DXGI_FORMAT_NV12,&supported),"Decoder format");if(supported){found=true;break;}}
    require(found,"Hardware has no NV12 decoder profile");
    D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=128;desc.MipLevels=desc.ArraySize=1;desc.Format=DXGI_FORMAT_NV12;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_DECODER;
    ComPtr<ID3D11Texture2D> decoded;check(device->CreateTexture2D(&desc,nullptr,&decoded),"Decode texture");
    D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC view_desc{};view_desc.DecodeProfile=profile;view_desc.ViewDimension=D3D11_VDOV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoDecoderOutputView> decoded_view;
    // This native call aborted inside d3d11 when a raw video interface received
    // a wrapped texture. It must succeed and return the same application object.
    check(video->CreateVideoDecoderOutputView(decoded.Get(),&view_desc,&decoded_view),"Decode output view");
    ComPtr<ID3D11Resource> back;decoded_view->GetResource(&back);require(same(back.Get(),decoded.Get()),"Decode resource identity escaped");
    ComPtr<ID3D11Device> parent;decoded_view->GetDevice(&parent);require(same(parent.Get(),device),"Video view device identity escaped");
    ID3D11Resource* resources[]={decoded.Get()};
    D3D11_VIDEO_DECODER_EXTENSION extension{};extension.ResourceCount=1;extension.ppResourceList=resources;
    VideoDecoderExtension translated(&extension);
    require(translated.get()->ppResourceList[0]==unwrap(resources[0]),"Nested decoder resource was not unwrapped");
    require(extension.ppResourceList==resources && resources[0]==decoded.Get(),"Decoder caller storage changed");

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};content.InputFrameFormat=D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate={30,1};content.OutputFrameRate={30,1};content.InputWidth=content.InputHeight=content.OutputWidth=content.OutputHeight=128;content.Usage=D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;check(video->CreateVideoProcessorEnumerator(&content,&enumerator),"Video processor enumeration");
    ComPtr<ID3D11VideoProcessor> processor;check(video->CreateVideoProcessor(enumerator.Get(),0,&processor),"Video processor");
    desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    std::vector<uint32_t> pixels(128*128,0xff204080);D3D11_SUBRESOURCE_DATA data{pixels.data(),128*4,0};
    ComPtr<ID3D11Texture2D> input,output;check(device->CreateTexture2D(&desc,&data,&input),"Processor input");check(device->CreateTexture2D(&desc,nullptr,&output),"Processor output");
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in_desc{};in_desc.ViewDimension=D3D11_VPIV_DIMENSION_TEXTURE2D;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out_desc{};out_desc.ViewDimension=D3D11_VPOV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorInputView> in_view;ComPtr<ID3D11VideoProcessorOutputView> out_view;
    check(video->CreateVideoProcessorInputView(input.Get(),enumerator.Get(),&in_desc,&in_view),"Processor input view");
    check(video->CreateVideoProcessorOutputView(output.Get(),enumerator.Get(),&out_desc,&out_view),"Processor output view");
    D3D11_VIDEO_PROCESSOR_STREAM stream{};stream.Enable=TRUE;stream.pInputSurface=in_view.Get();
    auto original=stream;check(processing->VideoProcessorBlt(processor.Get(),out_view.Get(),0,1,&stream),"Processor blit");
    require(!std::memcmp(&stream,&original,sizeof(stream)),"Processor caller storage changed");
    // An ignored disabled stream must not dereference its unused array fields.
    auto disabled=stream;disabled.Enable=FALSE;disabled.PastFrames=UINT_MAX;disabled.ppPastSurfaces=reinterpret_cast<ID3D11VideoProcessorInputView**>(1);
    VideoProcessorStreams ignored(unwrap(processing.Get()),unwrap(processor.Get()),&disabled,1);
    require(ignored.get()->ppPastSurfaces==disabled.ppPastSurfaces,"Disabled stream was interpreted");
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage;check(device->CreateTexture2D(&desc,nullptr,&stage),"Readback texture");context->CopyResource(stage.Get(),output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(stage.Get(),0,D3D11_MAP_READ,0,&mapped),"Processor readback");
    std::vector<uint8_t> result(128*128*4);for(UINT y=0;y<128;++y)std::memcpy(result.data()+y*128*4,static_cast<uint8_t*>(mapped.pData)+y*mapped.RowPitch,128*4);context->Unmap(stage.Get(),0);return result;
}
}
int main(){try{
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    if(FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_VIDEO_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context))){std::cout<<"Hardware video unavailable\n";return 77;}
    {ComPtr<ID3D11VideoDevice> video;bool supported=false;
     if(SUCCEEDED(device.As(&video)))for(UINT i=0;i<video->GetVideoDecoderProfileCount();++i){GUID profile{};BOOL nv12=FALSE;if(SUCCEEDED(video->GetVideoDecoderProfile(i,&profile)) && SUCCEEDED(video->CheckVideoDecoderFormat(&profile,DXGI_FORMAT_NV12,&nv12)) && nv12){supported=true;break;}}
     if(!supported){std::cout<<"Hardware NV12 decoder unavailable\n";return 77;}}
    const auto baseline=exercise(device.Get(),context.Get());
    require(baseline[0] || baseline[1] || baseline[2],"Native video reference is black");
    auto* wrapped_device=device.Detach();wrap_object(&wrapped_device,"video test");device.Attach(wrapped_device);
    auto* wrapped_context=context.Detach();wrap_object(&wrapped_context,"video test");context.Attach(wrapped_context);
    const auto forwarded=exercise(device.Get(),context.Get());
    require(baseline==forwarded,"Video pixels differ from native baseline");
    require(object_gap_count()==0,"Video forwarding left an interface gap");
    context->ClearState();context->Flush();context.Reset();device.Reset();
    require(object_status().at("active")==0,"Video proxies leaked");
    std::cout<<"Decoder output, COM identity, processor blit pixels and nested resources passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
