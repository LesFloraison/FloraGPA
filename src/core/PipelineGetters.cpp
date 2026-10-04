#include "PipelineGetters.h"
#include "Contexts.h"
namespace flora {
namespace {
enum class Kind {
    Shader,
    Srv,
    Cb,
    Sampler,
    Vertex,
    Index,
    Object,
    Rects,
    Outputs,
    Uav,
    So,
    Predicate,
    Type,
    Flags
};
Kind kind(uint16_t type) {
    switch (type) {
    case 0x352a:
    case 0x3530:
    case 0x3540:
    case 0x3544:
    case 0x3549:
        return Kind::Shader;
    case 0x3527:
    case 0x3532:
    case 0x3535:
    case 0x353f:
    case 0x3543:
    case 0x3547:
        return Kind::Srv;
    case 0x3526:
    case 0x352b:
    case 0x352f:
    case 0x3542:
    case 0x3546:
    case 0x354b:
        return Kind::Cb;
    case 0x3529:
    case 0x3533:
    case 0x3536:
    case 0x3541:
    case 0x3545:
    case 0x354a:
        return Kind::Sampler;
    case 0x352d:
        return Kind::Vertex;
    case 0x352e:
        return Kind::Index;
    case 0x352c:
    case 0x353c:
        return Kind::Object;
    case 0x353e:
        return Kind::Rects;
    case 0x3538:
        return Kind::Outputs;
    case 0x3548:
        return Kind::Uav;
    case 0x353b:
        return Kind::So;
    case 0x3534:
        return Kind::Predicate;
    case 0x354e:
        return Kind::Type;
    case 0x354f:
        return Kind::Flags;
    default:
        throw std::runtime_error("Not a recovered pipeline getter");
    }
}
void range(uint32_t start, uint32_t count, uint32_t limit) {
    if (start > limit || count > limit - start)
        throw std::runtime_error("Pipeline getter slot/count exceeds layout limit");
}
} // namespace
bool isPipelineGetter(uint16_t type) {
    // Keep this finite: neighboring methods include mutations and unverified versions.
    switch (type) {
    case 0x3526:
    case 0x3527:
    case 0x3529:
    case 0x352a:
    case 0x352b:
    case 0x352c:
    case 0x352d:
    case 0x352e:
    case 0x352f:
    case 0x3530:
    case 0x3532:
    case 0x3533:
    case 0x3534:
    case 0x3535:
    case 0x3536:
    case 0x3538:
    case 0x353b:
    case 0x353c:
    case 0x353e:
    case 0x353f:
    case 0x3540:
    case 0x3541:
    case 0x3542:
    case 0x3543:
    case 0x3544:
    case 0x3545:
    case 0x3546:
    case 0x3547:
    case 0x3548:
    case 0x3549:
    case 0x354a:
    case 0x354b:
    case 0x354e:
    case 0x354f:
        return true;
    default:
        return false;
    }
}
PipelineGetter readPipelineGetter(uint16_t type, Bytes payload) {
    const auto family = kind(type);
    Reader r(payload);
    PipelineGetter out{r.read<Id>(), r.read<Id>(), {}};
    if (out.link)
        throw std::runtime_error("Linked pipeline getter layout is unresolved");
    auto field = [&](const std::string &name, const std::string &format, bool ref = false) -> uint64_t {
        out.fields.push_back({name, format, r.position(), ref});
        if (format == "B")
            return r.flag();
        if (format == "Q")
            return r.read<Id>();
        if (format == "4i") {
            r.skip(16);
            return 0;
        }
        return r.read<uint32_t>();
    };
    auto flag = [&](const std::string &name) { return field(name + "_present", "B") != 0; };
    auto optional = [&](const std::string &name) {
        if (flag(name))
            field(name, "I");
    };
    auto array = [&](const std::string &name, uint32_t count, const std::string &format, bool ref) {
        if (flag(name))
            for (uint32_t i = 0; i < count; ++i)
                field(name + "[" + std::to_string(i) + "]", format, ref);
    };
    if (family == Kind::Shader) {
        field("returned_shader", "Q", true);
        const bool hasCount = flag("class_count");
        auto count = hasCount ? uint32_t(field("returned_class_count", "I")) : 0;
        range(0, count, 256);
        if (flag("returned_classes")) {
            if (!hasCount)
                throw std::runtime_error("Getter class array has no saved count");
            for (uint32_t i = 0; i < count; ++i)
                field("returned_classes[" + std::to_string(i) + "]", "Q", true);
        }
    } else if (family == Kind::Srv || family == Kind::Cb || family == Kind::Sampler || family == Kind::Uav) {
        auto start = uint32_t(field("start_slot", "I")), count = uint32_t(field("count", "I"));
        range(start, count,
              family == Kind::Srv       ? 128
              : family == Kind::Cb      ? 14
              : family == Kind::Sampler ? 16
                                        : 64);
        array("returned_objects", count, "Q", true);
    } else if (family == Kind::Vertex) {
        auto start = uint32_t(field("start_slot", "I")), count = uint32_t(field("count", "I"));
        range(start, count, 32);
        array("returned_buffers", count, "Q", true);
        array("returned_strides", count, "I", false);
        array("returned_offsets", count, "I", false);
    } else if (family == Kind::Index) {
        field("returned_buffer", "Q", true);
        optional("returned_format");
        optional("returned_offset");
    } else if (family == Kind::Object) {
        field(type == 0x352c ? "returned_input_layout" : "returned_rasterizer", "Q", true);
    } else if (family == Kind::Rects) {
        const bool hasCount = flag("rect_count");
        auto count = hasCount ? uint32_t(field("returned_rect_count", "I")) : 0;
        range(0, count, 16);
        if (flag("returned_rects")) {
            if (!hasCount)
                throw std::runtime_error("Getter rectangle array has no saved count");
            for (uint32_t i = 0; i < count; ++i)
                field("returned_rects[" + std::to_string(i) + "]", "4i");
        }
    } else if (family == Kind::Outputs) {
        auto count = uint32_t(field("rtv_count", "I"));
        range(0, count, 8);
        array("returned_rtvs", count, "Q", true);
        field("returned_dsv", "Q", true);
        auto start = uint32_t(field("uav_start", "I")), uavs = uint32_t(field("uav_count", "I"));
        range(start, uavs, 64);
        array("returned_uavs", uavs, "Q", true);
    } else if (family == Kind::So) {
        auto count = uint32_t(field("count", "I"));
        range(0, count, 4);
        array("returned_buffers", count, "Q", true);
    } else if (family == Kind::Predicate) {
        field("returned_predicate", "Q", true);
        optional("returned_predicate_value");
    } else {
        field(family == Kind::Type ? "returned_context_type" : "returned_context_flags", "I");
    }
    r.end();
    return out;
}
void validatePipelineGetter(const Frame &frame, const PipelineGetter &record) {
    requireImmediateContext(frame, record.context);
}
} // namespace flora
