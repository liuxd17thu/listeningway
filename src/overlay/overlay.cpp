// ---------------------------------------------
// v2 overlay — UX-pass edition.
//
// Layout philosophy:
//   - Each section's header carries the title plus a live hint (e.g.
//     "Spectrum (64 bands)" or "Beat (128 BPM)") and, when the section
//     has settings, a right-aligned "Settings" toggle on the same line.
//     One line of header instead of three; live data where you'd glance.
//   - Visual content (meters, bars, rose) is always shown. Only the
//     tunable knobs hide behind the Settings disclosure.
//   - Engineer-only knobs hide further behind an "Advanced" sub-disclosure
//     so newcomers see only the four or five settings they actually want.
//   - Tooltips lead with what the control DOES; technical name and units
//     live in a "Technical:" footer at the bottom of the tooltip.
// ---------------------------------------------
#define IMGUI_DISABLE_INCLUDE_IMCONFIG_H
#define ImTextureID ImU64
#include <imgui.h>

#include "overlay.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../audio/pipeline/audio_system.h"
#include "../config/store.h"
#include "../output/consumer_registry.h"
#include "../output/i_output_consumer.h"
#include "../output/openrgb_consumer.h"

namespace lw {

// ----- Visual constants --------------------------------------------------
namespace overlay_style {
constexpr float kBarHeightThin       = 6.0f;
constexpr float kSubGroupIndent      = 6.0f;
constexpr float kBarRounding         = 0.0f;
constexpr float kPanCenterThickness  = 1.0f;

constexpr ImU32 kColorBg            = IM_COL32(40, 40, 40, 128);
constexpr ImU32 kColorOutline       = IM_COL32(60, 60, 60, 128);
constexpr ImU32 kColorCenterMarker  = IM_COL32(255, 255, 255, 180);
constexpr ImU32 kColorProfiler      = IM_COL32(120, 200, 255, 200);

constexpr ImU32 kColorIntegrationOn        = IM_COL32(60, 160, 75, 255);
constexpr ImU32 kColorIntegrationOnHover   = IM_COL32(80, 190, 95, 255);
constexpr ImU32 kColorIntegrationOnActive  = IM_COL32(50, 140, 65, 255);
}  // namespace overlay_style

namespace {

float g_label_col = 0.0f;
float g_value_col_w = 0.0f;

constexpr float kLabelGap = 8.0f;  // padding between label's right edge and bar's left edge

void compute_columns() {
    // Probe strings without trailing colons (labels render colon-less now).
    // Picked the widest label currently in use across all sections so the
    // bar-start column accommodates everything without clipping.
    const char* probes[] = {
        "阈值窗口 (ms)",
        "平滑释放 (ms)",
        "节拍位置",
        "亮度",
    };
    float widest = 0.0f;
    for (const char* p : probes) widest = std::max(widest, ImGui::CalcTextSize(p).x);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    g_label_col   = ImGui::GetCursorPosX() + widest + spacing + kLabelGap;
    g_value_col_w = ImGui::CalcTextSize("99.99 \xC2\xB5s").x + spacing;
}

void tip(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", text);
    }
}

// Right-align the label so its right edge sits kLabelGap pixels to the left
// of g_label_col. Then jump to g_label_col so the bar starts there.
// Colons are no longer expected in the label text.
//
// Uses SetCursorPosX rather than SameLine(g_label_col) for the bar-column
// jump because SameLine(N) inside an active BeginGroup adds the group's
// X-offset to N, double-counting and pushing the bar far past the
// intended column. SetCursorPosX is group-offset-agnostic and treats its
// argument as an absolute window-content-relative position.
void label_left(const char* label) {
    ImGui::AlignTextToFramePadding();
    const float label_w = ImGui::CalcTextSize(label).x;
    const float target_x = g_label_col - kLabelGap - label_w;
    if (target_x > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(target_x);
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetCursorPosX(g_label_col);
}

// ---- Row helpers (unchanged semantics) ---------------------------------

void meter_row(const char* label, float value, ImU32 fill,
               const char* value_fmt = "%.2f") {
    using namespace overlay_style;
    label_left(label);
    const float frame_h = ImGui::GetFrameHeight();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float val_w  = g_value_col_w;
    const float bar_w  = std::max(8.0f, ImGui::GetContentRegionAvail().x - val_w - spacing);

    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float y_off = (frame_h - kBarHeightThin) * 0.5f;
    const ImVec2 bar_tl(p.x, p.y + y_off);
    const ImVec2 bar_br(p.x + bar_w, bar_tl.y + kBarHeightThin);
    dl->AddRectFilled(bar_tl, bar_br, kColorBg, kBarRounding);
    if (value > 0.0f) {
        const float v = std::clamp(value, 0.0f, 1.0f);
        dl->AddRectFilled(bar_tl, ImVec2(p.x + v * bar_w, bar_br.y), fill, kBarRounding);
    }
    dl->AddRect(bar_tl, bar_br, kColorOutline, kBarRounding);
    ImGui::Dummy(ImVec2(bar_w, frame_h));
    if (value_fmt && value_fmt[0]) {
        ImGui::SameLine();
        ImGui::Text(value_fmt, value);
    }
}

void center_meter_row(const char* label, float value) {
    using namespace overlay_style;
    label_left(label);
    const float frame_h = ImGui::GetFrameHeight();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float val_w  = g_value_col_w;
    const float bar_w  = std::max(8.0f, ImGui::GetContentRegionAvail().x - val_w - spacing);

    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float y_off = (frame_h - kBarHeightThin) * 0.5f;
    const ImVec2 bg_tl(p.x, p.y + y_off);
    const ImVec2 bg_br(p.x + bar_w, bg_tl.y + kBarHeightThin);
    dl->AddRectFilled(bg_tl, bg_br, kColorBg, kBarRounding);

    const float v = std::clamp(value, -1.0f, 1.0f);
    const float center_x = p.x + bar_w * 0.5f;
    const ImU32 fill = ImGui::GetColorU32(ImGuiCol_PlotHistogram);
    if (v < 0.0f) {
        const float w = -v * bar_w * 0.5f;
        dl->AddRectFilled(ImVec2(center_x - w, bg_tl.y),
                          ImVec2(center_x, bg_br.y), fill, kBarRounding);
    } else if (v > 0.0f) {
        const float w = v * bar_w * 0.5f;
        dl->AddRectFilled(ImVec2(center_x, bg_tl.y),
                          ImVec2(center_x + w, bg_br.y), fill, kBarRounding);
    }
    dl->AddLine(ImVec2(center_x, bg_tl.y), ImVec2(center_x, bg_br.y),
                kColorCenterMarker, kPanCenterThickness);
    dl->AddRect(bg_tl, bg_br, kColorOutline, kBarRounding);
    ImGui::Dummy(ImVec2(bar_w, frame_h));
    ImGui::SameLine();
    ImGui::Text("%+.2f", value);
}

bool slider_row(const char* label, float* v, float lo, float hi,
                const char* fmt = "%.2f", const char* tooltip = nullptr) {
    label_left(label);
    char id[40]; std::snprintf(id, sizeof(id), "##sf_%s", label);
    ImGui::PushItemWidth(-1);
    bool changed = ImGui::SliderFloat(id, v, lo, hi, fmt);
    ImGui::PopItemWidth();
    if (tooltip) tip(tooltip);
    return changed;
}

bool slider_int_row(const char* label, int* v, int lo, int hi,
                    const char* tooltip = nullptr) {
    label_left(label);
    char id[40]; std::snprintf(id, sizeof(id), "##si_%s", label);
    ImGui::PushItemWidth(-1);
    bool changed = ImGui::SliderInt(id, v, lo, hi);
    ImGui::PopItemWidth();
    if (tooltip) tip(tooltip);
    return changed;
}

bool combo_row(const char* label, int* sel,
               const char* const items[], int count,
               const char* tooltip = nullptr) {
    label_left(label);
    char id[40]; std::snprintf(id, sizeof(id), "##co_%s", label);
    ImGui::PushItemWidth(-1);
    bool changed = ImGui::Combo(id, sel, items, count);
    ImGui::PopItemWidth();
    if (tooltip) tip(tooltip);
    return changed;
}

void info_row(const char* label, const char* fmt, ...) {
    label_left(label);
    va_list ap; va_start(ap, fmt);
    ImGui::TextV(fmt, ap);
    va_end(ap);
}

// ---- Section header helpers --------------------------------------------

// RAII: tighter vertical metrics in panels of consecutive meter_row bars
// (Advanced, Performance). Shrinks frame padding and item spacing so rows
// stack ~8 px tighter without changing each bar's own visual style. Scope
// it only around the meter-row groups so sliders and headers keep their
// natural rhythm.
class TightRowSpacing {
public:
    TightRowSpacing() {
        const ImVec2 fp = ImGui::GetStyle().FramePadding;
        const ImVec2 sp = ImGui::GetStyle().ItemSpacing;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(fp.x, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(sp.x, 0.0f));
    }
    ~TightRowSpacing() { ImGui::PopStyleVar(2); }
    TightRowSpacing(const TightRowSpacing&)            = delete;
    TightRowSpacing& operator=(const TightRowSpacing&) = delete;
};

// Right-align cursor for a label of given pixel width.
void cursor_to_right_for(float label_w_with_padding) {
    const float right_x = ImGui::GetWindowContentRegionMax().x - label_w_with_padding;
    ImGui::SameLine();
    if (right_x > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(right_x);
}

// Subdued "settings ·/●" disclosure. Transparent background by default,
// subtle white tint on hover. Closed state shows a small middle dot;
// open state fills it in. The whole widget reads as quiet metadata until
// the user reaches for it. Toggles `open` on click; returns the new state.
//
// Caller is responsible for the right-align cursor positioning and for
// wrapping in PushID/PopID so the SmallButton's ID is unique per section.
bool subtle_settings_toggle(bool& open) {
    // ImGui's default font ships only Latin-1 in its glyph atlas, so
    // geometric-shape codepoints (U+25CF ● etc.) render as "?". The
    // middle dot · (U+00B7) is in Latin-1 and renders fine; we use a
    // plain ASCII '*' for the filled (open) state.
    //   Closed: "settings ·"
    //   Open:   "settings *"
    const char* label = open ? "设置 *" : "设置 \xC2\xB7";

    const float w = ImGui::CalcTextSize(label).x
                  + ImGui::GetStyle().FramePadding.x * 2.0f;
    cursor_to_right_for(w);

    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1, 1, 1, 0.16f));
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    const bool clicked = ImGui::SmallButton(label);
    ImGui::PopStyleColor(4);
    if (clicked) open = !open;
    return open;
}

// Render a section header with optional dim hint and a right-aligned
// Settings disclosure. Returns the disclosure's open/closed state.
// `id` MUST be unique per section (used for ImGui state storage).
bool section_header_with_settings(const char* title, const char* hint, const char* id) {
    ImGui::PushID(id);
    ImGui::Spacing();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(title);
    if (hint && hint[0]) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("(%s)", hint);
    }

    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID state_key = ImGui::GetID("settings_open");
    bool open = storage->GetBool(state_key, false);
    subtle_settings_toggle(open);
    storage->SetBool(state_key, open);

