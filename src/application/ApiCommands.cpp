#include "ApiCommands.h"
#include "core/ClassCreation.h"
#include "core/ClearView.h"
#include "core/Contexts.h"
#include "core/PipelineCreation.h"
#include "core/PredicateCreation.h"
#include "core/ResourceLod.h"
#include "core/TextureCreation.h"
#include <QChar>
#include <QDir>
#include <QRegularExpression>
#include <QSaveFile>
#include <QString>
#include <cmath>
#include <set>

namespace flora {
using Json = nlohmann::json;
namespace {
std::string hexBytes(Bytes bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2);
    for (auto b : bytes) {
        text += digits[b >> 4];
        text += digits[b & 15];
    }
    return text;
}
bool in(uint32_t t, std::initializer_list<uint32_t> types) {
    return std::find(types.begin(), types.end(), t) != types.end();
}
bool queryCreate(uint16_t t) { return in(t, {0x3074, 0x3235, 0x33a8, 0x3471, 0x34b2, 0x358d}); }
bool queryCreate1(uint16_t t) { return in(t, {0x3495, 0x34d6, 0x35b1}); }
bool queryGet(uint16_t t) { return in(t, {0x30b4, 0x31b4, 0x331d, 0x33e3, 0x34fb}); }
bool getter(uint16_t t) {
    return in(t, {0x304b, 0x304c, 0x304d, 0x3528, 0x3537, 0x3539, 0x353a, 0x353d, 0x4029, 0x402a, 0x30ea,
                  0x31ea, 0x3353, 0x3419, 0x3531, 0x313b, 0x300e, 0x359d});
}
const std::map<uint16_t, int> finishes{{0x3109, 0}, {0x3209, 1}, {0x3372, 2}, {0x3438, 3}, {0x3550, 4}};
class Wire {
    Bytes bytes_;

