#pragma once
#include "NativeDevice.h"
#include "ShaderReplacement.h"
#include <d3d12.h>
#include <optional>
#include <map>

namespace flora::dx12 {
struct RootSignatureRecipe {
    uint64_t device{}, object{};
    UINT nodeMask{};
    HRESULT result{};
    format::Bytes bytecode;
};
struct PipelineRecipe {
    enum class Kind { Graphics, Compute, Stream } kind = Kind::Graphics;
    uint64_t owner{}, object{}, rootSignature{};
    HRESULT result{};
    GUID iid{};
    std::string libraryName;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};
    // Only the small native stream structure is copied; shader blobs remain
    // read-only mapped spans owned by the caller's StreamDirectory.
    std::vector<uint64_t> stream;
    std::optional<size_t> rootPointerOffset;
    std::map<std::string, size_t> shaderPointerOffsets;
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputs;
    std::vector<D3D12_SO_DECLARATION_ENTRY> declarations;
    std::vector<UINT> strides;
    std::vector<D3D12_VIEW_INSTANCE_LOCATION> views;
    std::vector<std::pair<std::string, format::Bytes>> shaders;
    uint64_t cachedBytes{};
    PipelineRecipe() = default;
    PipelineRecipe(const PipelineRecipe &) = delete;
    PipelineRecipe &operator=(const PipelineRecipe &) = delete;
    PipelineRecipe(PipelineRecipe &&) noexcept = default;
    PipelineRecipe &operator=(PipelineRecipe &&) noexcept = default;
};
bool isPipelineRecipe(uint64_t type);
RootSignatureRecipe decodeRootSignature(format::Bytes);
PipelineRecipe decodePipelineRecipe(uint64_t type, format::Bytes);
nlohmann::json describePipelineRecipe(const PipelineRecipe &);

// Native objects are keyed by creation record, never by an execution event or
// a rounded JSON number. The directory and device must outlive this cache.
class NativePipelines final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
  public:
    NativePipelines(NativeDevice &, const StreamDirectory &, const CancelCheck &cancelled = {});
    ~NativePipelines();
    // Rebuild all edited PSOs before any GPU submission. The runtime validates
    // shader linkage and compatibility with the original root signature.
    void setShaderReplacements(std::vector<ShaderReplacement>);
    nlohmann::json shaderReplacementInformation() const;
    ID3D12PipelineState *create(uint64_t record);
    ID3D12PipelineState *pipeline(uint64_t object, uint64_t atRecord);
    ID3D12RootSignature *rootSignature(uint64_t object, uint64_t atRecord);
    nlohmann::json information(uint64_t record) const;
};
}
