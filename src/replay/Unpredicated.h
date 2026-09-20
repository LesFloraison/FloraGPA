#pragma once
#include "Replay.h"
namespace flora {
class Unpredicated {
    ID3D11DeviceContext *context_;
    Com<ID3D11Predicate> predicate_;
    BOOL value_{};

  public:
    explicit Unpredicated(ID3D11DeviceContext *context) : context_(context) {
        context_->GetPredication(&predicate_, &value_);
        if (predicate_)
            context_->SetPredication(nullptr, FALSE);
    }
    ~Unpredicated() { context_->SetPredication(predicate_.Get(), value_); }
    Unpredicated(const Unpredicated &) = delete;
    Unpredicated &operator=(const Unpredicated &) = delete;
};
} // namespace flora