    ImGui::Separator();
    ImGui::PopID();
    return open;
}

// Section header for sections that don't carry per-section settings
// (Performance, Integrations — Integrations has per-row settings instead).
void section_header_only(const char* title, const char* hint) {
    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(title);
    if (hint && hint[0]) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("(%s)", hint);
    }
    ImGui::Separator();
}

// Three-way segmented selector. Renders count buttons in a row at the
// current cursor; the active one is highlighted in the integration-on
// green so the picked option reads at a glance. Returns the new index
// (== current if nothing was clicked this frame).
int segmented_row(const char* label, const char* const* options, int count, int current) {
    using namespace overlay_style;
    label_left(label);
    int new_value = current;
    ImGui::PushID(label);
    for (int i = 0; i < count; ++i) {
        if (i > 0) ImGui::SameLine(0.0f, 1.0f);
        const bool active = (i == current);
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button,        kColorIntegrationOn);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColorIntegrationOnHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kColorIntegrationOnActive);
        }
        if (ImGui::Button(options[i])) new_value = i;
        if (active) ImGui::PopStyleColor(3);
    }
    ImGui::PopID();
    return new_value;
}

// Sub-group label inside a section.
void subgroup_label(const char* label) {
    ImGui::Spacing();
    ImGui::TextDisabled("%s", label);
}


// ---- Integration row (Integrations section) ----------------------------

// Renders one integration: [Name] toggle button, status text, right-aligned
// Settings disclosure. Returns true if per-integration settings should be
// drawn below the row. The toggle button is highlighted when enabled.
bool integration_row(const char* name, bool& enabled, bool& dirty,
                     std::string_view status, const char* id) {
    ImGui::PushID(id);
    ImGui::Indent(overlay_style::kSubGroupIndent);

    char btn_label[40];
    std::snprintf(btn_label, sizeof(btn_label), "%s", name);

    if (enabled) {
        ImGui::PushStyleColor(ImGuiCol_Button,        overlay_style::kColorIntegrationOn);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, overlay_style::kColorIntegrationOnHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  overlay_style::kColorIntegrationOnActive);
    }
    if (ImGui::Button(btn_label)) {
        enabled = !enabled;
        dirty = true;
    }
    if (enabled) ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered()) {
        // ImGui::SetTooltip("Click to %s.", enabled ? "disable" : "enable");
        ImGui::SetTooltip(enabled ? "点击以禁用" : "点击以启用");
    }

    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (status.empty()) {
        ImGui::TextDisabled("关");
    } else {
        // Use std::string for null-terminated TextDisabled().
        const std::string s(status);
        ImGui::TextDisabled("%s", s.c_str());
    }

    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID state_key = ImGui::GetID("settings_open");
    bool open = storage->GetBool(state_key, false);
    subtle_settings_toggle(open);
    storage->SetBool(state_key, open);

    ImGui::Unindent(overlay_style::kSubGroupIndent);
    ImGui::PopID();
    return open;
}

// ---- Common helpers -----------------------------------------------------

const char* state_label(State s) {
    switch (s) {
        case State::Off:      return "关";
        case State::Starting: return "启动";
        case State::Running:  return "运行";
        case State::Stopping: return "停止";
        case State::Error:    return "错误";
    }
    return "?";
}

const char* format_label(int channels) {
    switch (channels) {
        case 0:  return "无";
        case 1:  return "单声道";
        case 2:  return "立体声";
        case 6:  return "5.1";
        case 8:  return "7.1";
        default: return "多重";
    }
}

// Per-band peak hold buffer for the spectrum visualization. Sized for the
// max possible band count; only the first N entries are used at any time.
// Decays toward each frame's live amplitude so the user sees recent peaks
// trailing slightly above the live shape.
constexpr size_t kSpectrumMaxBands = 128;
struct SpectrumPeakState {
    std::array<float, kSpectrumMaxBands> peaks{};
    double                                last_t = 0.0;
};
SpectrumPeakState g_spectrum_peaks;

void update_peak_hold(std::span<const float> values, float amp) {
    const double now = ImGui::GetTime();
    const float dt = static_cast<float>(std::clamp(now - g_spectrum_peaks.last_t, 0.0, 0.25));
    g_spectrum_peaks.last_t = now;
    constexpr float kDecayPerSec = 0.55f;     // ~half decay over ~1.3 s
    const float decay = std::exp(-kDecayPerSec * dt);
    for (size_t i = 0; i < values.size() && i < kSpectrumMaxBands; ++i) {
        const float v = std::clamp(values[i] * amp, 0.0f, 1.0f);
        float& p = g_spectrum_peaks.peaks[i];
        p = std::max(v, p * decay);
    }
}

