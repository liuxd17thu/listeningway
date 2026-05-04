#include "matrix_patterns.h"

#include <algorithm>
#include <cmath>

#include "../../audio/snapshot/audio_snapshot.h"

namespace lw::output::patterns {

namespace {

constexpr float kStaticAccentHue = 0.50f;
constexpr float kStaticAccentSat = 0.85f;
constexpr float kStaticAccentVal = 0.40f;

float compress_volume(float v) noexcept { return clamp01(v * 0.5f); }

/// Resolve the zone-LED index for matrix cell (row, col). If the zone
/// supplied a matrix_values lookup, use it; otherwise fall back to
/// row-major flat layout. Returns SIZE_MAX for "no LED here" cells
/// (gaps between key clusters on some keyboards).
size_t cell_to_led(const MatrixGeometry& geom, uint32_t row, uint32_t col) noexcept {
    if (row >= geom.height || col >= geom.width) return SIZE_MAX;
    if (!geom.values.empty()) {
        const size_t idx = static_cast<size_t>(row) * geom.width + col;
        if (idx >= geom.values.size()) return SIZE_MAX;
        const uint32_t v = geom.values[idx];
        if (v == 0xFFFFFFFFu) return SIZE_MAX;  // explicitly "no LED"
        return static_cast<size_t>(v);
    }
    // Fallback: flat row-major.
    return static_cast<size_t>(row) * geom.width + col;
}

void set_pixel(std::span<ColorRgb> out, size_t led, ColorRgb c) noexcept {
    if (led >= out.size()) return;
    out[led] = c;
}

void clear_all(std::span<ColorRgb> out) noexcept {
    std::fill(out.begin(), out.end(), ColorRgb::black());
}

void render_off(std::span<ColorRgb> out) noexcept { clear_all(out); }

void render_static(std::span<ColorRgb> out, float brightness) noexcept {
    const ColorRgb c = apply_brightness(
        hsv_to_rgb(kStaticAccentHue, kStaticAccentSat, kStaticAccentVal),
        brightness);
    std::fill(out.begin(), out.end(), c);
}

void render_color_wash(const AudioSnapshot& snap, std::span<ColorRgb> out,
                        float brightness) noexcept {
    const float vol = noise_gate(compress_volume(snap.volume_att), 0.02f, 0.05f);
    const ColorRgb c = apply_brightness(
        hsv_to_rgb(centroid_to_hue(snap.spectral_centroid), 1.0f, lerp(0.10f, 1.0f, vol)),
        brightness);
    std::fill(out.begin(), out.end(), c);
}

void render_beat_flash(const AudioSnapshot& snap, std::span<ColorRgb> out,
                       float brightness) noexcept {
    const float beat = clamp01(snap.beat);
    const float v    = lerp(0.10f, 1.0f, beat);
    const ColorRgb c = apply_brightness(
        hsv_to_rgb(kStaticAccentHue, kStaticAccentSat, v),
        brightness);
    std::fill(out.begin(), out.end(), c);
}

// SpatialMap — direction8 projected onto matrix XY. The 8 buckets are
// treated as compass directions: F/B drive the top/bottom rows, L/R
// drive the left/right columns, FL/FR/BL/BR drive the corners. Each
// cell's brightness is the maximum of the four nearest buckets'
// contributions weighted by distance, so the lit areas blob outward
// from each loud direction.
void render_spatial_map(const AudioSnapshot& snap, MatrixGeometry geom,
                         std::span<ColorRgb> out, float brightness) noexcept {
    if (geom.width == 0 || geom.height == 0) {
        clear_all(out);
        return;
    }
    // Direction8 anchor positions in normalised (col, row) coords.
    // Indices: 0=F, 1=FR, 2=R, 3=BR, 4=B, 5=BL, 6=L, 7=FL
    static constexpr float anchors[8][2] = {
        {0.50f, 0.00f},  // F  — top centre
        {1.00f, 0.00f},  // FR — top right
        {1.00f, 0.50f},  // R  — middle right
        {1.00f, 1.00f},  // BR — bottom right
        {0.50f, 1.00f},  // B  — bottom centre
        {0.00f, 1.00f},  // BL — bottom left
        {0.00f, 0.50f},  // L  — middle left
        {0.00f, 0.00f},  // FL — top left
    };

    clear_all(out);
    for (uint32_t row = 0; row < geom.height; ++row) {
        const float ny = (geom.height == 1) ? 0.5f
            : static_cast<float>(row) / static_cast<float>(geom.height - 1);
        for (uint32_t col = 0; col < geom.width; ++col) {
            const size_t led = cell_to_led(geom, row, col);
            if (led == SIZE_MAX) continue;
            const float nx = (geom.width == 1) ? 0.5f
                : static_cast<float>(col) / static_cast<float>(geom.width - 1);

            // Weighted sum of all 8 directions: closer = stronger contribution.
            float r_acc = 0, g_acc = 0, b_acc = 0;
            float w_acc = 0;
            for (int i = 0; i < 8; ++i) {
                const float dx = nx - anchors[i][0];
                const float dy = ny - anchors[i][1];
                const float dist = std::sqrt(dx * dx + dy * dy);
                const float w = std::exp(-dist * 4.0f);  // gaussian-ish falloff
                const float v = clamp01(snap.direction8[i]);
                // Hue per direction: cycles around the rose so left/right
                // are visually distinguishable.
                const float h = static_cast<float>(i) / 8.0f;
                const ColorRgb c = hsv_to_rgb(h, 0.85f, v);
                r_acc += c.r * w;
                g_acc += c.g * w;
                b_acc += c.b * w;
                w_acc += w;
            }
            if (w_acc > 0.0f) {
                const ColorRgb c{
                    static_cast<uint8_t>(std::min(255.0f, r_acc / w_acc)),
                    static_cast<uint8_t>(std::min(255.0f, g_acc / w_acc)),
                    static_cast<uint8_t>(std::min(255.0f, b_acc / w_acc)),
                };
                set_pixel(out, led, apply_brightness(c, brightness));
            }
        }
    }
}

// EqualizerColumns — N freq bands as vertical bars across the columns.
// Each column maps to one freq band (interpolated); each row above the
// band's amplitude is dark, below is lit. Reads as a real-time EQ.
void render_equalizer_columns(const AudioSnapshot& snap, MatrixGeometry geom,
                                std::span<ColorRgb> out, float brightness) noexcept {
    if (geom.width == 0 || geom.height == 0) {
        clear_all(out);
        return;
    }
    const size_t nb = std::min<size_t>(snap.freq_band_count, kMaxBands);
    clear_all(out);
    if (nb == 0) return;

    for (uint32_t col = 0; col < geom.width; ++col) {
        const float t = (geom.width == 1) ? 0.5f
            : static_cast<float>(col) / static_cast<float>(geom.width - 1);
        const float fpos = t * static_cast<float>(nb - 1);
        const size_t lo  = static_cast<size_t>(std::floor(fpos));
        const size_t hi  = std::min(lo + 1, nb - 1);
        const float frac = fpos - static_cast<float>(lo);
        const float band = clamp01(snap.freq_bands[lo] * (1.0f - frac) +
                                    snap.freq_bands[hi] * frac);
        const float bar_height = band * static_cast<float>(geom.height);
        const ColorRgb col_color = spectrum_ramp(t);

        for (uint32_t row = 0; row < geom.height; ++row) {
            // Bars grow upward from bottom (row = height-1 is bottom).
            const uint32_t inv_row = geom.height - 1 - row;
            if (static_cast<float>(inv_row) >= bar_height) continue;
            const size_t led = cell_to_led(geom, row, col);
            if (led == SIZE_MAX) continue;
            // Brighten the top of each bar slightly (peak pixels brighter).
            const float intensity = 1.0f - 0.4f *
                (static_cast<float>(inv_row) / std::max(bar_height, 1.0f));
            set_pixel(out, led, apply_brightness(dim(col_color, intensity), brightness));
        }
    }
}

// PerRegion — bass→bottom rows, mid→middle rows, treble→top rows, beat
// flashes the entire bottom row (where the spacebar usually lives).
// No key-label lookup — geometric approximation works for any matrix.
void render_per_region(const AudioSnapshot& snap, MatrixGeometry geom,
                        std::span<ColorRgb> out, float brightness) noexcept {
    if (geom.width == 0 || geom.height == 0) {
        clear_all(out);
        return;
    }
    const float bass = clamp01(snap.bass_norm * 0.5f);
    const float mid  = clamp01(snap.mid_norm  * 0.5f);
    const float treb = clamp01(snap.treb_norm * 0.5f);
    const float beat = clamp01(snap.beat);

    for (uint32_t row = 0; row < geom.height; ++row) {
        // Treble at top (row 0), bass at bottom (row height-1). Lerp the
        // intensity between bands by row position.
        const float t_pos = (geom.height == 1) ? 0.5f
            : static_cast<float>(row) / static_cast<float>(geom.height - 1);
        // Three-band blend: treble dominant near top, mid in the middle,
        // bass dominant near bottom.
        const float w_treb = std::max(0.0f, 1.0f - 2.0f * t_pos);
        const float w_bass = std::max(0.0f, 2.0f * t_pos - 1.0f);
        const float w_mid  = 1.0f - w_treb - w_bass;
        const float intensity = clamp01(treb * w_treb + mid * w_mid + bass * w_bass);
        // Hue follows the spectrum ramp by row so the look reads as
        // "warm at the bottom, cool at the top."
        const ColorRgb base = spectrum_ramp(1.0f - t_pos);
        ColorRgb cell_c = dim(base, intensity);

        // Bottom row: layer beat flash on top.
        if (row + 1 == geom.height) {
            const ColorRgb flash = hsv_to_rgb(0.0f, 0.0f, beat);  // white
            cell_c = ColorRgb{
                static_cast<uint8_t>(std::min(255, cell_c.r + flash.r)),
                static_cast<uint8_t>(std::min(255, cell_c.g + flash.g)),
                static_cast<uint8_t>(std::min(255, cell_c.b + flash.b)),
            };
        }

        for (uint32_t col = 0; col < geom.width; ++col) {
            const size_t led = cell_to_led(geom, row, col);
            if (led == SIZE_MAX) continue;
            set_pixel(out, led, apply_brightness(cell_c, brightness));
        }
    }
}

// SpectrogramWaterfall — time scrolls down rows, freq across columns.
// Each frame we shift the waterfall buffer down one row and write the
// current snapshot into row 0. The buffer is sized lazily on first use
// and re-sized if the matrix dimensions change.
void render_spectrogram_waterfall(const AudioSnapshot& snap, MatrixGeometry geom,
                                    std::span<ColorRgb> out, PatternState& state,
                                    float brightness) noexcept {
    if (geom.width == 0 || geom.height == 0) {
        clear_all(out);
        return;
    }
    // Lazy-init / resize the rolling buffer.
    if (state.waterfall_rows.size() != geom.height ||
        (!state.waterfall_rows.empty() &&
         state.waterfall_rows[0].size() != geom.width)) {
        state.waterfall_rows.assign(geom.height, std::vector<float>(geom.width, 0.0f));
        state.waterfall_head = 0;
    }

    // Sample current freq spectrum into a new row.
    std::vector<float>& head_row = state.waterfall_rows[state.waterfall_head];
    const size_t nb = std::min<size_t>(snap.freq_band_count, kMaxBands);
    for (uint32_t col = 0; col < geom.width; ++col) {
        const float t = (geom.width == 1) ? 0.5f
            : static_cast<float>(col) / static_cast<float>(geom.width - 1);
        if (nb == 0) {
            head_row[col] = 0.0f;
        } else {
            const float fpos = t * static_cast<float>(nb - 1);
            const size_t lo  = static_cast<size_t>(std::floor(fpos));
            const size_t hi  = std::min(lo + 1, nb - 1);
            const float frac = fpos - static_cast<float>(lo);
            head_row[col] = snap.freq_bands[lo] * (1.0f - frac) +
                             snap.freq_bands[hi] * frac;
        }
    }
    // Render: row 0 in the matrix shows the most recent (head) sample;
    // each successive row down shows older history.
    for (uint32_t row = 0; row < geom.height; ++row) {
        const size_t src = (state.waterfall_head + state.waterfall_rows.size() - row)
                            % state.waterfall_rows.size();
        const std::vector<float>& src_row = state.waterfall_rows[src];
        for (uint32_t col = 0; col < geom.width; ++col) {
            const size_t led = cell_to_led(geom, row, col);
            if (led == SIZE_MAX) continue;
            const float t = (geom.width == 1) ? 0.5f
                : static_cast<float>(col) / static_cast<float>(geom.width - 1);
            const float v = clamp01(src_row[col] * 1.4f);
            const ColorRgb c = dim(spectrum_ramp(t), v);
            set_pixel(out, led, apply_brightness(c, brightness));
        }
    }
    // Advance ring head.
    state.waterfall_head = (state.waterfall_head + 1) % state.waterfall_rows.size();
}

}  // namespace

void render_matrix(config::OpenRgbConfig::MatrixPattern pattern,
                    const AudioSnapshot& snap,
                    MatrixGeometry       geom,
                    std::span<ColorRgb>  out,
                    PatternState&        state,
                    float                brightness,
                    float                /*dt_sec*/) noexcept {
    using P = config::OpenRgbConfig::MatrixPattern;
    switch (pattern) {
        case P::SpatialMap:           render_spatial_map(snap, geom, out, brightness); break;
        case P::EqualizerColumns:     render_equalizer_columns(snap, geom, out, brightness); break;
        case P::PerRegion:            render_per_region(snap, geom, out, brightness); break;
        case P::SpectrogramWaterfall: render_spectrogram_waterfall(snap, geom, out, state, brightness); break;
        case P::BeatFlash:            render_beat_flash(snap, out, brightness); break;
        case P::ColorWash:            render_color_wash(snap, out, brightness); break;
        case P::Static:               render_static(out, brightness); break;
        case P::Off:                  render_off(out); break;
    }
}

}  // namespace lw::output::patterns
