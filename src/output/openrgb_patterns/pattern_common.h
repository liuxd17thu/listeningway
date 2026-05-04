// Shared primitives for the OpenRGB pattern catalogue (ADR-0014).
//
// Patterns are pure functions where possible: take the AudioSnapshot,
// the target zone's metadata (LED count, matrix dimensions), and a
// per-zone scratch state for the patterns that need history (VU peak
// hold, spectrogram waterfall scroll, etc.). Output is a flat vector
// of ColorRgb in zone-LED order.
//
// We use a plain ColorRgb (3 × uint8_t) at this layer so the pattern
// modules don't pull in OpenRGB SDK headers. The consumer converts to
// orgb::Color at the boundary before calling setDeviceColors.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace lw::output::patterns {

struct ColorRgb {
    uint8_t r{0}, g{0}, b{0};
    constexpr ColorRgb() = default;
    constexpr ColorRgb(uint8_t r_, uint8_t g_, uint8_t b_) : r(r_), g(g_), b(b_) {}
    static constexpr ColorRgb black() { return {0, 0, 0}; }
};

/// Per-zone scratch state. Pattern functions read/write this; the consumer
/// keeps one instance per zone across frames. Zero-initialised default.
struct PatternState {
    // VU Meter: smoothed peak position [0, 1] and a decay timestamp.
    float vu_peak_pos = 0.0f;
    float vu_peak_decay_seconds = 0.0f;

    // Spectrogram Waterfall: ring of recent freq-band snapshots,
    // indexed [row][band]. Sized lazily to (matrix_height, band_count)
    // on first use; grows once and is kept stable thereafter.
    std::vector<std::vector<float>> waterfall_rows;
    size_t waterfall_head = 0;

    // ChronotensityCycle / Chase / Orbit don't need state — they read
    // the live phase_volume from the snapshot.

    // Last-frame colour for static-fallback patterns when audio is below
    // the noise gate. Lets a pattern fade smoothly to its static colour
    // rather than snap to black.
    ColorRgb idle_colour{0, 0, 0};
};

// --- Colour helpers ------------------------------------------------------

/// HSV → RGB. H in [0, 1) (wraps), S and V in [0, 1].
ColorRgb hsv_to_rgb(float h, float s, float v) noexcept;

/// Bass→treble five-stop ramp (blue → cyan → green → yellow → red).
/// `t` clamped to [0, 1]. Same palette the v1 spectrum-bar used.
ColorRgb spectrum_ramp(float t) noexcept;

/// Map spectral centroid → hue. Centroid in [0, 1] (0 = pure bass, 1 =
/// pure treble). Returns a hue suited to fixed-S/V conversion via
/// hsv_to_rgb. Convention: bass = blue (h=0.66), treble = red (h=0.0).
float centroid_to_hue(float centroid) noexcept;

/// Multiply each channel by a [0, 1] intensity. Saturating.
ColorRgb dim(ColorRgb c, float intensity) noexcept;

/// Apply the user's brightness multiplier and the safety cap (0.95 of
/// full per ADR-0014, so sustained max-white doesn't stress hardware).
ColorRgb apply_brightness(ColorRgb c, float brightness) noexcept;

// --- Audio shaping helpers ----------------------------------------------

/// Hard noise gate. If `level` < `floor`, returns 0 (silence). If
/// `level` ≥ `floor` + window, returns the level unchanged. In between,
/// linearly fades — avoids the on/off stutter that bites every other
/// audio-reactive RGB tool's silence-handling thread.
float noise_gate(float level, float floor = 0.02f, float window = 0.02f) noexcept;

constexpr float lerp(float a, float b, float t) noexcept {
    return a + (b - a) * t;
}

constexpr float clamp01(float x) noexcept {
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

}  // namespace lw::output::patterns