// Map an anchor frequency to an X position [0, width] using the same band
// scale as the analysis pipeline. A visual approximation: bands are assumed
// to span (min_f, max_f) with the chosen scale's geometry. Linear and Log
// match exactly; Mel uses the standard 2595·log10(1+f/700) curve.
float freq_to_x(float f, config::FrequencyConfig::BandScale scale,
                float min_f, float max_f, float width) {
    using BandScale = config::FrequencyConfig::BandScale;
    if (max_f <= min_f) return 0.0f;
    f = std::clamp(f, min_f, max_f);
    float t = 0.0f;
    switch (scale) {
        case BandScale::Linear:
            t = (f - min_f) / (max_f - min_f);
            break;
        case BandScale::Log: {
            const float lo = std::log(std::max(min_f, 1.0f));
            const float hi = std::log(std::max(max_f, lo + 1.0f));
            t = (std::log(f) - lo) / (hi - lo);
            break;
        }
        case BandScale::Mel: {
            auto to_mel = [](float hz) {
                return 2595.0f * std::log10(1.0f + hz / 700.0f);
            };
            const float lo = to_mel(min_f);
            const float hi = to_mel(max_f);
            t = (to_mel(f) - lo) / (hi - lo);
            break;
        }
    }
    return std::clamp(t, 0.0f, 1.0f) * width;
}

// Per-band hue along the spectrum (warm low → cool high). Same gradient
// for both render orientations so the panel's identity is consistent.
ImU32 band_color(float t, int alpha = 255) {
    t = std::clamp(t, 0.0f, 1.0f);
    const int r = 25 + static_cast<int>(230.0f * (1.0f - t));
    const int g = 25 + static_cast<int>(230.0f * t);
    const int b = 230;
    return IM_COL32(r, g, b, alpha);
}

// ---- Spectrum: horizontal orientation (default) -------------------------
//
// Frequency on X (left = low, right = high), amplitude on Y (bottom = 0,
// top = 1). Each adjacent band pair fills a quad with the top edge linearly
// interpolated between band amplitudes, producing a smooth-looking shape at
// 64+ bands without explicit spline math. Per-band hue gradient gives the
// spectrum its identity. Peak-hold trace is drawn as a thin polyline above
// the live fill.
//
// Frequency-axis anchor labels (30 / 200 / 1k / 5k / 20k Hz) sit at the top
// of the pane. A faint solid vertical guide line drops from each label down
// to the baseline. The guide draws BEFORE the spectrum fill so the live
// shape covers it where loud and it shows through where quiet.
void render_spectrum_horizontal(std::span<const float> values, float amp,
                                  ImVec2 size,
                                  config::FrequencyConfig::BandScale scale,
                                  float min_f, float max_f) {
    using namespace overlay_style;
    if (values.empty() || size.x <= 0.0f || size.y <= 0.0f) return;

    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 br(origin.x + size.x, origin.y + size.y);

    // Pane background.
    dl->AddRectFilled(origin, br, kColorBg, kBarRounding);

    update_peak_hold(values, amp);

    const size_t n = values.size();
    if (n < 2) {
        dl->AddRect(origin, br, kColorOutline, kBarRounding);
        ImGui::Dummy(size);
        return;
    }

    // Reserve a small strip at the top for the frequency-axis labels so the
    // shape doesn't draw over them. Labels live in [origin.y, label_band).
    const float label_h    = ImGui::GetTextLineHeight();
    const float label_band = origin.y + label_h + 2.0f;

    // Frequency-axis anchors. Geometry is needed before the fill so the
    // guide lines can be drawn underneath.
    static const float kAnchors[] = { 30.0f, 200.0f, 1000.0f, 5000.0f, 20000.0f };
    static const char* const kAnchorLabels[] = { "30", "200", "1k", "5k", "20k" };
    const ImU32 guide_col = IM_COL32(70, 70, 70, 255);
    const ImU32 label_col = IM_COL32(255, 255, 255, 110);

    // Pass 1: vertical guide lines from the label band down to the baseline.
    // Drawn before the live fill so the spectrum covers them when loud.
    for (size_t i = 0; i < std::size(kAnchors); ++i) {
        const float f = kAnchors[i];
        if (f < min_f || f > max_f) continue;
        const float x = origin.x + freq_to_x(f, scale, min_f, max_f, size.x);
        dl->AddLine(ImVec2(x, label_band), ImVec2(x, br.y), guide_col, 1.0f);
    }

    // Per-band X centers spread across the full width.
    auto band_x = [&](size_t i) {
        const float t = static_cast<float>(i) / static_cast<float>(n - 1);
        return origin.x + t * size.x;
    };
    const float baseline = br.y;
    // Reserve only the label-band strip for axis labels; the spectrum can
    // still reach the very top of the remaining area at peak amplitude.
    const float draw_h = std::max(0.0f, baseline - label_band);
    auto band_y = [&](float v) {
        return baseline - std::clamp(v, 0.0f, 1.0f) * draw_h;
    };

    // Live fill: one convex quad per inter-band segment. Linear top edge
    // between adjacent band amplitudes; the eye reads the assembled shape
    // as smooth at 64+ bands.
    for (size_t i = 0; i + 1 < n; ++i) {
        const float v0 = std::clamp(values[i]     * amp, 0.0f, 1.0f);
        const float v1 = std::clamp(values[i + 1] * amp, 0.0f, 1.0f);
        const float t  = (static_cast<float>(i) + 0.5f) / static_cast<float>(n - 1);
        const ImU32 col = band_color(t);
        const ImVec2 quad[4] = {
            ImVec2(band_x(i),     baseline),
            ImVec2(band_x(i + 1), baseline),
            ImVec2(band_x(i + 1), band_y(v1)),
            ImVec2(band_x(i),     band_y(v0)),
        };
        dl->AddConvexPolyFilled(quad, 4, col);
    }

    // Peak-hold outline: a thin polyline traced through the per-band peaks.
    {
        std::array<ImVec2, kSpectrumMaxBands> pts{};
        const size_t k = std::min(n, kSpectrumMaxBands);
        for (size_t i = 0; i < k; ++i) {
            pts[i] = ImVec2(band_x(i), band_y(g_spectrum_peaks.peaks[i]));
        }
        dl->AddPolyline(pts.data(), static_cast<int>(k),
                        IM_COL32(255, 255, 255, 110), 0, 1.0f);
    }

    // Pass 2: frequency-axis labels at the top. Drawn after the fill so they
    // sit over anything that might bleed into their reserved strip.
    for (size_t i = 0; i < std::size(kAnchors); ++i) {
        const float f = kAnchors[i];
        if (f < min_f || f > max_f) continue;
        const float x = origin.x + freq_to_x(f, scale, min_f, max_f, size.x);
        const ImVec2 ts = ImGui::CalcTextSize(kAnchorLabels[i]);
        const float lx = std::clamp(x - ts.x * 0.5f, origin.x + 1.0f, br.x - ts.x - 1.0f);
        dl->AddText(ImVec2(lx, origin.y + 1.0f), label_col, kAnchorLabels[i]);
    }

    dl->AddRect(origin, br, kColorOutline, kBarRounding);
    ImGui::Dummy(size);
}

