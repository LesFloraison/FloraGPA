#pragma once
#include "protocol.hpp"
#include <filesystem>
namespace flora::capture {
void configure_capture_requests(const std::filesystem::path& directory);
bool capture_requests_connected();
bool eligible_capture_swap(const Json& row);
bool capture_request_available(const Json& swaps,const Json& objects);
Json submit_capture_request(uint64_t selected,const std::string& request_id,uint32_t resource_limit_mib=default_resource_limit_mib);
void disconnect_capture_requests();
Json select_capture_swap(uint64_t selected);
uint64_t selected_capture_swap();
}
