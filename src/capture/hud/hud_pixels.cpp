#include "hud_pixels.hpp"
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace flora::capture::hud {
using Microsoft::WRL::ComPtr;
namespace {
void check(HRESULT hr,const char* operation){if(FAILED(hr))throw std::runtime_error(std::string(operation)+" ("+std::to_string(uint32_t(hr))+")");}
std::wstring rate(const std::optional<double>& value){return value?std::to_wstring(std::lround(*value)):L"--";}
struct Canvas {
    HDC dc=CreateCompatibleDC(nullptr);
    HBITMAP bitmap=nullptr;
    HGDIOBJ old=nullptr;
    uint32_t* pixels=nullptr;
    int width,height;
    Canvas(int w,int h):width(w),height(h){
        if(!dc)throw std::runtime_error("HUD font DC creation failed");
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=-h;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,reinterpret_cast<void**>(&pixels),nullptr,0);
        if(!bitmap){DeleteDC(dc);dc=nullptr;throw std::runtime_error("HUD bitmap creation failed");}
        old=SelectObject(dc,bitmap);SetBkMode(dc,TRANSPARENT);std::memset(pixels,0,size_t(w)*h*4);
    }
    ~Canvas(){if(dc){SelectObject(dc,old);DeleteDC(dc);}if(bitmap)DeleteObject(bitmap);}
    void text(const std::wstring& value,int x,int y,int w,int h,int font,bool bold=false,UINT align=DT_LEFT){
        HFONT face=CreateFontW(-font,0,0,0,bold?FW_BOLD:FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Arial");
        if(!face)throw std::runtime_error("HUD font creation failed");
        const auto previous=SelectObject(dc,face);SetTextColor(dc,RGB(255,255,255));RECT rect{x,y,x+w,y+h};
        DrawTextW(dc,value.c_str(),int(value.size()),&rect,DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX|align);SelectObject(dc,previous);DeleteObject(face);
    }
};
}
struct PixelTransaction::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> buffer,backup,stage,upload;
    UINT x=0,y=0,width=0,height=0;
    bool modified=false;
    std::vector<uint8_t> clean;
    void copy_out(){D3D11_BOX box{x,y,0,x+width,y+height,1};context->CopySubresourceRegion(backup.Get(),0,0,0,0,buffer.Get(),0,&box);}
    void copy_back(){context->CopySubresourceRegion(buffer.Get(),0,x,y,0,backup.Get(),0,nullptr);modified=false;}
};
PixelTransaction::PixelTransaction():impl_(std::make_unique<Impl>()){}
PixelTransaction::~PixelTransaction(){if(impl_->modified)impl_->copy_back();}
bool PixelTransaction::begin(IDXGISwapChain* native,const Panel& panel,UINT flags,bool partial,std::string& reason,bool isolated) noexcept {
    const auto last=GetLastError();
    try{
        if(impl_->modified)throw std::logic_error("HUD transaction already active");
        auto& p=*impl_;reason.clear();
        DXGI_SWAP_CHAIN_DESC desc{};check(native->GetDesc(&desc),"Read HUD swap chain");
        if(panel.mode==Mode::Hidden)throw std::runtime_error("Hidden");
        if((flags&~(isolated?DXGI_PRESENT_ALLOW_TEARING:0u)) || partial)throw std::runtime_error("HUD skips TEST, unsupported flags or partial Present");
        if(!isolated && (desc.BufferCount!=1 || (desc.SwapEffect!=DXGI_SWAP_EFFECT_DISCARD && desc.SwapEffect!=DXGI_SWAP_EFFECT_SEQUENTIAL)))throw std::runtime_error("Multi-buffer HUD requires isolated application storage");
        if(desc.SampleDesc.Count!=1 || (desc.BufferDesc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && desc.BufferDesc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM))throw std::runtime_error("Unsupported HUD sample count or format");
        if(desc.Flags&(DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE|DXGI_SWAP_CHAIN_FLAG_DISPLAY_ONLY|DXGI_SWAP_CHAIN_FLAG_RESTRICTED_CONTENT))throw std::runtime_error("Unsupported HUD shared or protected presentation");
        check(native->GetDevice(IID_PPV_ARGS(&p.device)),"Get HUD native device");p.device->GetImmediateContext(&p.context);
        check(native->GetBuffer(0,IID_PPV_ARGS(&p.buffer)),"Get HUD native buffer");
        D3D11_TEXTURE2D_DESC texture{};p.buffer->GetDesc(&texture);
        const UINT dpi=desc.OutputWindow?GetDpiForWindow(desc.OutputWindow):96;
        const double dpi_scale=double(dpi?dpi:96)/96.0;
        const auto margin=UINT(std::lround(12*dpi_scale));
        if(texture.Width<=2*margin)throw std::runtime_error("Window is too small for HUD");
        const double scale=(std::min)(dpi_scale,double(texture.Width-2*margin)/312.0);
        if(scale<0.6)throw std::runtime_error("Window is too small for HUD");
        const auto px=[&](int n){return (std::max)(1,int(std::lround(n*scale)));};
        p.x=margin;p.y=margin;
        const auto desired_height=panel.mode==Mode::Detailed?144:88;
        if(texture.Width<=p.x*2+UINT(px(120)) || texture.Height<=p.y*2+UINT(px(desired_height)))throw std::runtime_error("Window is too small for HUD");
        p.width=(std::min)(UINT(px(312)),texture.Width-p.x*2);p.height=UINT(px(desired_height));
        texture.Width=p.width;texture.Height=p.height;texture.MipLevels=1;texture.ArraySize=1;texture.BindFlags=0;texture.MiscFlags=0;texture.CPUAccessFlags=0;texture.Usage=D3D11_USAGE_DEFAULT;
        check(p.device->CreateTexture2D(&texture,nullptr,&p.backup),"Create private HUD backup");
        check(p.device->CreateTexture2D(&texture,nullptr,&p.upload),"Create private HUD upload");
        texture.Usage=D3D11_USAGE_STAGING;texture.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        check(p.device->CreateTexture2D(&texture,nullptr,&p.stage),"Create private HUD readback");
        p.copy_out();p.context->CopyResource(p.stage.Get(),p.backup.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};check(p.context->Map(p.stage.Get(),0,D3D11_MAP_READ,0,&mapped),"Read clean HUD region");
        // Balance Map even if allocation fails.
        try{p.clean.resize(size_t(p.width)*p.height*4);}catch(...){p.context->Unmap(p.stage.Get(),0);throw;}
        for(UINT row=0;row<p.height;++row)std::memcpy(p.clean.data()+size_t(row)*p.width*4,static_cast<uint8_t*>(mapped.pData)+size_t(row)*mapped.RowPitch,size_t(p.width)*4);
        p.context->Unmap(p.stage.Get(),0);
        Canvas canvas(int(p.width),int(p.height));
        const auto text=[&](const std::wstring& s,int x,int y,int w,int h,int font,bool bold=false,UINT align=DT_LEFT){canvas.text(s,px(x),px(y),px(w),px(h),px(font),bold,align);};
        text(panel.message.empty()?L"FloraGPA · Hooked":panel.message,10,5,208,18,13,true);text(L"Mode  "+panel.mode_key,10,23,208,16,12);
        text(panel.device,10,39,208,16,12);text(L"Res  "+std::to_wstring(desc.BufferDesc.Width)+L"×"+std::to_wstring(desc.BufferDesc.Height),10,55,208,16,12);
        text(rate(panel.fps.current),225,5,75,56,48,true,DT_RIGHT);
        const int footer=desired_height-20;
        text(panel.adapter,8,footer+2,207,17,12,true);text(rate(panel.fps.minimum),223,footer+2,35,17,12,true,DT_RIGHT);text(rate(panel.fps.maximum),264,footer+2,35,17,12,true,DT_RIGHT);
        if(panel.mode==Mode::Detailed){
            text(L"Swap "+std::to_wstring(panel.swap)+L" · Frame "+std::to_wstring(panel.fps.frames),10,75,290,16,12);
            text(L"Capture  "+panel.capture_key,10,91,290,16,12);
            text(panel.message.empty()?L"Hooked":panel.message,10,107,290,16,12);
        }
        GdiFlush();auto pixels=p.clean;const bool bgra=desc.BufferDesc.Format==DXGI_FORMAT_B8G8R8A8_UNORM;
        const int radius=px(12);
        for(UINT y=0;y<p.height;++y)for(UINT x=0;x<p.width;++x){
            const int cx=x<UINT(radius)?radius:x>=p.width-UINT(radius)?int(p.width)-radius-1:int(x);
            const int cy=y<UINT(radius)?radius:y>=p.height-UINT(radius)?int(p.height)-radius-1:int(y);
            const int dx=int(x)-cx,dy=int(y)-cy;if(dx*dx+dy*dy>radius*radius)continue;
            const size_t at=(size_t(y)*p.width+x)*4;const auto mask=canvas.pixels[at/4]&255u;
            const bool bottom=y>=UINT(px(footer));const unsigned alpha=bottom?220u:210u;
            unsigned ink[3]{255,255,255};if(bottom && x>=UINT(px(223)) && x<UINT(px(263))){ink[0]=255;ink[1]=54;ink[2]=40;}else if(bottom && x>=UINT(px(264))){ink[0]=54;ink[1]=210;ink[2]=70;}
            const unsigned background[3]{bottom?0u:20u,bottom?0u:37u,bottom?0u:70u};
            for(unsigned c=0;c<3;++c){const unsigned component=bgra?2-c:c;const unsigned base=(unsigned(pixels[at+c])*(255-alpha)+background[component]*alpha+127)/255;pixels[at+c]=uint8_t((base*(255-mask)+ink[component]*mask+127)/255);}
            // Preserve alpha: HUD is display decoration, not an application alpha edit.
        }
        p.context->UpdateSubresource(p.upload.Get(),0,nullptr,pixels.data(),p.width*4,0);
        p.modified=true;p.context->CopySubresourceRegion(p.buffer.Get(),0,p.x,p.y,0,p.upload.Get(),0,nullptr);
        SetLastError(last);return true;
    }catch(const std::exception& e){reason=e.what();}catch(...){reason="HUD rendering failed";}
    SetLastError(last);return false;
}
bool PixelTransaction::restore(std::string& reason) noexcept {
    const auto last=GetLastError();
    try{
        auto& p=*impl_;if(!p.modified){SetLastError(last);return true;}
        p.copy_back();check(p.device->GetDeviceRemovedReason(),"HUD restore device status");
        SetLastError(last);return true;
    }catch(const std::exception& e){reason=e.what();}catch(...){reason="HUD pixel restoration failed";}
    SetLastError(last);return false;
}
void PixelTransaction::discard_backup() noexcept {impl_->modified=false;}
}