// ---- Spectrum: vertical orientation (alternate) -------------------------
//
// Bands stacked top-to-bottom, each filling the full pane width based on
// amplitude. Per-band height = pane_height / N (with leftover pixels
// distributed across the first few rows so the total fills the pane). Same
// per-band hue gradient as the horizontal mode.
void render_spectrum_vertical(std::span<const float> values, float amp,
                               ImVec2 size) {
    using namespace overlay_style;
    if (values.empty() || size.x <= 0.0f || size.y <= 0.0f) return;

    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 br(origin.x + size.x, origin.y + size.y);

    dl->AddRectFilled(origin, br, kColorBg, kBarRounding);

    const size_t n = values.size();
    const int total_h = static_cast<int>(size.y);
    const int base_row = total_h / static_cast<int>(std::max<size_t>(1, n));
    const int leftover = total_h - base_row * static_cast<int>(n);

    float y = origin.y;
    for (size_t i = 0; i < n; ++i) {
        const float v = std::clamp(values[i] * amp, 0.0f, 1.0f);
        const float t = static_cast<float>(i) / static_cast<float>(std::max<size_t>(1, n - 1));
        const int row_h = base_row + (static_cast<int>(i) < leftover ? 1 : 0);
        const float y_next = y + static_cast<float>(row_h);
        if (v > 0.0f) {
            dl->AddRectFilled(ImVec2(origin.x, y),
                              ImVec2(origin.x + v * size.x, y_next),
                              band_color(t), 0.0f);
        }
        y = y_next;
    }

    dl->AddRect(origin, br, kColorOutline, kBarRounding);
    ImGui::Dummy(size);
}


// Build the live hint string for each section.
const char* current_source_label(AudioSystem& system, const config::Settings& cfg) {
    const auto sources = system.available_sources();
    for (const auto& s : sources) {
        if (s.code == cfg.audio.capture_source_code) return s.display.c_str();
    }
    return cfg.audio.capture_source_code.c_str();
}

}  // namespace

// ============================ Sections ===================================

// ---- Audio Source -------------------------------------------------------
//
// Single row: title on the left, the Source dropdown filling the rest of
// the line. The dropdown's selected text already names the active source,
// so no separate hint or "Source:" body row is needed.

static void section_audio_source(AudioSystem& system, config::Settings& cfg, bool&) {
    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("音频源");
    ImGui::SameLine();

    const auto sources = system.available_sources();
    int current = 0;
    std::vector<std::string> names;
    for (size_t i = 0; i < sources.size(); ++i) {
        names.push_back(sources[i].display);
        if (sources[i].code == cfg.audio.capture_source_code) current = (int)i;
    }
    std::vector<const char*> name_ptrs;
    for (auto& n : names) name_ptrs.push_back(n.c_str());

    int sel = current;
    ImGui::PushItemWidth(-1);
    if (ImGui::Combo("##source_combo", &sel, name_ptrs.data(), (int)name_ptrs.size())
        && sel >= 0 && sel < (int)sources.size()) {
        system.switch_source(sources[sel].code);
    }
    ImGui::PopItemWidth();
    tip("聆听威要听哪里？\n"
        "  - 系统音频：扬声器播放的任何内容\n"
        "  - 游戏音频：只是游戏本身（仅限Windows 10 22H2以上）\n"
        "  - 无：关掉音频分析");

    ImGui::Separator();
}

// ---- Levels -------------------------------------------------------------

static void section_levels(const AudioSnapshot& snap, config::Settings& cfg, bool& dirty) {
    using namespace overlay_style;
    const char* fmt_label = format_label(static_cast<int>(snap.audio_format));
    const bool show = section_header_with_settings("电平", fmt_label, "levels");

    const float vol_amp = cfg.frequency.amplifier_volume;
    const ImU32 fill = ImGui::GetColorU32(ImGuiCol_PlotHistogram);

    {
        TightRowSpacing tight;
        meter_row("音量", std::clamp(snap.volume * vol_amp, 0.0f, 1.0f), fill);
    }

    subgroup_label("立体声:");
    ImGui::Indent(kSubGroupIndent);
    {
        TightRowSpacing tight;
        meter_row("左",  std::clamp(snap.volume_left  * vol_amp, 0.0f, 1.0f), fill);
        meter_row("右", std::clamp(snap.volume_right * vol_amp, 0.0f, 1.0f), fill);
        center_meter_row("声像", snap.audio_pan);
    }
    ImGui::Unindent(kSubGroupIndent);

    if (show) {
        ImGui::Indent(kSubGroupIndent);
        if (slider_row("音量提升", &cfg.frequency.amplifier_volume, 1.0f, 11.0f, "%.2f"))
            dirty = true;
        tip("放大读出的音量值和相应的 listeningway_volume参数，不影响节拍检测和分析。\n实现: frequency.amplifier_volume, [1, 11]");
        if (slider_row("声像平滑", &cfg.audio.pan_smoothing, 0.0f, 1.0f, "%.2f"))
            dirty = true;
        tip("平滑声像抖动。0 = 立刻响应，1 = 很慢的响应。\n实现: audio.pan_smoothing, [0, 1]");
        if (slider_row("声像偏移", &cfg.audio.pan_offset, -1.0f, 1.0f, "%.2f"))
            dirty = true;
        tip("移动感知立体声中心，适用于环境/耳机不平衡的情况。\n实现: audio.pan_offset, [-1, +1]");
        ImGui::Unindent(kSubGroupIndent);
    }
}

// ---- Beat Detection -----------------------------------------------------
//
// Three-mode UX following the convergent pattern of pro audio tools (Logic
// Smart Tempo, iZotope Master Assistant, LANDR, Ableton Warp): Auto adapts
// itself, Profile picks a named signal-character preset, Custom exposes
// the slider. Live Pulse meter and Tempo readout sit at the top in all
// modes so the user can see the system reacting; status badge below the
// Mode selector tells them whether Auto is still adapting or has locked.

static void section_beat(const AudioSnapshot& snap, config::Settings& cfg, bool& dirty) {
    using namespace overlay_style;
    using BMode = config::BeatConfig::Mode;
    using BProf = config::BeatConfig::Profile;

    char hint[32];
    if (snap.tempo_detected) {
        std::snprintf(hint, sizeof(hint), "%.0f BPM", snap.tempo_bpm);
    } else {
        std::snprintf(hint, sizeof(hint), "搜索中...");
    }
    const bool show = section_header_with_settings("节拍检测", hint, "beat");

    const ImU32 fill = ImGui::GetColorU32(ImGuiCol_PlotHistogram);

    {
        TightRowSpacing tight;
        meter_row("脉冲", std::clamp(snap.beat, 0.0f, 1.0f), fill);
        if (snap.tempo_detected) {
            info_row("节拍", "%.1f BPM (%.0f%% 置信度)",
                     snap.tempo_bpm, snap.tempo_confidence * 100.0f);
        } else {
            label_left("节拍");
            ImGui::TextDisabled("搜索中... (%.0f%% 置信度)",
                                snap.tempo_confidence * 100.0f);
        }
    }

    if (show) {
        ImGui::Indent(kSubGroupIndent);

        static const char* const kModeOptions[]    = { "自动", "预设", "自定" };
        static const char* const kProfileOptions[] = { "敲击", "旋律", "持续" };

        const int prev_mode_idx = static_cast<int>(cfg.beat.mode);
        const int new_mode_idx = segmented_row("模式", kModeOptions, 3, prev_mode_idx);
        if (new_mode_idx != prev_mode_idx) {
            // Mode transition. If the user is heading into Custom, seed the
            // slider from whatever the system was using a moment ago — Auto's
            // converged value, or the profile's preset — so the slider lands
            // where the audio is, not at the default.
            if (static_cast<BMode>(new_mode_idx) == BMode::Custom) {
                cfg.beat.pulse_strength =
                    std::clamp(snap.beat_pulse_strength, 0.0f, 3.0f);
            }
            cfg.beat.mode = static_cast<BMode>(new_mode_idx);
            dirty = true;
        }
        tip("节拍检测敏感方法：\n"
            "  • 自动 — 侦测音频并自动调节，需要数秒时间\n"
            "  • 预设 — 手动选择音频类型预设\n"
            "  • 自定 — 完全手动调整各个脉冲滑块");

        if (cfg.beat.mode == BMode::Auto) {
            label_left("状态");
            if (snap.beat_auto_locked) {
                ImGui::TextDisabled("锁定 (强度 %.2f)", snap.beat_pulse_strength);
            } else {
                ImGui::TextDisabled("适应... (强度 %.2f)",
                                    snap.beat_pulse_strength);
            }
        } else if (cfg.beat.mode == BMode::Profile) {
            const int prev_prof_idx = static_cast<int>(cfg.beat.profile);
            const int new_prof_idx = segmented_row("特征", kProfileOptions, 3,
                                                   prev_prof_idx);
            if (new_prof_idx != prev_prof_idx) {
                cfg.beat.profile = static_cast<BProf>(new_prof_idx);
                dirty = true;
            }
            tip("预置的检测敏感类型，频带权重与衰减时间特征已调好。\n"
                "  • 敲击 — 鼓点、电子舞曲、hip-hop。检测低频，短脉冲。\n"
                "  • 旋律 — 人声、摇滚、爵士、古典。均衡频带。\n"
                "  • 持续 — 环境、电影、空旷。跨频带检测，衰减更长。\n"
                "以上描述的是音频信号类型，不完全对应音乐流派。");
        } else {  // Custom
            if (slider_row("脉冲强度", &cfg.beat.pulse_strength, 0.0f, 3.0f, "%.2f"))
                dirty = true;
            tip("脉冲量表以及listeningway_beat参数的响应强度。\n"
                "  • 0.0 — 关，不触发\n"
                "  • 1.0 — 平衡的默认值\n"
                "  • 2-3 — 响应更活跃，适用于空旷安静内容");
        }

        ImGui::Unindent(kSubGroupIndent);
    }
}

