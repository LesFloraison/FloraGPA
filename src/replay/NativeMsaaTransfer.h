#pragma once
#include "Replay.h"

namespace flora {
// Allocate replay-owned MSAA resources in their raw-access family while keeping
// the recorded format and typed views unchanged.
uint32_t nativeMsaaStorageFormat(uint32_t format);
using NativeTextureReadback = std::function<std::vector<uint8_t>(ID3D11Resource *, const Resource &)>;
using NativeTextureCreate = std::function<Com<ID3D11Resource>(const Resource &, std::optional<Bytes>)>;
// Shared native transfer implementation. The caller owns predicate isolation,
// completion/cancellation and allocation limits. Contexts must be unpredicated.
// Private diagnostics use a separate device/context; replay retains its guards.
MsaaStorage readNativeMsaaStorage(ID3D11Device *, ID3D11DeviceContext *, ID3D11Resource *, const Resource &,
                                  std::optional<uint32_t> sample, uint32_t typedFormat,
                                  const NativeTextureReadback &);
void writeNativeMsaaSample(ID3D11Device *, ID3D11DeviceContext *, ID3D11Resource *, const Resource &,
                           const TexturePatch &, const NativeTextureCreate &);
} // namespace flora
