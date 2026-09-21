#pragma once
#include <d3d11_1.h>
#include <string>
namespace flora {
class ReplayAnnotation final {
    ID3DUserDefinedAnnotation *annotation_;

  public:
    ReplayAnnotation(ID3DUserDefinedAnnotation *annotation, uint64_t event, const std::string &command,
                     bool api = true)
        : annotation_(annotation) {
        if (annotation_) {
            const auto label =
                std::string(api ? "GPA API " : "GPA ") + std::to_string(event) + ": " + command;
            // These labels contain only canonical ASCII command names and numeric IDs.
            const std::wstring wide(label.begin(), label.end());
            annotation_->BeginEvent(wide.c_str());
        }
    }
    ~ReplayAnnotation() {
        if (annotation_)
            annotation_->EndEvent();
    }
    ReplayAnnotation(const ReplayAnnotation &) = delete;
    ReplayAnnotation &operator=(const ReplayAnnotation &) = delete;
};
} // namespace flora
