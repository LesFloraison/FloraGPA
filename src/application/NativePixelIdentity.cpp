#include "NativePixelIdentity.h"
#include "NativePixel.h"
#include <chrono>
#include <cmath>
namespace flora {
using Json = nlohmann::json;
Json decodeNativePixelPredicate(const NativePixelPredicateStorage &storage) {
    const auto decoded = decodeNativePixel(storage.bytes, storage.format);
    Json result{{"schema", 1},
                {"kind", "native_pixel_predicate_decode"},
                {"typed_format", storage.format},
                {"bytes", storage.bytes},
                {"decoded", decoded},
                {"rgb_predicate", nullptr},
                {"reason", "unavailable_color_decode"}};
    if (decoded.at("status") != "decoded" || decoded.at("color").is_null())
        return result;
    const auto &color = decoded.at("color");
    unsigned stored = 0, zero = 0, positive = 0;
    for (size_t channel = 0; channel < 3; ++channel) {
        if (color.at("stored_channels")[channel] != true)
            continue;
        ++stored;
        const auto &value = color.at("rgba")[channel];
        if (!value.is_number())
            continue;
        const auto number = value.get<double>();
        if (!std::isfinite(number))
            continue;
        zero += number == 0;
        positive += number > 0;
    }
    if (!stored)
        result["reason"] = "no_stored_rgb_channels";
    else if (zero == stored || positive == stored) {
        result["rgb_predicate"] = positive == stored;
        result["reason"] = positive == stored ? "positive_stored_rgb" : "zero_stored_rgb";
    } else
        result["reason"] = "unexpected_mixed_or_nonfinite_rgb";
    return result;
}
Json observeNativePixelIdentity(Replay &replay, Id event, uint32_t target,
                                const std::function<NativePixelPredicateStorage(Replay &)> &read,
                                uint64_t maxBytes, uint32_t timeoutMs, const CancelCheck &cancelled) {
    if (!read || !maxBytes || !timeoutMs || timeoutMs > 60000)
        throw std::invalid_argument("Invalid pixel identity observation limits");
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds(timeoutMs);
    const CancelCheck bounded = [&] {
        checkCancellation(cancelled);
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Native pixel identity deadline");
        return false;
    };
    checkCancellation(bounded);
    Json result{{"schema", 1},
                {"kind", "native_pixel_identity_observation"},
                {"backend", "native-pixel-identity/2"},
                {"event", event},
                {"target", target},
                {"status", "unverified"},
                {"system_primitive_id", nullptr},
                {"candidate_system_id", nullptr},
                {"primitive_ordinal", nullptr},
                {"instance", nullptr},
                {"original_fragment_eligibility", nullptr},
                {"original_gpu_schedule", false},
                {"scope", "repeated_original_ps_private_color_predicates"},
                {"payload_limit", maxBytes},
                {"timeout_ms", timeoutMs},
                {"passes", Json::array()}};
    uint64_t retained = 0;
    auto pass = [&](const std::string &kind, std::optional<PixelPrimitiveMask> predicate = {}) {
        checkCancellation(bounded);
        Json before, after;
        const auto observer = [&](Replay &r, bool done) {
            checkCancellation(bounded);
            auto storage = read(r);
            if (storage.bytes.size() > maxBytes)
                throw std::runtime_error("Pixel identity readback payload budget exceeded");
            (done ? after : before) = decodeNativePixelPredicate(storage);
            checkCancellation(bounded);
        };
        auto observation =
            predicate ? replayNativePixelPrimitiveMask(replay, event, target, *predicate, observer, maxBytes,
                                                       timeoutMs, bounded)
                      : replayNativePixelCompletion(replay, event, target, observer, maxBytes, timeoutMs,
                                                    bounded, {0x3f800001, 0x40000001, 0x40400001});
        const auto format = observation.at("completion_target").at("format");
        if (before.at("typed_format") != format || after.at("typed_format") != format)
            throw std::runtime_error("Pixel identity reader differs from the selected RTV format");
        if (before.at("rgb_predicate") == true)
            throw std::runtime_error("Pixel identity private target was not cleared");
        if (result.at("passes").empty()) {
            result["frame_sha256"] = observation.at("frame_sha256");
            result["adapter"] = observation.at("adapter");
            result["shaders"] = observation.at("shaders");
            result["completion_target"] = observation.at("completion_target");
        } else if (result.at("frame_sha256") != observation.at("frame_sha256") ||
                   result.at("adapter") != observation.at("adapter") ||
                   result.at("shaders") != observation.at("shaders") ||
                   result.at("completion_target") != observation.at("completion_target"))
            throw std::runtime_error("Pixel identity context changed between passes");
        const auto value = before.at("rgb_predicate") == false ? after.at("rgb_predicate") : Json(nullptr);
        Json row{{"role", kind}, {"before", before}, {"after", after}, {"observation", observation}};
        const auto bytes = row.dump().size();
        if (bytes > maxBytes - retained)
            throw std::runtime_error("Pixel identity retained payload budget exceeded");
        retained += bytes;
        result["passes"].push_back(std::move(row));
        return value;
    };
    const auto reference = pass("reference");
    if (reference.is_null())
        result["status"] = "uninterpretable";
    else if (reference == false)
        result["status"] = "no_color_completion";
    else {
        uint32_t candidate = 0;
        bool decoded = true;
        for (uint32_t bit = 0; bit < 32; ++bit) {
            const auto mask = 1u << bit;
            const auto value = pass("bit", PixelPrimitiveMask{mask, mask});
            if (value.is_null()) {
                decoded = false;
                break;
            }
            if (value == true)
                candidate |= mask;
        }
        if (!decoded)
            result["status"] = "uninterpretable";
        else {
            result["candidate_system_id"] = candidate;
            const auto equal = pass("equal", PixelPrimitiveMask{UINT32_MAX, candidate});
            const auto different = pass("different", PixelPrimitiveMask{UINT32_MAX, candidate, true});
            const auto repeated = pass("reference_repeat");
            if (equal == true && different == false && repeated == true) {
                result["status"] = "verified_conditional_replay";
                result["system_primitive_id"] = candidate;
            } else
                result["status"] = "inconsistent";
        }
    }
    result["retained_payload_bytes"] = retained;
    result["pass_count"] = result.at("passes").size();
    result["elapsed_ms"] =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
            .count();
    checkCancellation(bounded);
    return result;
}
} // namespace flora
