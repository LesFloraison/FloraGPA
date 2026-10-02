#pragma once
// Development-only unmodified GPA texture transfer fixtures with independent CPU byte oracles.
#define NOMINMAX
#include <algorithm>
#include <array>
#include <cstring>
#include <d3dcompiler.h>
#define wmain UnusedPresentCaptureMain
#include "present_capture_probe.cpp"
#include <d3d11sdklayers.h>
#undef wmain
namespace {
struct Tex {
    UINT dimension = 2, width = 16, height = 16, depth = 1, mips = 2, layers = 2, samples = 1;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    UINT bind = D3D11_BIND_SHADER_RESOURCE;
};
struct Sub {
    UINT width, height, depth, row, rows, slice;
    std::vector<uint8_t> bytes;
};
std::vector<Sub> storage(const Tex &t, bool green) {
    std::vector<Sub> result;
    for (UINT layer = 0; layer < t.layers; ++layer)
        for (UINT mip = 0; mip < t.mips; ++mip) {
            UINT w = std::max(1u, t.width >> mip), h = std::max(1u, t.height >> mip),
                 d = std::max(1u, t.depth >> mip);
            const bool bc = t.format == DXGI_FORMAT_BC1_UNORM;
            UINT row = bc ? ((w + 3) / 4) * 8 : w * 4, rows = bc ? (h + 3) / 4 : h;
            Sub sub{w, h, d, row, rows, row * rows, std::vector<uint8_t>(size_t(row) * rows * d)};
            if (green)
                for (size_t i = 0; i < sub.bytes.size(); i += bc ? 8 : 4) {
                    if (bc) {
                        sub.bytes[i] = 0xe0;
                        sub.bytes[i + 1] = 7;
                        sub.bytes[i + 2] = 0xe0;
                        sub.bytes[i + 3] = 7;
                    } else if (t.format == DXGI_FORMAT_R32_TYPELESS) {
                        float v = .25f;
                        memcpy(sub.bytes.data() + i, &v, 4);
                    } else {
                        sub.bytes[i + 1] = 255;
                        sub.bytes[i + 3] = 255;
                    }
                }
            result.push_back(std::move(sub));
        }
    return result;
}
ComPtr<ID3D11Resource> create(ID3D11Device *d, const Tex &t, bool staging = false,
                              const D3D11_SUBRESOURCE_DATA *initial = nullptr) {
    const auto usage = staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
    UINT bind = staging ? 0 : t.bind, cpu = staging ? D3D11_CPU_ACCESS_READ : 0;
    ComPtr<ID3D11Resource> result;
    if (t.dimension == 1) {
        D3D11_TEXTURE1D_DESC desc{t.width, t.mips, t.layers, t.format, usage, bind, cpu, 0};
        ComPtr<ID3D11Texture1D> r;
        checked(d->CreateTexture1D(&desc, initial, &r));
        result = r;
    } else if (t.dimension == 3) {
        D3D11_TEXTURE3D_DESC desc{t.width, t.height, t.depth, t.mips, t.format, usage, bind, cpu, 0};
        ComPtr<ID3D11Texture3D> r;
        checked(d->CreateTexture3D(&desc, initial, &r));
        result = r;
    } else {
        D3D11_TEXTURE2D_DESC desc{t.width,        t.height, t.mips, t.layers, t.format,
                                  {t.samples, 0}, usage,    bind,   cpu,      0};
        ComPtr<ID3D11Texture2D> r;
        checked(d->CreateTexture2D(&desc, initial, &r));
        result = r;
    }
    return result;
}
std::vector<uint8_t> read(ID3D11DeviceContext *c, ID3D11Resource *r, const std::vector<Sub> &subs) {
    std::vector<uint8_t> bytes;
    for (UINT i = 0; i < subs.size(); ++i) {
        D3D11_MAPPED_SUBRESOURCE m{};
        checked(c->Map(r, i, D3D11_MAP_READ, 0, &m));
        const auto &s = subs[i];
        for (UINT z = 0; z < s.depth; ++z)
            for (UINT y = 0; y < s.rows; ++y) {
                auto p =
                    static_cast<const uint8_t *>(m.pData) + size_t(z) * m.DepthPitch + size_t(y) * m.RowPitch;
                bytes.insert(bytes.end(), p, p + s.row);
            }
        c->Unmap(r, i);
    }
    return bytes;
}
void save(const fs::path &p, const std::vector<uint8_t> &bytes) {
    std::ofstream f(p, std::ios::binary);
    f.exceptions(std::ios::badbit | std::ios::failbit);
    f.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}
} // namespace
