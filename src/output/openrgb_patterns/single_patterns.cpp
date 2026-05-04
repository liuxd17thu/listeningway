#include "single_patterns.h"

#include <algorithm>
#include <cmath>

#include "../../audio/snapshot/audio_snapshot.h"
#include "../../config/settings.h"

namespace lw::output::patterns {

namespace {

// Default accent hue for the patterns that don't derive their colour
// from audio (BeatFlash base, VolumePulse, Static). Picked from the
// middle of the bass→treble palette so it harmonises with multi-zone
// setups where Linear / Matrix patterns drive their own hues.
constexpr float kAccentHue = 0.50f;   // teal/cyan
constexpr float kAccentSat = 0.85f;
constexpr float kBeatFlashFromV = 0.20f;  // base dimness when no beat
constexpr float kBeatFlashToV   = 1.00f;  // peak brightness on beat

// Volume scaling for VolumePulse: volume_att is AGC-normalised around
// 1.0 (clamped at agc.clamp_max). Compress to [0, 1] for visual range.
float compress_volume(float v) noexcept {
    return clamp01(v * 0.5f);
}

}  // namespace

ColorRgb render_single(config::OpenRgbConfig::SinglePattern pattern,
                        const AudioSnapshot& snap,
                        PatternState&        state,
                        float                brightness) noexcept {
    using Pattern = config::OpenRgbConfig::SinglePattern;

    switch (pattern) {
        case Pattern::Off:
            return ColorRgb::black();

        case Pattern::Static: {
            // Stable accent at moderate brightness.
            const ColorRgb c = hsv_to_rgb(kAccentHue, kAccentSat, 0.60f);
            state.idle_colour = c;
            return apply_brightness(c, brightness);
        }

        case Pattern::SpectralHue: {
            // Hue from centroid (warm bass → cool treble), brightness from
            // smoothed volume. The "set it and forget it" default per ADR-0014.
            const float vol = noise_gate(compress_volume(snap.volume_att), 0.02f, 0.05f);
            const float h   = centroid_to_hue(snap.spectral_centroid);
            const ColorRgb c = hsv_to_rgb(h, 1.0f, lerp(0.05f, 1.0f, vol));
            state.idle_colour = c;
            return apply_brightness(c, brightness);
        }

        case Pattern::ChronotensityCycle: {
            // Full hue cycle driven by phase_volume. Always advances even
            // during silence (chronotensity is a phase accumulator, not a
            // beat detector), so the LED never sits still.
            const float h   = snap.phase_volume - std::floor(snap.phase_volume);
            const float vol = compress_volume(snap.volume_att);
            const ColorRgb c = hsv_to_rgb(h, 0.85f, lerp(0.30f, 1.0f, clamp01(vol)));
            state.idle_colour = c;
            return apply_brightness(c, brightness);
        }

        case Pattern::VolumePulse: {
            // Fixed accent colour, brightness rises and falls with volume.
            const float vol = noise_gate(compress_volume(snap.volume_att), 0.02f, 0.05f);
            const ColorRgb c = hsv_to_rgb(kAccentHue, kAccentSat, lerp(0.05f, 1.0f, vol));
            state.idle_colour = c;
            return apply_brightness(c, brightness);
        }

        case Pattern::BeatFlash: {
            // Fixed dim accent colour, brightness pulses on each beat.
            // Uses the smoothed `beat` curve from the new tracker (ADR-0013),
            // so flashes are inherently capped at ~3 Hz even at extreme
            // tempos — built-in epilepsy-band safety.
            const float beat = clamp01(snap.beat);
            const float v    = lerp(kBeatFlashFromV, kBeatFlashToV, beat);
            const ColorRgb c = hsv_to_rgb(kAccentHue, kAccentSat, v);
            state.idle_colour = c;
            return apply_brightness(c, brightness);
        }
    }
    return ColorRgb::black();
}

}  // namespace lw::output::patterns