// ---- Spectrum (was: Frequency Bands) ------------------------------------
//
// The spectrum panel has two render orientations (toggled by the inline
// "~" button in the section header):
//   - Horizontal (default): frequency on X, amplitude on Y, smooth filled
//     shape with peak-hold trace and frequency-axis anchors.
//   - Vertical (alternate): bands stacked top-to-bottom, each filling the
//     full pane width based on amplitude.
// Both render into a fixed 300 px tall pane with variable width, so the
// panel's footprint stays stable across band-count changes.

constexpr float kSpectrumPaneHeight = 300.0f;

static void section_spectrum(const AudioSnapshot& snap, config::Settings& cfg, bool& dirty) {
    using namespace overlay_style;
    using Orient = config::UiConfig::SpectrumOrientation;

    const uint32_t n = std::min<uint32_t>(snap.freq_band_count,
                                           static_cast<uint32_t>(kSpectrumMaxBands));
    char hint[24];
    std::snprintf(hint, sizeof(hint), "%u 频带", n);

    // Custom section header: title + hint + ~ toggle + subtle settings
    // disclosure, all on one line.
    ImGui::PushID("spectrum");
    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("频谱");
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("(%s)", hint);

    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID state_key = ImGui::GetID("settings_open");
    bool show = storage->GetBool(state_key, false);

    // Compute right-aligned position for [~] [settings] cluster.
    const char* tilde = "~";
    const char* settings_label = show ? "设置 *" : "设置 \xC2\xB7";
    const float pad      = ImGui::GetStyle().FramePadding.x * 2.0f;
    const float spacing  = ImGui::GetStyle().ItemSpacing.x;
    const float tilde_w    = ImGui::CalcTextSize(tilde).x          + pad;
    const float settings_w = ImGui::CalcTextSize(settings_label).x + pad;
    cursor_to_right_for(tilde_w + spacing + settings_w);

    // ~ orientation toggle.
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.12f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1, 1, 1, 0.20f));
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    if (ImGui::SmallButton(tilde)) {
        cfg.ui.spectrum_orientation =
            (cfg.ui.spectrum_orientation == Orient::Horizontal)
                ? Orient::Vertical : Orient::Horizontal;
        dirty = true;
    }
    ImGui::PopStyleColor(4);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", cfg.ui.spectrum_orientation == Orient::Horizontal
            ? "切换到垂直频带堆叠"
            : "切换到水平频谱");
    }

    // Subtle settings disclosure (inline; mirrors subtle_settings_toggle).
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1, 1, 1, 0.16f));
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    if (ImGui::SmallButton(settings_label)) show = !show;
    ImGui::PopStyleColor(4);
    storage->SetBool(state_key, show);

    ImGui::Separator();
    ImGui::PopID();

    const float bands_amp = cfg.frequency.amplifier_bands;
    const float pane_w = std::max(80.0f, ImGui::GetContentRegionAvail().x);
    const std::span<const float> values(snap.freq_bands.data(), n);

    if (cfg.ui.spectrum_orientation == Orient::Horizontal) {
        render_spectrum_horizontal(values, bands_amp,
                                    ImVec2(pane_w, kSpectrumPaneHeight),
                                    cfg.frequency.band_scale,
                                    cfg.frequency.min_freq,
                                    cfg.frequency.max_freq);
    } else {
        render_spectrum_vertical(values, bands_amp,
                                  ImVec2(pane_w, kSpectrumPaneHeight));
    }

    if (show) {
        ImGui::Indent(kSubGroupIndent);
        if (slider_row("频带增强", &cfg.frequency.amplifier_bands, 1.0f, 11.0f, "%.2f"))
            dirty = true;
        tip("放大读出的频谱值和相应的listeningway_freqbands参数。\n实现: frequency.amplifier_bands, [1, 11]");
        if (slider_row("低频细节", &cfg.frequency.log_strength, 0.01f, 1.5f, "%.2f"))
            dirty = true;
        tip("数值越高，低频带的可见细节越多，反之频谱越平。\n实现: frequency.log_strength, [0.01, 1.5]");

        subgroup_label("均衡器 (5频带):");
        ImGui::Indent(kSubGroupIndent);
        const char* const eq_names[5] = {"低", "中低", "中", "中高", "高"};
        for (int i = 0; i < 5; ++i) {
            if (slider_row(eq_names[i], &cfg.frequency.equalizer_bands[i], 0.0f, 4.0f, "%.2f"))
                dirty = true;
        }
        if (slider_row("均衡器宽度", &cfg.frequency.equalizer_width, 0.05f, 0.5f, "%.2f"))
            dirty = true;
        tip("每个EQ滑块影响的频宽，单位为标准差。\n实现: frequency.equalizer_width");
        ImGui::Unindent(kSubGroupIndent);

        subgroup_label("高级:");
        ImGui::Indent(kSubGroupIndent);
        const char* const scales[] = {"线性", "对数", "梅尔 (Slaney)"};
        int scale = static_cast<int>(cfg.frequency.band_scale);
        if (combo_row("频带比例", &scale, scales, 3)) {
            cfg.frequency.band_scale =
                static_cast<config::FrequencyConfig::BandScale>(scale);
            dirty = true;
        }
        tip("频率映射到频带的方法。\n  • 梅尔：对应人耳感知。\n  • 对数：聆听威V1默认选项。\n  • 线性：旧模式。\n实现: frequency.band_scale");

        if (slider_int_row("频带数", &cfg.frequency.band_count, 8, 128))
            dirty = true;
        tip("提供的频带数量。必须对应着色器的数组大小。\n实现: frequency.band_count, [8, 128]");
        if (slider_int_row("分析结果 (FFT)", &cfg.frequency.fft_size, 256, 8192))
            dirty = true;
        tip("FFT窗宽度。数值越高，频率细节越多，CPU负载越大。推荐设为2的幂次。\n实现: frequency.fft_size");
        if (slider_row("低频截止 (Hz)", &cfg.frequency.min_freq, 10.0f, 500.0f, "%.0f"))
            dirty = true;
        tip("低频边界。\n实现: frequency.min_freq, Hz");
        if (slider_row("高频截止 (Hz)", &cfg.frequency.max_freq, 2000.0f, 22050.0f, "%.0f"))
            dirty = true;
        tip("高频边界。\n实现: frequency.max_freq, Hz");
        if (slider_row("幅度缩放", &cfg.frequency.band_norm, 0.001f, 1.0f, "%.3f"))
            dirty = true;
        tip("原始FFT幅度到频带幅度的缩放因子。\n实现: frequency.band_norm");
        ImGui::Unindent(kSubGroupIndent);
        ImGui::Unindent(kSubGroupIndent);
    }
}

