#pragma once
#include "monitor_support.hpp"
#include <memory>

namespace flora::capture::autodetect {
// Validates an existing x64 PE and resolves an absolute path before elevation.
fs::path validate_target(const std::wstring& path);
class ActiveAttach {
public:
    ActiveAttach(const fs::path& target,const fs::path& shim,const Lease& lease,Progress& progress);
    ~ActiveAttach();
    ActiveAttach(const ActiveAttach&)=delete;
    ActiveAttach& operator=(const ActiveAttach&)=delete;
    std::vector<Json> drain();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
