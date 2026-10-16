#pragma once
#include "protocol.hpp"
#include <filesystem>
#include <optional>
namespace flora::capture {
void configure_capture_requests(const std::filesystem::path& directory);
bool capture_requests_connected();
bool eligible_capture_swap(const Json& row);
bool capture_request_available(const Json& swaps,const Json& objects);
Json submit_capture_request(uint64_t selected,const std::string& request_id,std::optional<uint32_t> resource_limit_mib=std::nullopt);
Json configure_capture_limit(uint32_t resource_limit_mib);
uint32_t capture_resource_limit();
void disconnect_capture_requests();
Json select_capture_swap(uint64_t selected);
uint64_t selected_capture_swap();
}