// ---- Spatial (was: Directional Intensity) -------------------------------

static void section_spatial(const AudioSnapshot& snap, config::Settings& cfg, bool& dirty) {
    using namespace overlay_style;
    const char* fmt_label = format_label(static_cast<int>(snap.audio_format));
    const bool show = section_header_with_settings("空间", fmt_label, "spatial");

    const float dir_amp = cfg.frequency.amplifier_direction;
    const char* const labels[8] = { "F", "FR", "R", "BR", "B", "BL", "L", "FL" };
    const ImU32 fill = ImGui::GetColorU32(ImGuiCol_PlotHistogram);

    // Save the section's top so the bars can rewind to it after the rose
    // claims its vertical space. The rose sits in the left column (X = 0
    // to ~120 px), the bar labels right-align at the global g_label_col
    // (~155 px), and the bars themselves run full-width like every other
    // meter section. No BeginGroup needed.
    constexpr float kRoseSide = 120.0f;
    constexpr float kLabelMargin = 9.0f;     // extra padding outside the rose for cardinal labels
    const float side = kRoseSide;
    const float section_top_y = ImGui::GetCursorPosY();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 center(origin.x + side * 0.5f, origin.y + side * 0.5f);
    const float radius = side * 0.5f - kLabelMargin;
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(center, radius, IM_COL32(30, 30, 30, 128), 64);
    dl->AddCircle(center, radius, kColorOutline, 64, 1.0f);

    // F = North. Rotate so index 0 (Front) sits at the top of the rose.
    constexpr float two_pi = 2.0f * std::numbers::pi_v<float>;
    constexpr float kRoseAngleOffset = -std::numbers::pi_v<float> / 2.0f;
    for (int i = 0; i < 8; ++i) {
        const float v = std::clamp(snap.direction8[i] * dir_amp, 0.0f, 1.0f);
        const float a0 = kRoseAngleOffset + (two_pi / 8.0f) * (i - 0.5f);
        const float a1 = kRoseAngleOffset + (two_pi / 8.0f) * (i + 0.5f);
        const float r  = radius * v;
        const ImVec2 p0(center.x + r * cosf(a0), center.y + r * sinf(a0));
        const ImVec2 p1(center.x + r * cosf(a1), center.y + r * sinf(a1));
        dl->AddTriangleFilled(center, p0, p1, fill);
    }

    // Cardinal labels positioned inside the rose, centered along each slice's
    // direction at a radius about 12 px short of the perimeter. Faint white
    // so the labels read as quiet annotation, not chrome.
    const ImU32 cardinal_col = IM_COL32(255, 255, 255, 130);
    const float label_radius = std::max(0.0f, radius - 12.0f);
    for (int i = 0; i < 8; ++i) {
        const float a = kRoseAngleOffset + (two_pi / 8.0f) * static_cast<float>(i);
        const ImVec2 anchor(center.x + label_radius * cosf(a),
                             center.y + label_radius * sinf(a));
        const ImVec2 ts = ImGui::CalcTextSize(labels[i]);
        const float lx = anchor.x - ts.x * 0.5f;
        const float ly = anchor.y - ts.y * 0.5f;
        dl->AddText(ImVec2(lx, ly), cardinal_col, labels[i]);
    }

    // Claim the rose's vertical/horizontal extent for layout purposes.
    ImGui::Dummy(ImVec2(side, side));
    const float after_rose_y = ImGui::GetCursorPosY();

    // Rewind to the section top and draw the bars using the standard
    // meter_row layout (full-width, labels right-aligned at g_label_col).
    // Because g_label_col sits well past the rose's right edge, the bars
    // and the rose visually sit side-by-side without any group / SameLine
    // gymnastics.
    ImGui::SetCursorPosY(section_top_y);
    {
        TightRowSpacing tight;
        for (int i = 0; i < 8; ++i) {
            meter_row(labels[i],
                      std::clamp(snap.direction8[i] * dir_amp, 0.0f, 1.0f),
                      fill, "%.2f");
        }
    }

    // The rose is taller than 8 tight rows, so push the cursor down past
    // it before the next sub-section draws.
    if (after_rose_y > ImGui::GetCursorPosY()) {
        ImGui::SetCursorPosY(after_rose_y);
    }

    if (show) {
        ImGui::Indent(kSubGroupIndent);
        if (slider_row("方向增强", &cfg.frequency.amplifier_direction,
                        1.0f, 11.0f, "%.2f"))
            dirty = true;
        tip("放大读出的方向参数（listeningway_front、_front_right等）\n实现: frequency.amplifier_direction, [1, 11]");
        if (slider_row("扩散", &cfg.frequency.spatial_spread, 0.0f, 0.5f, "%.2f"))
            dirty = true;
        tip("玫瑰图每个方向的能量扩散到相邻方向的强度。0 = 每个方向出现锐利峰值；0.5 = 平滑。\n实现: frequency.spatial_spread, [0, 0.5]");
        if (slider_row("平滑", &cfg.frequency.spatial_smoothing, 0.0f, 0.95f, "%.2f"))
            dirty = true;
        tip("玫瑰图的时间平滑。0 = 原样逐帧显示；数值越高玫瑰越平滑。\n实现: frequency.spatial_smoothing, [0, 0.95]");
        ImGui::Unindent(kSubGroupIndent);
    }
}

// ---- Advanced metrics (auto-leveled, phases, perceptual) ---------------

static void section_advanced(const AudioSnapshot& snap, config::Settings& cfg, bool& dirty) {
    using namespace overlay_style;
    const bool show = section_header_with_settings("高级", nullptr, "advanced");

    const ImU32 fill = ImGui::GetColorU32(ImGuiCol_PlotHistogram);

    subgroup_label("自动电平 (1.0 = 近期平均响度):");
    ImGui::Indent(kSubGroupIndent);
    const float scale = 1.0f / std::max(0.1f, cfg.agc.clamp_max);
    auto leveled_row = [&](const char* lbl, float v) {
        meter_row(lbl, std::clamp(v * scale, 0.0f, 1.0f), fill, nullptr);
        ImGui::SameLine();
        ImGui::Text("%.2f", v);
    };
    {
        TightRowSpacing tight;
        leveled_row("音量", snap.volume_norm);
        leveled_row("低频",   snap.bass_norm);
        leveled_row("中频",    snap.mid_norm);
        leveled_row("高频", snap.treb_norm);
    }
    ImGui::Unindent(kSubGroupIndent);

    subgroup_label("能量相位:");
    ImGui::Indent(kSubGroupIndent);
    {
        TightRowSpacing tight;
        meter_row("音量", snap.phase_volume, fill);
        meter_row("低频",   snap.phase_bass,   fill);
        meter_row("高频", snap.phase_treble, fill);
    }
    ImGui::Unindent(kSubGroupIndent);

    subgroup_label("感知:");
    ImGui::Indent(kSubGroupIndent);
    {
        TightRowSpacing tight;
        meter_row("亮度", snap.spectral_centroid, fill, "%.3f");
        meter_row("响度",   std::clamp(snap.loudness, 0.0f, 1.0f), fill, "%.2f");
    }
    ImGui::Unindent(kSubGroupIndent);

    if (show) {
        ImGui::Indent(kSubGroupIndent);

        subgroup_label("能量相位 (分频带比率):");
        ImGui::Indent(kSubGroupIndent);
        if (slider_row("音量比率", &cfg.chronotensity.gain_volume, 0.0f, 5.0f, "%.2f"))
            dirty = true;
        tip("音量驱动的相位超前于自动电平音量的程度。\n实现: chronotensity.gain_volume");
        if (slider_row("低频比率", &cfg.chronotensity.gain_bass, 0.0f, 5.0f, "%.2f"))
            dirty = true;
        tip("低频相位比率。\n实现: chronotensity.gain_bass");
        if (slider_row("高频比率", &cfg.chronotensity.gain_treble, 0.0f, 5.0f, "%.2f"))
            dirty = true;
        tip("高频相位比率。\n实现: chronotensity.gain_treble");
        ImGui::Unindent(kSubGroupIndent);

        subgroup_label("自动电平 (AGC):");
        ImGui::Indent(kSubGroupIndent);
        if (slider_row("滑窗 (s)", &cfg.agc.window_seconds, 0.5f, 30.0f, "%.1f"))
            dirty = true;
        tip("自动电平的均值滑窗。\n实现: agc.window_seconds");
        if (slider_row("限位", &cfg.agc.clamp_max, 1.5f, 8.0f, "%.1f"))
            dirty = true;
        tip("自动电平上界。\n实现: agc.clamp_max");
        if (slider_row("平滑突发 (ms)", &cfg.agc.att_attack_ms, 1.0f, 1000.0f, "%.0f"))
            dirty = true;
        tip("平滑电平的上升速度。\n实现: agc.att_attack_ms");
        if (slider_row("平滑释放 (ms)", &cfg.agc.att_release_ms, 1.0f, 5000.0f, "%.0f"))
            dirty = true;
        tip("平滑电平的释放速度。\n实现: agc.att_release_ms");
        ImGui::Unindent(kSubGroupIndent);

        subgroup_label("响度:");
        ImGui::Indent(kSubGroupIndent);
        if (slider_row("滑窗 (ms)", &cfg.loudness.window_ms, 50.0f, 3000.0f, "%.0f"))
            dirty = true;
        tip("加权方均根滑窗。400 ms = ITU BS.1770 短时响度。\n实现: loudness.window_ms");
        ImGui::Unindent(kSubGroupIndent);
        ImGui::Unindent(kSubGroupIndent);
    }
}

