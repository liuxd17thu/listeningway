// Matrix-zone pattern dispatcher (ADR-0014).
//
// 2D-grid zones (keyboards, mouse pads). Eight patterns ordered active
// → soothing.
//
// Output is written into the caller-provided span, sized to the
// zone's leds_count. Matrix dimensions (width × height) are taken from
// the OpenRGB Zone metadata; if they look invalid (0 × 0) we fall back
// to treating the LEDs as a single row.
//
// LED indexing: per OpenRGB's Zone::matrix_values convention, the LED
// at row r, col c lives at zone-LED index `matrix_values[r·width + c]`.
// For v1 we accept a flat row-major fallback when matrix_values is
// unavailable — wrong for some weird keyboard layouts (extra-wide
// modifiers, gap above arrow cluster) but a reasonable approximation
// for the most common cases.
#pragma once

#include <span>

#include "pattern_common.h"

#include "../../config/settings.h"  // for OpenRgbConfig::MatrixPattern

namespace lw { struct AudioSnapshot; }

namespace lw::output::patterns {

struct MatrixGeometry {
    uint32_t width  = 0;
    uint32_t height = 0;
    /// Optional row-major (height × width) lookup of zone-LED indices.
    /// Empty → fall back to flat row-major (i = r·width + c).
    std::span<const uint32_t> values;
};

void render_matrix(
    config::OpenRgbConfig::MatrixPattern pattern,
    const AudioSnapshot& snap,
    MatrixGeometry       geom,
    std::span<ColorRgb>  out,
    PatternState&        state,
    float                brightness,
    float                dt_sec) noexcept;

}  // namespace lw::output::patterns
