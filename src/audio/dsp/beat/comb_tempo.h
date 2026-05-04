// comb_tempo — tempo estimation via balanced ACF + shift-invariant comb
// filterbank with Rayleigh prior + 41-state Viterbi smoothing.
//
// Pipeline (each call to estimate()):
//
//   1. Adaptive threshold the resampled ODF (sliding-window mean, clip ≥0).
//   2. Balanced FFT-ACF: zero-pad 512 ODF samples to 1024, |X|², IFFT,
//      normalise each lag by (N - lag) for unbiased autocorrelation.
//   3. Comb filterbank: for each candidate lag τ ∈ [2, 127], sum ACF at
//      integer multiples a·τ + b for a ∈ [1, 4] (harmonics) and
//      b ∈ [1-a, a-1] (a `2a-1`-wide shift-tolerant neighbourhood),
//      multiplied by a precomputed Rayleigh weighting peaked at τ=43
//      (≈ 120 BPM at 86 Hz envelope rate). Sums energy across tempo
//      harmonics, which is what mitigates octave errors at this stage.
//   4. Adaptive threshold the comb output (sharpens peaks).
//   5. Tempo observation vector (length 41): each index k corresponds
//      to a tempo of (80 + 2k) BPM. Score is the comb output at the
//      candidate lag plus the comb output at the half-tempo lag — the
//      "double-sampling" trick from BTrack that boosts low-tempo
//      accuracy.
//   6. Viterbi step: 41-state lattice with Gaussian transition matrix
//      σ = 41/8 ≈ 5.1 (≈ 10 BPM tempo drift per call). delta[j] =
//      max_i(prevDelta[i] · transition[i][j]) · observation[j].
//      Normalise; argmax is the new tempo.
//   7. Confidence: aubio's quadratic_peak_mag(observation, peak) /
//      sum(observation). Robust, dimensionless, in [0, 1].
//
// References cited per primitive:
//   - Davies & Plumbley 2007 (comb filterbank, Rayleigh prior, Viterbi).
//   - Stark thesis 2011, §3.3 (BTrack's specific constants:
//     β = 43, σ = 41/8, observation-vector double-sampling).
//   - Brossier 2004 (parabolic peak refinement, confidence formula).
//   - Wiener-Khinchin (FFT-based balanced autocorrelation).
//
// Implementation cross-checked against BTrack/src/BTrack.cpp:373-590
// (GPLv3) for fidelity. No source copied; algorithm derived from the
// papers above.
#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <vector>

#include <kiss_fft.h>

namespace lw::dsp::beat {

class CombTempo {
public:
    CombTempo();
    ~CombTempo();
    CombTempo(const CombTempo&)            = delete;
    CombTempo& operator=(const CombTempo&) = delete;

    /// Process one resampled ODF window (must be exactly 512 samples).
    /// Returns the dominant tempo estimate in BPM (or 0 if no estimate yet).
    float estimate(std::span<const float> odf_512);

    /// Confidence of the most recent estimate, in [0, 1]. 0 means the
    /// observation vector has no clear peak (or no estimate yet).
    float confidence() const noexcept { return last_confidence_; }

    /// Period of the dominant tempo, in samples of the canonical 86 Hz
    /// envelope. Useful for the downstream beat tracker (it needs the
    /// period, not the BPM, for its cumulative-score window).
    float beat_period_samples() const noexcept { return last_beat_period_samples_; }

    void reset();

private:
    void adaptive_threshold(std::span<float> x) const;
    void balanced_acf(std::span<const float> odf, std::span<float> acf_out);
    void comb_filterbank(std::span<const float> acf, std::span<float> comb_out) const;

    static constexpr size_t kOdfLength       = 512;
    static constexpr size_t kFftLength       = 1024;
    static constexpr size_t kCombOutputSize  = 128;
    static constexpr size_t kNumTempoStates  = 41;       // 80..160 BPM, 2 BPM step
    static constexpr float  kEnvelopeFps     = 86.0f;    // canonical rate (matches 5.95 s / 512)
    static constexpr float  kRayleighBeta    = 43.0f;    // Rayleigh peak in lag samples
    static constexpr float  kTransitionSigma = 41.0f / 8.0f;

    // Precomputed at construction.
    std::array<float, kCombOutputSize>                                       rayleigh_;
    std::array<std::array<float, kNumTempoStates>, kNumTempoStates>          transition_;

    // FFT state (1024-point forward + inverse).
    kiss_fft_state*           fft_fwd_ = nullptr;
    kiss_fft_state*           fft_inv_ = nullptr;
    std::vector<kiss_fft_cpx> fft_in_;
    std::vector<kiss_fft_cpx> fft_out_;

    // Working buffers (reused).
    std::vector<float>                            threshed_odf_;
    std::vector<float>                            acf_;
    std::vector<float>                            comb_;
    std::array<float, kNumTempoStates>            tempo_obs_{};

    // Viterbi state.
    std::array<float, kNumTempoStates> prev_delta_{};
    std::array<float, kNumTempoStates> delta_{};

    // Most recent results.
    float last_bpm_                  = 0.0f;
    float last_confidence_           = 0.0f;
    float last_beat_period_samples_  = 0.0f;
};

}  // namespace lw::dsp::beat
