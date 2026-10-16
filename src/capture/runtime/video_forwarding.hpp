#pragma once
#include "objects.hpp"

namespace flora::capture {
class VideoBeginFrameKey {
    D3D11_VIDEO_DECODER_BEGIN_FRAME_CRYPTO_SESSION value_{};
    const void* pointer_;
public:
    VideoBeginFrameKey(ID3D11VideoDecoder* decoder, UINT size, const void* key):pointer_(key) {
        if (!decoder || !key || size!=sizeof(value_)) return;
        D3D11_VIDEO_DECODER_DESC desc{}; D3D11_VIDEO_DECODER_CONFIG config{};
        if (SUCCEEDED(decoder->GetCreationParameters(&desc,&config)) &&
            config.guidConfigBitstreamEncryption==D3D11_DECODER_ENCRYPTION_HW_CENC) {
            value_=*static_cast<const D3D11_VIDEO_DECODER_BEGIN_FRAME_CRYPTO_SESSION*>(key);
            value_.pCryptoSession=unwrap(value_.pCryptoSession); pointer_=&value_;
        }
    }
    const void* get() const noexcept { return pointer_; }
};
// Video interfaces share device/context/resource identities with graphics.
// Forwarding their outer COM pointers is insufficient: these SDK structures
// also contain application pointers which must never reach the native driver.
class VideoDecoderExtension {
    D3D11_VIDEO_DECODER_EXTENSION value_{};
    std::vector<ID3D11Resource*> resources_;
    const D3D11_VIDEO_DECODER_EXTENSION* pointer_ = nullptr;
public:
    explicit VideoDecoderExtension(const D3D11_VIDEO_DECODER_EXTENSION* input) {
        if (!input) return;
        value_ = *input; pointer_ = &value_;
        if (input->ppResourceList) {
            resources_.reserve(input->ResourceCount);
            for (UINT i=0; i<input->ResourceCount; ++i) resources_.push_back(unwrap(input->ppResourceList[i]));
            value_.ppResourceList = resources_.data();
        }
    }
    const D3D11_VIDEO_DECODER_EXTENSION* get() const noexcept { return pointer_; }
};

class VideoProcessorStreams {
    struct Surfaces {
        std::vector<ID3D11VideoProcessorInputView*> past, future, past_right, future_right;
    };
    std::vector<D3D11_VIDEO_PROCESSOR_STREAM> values_;
    std::vector<Surfaces> surfaces_;
    const D3D11_VIDEO_PROCESSOR_STREAM* pointer_;
    static void translate(ID3D11VideoProcessorInputView**& value, UINT count,
                          std::vector<ID3D11VideoProcessorInputView*>& storage) {
        if (!value) return;
        storage.reserve(count);
        for (UINT i=0; i<count; ++i) storage.push_back(unwrap(value[i]));
        value = storage.data();
    }
public:
    VideoProcessorStreams(ID3D11VideoContext* context, ID3D11VideoProcessor* processor,
                          const D3D11_VIDEO_PROCESSOR_STREAM* input, UINT count):pointer_(input) {
        if (!input || !count) return;
        values_.assign(input,input+count); surfaces_.resize(count);
        for (UINT i=0; i<count; ++i) {
            auto& value=values_[i]; auto& surfaces=surfaces_[i];
            // Disabled stream fields are ignored by D3D11 and may be garbage.
            if (!value.Enable) continue;
            value.pInputSurface=unwrap(value.pInputSurface);
            translate(value.ppPastSurfaces,value.PastFrames,surfaces.past);
            translate(value.ppFutureSurfaces,value.FutureFrames,surfaces.future);
            BOOL stereo=FALSE,left=FALSE,base=FALSE; int offset=0;
            D3D11_VIDEO_PROCESSOR_STEREO_FORMAT format{};
            D3D11_VIDEO_PROCESSOR_STEREO_FLIP_MODE flip{};
            context->VideoProcessorGetStreamStereoFormat(processor,i,&stereo,&format,&left,&base,&flip,&offset);
            if (stereo && format==D3D11_VIDEO_PROCESSOR_STEREO_FORMAT_SEPARATE) {
                value.pInputSurfaceRight=unwrap(value.pInputSurfaceRight);
                translate(value.ppPastSurfacesRight,value.PastFrames,surfaces.past_right);
                translate(value.ppFutureSurfacesRight,value.FutureFrames,surfaces.future_right);
            }
        }
        pointer_=values_.data();
    }
    const D3D11_VIDEO_PROCESSOR_STREAM* get() const noexcept { return pointer_; }
};
}