  public:
    Reader reader;
    Json fields = Json::array(), references = Json::array();
    explicit Wire(Bytes bytes) : bytes_(bytes), reader(bytes) {}
    Json field(const std::string &name, const std::string &format, bool reference = false) {
        size_t n = 0;
        for (char c : format) {
            if (c < '0' || c > '9')
                break;
            n = n * 10 + size_t(c - '0');
        }
        if (!n && format.size() == 1)
            n = 1;
        char kind = format.back();
        size_t unit = kind == 'Q' ? 8 : (kind == 's' || kind == 'B' ? 1 : 4), size = n * unit;
        auto start = reader.position();
        if (size > reader.remaining())
            throw std::runtime_error(name + ": requires " + std::to_string(size) + " bytes at " +
                                     std::to_string(start));
        auto raw = reader.take(size);
        Json value;
        if (kind == 's')
            value = hexBytes(raw);
        else {
            Reader part(raw);
            auto values = Json::array();
            for (size_t i = 0; i < n; ++i) {
                if (kind == 'Q')
                    values.push_back(part.read<uint64_t>());
                else if (kind == 'I')
                    values.push_back(part.read<uint32_t>());
                else if (kind == 'i')
                    values.push_back(part.read<int32_t>());
                else if (kind == 'B')
                    values.push_back(part.read<uint8_t>());
                else if (kind == 'f') {
                    auto v = part.read<float>();
                    if (std::isfinite(v))
                        values.push_back(v);
                    else
                        values.push_back(std::isnan(v) ? "nan" : v < 0 ? "-inf" : "inf");
                } else
                    throw std::runtime_error("Unknown inspection scalar encoding");
            }
            value = n == 1 ? values[0] : values;
            if (reference)
                for (size_t i = 0; i < n; ++i)
                    if (values[i] != 0)
                        references.push_back({{"field", n == 1 ? name : name + "[" + std::to_string(i) + "]"},
                                              {"id", values[i]}});
        }
        fields.push_back({{"name", name},
                          {"offset", start},
                          {"size", size},
                          {"encoding", format},
                          {"value", value},
                          {"hex", hexBytes(raw)}});
        return value;
    }
    uint32_t u(const std::string &name) { return field(name, "I").get<uint32_t>(); }
    Id id(const std::string &name, bool ref = true) { return field(name, "Q", ref).get<Id>(); }
    bool flag(const std::string &name) {
        auto value = field(name, "B").get<unsigned>();
        if (value > 1)
            throw std::runtime_error(name + ": invalid optional-array flag " + std::to_string(value));
        return value != 0;
    }
    Json array(const std::string &name, const std::string &fmt, uint32_t count, bool reference = false,
               bool optional = true) {
        if (optional && !flag(name + "_present"))
            return nullptr;
        if (count > 65536)
            throw std::runtime_error(name + ": array count outside supported inspection bounds");
        auto out = Json::array();
        for (uint32_t i = 0; i < count; ++i)
            out.push_back(field(name + "[" + std::to_string(i) + "]", fmt, reference));
        return out;
    }
    Json values(const std::string &fmt, std::initializer_list<const char *> names,
                std::initializer_list<std::string> refs = {}) {
        Json out = Json::object();
        size_t i = 0;
        for (auto name : names)
            out[name] = field(name, std::string(1, fmt.at(i++)),
                              std::find(refs.begin(), refs.end(), name) != refs.end());
        return out;
    }
};
Json annotation(uint16_t t, Wire &r) {
    auto name = commandName(t);
    Json info{{"kind", name.substr(name.find('.') + 1)}};
    if (t == 0x3278) {
        r.field("hresult", "i");
        r.field("interface_guid", "16s");
        r.id("returned_object");
    } else if (t == 0x3279 || t == 0x327a)
        info["ref_count"] = r.u("return_ref_count");
    else if (t == 0x327e)
        info["status_value"] = r.u("return_status");
    if (t == 0x327b || t == 0x327c)
        info["return_level"] = r.field("return_level", "i");
    if (t == 0x327b || t == 0x327d) {
        auto size = r.u("name_byte_count");
        if (size % 2 || size > 0x8002)
            throw std::runtime_error("Annotation name length must be even and at most 32770 bytes");
        auto value = size ? r.field("name_utf16le", std::to_string(size) + "s").get<std::string>() : "";
        auto raw = QByteArray::fromHex(QByteArray::fromStdString(value));
        bool terminated = size >= 2 && raw.endsWith(QByteArray(2, '\0')), valid = true;
        QString text;
        auto length = int(size) - (terminated ? 2 : 0);
        for (int i = 0; i < length; i += 2) {
            auto unit = uint16_t(uint8_t(raw[i])) | uint16_t(uint8_t(raw[i + 1]) << 8);
            if (QChar::isHighSurrogate(unit) && i + 3 < length) {
                auto low = uint16_t(uint8_t(raw[i + 2])) | uint16_t(uint8_t(raw[i + 3]) << 8);
                if (QChar::isLowSurrogate(low)) {
                    text += QChar(unit);
                    text += QChar(low);
                    i += 2;
                    continue;
                }
            }
            if (QChar::isSurrogate(unit)) {
                valid = false;
                text += QString("\\x%1\\x%2")
                            .arg(uint8_t(raw[i]), 2, 16, QChar('0'))
                            .arg(uint8_t(raw[i + 1]), 2, 16, QChar('0'));
            } else
                text += QChar(unit);
        }
        QString display;
        for (auto cp : text.toUcs4()) {
            auto category = QChar::category(cp);
            bool printable =
                cp == 32 || !(category == QChar::Other_Control || category == QChar::Other_Format ||
                              category == QChar::Other_Surrogate || category == QChar::Other_PrivateUse ||
                              category == QChar::Other_NotAssigned || category == QChar::Separator_Space ||
                              category == QChar::Separator_Line || category == QChar::Separator_Paragraph);
            if (printable)
                display += QString::fromUcs4(std::u32string(1, char32_t(cp)).data(), 1);
            else
                display += QString("\\u%1").arg(cp, 4, 16, QChar('0'));
        }
        info.update({{"name", size ? Json(text.toStdString()) : Json(nullptr)},
                     {"display_name", size ? display.toStdString() : "<NULL>"},
                     {"name_hex", value},
                     {"utf16_valid", valid},
                     {"null_name", size == 0},
                     {"terminated", terminated},
                     {"capture_limit_reached", size == 0x8002},
                     {"embedded_nul", text.contains(QChar(0))}});
    }
    return info;
}
void readGetter(uint16_t t, Wire &r) {
    if (t == 0x359d)
        r.id("returned_context");
    else if (in(t, {0x30ea, 0x31ea, 0x3353, 0x3419, 0x3531})) {
        if (r.flag("topology_present"))
            r.u("returned_topology");
    } else if (in(t, {0x313b, 0x300e}))
        r.array("resource_dimension", "I", 1);
    else if (t == 0x304b) {
        r.field("hresult", "i");
        r.field("interface_guid", "16s");
        r.id("returned_object");
    } else if (in(t, {0x304c, 0x304d, 0x4029, 0x402a}))
        r.u("return_ref_count");
    else if (t == 0x3528) {
        r.id("returned_shader");
        auto n = r.flag("class_count_present") ? r.u("returned_class_count") : 0;
        r.array("returned_classes", "Q", n, true);
    } else if (t == 0x3537) {
        auto n = r.u("rtv_count");
        r.array("returned_rtvs", "Q", n, true);
        r.id("returned_dsv");
    } else if (t == 0x3539) {
        r.id("returned_blend_state");
        if (r.flag("blend_factor_present"))
            r.field("returned_blend_factor", "4f");
        if (r.flag("sample_mask_present"))
            r.u("returned_sample_mask");
    } else if (t == 0x353a) {
        r.id("returned_depth_stencil_state");
        if (r.flag("stencil_ref_present"))
            r.u("returned_stencil_ref");
    } else if (t == 0x353d) {
        auto n = r.flag("viewport_count_present") ? r.u("returned_viewport_count") : 0;
        if (r.flag("viewports_present")) {
            if (!n)
                throw std::runtime_error("Captured viewport array requires a present nonzero returned count");
            r.array("returned_viewports", "6f", n, false, false);
        }
    }
}
void drawAuto(const Frame &frame, Id state, Json &out) {
    try {
        auto s = frame.state(state);
        auto gs = s.stages[3].shader;
        Id declaration = 0;
        uint32_t stream = 0;
        if (gs) {
            Reader shader(frame.payload(gs, 5, 0x91));
            shader.skip(40);
            declaration = shader.read<Id>();
            if (declaration) {
                Reader d(frame.payload(declaration, 9, 0x85));
                auto count = d.read<uint32_t>();
                if (count > 512)
                    throw std::runtime_error("Stream-output declaration count or size");
                for (uint32_t i = 0; i < count; ++i) {
                    auto output = d.read<uint32_t>();
                    d.skip(4);
                    auto semantic = d.read<Id>();
                    auto index = d.read<uint32_t>();
                    (void)index;
                    auto start = d.read<uint8_t>(), n = d.read<uint8_t>(), slot = d.read<uint8_t>();
                    d.skip(1);
                    if (output > 3 || slot > 3 || !n || start > 3 || (semantic && start + n > 4))
                        throw std::runtime_error("Stream-output declaration component or slot range");
                    if (semantic) {
                        auto text = frame.data(semantic);
                        if (text.empty() || text.back() != 0 || text.size() > 256 ||
                            std::find(text.begin(), text.end() - 1, 0) != text.end() - 1 ||
                            std::any_of(text.begin(), text.end(), [](auto c) { return c > 127; }))
                            throw std::runtime_error("Stream-output semantic name is not a bounded C string");
                    }
                }
                auto strides = d.read<uint32_t>();
                if (strides > 4)
                    throw std::runtime_error("Stream-output strides or trailing bytes");
                for (uint32_t i = 0; i < strides; ++i) {
                    auto stride = d.read<uint32_t>();
                    if (stride > 2048 || stride % 4)
                        throw std::runtime_error("Stream-output stride or rasterized stream range");
                }
                stream = d.read<uint32_t>();
                d.end();
                if (stream > 3 && stream != UINT32_MAX)
                    throw std::runtime_error("Stream-output stride or rasterized stream range");
            }
        }
        auto selected = stream == UINT32_MAX ? 0 : stream;
        Json result{{"api", "Draw"},
                    {"parameters", {{"vertex_count", s.soCounts[selected]}, {"start_vertex", 0}}},
                    {"source", "captured_stream_output_vertex_count"},
                    {"stream", selected},
                    {"geometry_shader", gs},
                    {"stream_output_declaration", declaration}};
        if (!gs || stream == UINT32_MAX)
            result["original_player_vertex_count"] = 0;
        if (stream == UINT32_MAX)
            result["rasterized_stream"] = stream;
        out["replay"] = result;
    } catch (const std::exception &e) {
        out["replay_unavailable"] = e.what();
    }
}
} // namespace

