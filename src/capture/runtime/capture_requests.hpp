#pragma once
#include "protocol.hpp"
#include <filesystem>
namespace flora::capture {
void configure_capture_requests(const std::filesystem::path& directory);
bool capture_requests_connected();
bool eligible_capture_swap(const Json& row);
bool capture_request_available(const Json& swaps,const Json& objects);
Json submit_capture_request(uint64_t selected,const std::string& request_id);
void disconnect_capture_requests();
}
