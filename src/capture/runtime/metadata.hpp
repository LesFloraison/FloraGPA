#pragma once
#include "protocol.hpp"
#include "resource_data.hpp"
#include <d3d11_4.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <set>
#include <vector>

namespace flora::capture {
struct DeferredHistory;
struct DeferredCommandList;
// Scalar provenance only. Never retain a mapped CPU pointer after the API call.
struct MappingStamp {
    uint64_t generation=0;
    DWORD thread=0;
    D3D11_MAP type{};
    UINT flags=0,row_pitch=0,depth_pitch=0;
    HRESULT result=E_FAIL;
    bool cpu_storage=false;
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
    std::vector<SubresourceData> creation_subresources;
    std::map<std::pair<uint64_t,UINT>,MappingStamp> mapped_subresources;
    uint64_t mapping_serial=0;
    std::set<uint64_t> active_queries;
    QueryEndStamp last_query_end;
    uint64_t query_end_serial=0;
    bool external_mapping=false;
    std::optional<D3D_DRIVER_TYPE> driver_type;
    std::vector<InputElementRecord> input_elements;
    std::vector<StreamElementRecord> stream_elements;
    std::vector<UINT> stream_strides;
    UINT rasterized_stream = 0;
    uint64_t class_linkage = 0;
    bool creation_observed = false, initial_data_supplied = false;
    // CPU records only; these must never keep native COM objects alive.
    std::shared_ptr<DeferredHistory> deferred_history;
    std::shared_ptr<const DeferredCommandList> command_list;
    explicit ObjectRecord(uint64_t value) : id(value) {}
};
std::shared_ptr<ObjectRecord> track_native_object(IUnknown* identity);
std::shared_ptr<ObjectRecord> find_native_record(IUnknown* identity);
void track_query(const void* context_proxy, IUnknown* query, bool begin) noexcept;
void track_external_mapping(const void* proxy,bool mapped) noexcept;
bool has_external_mapping(bool include_resource_mappings=false,bool ignore_buffer_mappings=false);
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