Json inspectCommand(const Frame &frame, Id id) {
    auto e = frame.entry(id);
    if (e.category != 7)
        throw std::runtime_error("Entry is not an API command");
    auto raw = frame.payload(id);
    Wire r(raw);
    auto t = e.type;
    auto name = commandName(t);
    bool known = !name.starts_with("Unknown");
    if (!known)
        name = QString("Unknown 0x%1").arg(t, 4, 16, QChar('0')).toStdString();
    Json out{{"id", id}, {"name", name}, {"type", t}, {"wire_size", raw.size()}, {"status", "decoded"}};
    if (queryGet(t))
        out["note"] = "Only one uint32 memory word is retained. It is not valid query data unless HRESULT is "
                      "S_OK; larger results remain incomplete.";
    if (queryCreate(t))
        out["note"] =
            "The descriptor is captured, but this shim maps IID_ID3D11Query to no resource type and writes a "
            "zero returned reference. This record does not restore a replayable query.";
    if (queryCreate1(t))
        out["note"] = "This hook records only HRESULT: the Query1 descriptor and returned object are absent, "
                      "not zero-valued defaults.";
    if (in(t, {0x3151, 0x3152}))
        out["note"] = "Captured query metadata only; a Query object ID does not prove that its resource has "
                      "a restorable player layout.";
    if (t >= 0x3278 && t <= 0x327e)
        out["note"] = "Captured annotation interface call. Object ID itself is not a context ID; context and "
                      "Draw links require validated QueryInterface evidence.";
    if (getter(t))
        out["note"] = "Captured inspection/lifetime metadata; it does not set replay state.";
    if (in(t, {0x3528, 0x3537, 0x3539, 0x353a}))
        out["note"] = out["note"].get<std::string>() +
                      " A zero returned object reference cannot distinguish an omitted "
                      "output pointer from a null returned binding.";
    if (in(t, {0x3017, 0x3597, 0x3578}) || isTextureCreation(t))
        out["note"] = "Captured process pointers are numeric values, not resource IDs or readable addresses "
                      "in this process.";
    if (t == 0x246)
        out["note"] = "This replay record links captured Map writes; the original API Map call may also have "
                      "its own record.";
    try {
        if (isDraw(t)) {
            // Validate flags and complete record size before exposing draw parameters.
            auto event = frame.event(id);
            Json params;
            out["draw"] = true;
            out["state_id"] = event.state;
            r.id("state");
            r.id("draw_flags", false);
            r.id("context");
            switch (t) {
            case 0x35:
                params = r.values("III", {"x", "y", "z"});
                break;
            case 0x37:
                params = r.values("II", {"vertex_count", "start_vertex"});
                break;
            case 0x38:
                params = Json::object();
                break;
            case 0x39:
                params = r.values("IIi", {"index_count", "start_index", "base_vertex"});
                break;
            case 0x3a:
                params = r.values("IIIiI", {"index_count", "instance_count", "start_index", "base_vertex",
                                            "start_instance"});
                break;
            case 0x3c:
                params =
                    r.values("IIII", {"vertex_count", "instance_count", "start_vertex", "start_instance"});
                break;
            default:
                params = r.values("QI", {"argument_buffer", "offset"}, {"argument_buffer"});
            }
            out["parameters"] = params;
            if (t == 0x38)
                drawAuto(frame, event.state, out);
        } else {
            r.id("record_link");
            out["object"] = r.id("object");
            int slot = int(t) - 0x34de;
            if (name == "Begin")
                slot = 27;
            else if (name == "End")
                slot = 28;
            else if (name == "SetPredication")
                slot = 30;
            else if (name == "SOSetTargets")
                slot = 37;
            if (isResourceLodRecord(t)) {
                readResourceLod(t, raw);
                if (t == 0x3515)
                    r.values("Qf", {"resource", "min_lod"}, {"resource"});
                else
                    r.values("fQ", {"observed_min_lod", "resource"}, {"resource"});
            } else if (t >= 0x3278 && t <= 0x327e)
                out["annotation"] = annotation(t, r);
            else if (getter(t))
                readGetter(t, r);
            else if (t == 0x41 || t == 0x30d1) {
                if (raw.size() != 28)
                    throw std::runtime_error("ExecuteCommandList requires a 28-byte payload");
                Reader copy(raw);
                auto link = copy.read<Id>(), owner = copy.read<Id>();
                auto operand = r.id("command_list_operand", false);
                auto restore = r.u("restore_context_state");
                Json truncated = Json::array();
                if (link > UINT32_MAX)
                    truncated.push_back("record_link");
                if (owner > UINT32_MAX)
                    truncated.push_back("owner");
                out["command_list"] = {{"record_link", link},
                                       {"owner", owner},
                                       {"command_list_operand", operand},
                                       {"restore_context_state", restore},
                                       {"restore_context_state_enabled", restore != 0},
                                       {"operand_domain", "unproven_pointer_or_resource_id"},
                                       {"original_player_fields",
                                        {{"record_link", uint32_t(link)},
                                         {"owner", uint32_t(owner)},
                                         {"command_list_operand", operand},
                                         {"restore_context_state", restore}}},
                                       {"original_player_truncated_fields", truncated}};
                out["replay_unavailable"] =
                    "Command-list execution and operand pointer/ID identity remain unverified.";
            } else if (finishes.contains(t)) {
                if (raw.size() != 32)
                    throw std::runtime_error("FinishCommandList requires a 32-byte payload");
                auto hr = r.field("hresult", "i").get<int32_t>();
                auto restore = r.u("restore_deferred_context_state");
                auto returned = r.id("returned_reference", false);
                Reader copy(raw);
                auto link = copy.read<Id>(), context = copy.read<Id>();
                std::string note = "HRESULT success alone does not prove a command list was returned; this "
                                   "GPA immediate-context manager returns S_OK without creating one.";
                out["command_list_finish"] = {
                    {"record_link", link},
                    {"context", context},
                    {"interface_version", finishes.at(t)},
                    {"hresult", hr},
                    {"hresult_hex", QString("0x%1").arg(uint32_t(hr), 8, 16, QChar('0')).toStdString()},
                    {"hresult_succeeded", hr >= 0},
                    {"restore_deferred_context_state", restore},
                    {"restore_deferred_context_state_enabled", restore != 0},
                    {"returned_reference", returned},
                    {"output_pointer_presence", "not_captured"},
                    {"list_execution_restored", false},
                    {"note", note}};
                out["note"] = note;
                try {
                    acceptFinishCommandList(frame, readFinishCommandList(t, raw));
                    out["replay_class"] = "captured_immediate_finish_noop";
                } catch (const std::exception &e2) {
                    out["replay_unavailable"] = e2.what();
                }
            } else if ((t >= 0x249 && t <= 0x254) || in(uint16_t(slot), {7, 16, 22, 25, 26, 8, 10, 31, 32, 59,
                                                                         61, 62, 63, 65, 66, 67, 70, 71})) {
                r.u("start_slot");
                auto count = r.u("count");
                r.array("bindings", "Q", count, true);
                if (t >= 0x24f && t <= 0x254) {
                    r.array("first_constants", "I", count);
                    r.array("constant_counts", "I", count);
                }
            } else if (in(uint16_t(slot), {9, 11, 23, 60, 64, 69})) {
                r.id("shader");
                auto n = r.u("class_count");
                r.array("class_instances", "Q", n, true);
            } else if (slot == 18) {
                r.u("start_slot");
                auto n = r.u("count");
                r.array("buffers", "Q", n, true);
                r.array("strides", "I", n);
                r.array("offsets", "I", n);
            } else if (in(uint16_t(slot), {17, 43, 27, 28}) || t == 0x245)
                r.id(slot == 17                 ? "input_layout"
                     : slot == 43               ? "rasterizer"
                     : slot == 27 || slot == 28 ? "query"
                                                : "view");
            else if (slot == 19)
                r.values("QII", {"buffer", "format", "offset"}, {"buffer"});
            else if (slot == 24)
                r.u("topology");
            else if (slot == 15)
                r.values("QI", {"resource", "subresource"}, {"resource"});
            else if (slot == 30)
                r.values("QI", {"predicate", "value"}, {"predicate"});
            else if (slot == 36)
                r.values("QI", {"depth_state", "stencil_ref"}, {"depth_state"});
            else if (slot == 33) {
                auto n = r.u("count");
                r.array("rtvs", "Q", n, true);
                r.id("dsv");
            } else if (slot == 34) {
                auto n = r.u("rtv_count");
                r.array("rtvs", "Q", n, true);
                r.id("dsv");
                r.u("uav_start");
                n = r.u("uav_count");
                r.array("uavs", "Q", n, true);
                r.array("initial_counts", "I", n);
            } else if (slot == 35) {
                r.id("blend_state");
                r.array("blend_factor", "f", 4);
                r.u("sample_mask");
            } else if (slot == 44 || slot == 45) {
                auto n = r.u("count");
                r.array(slot == 44 ? "viewports" : "scissors", slot == 44 ? "6f" : "4i", n);
            } else if (slot == 37) {
                auto n = r.u("count");
                r.array("buffers", "Q", n, true);
                r.array("offsets", "I", n);
            } else if (t == 0x25e || slot == 68) {
                r.u("start_slot");
                auto n = r.u("count");
                r.array("uavs", "Q", n, true);
                r.array("initial_counts", "I", n);
            } else if (t == 0x246 || t == 0x34ec)
                r.values("iQIIIQ", {"hresult", "resource", "subresource", "map_type", "map_flags", "data"},
                         {"resource", "data"});
            else if (t == 0x247 || t == 0x255) {
                r.values("QI", {"destination", "subresource"}, {"destination"});
                r.array("box", "6I", 1);
                r.values("QII", {"data", "row_pitch", "depth_pitch"}, {"data"});
                if (t == 0x255)
                    r.u("copy_flags");
            } else if (t == 0x3e)
                r.values("QQ", {"destination", "source"}, {"destination", "source"});
            else if (t == 0x3f)
                r.values("QIQ", {"destination", "offset", "source_uav"}, {"destination", "source_uav"});
            else if (t == 0x40 || t == 0x256) {
                r.values(
                    "QIIIIQI",
                    {"destination", "destination_subresource", "x", "y", "z", "source", "source_subresource"},
                    {"destination", "source"});
                r.array("source_box", "6I", 1);
                if (t == 0x256)
                    r.u("copy_flags");
            } else if (t == 0x42)
                r.values("QIQII",
                         {"destination", "destination_subresource", "source", "source_subresource", "format"},
                         {"destination", "source"});
            else if (t == 0x31)
                r.values("QIfB", {"view", "clear_flags", "depth", "stencil"}, {"view"});
            else if (in(t, {0x32, 0x33, 0x34})) {
                r.id("view");
                r.array("values", t == 0x33 ? "I" : "f", 4);
            } else if (t == 0x257) {
                readClearView(raw);
                r.id("view");
                r.array("color", "f", 4);
                const auto count = r.u("rectangle_count");
                r.array("rectangles", "4i", count);
            } else if (t == 0x242 || t == 0x244) {
            } else if (in(t, {0x3163, 0x3164, 0x3113, 0x3114, 0x318f, 0x3190, 0x3013, 0x3014, 0x3250, 0x3251,
                              0x3576, 0x3577}))
                r.u("return_ref_count");
            else if (t == 0x311a) {
                if (r.flag("descriptor_present"))
                    r.field("class_descriptor", "8I");
            } else if (t == 0x311b || t == 0x311c) {
                auto length = r.flag("length_present") ? r.field("returned_length", "Q").get<uint64_t>() : 0;
                if (r.flag("name_present"))
                    r.field("name_bytes", std::to_string(length) + "s");
            } else if (t == 0x3019 || t == 0x3146)
                r.array("resource_dimension", "I", 1);
            else if (in(t, {0x3162, 0x3112, 0x318e, 0x3012, 0x324f, 0x3256, 0x3575})) {
                r.field("hresult", "i");
                r.field("interface_guid", "16s");
                r.id("returned_object");
            } else if (t == 0x3257)
                r.values("iII", {"hresult", "sync_interval", "flags"});
            else if (queryGet(t)) {
                auto head = r.values("iQ", {"hresult", "query"}, {"query"});
                auto word = r.array("captured_data_word", "I", 1);
                auto tail = r.values("II", {"data_size", "get_data_flags"});
                auto size = tail["data_size"].get<uint32_t>();
                bool ready = head["hresult"] == 0;
                out["query_capture"] = {
                    {"source", "captured_api_record"},
                    {"result_ready", ready},
                    {"captured_memory_bytes", word.is_null() ? 0 : 4},
                    {"requested_bytes", size},
                    {"complete_requested_bytes", ready && !word.is_null() && size > 0 && size <= 4},
                    {"status_only", size == 0},
                    {"typed_result_restored", false}};
            } else if (queryCreate(t)) {
                r.field("hresult", "i");
                auto present = r.flag("descriptor_present");
                if (present)
                    r.values("II", {"query_type", "misc_flags"});
                auto returned = r.id("returned_query");
                out["query_capture"] = {{"source", "captured_api_record"},
                                        {"descriptor_present", present},
                                        {"returned_reference", returned},
                                        {"resource_replay_supported", false}};
            } else if (queryCreate1(t)) {
                r.field("hresult", "i");
                out["query_capture"] = {{"source", "captured_api_record"},
                                        {"descriptor_saved", false},
                                        {"returned_reference_saved", false},
                                        {"resource_replay_supported", false}};
            } else if (t == 0x3169 || t == 0x3151)
                r.u("return_data_size");
            else if (t == 0x316a || t == 0x3152) {
                if (r.flag("descriptor_present"))
                    r.values("II", {"query_type", "misc_flags"});
            } else if (t == 0x3017 || t == 0x3167) {
                r.field("hresult", "i");
                r.field("guid", "16s");
                r.u("data_size");
                r.id("captured_data_pointer", false);
            } else if (t == 0x3597 || t == 0x3166) {
                r.field("hresult", "i");
                r.field("guid", "16s");
                r.array("data_size", "I", 1);
                r.id("captured_data_pointer", false);
            } else if (t == 0x302e) {
                if (r.flag("descriptor_present")) {
                    r.u("format");
                    r.u("view_dimension");
                    r.field("descriptor_union", "4I");
                }
            } else if (t == 0x358e) {
                readPredicateCreation(raw);
                r.field("hresult", "i");
                if (r.flag("descriptor_present"))
                    r.values("II", {"query_type", "misc_flags"});
                r.id("returned_resource");
            } else if (isClassCreation(t)) {
                readClassCreation(t, raw);
                r.field("hresult", "i");
                if (t == 0x3195)
                    r.u("instance_index");
                if (t == 0x3196)
                    r.values("IIII",
                             {"constant_buffer", "constant_vector", "texture_offset", "sampler_offset"});
                r.id("returned_resource");
            } else if (isPipelineCreation(t)) {
                const auto creation = readPipelineCreation(t, raw);
                r.field("hresult", "i");
                if (isStateCreation(t)) {
                    if (r.flag("descriptor_present"))
                        r.field("descriptor", std::to_string(creation.descriptor.size()) + "s");
                } else {
                    if (t == 0x3580) {
                        r.u("element_count");
                        if (r.flag("elements_present"))
                            for (size_t i = 0; i < creation.elements.size(); ++i) {
                                const auto prefix = "element[" + std::to_string(i) + "].";
                                r.id(prefix + "semantic_name_pointer", false);
                                r.field(prefix + "descriptor", "6I");
                            }
                    }
                    r.field("bytecode_length", "Q");
                    if (r.flag("bytecode_present"))
                        r.field("bytecode", std::to_string(creation.bytecode.size()) + "s");
                    if (t == 0x3583) {
                        r.u("so_element_count");
                        if (r.flag("so_elements_present"))
                            for (size_t i = 0; i < creation.streamOutput.entries.size(); ++i) {
                                const auto prefix = "so_element[" + std::to_string(i) + "].";
                                r.u(prefix + "stream");
                                r.field(prefix + "padding", "4s");
                                r.id(prefix + "semantic_name_pointer", false);
                                r.u(prefix + "semantic_index");
                                r.field(prefix + "components_slot_padding", "4B");
                            }
                        if (r.flag("strides_present"))
                            r.u("first_stride");
                        r.u("stride_count");
                        r.u("rasterized_stream");
                    }
                    if (t != 0x3580)
                        r.id("class_linkage");
                }
                r.id("returned_resource");
            } else if (isTextureCreation(t)) {
                const auto creation = readTextureCreation(t, raw);
                r.field("hresult", "i");
                if (isViewCreation(t))
                    r.id("source_resource");
                if (r.flag("descriptor_present"))
                    r.field("descriptor", std::to_string(creation.descriptor.size()) + "I");
                if (!isViewCreation(t) && r.flag("initial_data_present"))
                    for (size_t i = 0; i < creation.initial.size(); ++i) {
                        auto prefix = "initial[" + std::to_string(i) + "].";
                        r.id(prefix + "captured_pointer", false);
                        r.u(prefix + "row_pitch");
                        r.u(prefix + "slice_pitch");
                    }
                r.id("returned_resource");
            } else if (isTextureCreationObservation(t)) {
                acceptTextureCreationObservation(t, raw);
                const auto view = viewCreationObservation(t);
                if (in(t, {0x313f, 0x3134, 0x3007}) || view == ViewObservation::QueryInterface) {
                    r.field("hresult", "i");
                    r.field("iid", "16s");
                    r.id("returned_interface");
                } else if (in(t, {0x3140, 0x3141, 0x3135, 0x3136, 0x3008, 0x3009}) ||
                           view == ViewObservation::ReferenceCount)
                    r.u("observed_reference_count");
                else if (in(t, {0x3015, 0x3142, 0x3137, 0x300a}) || view == ViewObservation::GetDevice)
                    r.id("returned_device");
                else if (view == ViewObservation::GetResource)
                    r.id("returned_resource");
                else if (view == ViewObservation::GetDescriptor) {
                    if (r.flag("descriptor_present"))
                        r.field("descriptor", t == 0x3025 ? "6I" : "5I");
                } else if (in(t, {0x301c, 0x3149, 0x313e, 0x3011})) {
                    if (r.flag("descriptor_present"))
                        r.field("descriptor", t == 0x301c   ? "6I"
                                              : t == 0x313e ? "8I"
                                              : t == 0x3011 ? "9I"
                                                            : "11I");
                } else {
                    r.field("hresult", "i");
                    r.u("format");
                    if (t == 0x35aa) {
                        r.u("sample_count");
                        r.u("flags");
                    }
                    if (r.flag("output_present"))
                        r.u(t == 0x3592 ? "format_support" : "quality_levels");
                }
            } else if (t == 0x3578) {
                r.field("hresult", "i");
                if (r.flag("descriptor_present"))
                    r.values("IIIIII", {"byte_width", "usage", "bind_flags", "cpu_access_flags", "misc_flags",
                                        "structure_stride"});
                if (r.flag("initial_data_present"))
                    r.values("QII", {"captured_initial_data_pointer", "row_pitch", "slice_pitch"});
                r.id("returned_buffer");
            } else if (t == 0x3261) {
                r.field("hresult", "i");
                if (r.flag("descriptor_present"))
                    r.values("IIIIIIIIIIIIQIIII",
                             {"width", "height", "refresh_numerator", "refresh_denominator", "format",
                              "scanline_order", "scaling", "sample_count", "sample_quality", "buffer_usage",
                              "buffer_count", "reserved_padding0", "output_window", "windowed", "swap_effect",
                              "flags", "reserved_padding1"});
            } else
                out["status"] = known ? "partial" : "unknown";
        }
        if (r.reader.remaining()) {
            if (out["status"] == "decoded")
                out["status"] = "partial";
            out["remaining_offset"] = r.reader.position();
            out["remaining_hex"] = hexBytes(raw.subspan(r.reader.position()));
        }
    } catch (const std::exception &error) {
        out.update({{"status", "invalid"},
                    {"error", error.what()},
                    {"remaining_offset", r.reader.position()},
                    {"remaining_hex", hexBytes(raw.subspan(r.reader.position()))}});
    }
    for (auto &ref : r.references) {
        auto it = frame.entries().find(ref["id"].get<Id>());
        ref["exists"] = it != frame.entries().end();
        if (it != frame.entries().end()) {
            auto &target = it->second;
            ref.update({{"category", target.category}, {"type", target.type}, {"size", target.size}});
            if (target.category == 5 && target.type >= 0x8c && target.type <= 0x8f) {
                auto data = frame.payload(target.id);
                if (data.size() >= 24) {
                    Reader v(data);
                    v.skip(16);
                    ref["resource"] = v.read<Id>();
                }
            }
        }
    }
    out["fields"] = std::move(r.fields);
    out["references"] = std::move(r.references);
    return out;
}

