#pragma once
#include "protocol.hpp"
#include <d3d11_4.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace flora::capture {
struct DeviceRecordDesc { uint32_t feature_level, creation_flags; };
struct ContextRecordDesc { uint32_t type, flags; };
struct InputElementRecord {
    std::string semantic;
    uint32_t semantic_index, format, slot, offset, slot_class, step_rate;
};
struct StreamElementRecord {
    std::string semantic;
    bool has_semantic;
    uint32_t stream, semantic_index, start_component, component_count, output_slot;
};
// Capture-owned model. No replay objects, GPU executors or wire record types.
// Holding a record never holds a native COM object alive.
struct ObjectRecord {
    const uint64_t id;
    std::atomic<bool> alive{true};
    std::string kind;
    uint64_t device = 0, resource = 0;
    std::vector<uint8_t> descriptor;
    std::mutex mutex;
    std::string creation_api;
    std::vector<uint8_t> creation_bytes;
    std::vector<InputElementRecord> input_elements;
    std::vector<StreamElementRecord> stream_elements;
    std::vector<UINT> stream_strides;
    UINT rasterized_stream = 0;
    uint64_t class_linkage = 0;
    bool creation_observed = false, initial_data_supplied = false;
    explicit ObjectRecord(uint64_t value) : id(value) {}
};
std::shared_ptr<ObjectRecord> track_native_object(IUnknown* identity);
Json metadata_status();
void observe_private_data(REFGUID key, const char* api) noexcept;
// Input is an application proxy; these functions never issue GPU commands.
std::shared_ptr<ObjectRecord> object_record(const void* proxy);
void remember_shader(IUnknown* output, const char* api, const void* bytes, SIZE_T size, IUnknown* linkage) noexcept;
void remember_buffer(ID3D11Buffer* output, const D3D11_BUFFER_DESC* desc, const D3D11_SUBRESOURCE_DATA* data) noexcept;
void remember_input_layout(ID3D11InputLayout* output, const D3D11_INPUT_ELEMENT_DESC* elements, UINT count, const void* bytes, SIZE_T size) noexcept;
void remember_stream_output(ID3D11GeometryShader* output, const void* bytes, SIZE_T size,
    const D3D11_SO_DECLARATION_ENTRY* entries, UINT entry_count, const UINT* strides, UINT stride_count, UINT rasterized_stream, IUnknown* linkage) noexcept;
}