// ---- Performance (was: DSP Profiler) -----------------------------------

static void section_performance(const AudioSnapshot& snap) {
    using namespace overlay_style;

    char hint[32];
    std::snprintf(hint, sizeof(hint), "总 %.1f us", snap.pipeline_micros);
    section_header_only("性能", hint);

    if (snap.stage_count == 0) {
        ImGui::TextDisabled("(暂无时间)");
        return;
    }
    float max_micros = 1.0f;
    for (uint32_t i = 0; i < snap.stage_count; ++i) {
        max_micros = std::max(max_micros, snap.stage_timings[i].micros);
    }
    TightRowSpacing tight;
    for (uint32_t i = 0; i < snap.stage_count; ++i) {
        const auto& t = snap.stage_timings[i];
        label_left(t.name);
        const float frame_h = ImGui::GetFrameHeight();
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float val_w = g_value_col_w;
        const float bar_w = std::max(8.0f, ImGui::GetContentRegionAvail().x - val_w - spacing);
        auto* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float y_off = (frame_h - kBarHeightThin) * 0.5f;
        const ImVec2 bg_tl(p.x, p.y + y_off);
        const ImVec2 bg_br(p.x + bar_w, bg_tl.y + kBarHeightThin);
        dl->AddRectFilled(bg_tl, bg_br, kColorBg, kBarRounding);
        const float v = std::clamp(t.micros / max_micros, 0.0f, 1.0f);
        if (v > 0.0f) {
            dl->AddRectFilled(bg_tl, ImVec2(p.x + v * bar_w, bg_br.y),
                              kColorProfiler, kBarRounding);
        }
        dl->AddRect(bg_tl, bg_br, kColorOutline, kBarRounding);
        ImGui::Dummy(ImVec2(bar_w, frame_h));
        ImGui::SameLine();
        ImGui::Text("%.1f \xC2\xB5s", t.micros);
    }
}

// ---- Integrations (was: Network Outputs) --------------------------------

