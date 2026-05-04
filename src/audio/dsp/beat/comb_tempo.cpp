#include "comb_tempo.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <numeric>

namespace lw::dsp::beat {

namespace {

constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;

// tempoToLagFactor at the canonical 86 Hz envelope rate.
//   lag (samples) = 60 · envelope_fps / BPM
constexpr float kTempoToLagFactor = 60.0f * 86.0f;  // 5160 — close to BTrack's 5168.75 for 44.1k/512

// Adaptive-threshold sliding window (BTrack uses pre=8, post=7).
constexpr int kAdaptPre  = 8;
constexpr int kAdaptPost = 7;

// Comb filter parameters.
constexpr int kLagMin       = 2;
constexpr int kLagMax       = 127;
constexpr int kNumHarmonics = 4;   // a ∈ [1, 4]

float quadratic_peak_mag(std::span<const float> x, int peak_idx) {
    // Parabolic peak refinement: fit y(i-1), y(i), y(i+1) to a parabola
    // and return the magnitude at the vertex. Standard MIR trick.
    if (peak_idx <= 0 || peak_idx + 1 >= static_cast<int>(x.size())) {
        return x[std::clamp(peak_idx, 0, static_cast<int>(x.size()) - 1)];
    }
    const float ym1 = x[peak_idx - 1];
    const float y0  = x[peak_idx];
    const float yp1 = x[peak_idx + 1];
    const float denom = ym1 - 2.0f * y0 + yp1;
    if (std::abs(denom) < 1e-12f) return y0;  // flat / no curvature
    const float frac = (ym1 - yp1) / (2.0f * denom);
    return y0 - 0.25f * frac * (ym1 - yp1);
}

}  // namespace

CombTempo::CombTempo() {
    // Rayleigh prior, peak at lag = β: w(n) = (n/β²) · exp(-n²/(2β²))
    const float beta_sq = kRayleighBeta * kRayleighBeta;
    for (size_t n = 0; n < kCombOutputSize; ++n) {
        const float nf = static_cast<float>(n);
        rayleigh_[n] = (nf / beta_sq) * std::exp(-(nf * nf) / (2.0f * beta_sq));
    }

    // Tempo transition matrix: Gaussian centred on each previous state.
    // entry (i, j) = N(j; μ=i, σ=kTransitionSigma)  (BTrack uses 1-indexed
    // x = j+1, μ = i+1; the +1 cancels in the difference so we can stay 0-indexed).
    const float sigma_sq    = kTransitionSigma * kTransitionSigma;
    const float norm_factor = 1.0f / (kTransitionSigma * std::sqrt(kTwoPi));
    for (size_t i = 0; i < kNumTempoStates; ++i) {
        for (size_t j = 0; j < kNumTempoStates; ++j) {
            const float dx = static_cast<float>(j) - static_cast<float>(i);
            transition_[i][j] = norm_factor * std::exp(-(dx * dx) / (2.0f * sigma_sq));
        }
    }

    // prevDelta initialised to uniform 1.0 (BTrack does the same).
    std::fill(prev_delta_.begin(), prev_delta_.end(), 1.0f);

    // Allocate kissfft state for forward + inverse 1024.
    fft_fwd_ = kiss_fft_alloc(static_cast<int>(kFftLength), /*inverse*/ 0, nullptr, nullptr);
    fft_inv_ = kiss_fft_alloc(static_cast<int>(kFftLength), /*inverse*/ 1, nullptr, nullptr);
    fft_in_.resize(kFftLength);
    fft_out_.resize(kFftLength);

    threshed_odf_.assign(kOdfLength, 0.0f);
    acf_.assign(kOdfLength, 0.0f);
    comb_.assign(kCombOutputSize, 0.0f);
}

CombTempo::~CombTempo() {
    if (fft_fwd_) std::free(fft_fwd_);
    if (fft_inv_) std::free(fft_inv_);
}

void CombTempo::reset() {
    std::fill(prev_delta_.begin(), prev_delta_.end(), 1.0f);
    std::fill(delta_.begin(),       delta_.end(),       0.0f);
    std::fill(tempo_obs_.begin(),   tempo_obs_.end(),   0.0f);
    std::fill(threshed_odf_.begin(),threshed_odf_.end(),0.0f);
    std::fill(acf_.begin(),         acf_.end(),         0.0f);
    std::fill(comb_.begin(),        comb_.end(),        0.0f);
    last_bpm_ = 0.0f;
    last_confidence_ = 0.0f;
    last_beat_period_samples_ = 0.0f;
}

void CombTempo::adaptive_threshold(std::span<float> x) const {
    // Compute moving-window mean over [i-pre, i+post], in-place subtract,
    // clip to ≥ 0. BTrack handles the head and tail separately to avoid
    // accessing outside the array — same pattern here.
    const int n = static_cast<int>(x.size());
    if (n == 0) return;
    std::vector<float> threshold(static_cast<size_t>(n), 0.0f);

    auto window_mean = [&](int lo, int hi) {
        lo = std::max(0, lo);
        hi = std::min(n, hi);
        if (hi <= lo) return 0.0f;
        float sum = std::accumulate(x.begin() + lo, x.begin() + hi, 0.0f);
        return sum / static_cast<float>(hi - lo);
    };

    const int t = std::min(n, kAdaptPost);
    for (int i = 0; i <= t; ++i) {
        threshold[static_cast<size_t>(i)] = window_mean(0, std::min(i + kAdaptPre, n));
    }
    for (int i = t + 1; i < n - kAdaptPost; ++i) {
        threshold[static_cast<size_t>(i)] = window_mean(i - kAdaptPre, i + kAdaptPost);
    }
    for (int i = std::max(0, n - kAdaptPost); i < n; ++i) {
        threshold[static_cast<size_t>(i)] = window_mean(std::max(i - kAdaptPost, 0), n);
    }

    for (int i = 0; i < n; ++i) {
        const float v = x[static_cast<size_t>(i)] - threshold[static_cast<size_t>(i)];
        x[static_cast<size_t>(i)] = std::max(0.0f, v);
    }
}

void CombTempo::balanced_acf(std::span<const float> odf, std::span<float> acf_out) {
    // Zero-pad ODF to kFftLength.
    for (size_t i = 0; i < kFftLength; ++i) {
        fft_in_[i].r = (i < odf.size()) ? odf[i] : 0.0f;
        fft_in_[i].i = 0.0f;
    }
    kiss_fft(fft_fwd_, fft_in_.data(), fft_out_.data());

    // Power spectrum: replace with |X|² (real), zero imag.
    for (size_t i = 0; i < kFftLength; ++i) {
        const float re = fft_out_[i].r;
        const float im = fft_out_[i].i;
        fft_out_[i].r = re * re + im * im;
        fft_out_[i].i = 0.0f;
    }

    // Inverse FFT to get the (un-normalised) autocorrelation.
    kiss_fft(fft_inv_, fft_out_.data(), fft_in_.data());

    // Take magnitude (the ACF should be real, but kissfft leaves a tiny
    // imaginary residue; we take |z| for safety, BTrack does the same).
    // Then divide each lag by (kOdfLength - lag) for the unbiased / "balanced"
    // normalisation, plus a kFftLength factor that BTrack keeps for backward
    // compatibility with its earlier time-domain implementation.
    float lag = static_cast<float>(kOdfLength);
    for (size_t i = 0; i < kOdfLength && i < acf_out.size(); ++i) {
        const float r  = fft_in_[i].r;
        const float im = fft_in_[i].i;
        const float abs_v = std::sqrt(r * r + im * im);
        acf_out[i] = (abs_v / lag) / static_cast<float>(kFftLength);
        lag -= 1.0f;
    }
}

void CombTempo::comb_filterbank(std::span<const float> acf, std::span<float> comb_out) const {
    std::fill(comb_out.begin(), comb_out.end(), 0.0f);
    for (int i = kLagMin; i <= kLagMax; ++i) {
        for (int a = 1; a <= kNumHarmonics; ++a) {
            const float norm = 1.0f / static_cast<float>(2 * a - 1);
            for (int b = 1 - a; b <= a - 1; ++b) {
                const int idx = a * i + b - 1;
                if (idx < 0 || idx >= static_cast<int>(acf.size())) continue;
                comb_out[static_cast<size_t>(i - 1)] +=
                    acf[static_cast<size_t>(idx)] * rayleigh_[static_cast<size_t>(i - 1)] * norm;
            }
        }
    }
}

float CombTempo::estimate(std::span<const float> odf_512) {
    if (odf_512.size() != kOdfLength) {
        return last_bpm_;  // refuse to lie; downstream constants assume 512
    }

    // Step 1: adaptive threshold the ODF (in a private copy).
    std::copy(odf_512.begin(), odf_512.end(), threshed_odf_.begin());
    adaptive_threshold(threshed_odf_);

    // Step 2: balanced FFT-ACF.
    balanced_acf(threshed_odf_, acf_);

    // Step 3: comb filterbank with Rayleigh prior.
    comb_filterbank(acf_, comb_);

    // Step 4: adaptive threshold on comb output (sharpen peaks).
    adaptive_threshold(comb_);

    // Step 5: tempo observation vector. BPM = 80 + 2k for k ∈ [0, 41).
    // Two-component score: comb at the candidate lag + comb at half-tempo lag.
    for (size_t k = 0; k < kNumTempoStates; ++k) {
        const float bpm1 = static_cast<float>(2 * k + 80);
        const float bpm2 = static_cast<float>(4 * k + 160);
        const int lag1 = std::clamp(static_cast<int>(std::round(kTempoToLagFactor / bpm1)),
                                     1, static_cast<int>(comb_.size()));
        const int lag2 = std::clamp(static_cast<int>(std::round(kTempoToLagFactor / bpm2)),
                                     1, static_cast<int>(comb_.size()));
        tempo_obs_[k] = comb_[static_cast<size_t>(lag1 - 1)]
                      + comb_[static_cast<size_t>(lag2 - 1)];
    }

    // Step 6: Viterbi step.
    for (size_t j = 0; j < kNumTempoStates; ++j) {
        float best = -1.0f;
        for (size_t i = 0; i < kNumTempoStates; ++i) {
            const float v = prev_delta_[i] * transition_[i][j];
            if (v > best) best = v;
        }
        delta_[j] = std::max(0.0f, best) * tempo_obs_[j];
    }
    // Normalise delta (sum to 1, treat the all-zero case as uniform).
    float sum = 0.0f;
    for (float v : delta_) sum += v;
    if (sum > 1e-12f) {
        const float inv = 1.0f / sum;
        for (float& v : delta_) v *= inv;
    } else {
        const float u = 1.0f / static_cast<float>(kNumTempoStates);
        std::fill(delta_.begin(), delta_.end(), u);
    }

    // Argmax → tempo BPM.
    int max_idx = 0;
    float max_val = delta_[0];
    for (size_t k = 1; k < kNumTempoStates; ++k) {
        if (delta_[k] > max_val) { max_val = delta_[k]; max_idx = static_cast<int>(k); }
    }
    prev_delta_ = delta_;

    last_bpm_ = static_cast<float>(2 * max_idx + 80);
    last_beat_period_samples_ = kTempoToLagFactor / std::max(last_bpm_, 1.0f);

    // Step 7: confidence = parabolic-peak-refined peak / sum.
    // Use the tempo observation vector (length 41) for the peak; the comb
    // output isn't normalised across lags the same way, so the obs vector
    // gives a more comparable measure across estimates.
    float obs_sum = 0.0f;
    for (float v : tempo_obs_) obs_sum += v;
    if (obs_sum > 1e-12f) {
        const float peak_mag = quadratic_peak_mag(
            std::span<const float>(tempo_obs_.data(), tempo_obs_.size()),
            max_idx);
        last_confidence_ = std::clamp(peak_mag / obs_sum, 0.0f, 1.0f);
    } else {
        last_confidence_ = 0.0f;
    }

    return last_bpm_;
}

}  // namespace lw::dsp::beat