bool commandMatches(const Json &command, const std::string &text, std::optional<Id> resource) {
    if (resource &&
        std::none_of(command["references"].begin(), command["references"].end(), [&](const auto &r) {
            return r["id"] == *resource || (r.contains("resource") && r["resource"] == *resource);
        }))
        return false;
    auto hay = QString::fromStdString(command["id"].dump() + " " + command["name"].get<std::string>() + " " +
                                      command["status"].get<std::string>())
                   .toCaseFolded();
    for (auto word :
         QString::fromStdString(text).toCaseFolded().split(QRegularExpression("\\s+"), Qt::SkipEmptyParts))
        if (!hay.contains(word))
            return false;
    return true;
}
Json commandResourceSelection(const Frame &frame, const Json &reference, const Json &command) {
    auto id = reference.value("resource", reference.at("id").get<Id>());
    auto it = frame.entries().find(id);
    if (it == frame.entries().end() || it->second.category != 5 ||
        !in(it->second.type, {0x83, 0x84, 0x85, 0x86, 0x87, 0x96}))
        return nullptr;
    Json result{{"resource", id}, {"mip", 0}, {"layer", 0}, {"slice", 0}, {"typed_format", nullptr}};
    if (!command.is_null() && command.value("status", "") == "decoded" &&
        in(command.at("type").get<uint16_t>(), {0x246, 0x247, 0x34ec, 0x34ed}) && it->second.type >= 0x84 &&
        it->second.type <= 0x87) {
        auto target = command["type"] == 0x247 ? "destination" : "resource";
        if (reference.value("field", "") == target) {
            Json fields;
            for (auto &f : command["fields"])
                fields[f["name"].get<std::string>()] = f["value"];
            auto info = textureInfo(frame.resource(id));
            if (fields.contains("subresource") && fields.value(target, Id(0)) == id && info.mips) {
                auto sub = fields["subresource"].get<uint32_t>();
                if (sub < uint64_t(info.mips) * info.layers) {
                    result["mip"] = sub % info.mips;
                    result["layer"] = sub / info.mips;
                }
            }
        }
    }
    auto view = frame.entries().find(reference.at("id").get<Id>());
    if (id == reference["id"] || it->second.type == 0x83 || view == frame.entries().end() ||
        view->second.type < 0x8c || view->second.type > 0x8f)
        return result;
    Reader r(frame.payload(view->first));
    r.skip(24);
    std::vector<uint32_t> fields;
    while (r.remaining())
        fields.push_back(r.read<uint32_t>());
    auto fmt = fields.at(0), dim = fields.at(1);
    result["typed_format"] = fmt ? Json(fmt) : Json(nullptr);
    auto set = [&](const char *key, size_t offset) { result[key] = fields.at(offset); };
    if (view->second.type == 0x8c) {
        if (in(dim, {2, 3, 4, 5, 8, 9, 10}))
            set("mip", 2);
        if (in(dim, {3, 5, 10}))
            set("layer", 4);
        if (dim == 7)
            set("layer", 2);
    } else if (view->second.type == 0x8e) {
        if (in(dim, {1, 2, 3, 4}))
            set("mip", 3);
        if (dim == 2 || dim == 4)
            set("layer", 4);
        if (dim == 6)
            set("layer", 3);
    } else {
        if (in(dim, {2, 3, 4, 5, 8}))
            set("mip", 2);
        if (dim == 3 || dim == 5)
            set("layer", 3);
        if (dim == 7)
            set("layer", 2);
        if (dim == 8)
            set("slice", 3);
    }
    return result;
}
// Query interpretation is attached in capture order; decoding a single record never borrows future metadata.
void attachQueryHistory(const Frame &frame, Json &rows);
Json inspectCommands(const Frame &frame) {
    auto rows = Json::array();
    for (auto &[id, e] : frame.entries())
        if (e.category == 7)
            rows.push_back(inspectCommand(frame, id));
    attachQueryHistory(frame, rows);
    return rows;
}
void exportCommands(const Frame &frame, const std::filesystem::path &directory, const std::string &text,
                    std::optional<Id> resource, bool gpuOnly) {
    auto rows = inspectCommands(frame), selected = Json::array();
    QByteArray csv("\xef\xbb\xbf");
    csv += "id,api,status,wire_type,wire_bytes,referenced_ids\r\n";
    auto quote = [](QString s) {
        s.replace('"', "\"\"");
        return '"' + s + '"';
    };
    for (auto &row : rows)
        if ((!gpuOnly || (row["type"] >= 0x31 && row["type"] <= 0x42)) &&
            commandMatches(row, text, resource)) {
            selected.push_back(row);
            QStringList refs;
            for (auto &r : row["references"])
                refs << QString::number(r["id"].get<Id>());
            csv +=
                (QStringList{QString::number(row["id"].get<Id>()), quote(QString::fromStdString(row["name"])),
                             QString::fromStdString(row["status"]),
                             QString("0x%1").arg(row["type"].get<uint16_t>(), 0, 16),
                             QString::number(row["wire_size"].get<size_t>()), quote(refs.join(' '))}
                     .join(',') +
                 "\r\n")
                    .toUtf8();
        }
    auto path = QString::fromStdWString(directory.wstring());
    if (!QDir().mkpath(path))
        throw std::runtime_error("Cannot create API export directory");
    auto write = [&](const QString &name, const QByteArray &data) {
        QSaveFile file(path + '/' + name);
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
            throw std::runtime_error("Cannot save API export");
    };
    Json report{{"frame", QString::fromStdWString(frame.path().filename().wstring()).toStdString()},
                {"filter", {{"text", text}, {"resource", resource ? Json(*resource) : Json(nullptr)}}},
                {"commands", selected}};
    if (gpuOnly)
        report["filter"]["gpu_commands"] = true;
    write("commands.json", QByteArray::fromStdString(report.dump(2) + "\n"));
    write("commands.csv", csv);
}
} // namespace flora