namespace {

bool host_port_rows(const char* prefix, std::string& host, int& port,
                     int port_lo, int port_hi) {
    bool changed = false;

    label_left("主机");
    char host_buf[64] = {};
    std::snprintf(host_buf, sizeof(host_buf), "%s", host.c_str());
    char id[40]; std::snprintf(id, sizeof(id), "##host_%s", prefix);
    ImGui::PushItemWidth(-1);
    if (ImGui::InputText(id, host_buf, sizeof(host_buf))) {
        host = host_buf;
        changed = true;
    }
    ImGui::PopItemWidth();
    tip("目标地址。127.0.0.1 = 本机。设置为其他值则通过网络发送数据。");

    label_left("端口");
    char id2[40]; std::snprintf(id2, sizeof(id2), "##port_%s", prefix);
    ImGui::PushItemWidth(-1);
    if (ImGui::InputInt(id2, &port)) {
        port = std::clamp(port, port_lo, port_hi);
        changed = true;
    }
    ImGui::PopItemWidth();

    return changed;
}

void section_integrations(config::Settings& cfg, bool& dirty,
                            output::ConsumerRegistry& registry) {
    using namespace overlay_style;

    const int active_count =
        (cfg.network.osc.enabled ? 1 : 0) +
        (cfg.network.openrgb.enabled ? 1 : 0);
    char hint[24];
    if (active_count == 0) std::snprintf(hint, sizeof(hint), "无活跃");
    else                   std::snprintf(hint, sizeof(hint), "%d 活跃", active_count);
    section_header_only("集成", hint);

    // ---- OSC ------------------------------------------------------------
    {
        std::string status;
        if (auto* c = registry.find_by_id("osc")) status = c->status_line();

        if (integration_row("振荡器[OSC]", cfg.network.osc.enabled, dirty,
                             status, "osc")) {
            ImGui::Indent(kSubGroupIndent * 2.0f);
            ImGui::TextDisabled("发送到创意工具：\n"
                "TouchDesigner, Resolume, Max/MSP, vvvv, MadMapper, VRChat OSC.\n"
                "只发送 — 本机不开放端口，不会触发反作弊。");
            ImGui::Spacing();
            if (host_port_rows("osc", cfg.network.osc.host, cfg.network.osc.port, 1, 65535))
                dirty = true;
            if (slider_int_row("回报率 (Hz)", &cfg.network.osc.rate_hz, 1, 120)) {
                cfg.network.osc.rate_hz = std::clamp(cfg.network.osc.rate_hz, 1, 120);
                dirty = true;
            }
            tip("每秒发送OSC消息的次数。默认为60。\n实现: network.osc.rate_hz, [1, 120]");
            label_left("测试");
            if (ImGui::Button("发送测试数据包##osc", ImVec2(-1, 0))) {
                if (auto* c = registry.find_by_id("osc")) c->send_test_packet();
            }
            tip("发送单个聆听威测试包。使用samples/integration_harness.py验证接收端。");
            ImGui::Unindent(kSubGroupIndent * 2.0f);
        }
    }

    // ---- OpenRGB --------------------------------------------------------
    {
        // Pull live counts from the consumer (cast through IOutputConsumer*
        // is safe — we put it in the registry ourselves with this concrete
        // type). Counts are 0 until the worker connects and enumerates;
        // null when the consumer isn't registered yet.
        auto* orgb = static_cast<output::OpenRgbConsumer*>(registry.find_by_id("openrgb"));
        const int n_single = orgb ? orgb->count_single() : 0;
        const int n_linear = orgb ? orgb->count_linear() : 0;
        const int n_matrix = orgb ? orgb->count_matrix() : 0;

        std::string status;
        if (orgb) status = orgb->status_line();

        if (integration_row("OpenRGB", cfg.network.openrgb.enabled, dirty,
                             status, "openrgb")) {
            ImGui::Indent(kSubGroupIndent * 2.0f);
            ImGui::TextDisabled("驱动RGB外设:\n"
                "连接到运行中的OpenRGB服务器，用音乐点亮键盘鼠标内存风扇机箱灯条。\n"
                "仅客户端 — 本机不开放端口。");
            ImGui::Spacing();
            if (host_port_rows("openrgb", cfg.network.openrgb.host,
                                cfg.network.openrgb.port, 1, 65535))
                dirty = true;

            // ---- Pattern dropdowns (per zone type) -----------------------
            // Each row: "Single (N)" left-aligned, dropdown filling the
            // rest. Dropdown disabled when N == 0. Pattern names listed
            // active → soothing, matching the enum order.
            using SingleP = config::OpenRgbConfig::SinglePattern;
            using LinearP = config::OpenRgbConfig::LinearPattern;
            using MatrixP = config::OpenRgbConfig::MatrixPattern;

            static const char* const kSingleNames[] = {
                "节拍闪烁", "音量脉冲", "频谱色相",
                "时间循环", "静态", "关",
            };
            static const char* const kLinearNames[] = {
                "频谱条", "VU 量表", "追逐 / 环绕",
                "中心涟漪", "立体声分区", "染色",
                "静态", "关",
            };
            static const char* const kMatrixNames[] = {
                "空间图", "均衡列", "分区",
                "频谱瀑布图", "节拍刷新", "染色",
                "静态", "关",
            };

            auto pattern_dropdown_row = [&](const char* type_name, int count,
                                              const char* const* names, int n_names,
                                              int* selected) -> bool {
                char label[40];
                std::snprintf(label, sizeof(label), "%s (%d)", type_name, count);
                label_left(label);
                char id[40];
                std::snprintf(id, sizeof(id), "##patdd_%s", type_name);
                ImGui::PushItemWidth(-1);
                bool changed = false;
                if (count == 0) {
                    ImGui::BeginDisabled();
                    ImGui::Combo(id, selected, names, n_names);
                    ImGui::EndDisabled();
                } else {
                    if (ImGui::Combo(id, selected, names, n_names)) changed = true;
                }
                ImGui::PopItemWidth();
                return changed;
            };

            int s = static_cast<int>(cfg.network.openrgb.pattern_single);
            if (pattern_dropdown_row("单灯", n_single, kSingleNames,
                                       static_cast<int>(std::size(kSingleNames)), &s)) {
                cfg.network.openrgb.pattern_single = static_cast<SingleP>(s);
                dirty = true;
            }
            tip("用于单个LED区域的图案（GPU氛围灯、水冷水泵）\n"
                "以下从活跃到舒缓排列\n"
                "  • 节拍闪烁 — 按节拍脉冲亮起\n"
                "  • 音量 — 亮度随音量升降\n"
                "  • 频谱色相 — 频谱中心色相 （默认）\n"
                "  • 时间循环 — 色相总是循环\n"
                "  • 静态 / 关");

            int l = static_cast<int>(cfg.network.openrgb.pattern_linear);
            if (pattern_dropdown_row("灯带", n_linear, kLinearNames,
                                       static_cast<int>(std::size(kLinearNames)), &l)) {
                cfg.network.openrgb.pattern_linear = static_cast<LinearP>(l);
                dirty = true;
            }
            tip("用于线形区域的图案 （内存、机箱灯条、风扇灯环、主板氛围灯）\n"
                "以下从活跃到舒缓排列\n"
                "  • 频谱条 — 频率沿长度方向分布（默认）\n"
                "  • VU 量表 — 显示音量与峰值保持点\n"
                "  • 追逐 / 环绕 — 时间驱动的彗星拖尾\n"
                "  • 中心涟漪 — 低频与节拍产生向外波纹\n"
                "  • 立体声分区 — 根据左右音量切换左半/右半\n"
                "  • 节拍刷新 / 染色 / 静态 / 关");

            int m = static_cast<int>(cfg.network.openrgb.pattern_matrix);
            if (pattern_dropdown_row("矩阵", n_matrix, kMatrixNames,
                                       static_cast<int>(std::size(kMatrixNames)), &m)) {
                cfg.network.openrgb.pattern_matrix = static_cast<MatrixP>(m);
                dirty = true;
            }
            tip("灯光矩阵的图案（键盘、鼠标垫）\n"
                "以下从活跃到舒缓排列\n"
                "  • 空间图 — 8方向 → 键盘 XY\n"
                "  • 均衡列 — N频率带转换为纵向条带\n"
                "  • 分区 — 低频在下高频在上，节拍位于空格键（默认）\n"
                "  • 频谱瀑布图 — 随时间滚动行\n"
                "  • 节拍刷新 / 染色 / 静态 / 关");

            ImGui::Spacing();

            if (slider_int_row("更新率 (Hz)", &cfg.network.openrgb.rate_hz, 5, 60)) {
                cfg.network.openrgb.rate_hz = std::clamp(cfg.network.openrgb.rate_hz, 5, 60);
                dirty = true;
            }
            tip("帧率。默认为30，当前OpenRGB服务器在帧率大于~60Hz时存在CPU问题。\n实现: network.openrgb.rate_hz, [5, 60]");
            if (slider_row("亮度", &cfg.network.openrgb.brightness, 0.0f, 1.0f, "%.2f"))
                dirty = true;
            tip("输出颜色强度的全局倍率 (0..1).\n实现: network.openrgb.brightness");
            label_left("测试");
            if (ImGui::Button("闪烁所有LED ##openrgb", ImVec2(-1, 0))) {
                if (orgb) orgb->send_test_packet();
            }
            tip("连接并闪烁所有LED 1帧时间。验证OpenRGB服务器的连接性以及设备响应。");
            ImGui::Unindent(kSubGroupIndent * 2.0f);
        }
    }
}

}  // namespace

// ---- Settings (was: Settings Management) -------------------------------

static void section_settings(config::Store& store, config::Settings& cfg,
                              AudioSystem& system, bool& dirty) {
    section_header_only("设置", nullptr);

    ImGui::Columns(3, "##settings_buttons", false);
    if (ImGui::Button("保存", ImVec2(-1, 0))) store.save();
    tip("保存当前设置到 Listeningway.json（插件旁）");
    ImGui::NextColumn();
    if (ImGui::Button("加载", ImVec2(-1, 0))) store.load();
    tip("放弃未保存的更改，重新从磁盘加载 Listeningway.json。");
    ImGui::NextColumn();
    if (ImGui::Button("重置", ImVec2(-1, 0))) {
        store.publish(config::Settings{});
        store.save();
    }
    tip("重置所有选项为默认值并保存。");
    ImGui::Columns(1);

    if (ImGui::Button("重启音频流水线", ImVec2(-1, 0))) {
        system.switch_source(cfg.audio.capture_source_code);
    }
    tip("停止并重启活跃源。调整FFT尺寸和频带数量后需要使用。");

    if (ImGui::Checkbox("调试日志", &cfg.debug.debug_logging)) dirty = true;
    tip("导出详细日志为插件旁的 listeningway.log。\n实现: debug.debug_logging");

    ImGui::TextDisabled("配置文件: %s", store.path().u8string().c_str());
}

// ---- Top level ----------------------------------------------------------

void draw_overlay(reshade::api::effect_runtime*,
                  AudioSystem& system,
                  config::Store& store,
                  output::ConsumerRegistry& consumers,
                  HMODULE addon_module) {
    const auto snap = system.snapshot();
    auto cfg = store.snapshot();
    bool dirty = false;

    compute_columns();

    section_audio_source(system, cfg, dirty);
    section_levels(snap, cfg, dirty);
    section_beat(snap, cfg, dirty);
    section_spectrum(snap, cfg, dirty);
    section_spatial(snap, cfg, dirty);
    section_advanced(snap, cfg, dirty);
    section_integrations(cfg, dirty, consumers);
    section_performance(snap);
    section_settings(store, cfg, system, dirty);

    if (dirty) {
        store.publish(cfg);
        consumers.on_settings_changed(system, addon_module, cfg);
    }
}

}  // namespace lw
