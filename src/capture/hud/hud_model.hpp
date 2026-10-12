#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

namespace flora::capture::hud {
enum class Mode { Compact, Detailed, Hidden };
const char* mode_name(Mode mode) noexcept;
Mode parse_mode(std::string_view text);
struct PanelLayout {
    uint32_t x=0,y=0,width=0,height=0,canvas_width=0,canvas_height=0;
    double scale=1;
};
// Client dimensions use the window's DPI coordinate space. Surface dimensions
// may differ when DXGI stretches the backbuffer into the client area.
std::optional<PanelLayout> panel_layout(uint32_t surface_width,uint32_t surface_height,
    uint32_t client_width,uint32_t client_height,uint32_t dpi,Mode mode) noexcept;
struct Hotkey {
    uint32_t key=0;
    bool control=false,shift=false,alt=false;
    bool operator==(const Hotkey&) const = default;
};
Hotkey parse_hotkey(std::string_view text);
std::string hotkey_name(const Hotkey& key);
std::string failure_summary(std::string_view reason);
struct Settings {
    Mode mode=Mode::Compact;
    Hotkey mode_key{0x70,true,false,false};
    Hotkey capture_key{'C',true,true,false};
    void validate() const;
};
// Caller supplies high-bit key state; no global registration or message filtering.
class KeyEdge {
    bool down_=false,focused_=false;
public:
    bool update(const Hotkey& key,const std::array<bool,256>& keys,bool foreground) noexcept;
};
struct KeyActions {bool mode=false,capture=false;};
// Ordered raw make/break state, with independent left/right modifiers.
class RawKeyboard {
    std::array<bool,256> keys_{};
    KeyEdge mode_,capture_;
    void modifiers() noexcept;
public:
    void seed(const std::array<bool,256>& keys,bool foreground,const Settings& settings) noexcept;
    KeyActions event(uint32_t key,bool down,bool foreground,const Settings& settings) noexcept;
};
struct Fps { std::optional<double> current,minimum,maximum; uint64_t frames=0; };
class FrameClock {
    std::optional<uint64_t> start_;
    uint64_t count_=0;
    Fps fps_;
    std::deque<std::pair<uint64_t,double>> history_;
public:
    void present(uint64_t now_ns,bool successful,bool test);
    Fps value(uint64_t now_ns) const;
};
class Message {
    std::string identity_,text_;
    uint64_t expiry_=0;
    bool persistent_=false;
public:
    void update(std::string identity,std::string text,uint64_t now_ms,uint64_t duration_ms);
    std::string value(uint64_t now_ms) const;
};
}
