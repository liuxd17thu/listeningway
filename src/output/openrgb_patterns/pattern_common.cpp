#include "pattern_common.h"

#include <algorithm>
#include <cmath>

namespace lw::output::patterns {

namespace {

constexpr float kBrightnessSafetyCap = 0.95f;  // ADR-0014: avoid sustained max-white

uint8_t to_byte(float v01) noexcept {
    const float scaled = v01 * 255.0f;
    if (scaled <= 0.0f)   return 0;
    if (scaled >= 255.0f) return 255;
    return static_cast<uint8_t>(scaled);
}

}  // namespace

ColorRgb hsv_to_rgb(float h, float s, float v) noexcept {
    h = h - std::floor(h);  // wrap into [0, 1)
    s = clamp01(s);
    v = clamp01(v);

    const float h6 = h * 6.0f;
    const int   i  = static_cast<int>(std::floor(h6)) % 6;
    const float f  = h6 - std::floor(h6);
    const float p  = v * (1.0f - s);
    const float q  = v * (1.0f - s * f);
    const float t  = v * (1.0f - s * (1.0f - f));

    float r = 0, g = 0, b = 0;
    switch (i) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        case 5: r = v; g = p; b = q; break;
    }
    return ColorRgb{to_byte(r), to_byte(g), to_byte(b)};
}

ColorRgb spectrum_ramp(float t) noexcept {
    t = clamp01(t);
    // Five-stop palette (matches the v1 single-mapping ramp): blue → cyan
    // → green → yellow → red. Bass-to-treble.
    static constexpr float stops[5][3] = {
        {0.10f, 0.20f, 1.00f},  // blue
        {0.10f, 1.00f, 1.00f},  // cyan
        {0.10f, 1.00f, 0.30f},  // green
        {1.00f, 1.00f, 0.10f},  // yellow
        {1.00f, 0.20f, 0.10f},  // red
    };
    const float scaled = t * 4.0f;
    const int   lo     = std::min(4, static_cast<int>(std::floor(scaled)));
    const int   hi     = std::min(4, lo + 1);
    const float frac   = scaled - static_cast<float>(lo);
    return ColorRgb{
        to_byte(stops[lo][0] * (1.0f - frac) + stops[hi][0] * frac),
        to_byte(stops[lo][1] * (1.0f - frac) + stops[hi][1] * frac),
        to_byte(stops[lo][2] * (1.0f - frac) + stops[hi][2] * frac),
    };
}

float centroid_to_hue(float centroid) noexcept {
    // Convention (matches spectrum_ramp's intuition): bass = blue, treble
    // = red. h=0.66 (blue) at centroid=0; h=0.0 (red) at centroid=1.
    // Linear interpolate; the perceptual non-linearity is negligible at
    // this layer because we're already integrating spectral energy upstream.
    return lerp(0.66f, 0.0f, clamp01(centroid));
}

ColorRgb dim(ColorRgb c, float intensity) noexcept {
    const float i = clamp01(intensity);
    return ColorRgb{
        static_cast<uint8_t>(c.r * i),
        static_cast<uint8_t>(c.g * i),
        static_cast<uint8_t>(c.b * i),
    };
}

ColorRgb apply_brightness(ColorRgb c, float brightness) noexcept {
    return dim(c, std::min(clamp01(brightness), kBrightnessSafetyCap));
}

float noise_gate(float level, float floor, float window) noexcept {
    if (level <= floor) return 0.0f;
    if (level >= floor + window) return level;
    // Linear ramp through the gate window.
    const float t = (level - floor) / window;
    return level * t;
}

}  // namespace lw::output::patterns
