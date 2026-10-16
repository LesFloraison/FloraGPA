#include "capture_requests.hpp"
#include "capture_session.hpp"
#include "capture_model.hpp"
#include "objects.hpp"
#include "swap_chain.hpp"
#include <mutex>

namespace flora::capture {
namespace {
struct Requests {std::mutex mutex;std::filesystem::path directory;bool connected=false;uint64_t selected=0;uint32_t resource_limit_mib=default_resource_limit_mib;};
Requests& requests(){static auto* value=new Requests;return *value;}
}
void configure_capture_requests(const std::filesystem::path& directory){
    if(!directory.is_absolute() || !std::filesystem::is_directory(directory))throw std::invalid_argument("Invalid capture output directory");
    auto& r=requests();std::lock_guard lock(r.mutex);
    if(!r.directory.empty())throw std::logic_error("Capture requests already configured");
    r.directory=directory;r.connected=true;
}
bool capture_requests_connected(){auto& r=requests();std::lock_guard lock(r.mutex);return r.connected;}
bool eligible_capture_swap(const Json& row){
    return row.value("recent_present",false) && row.value("successful_presents",0ull)>=2 &&
        supported_swap_buffers(row.value("swap_effect",99u),row.value("buffer_count",0u)) && row.value("samples",0u)==1 &&
        (row.value("format",0u)==DXGI_FORMAT_R8G8B8A8_UNORM || row.value("format",0u)==DXGI_FORMAT_B8G8R8A8_UNORM || row.value("format",0u)==DXGI_FORMAT_R10G10B10A2_UNORM);
}
bool capture_request_available(const Json& swaps,const Json& objects){
    if(!capture_requests_connected() || objects.value("interface_gaps",0ull) || swaps.value("tracking_failures",0u))return false;
    for(const auto& row:swaps["active"])if(eligible_capture_swap(row))return true;return false;
}
Json configure_capture_limit(uint32_t resource_limit_mib){
    if(!resource_limit_mib || resource_limit_mib>max_resource_limit_mib)return {{"ok",false},{"error","invalid_resource_limit"}};
    auto& r=requests();std::lock_guard lock(r.mutex);
    if(!r.connected)return {{"ok",false},{"error","disconnected"}};
    // Captures already armed retain their own budget. This setting applies to
    // the next request, including requests from the in-process hotkey worker.
    r.resource_limit_mib=resource_limit_mib;
    return {{"ok",true},{"resource_limit_mib",r.resource_limit_mib}};
}
uint32_t capture_resource_limit(){auto& r=requests();std::lock_guard lock(r.mutex);return r.resource_limit_mib;}
Json submit_capture_request(uint64_t selected,const std::string& request_id,std::optional<uint32_t> resource_limit_mib){
    pipe_name(request_id);
    auto& r=requests();std::lock_guard lock(r.mutex);
    if(!r.connected)return {{"ok",false},{"error","disconnected"}};
    if(capture_progress().value("busy",false))return {{"ok",false},{"error","busy"}};
    const auto swaps=swap_chain_status(),objects=object_status();
    if(objects.value("interface_gaps",0ull) || swaps.value("tracking_failures",0u))return {{"ok",false},{"error","capture_capability_incomplete"}};
    std::vector<uint64_t> candidates;bool available=false;
    for(const auto& row:swaps["active"]){
        if(!eligible_capture_swap(row))continue;available=true;
        if(selected?row["id"]==selected:row.value("foreground",false))candidates.push_back(row["id"].get<uint64_t>());
    }
    if(!available)return {{"ok",false},{"error","capture_capability_incomplete"}};
    if(candidates.size()!=1)return {{"ok",false},{"error",selected?"swap_chain_unavailable":"swap_chain_selection_required"}};
    return arm_capture(candidates.front(),r.directory/("frame-"+request_id+".gpa_frame"),resource_limit_mib.value_or(r.resource_limit_mib));
}
void disconnect_capture_requests(){auto& r=requests();std::lock_guard lock(r.mutex);r.connected=false;disconnect_capture();}
Json select_capture_swap(uint64_t selected){
    auto& r=requests();std::lock_guard lock(r.mutex);if(!r.connected)return {{"ok",false},{"error","disconnected"}};
    if(selected){bool found=false;const auto swaps=swap_chain_status();for(const auto& row:swaps["active"])if(row["id"]==selected)found=true;
        if(!found)return {{"ok",false},{"error","swap_chain_unavailable"}};}
    r.selected=selected;return {{"ok",true},{"selected_swap_chain",selected}};
}
uint64_t selected_capture_swap(){auto& r=requests();std::lock_guard lock(r.mutex);return r.selected;}
}
