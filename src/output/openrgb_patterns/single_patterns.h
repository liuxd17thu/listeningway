// Single-LED pattern dispatcher (ADR-0014).
//
// Single-zone devices (GPU accents, AIO pumps, single-LED mice) want
// one slow scalar communicated through colour or brightness, not a
// spectrogram. Six patterns ordered active → soothing.
//
// All patterns produce ONE ColorRgb (Single zones treat their LEDs
// uniformly per OpenRGB convention); the consumer replicates across
// however many LEDs the zone reports.
#pragma once

#include "pattern_common.h"

#include "../../config/settings.h"  // for OpenRgbConfig::SinglePattern (nested enum)

namespace lw { struct AudioSnapshot; }

namespace lw::output::patterns {

ColorRgb render_single(
    config::OpenRgbConfig::SinglePattern pattern,
    const AudioSnapshot& snap,
    PatternState&        state,
    float                brightness) noexcept;

}  // namespace lw::output::patterns
