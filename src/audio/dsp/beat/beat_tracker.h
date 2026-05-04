// beat_tracker — cumulative-score beat tracker with log-Gaussian
// transition window (W1), forward-projected beat prediction (W2), and
// per-call time-to-next-beat output.
//
// On each new ODF sample:
//   C[n] = (1-α)·ODF[n] + α · max_{k ∈ window} W1[k] · C[n-k]
//
// where the window covers k ∈ [β/2, 2β] (looking back 0.5β to 2β
// samples), W1 is a log-Gaussian peaked at exactly k=β, and β is the
// beat period in ODF samples (from the comb_tempo estimator).
//
//   W1[k] = exp(-½·(γ·log(k/β))²)
//
// γ = tightness = 5 (BTrack default; controls how sharply W1 peaks at
// k=β). α = 0.9 (BTrack default; momentum vs new-evidence weight).
//
// predict_beat() runs at the offbeat (halfway between detected beats)
// and projects the cumulative score forward by one beat period using
// pure momentum (α=1, no new ODF input). Argmax of the projected
// score against a Gaussian beat-expectation window W2 centred at β/2
// ahead gives time_to_next_beat in ODF samples — i.e. a sub-hop
// forward prediction of when the next beat will fire.
//
// References:
//   - Stark, Davies, Plumbley. "Real-Time Beat-Synchronous Analysis of
//     Musical Audio." DAFx-09, Como, 2009.
//   - Stark thesis 2011, §3.3 (W1 = equation 3.2; updateCumulativeScore
//     = equation 3.4; predictBeat / W2 = equation 3.6).
//
// Implementation cross-checked against BTrack/src/BTrack.cpp:617-720
// (GPLv3) for fidelity. No source copied; algorithm derived from the
// thesis and DAFx-09 paper above.
#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace lw::dsp::beat {

class BeatTracker {
public:
    BeatTracker();

    /// Process one ODF sample. `beat_period_samples` is the current
    /// estimate from the comb_tempo module; the tracker uses it to
    /// shape the look-back window and the forward prediction.
    /// Returns true if a beat fires on this call.
    bool process(float odf_value, float beat_period_samples);

    /// Samples until the next predicted beat. Decrements each process()
    /// until 0; refreshed at the offbeat by predict_beat().
    int   time_to_next_beat() const noexcept       { return time_to_next_beat_; }
    int   time_to_next_prediction() const noexcept { return time_to_next_prediction_; }
    float beat_period_samples() const noexcept     { return beat_period_; }

    /// Beat phase in [0, 1): 0 just after a beat, → 1 just before the next.
    float beat_phase() const noexcept;

    void reset();

private:
    void predict_beat();
    void log_gaussian_window(std::span<float> out, float beat_period) const;
    float weighted_max_in_window(std::span<const float> source,
                                  int start_idx, int end_idx,
                                  std::span<const float> weights) const;

    static constexpr size_t kBufferSize = 512;
    static constexpr float  kAlpha      = 0.9f;
    static constexpr float  kTightness  = 5.0f;

    // Cumulative-score ring. cs_head_ points at the OLDEST sample;
    // (cs_head_ + kBufferSize - 1) % kBufferSize is the NEWEST.
    std::vector<float> cumulative_score_;
    size_t             cs_head_ = 0;

    // Working buffers for predict_beat (sized once, reused).
    std::vector<float> future_cs_;
    std::vector<float> w1_window_;
    std::vector<float> w2_window_;

    // Tracker state (units = ODF samples).
    float beat_period_              = 86.0f;  // 60 BPM at 86 Hz default; comb_tempo overrides
    int   time_to_next_beat_        = -1;
    int   time_to_next_prediction_  = 10;     // BTrack's initial value
    int   beat_phase_counter_       = 0;      // counts up from 0; resets on each beat
};

}  // namespace lw::dsp::beat
