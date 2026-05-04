#include "beat_tracker.h"

#include <algorithm>
#include <cmath>

namespace lw::dsp::beat {

BeatTracker::BeatTracker()
    : cumulative_score_(kBufferSize, 0.0f)
    , future_cs_(kBufferSize + 256, 0.0f)
    , w1_window_(kBufferSize, 0.0f)
    , w2_window_(256, 0.0f)
{}

void BeatTracker::reset() {
    std::fill(cumulative_score_.begin(), cumulative_score_.end(), 0.0f);
    cs_head_ = 0;
    beat_period_ = 86.0f;
    time_to_next_beat_ = -1;
    time_to_next_prediction_ = 10;
    beat_phase_counter_ = 0;
}

void BeatTracker::log_gaussian_window(std::span<float> out, float beat_period) const {
    // W1[i]: i = 0 corresponds to v = -2β, i = end-1 corresponds to v = -β/2.
    // a = γ · log(-v / β); W1 = exp(-a²/2).
    float v = -2.0f * beat_period;
    for (size_t i = 0; i < out.size(); ++i) {
        const float ratio = -v / std::max(beat_period, 1e-6f);
        const float a = (ratio > 0.0f) ? kTightness * std::log(ratio) : 0.0f;
        out[i] = std::exp(-(a * a) * 0.5f);
        v += 1.0f;
    }
}

float BeatTracker::weighted_max_in_window(std::span<const float> source,
                                          int start_idx, int end_idx,
                                          std::span<const float> weights) const {
    float best = 0.0f;
    int w = 0;
    for (int i = start_idx; i <= end_idx; ++i) {
        if (i < 0 || i >= static_cast<int>(source.size())) { ++w; continue; }
        if (w >= static_cast<int>(weights.size())) break;
        const float v = source[static_cast<size_t>(i)] * weights[static_cast<size_t>(w)];
        if (v > best) best = v;
        ++w;
    }
    return best;
}

bool BeatTracker::process(float odf_value, float beat_period_samples) {
    // Guard against degenerate beat periods coming from a cold-start
    // tempo estimator. Below 10 ODF samples the look-back window
    // collapses; above kBufferSize/2 we'd run out of history.
    beat_period_ = std::clamp(beat_period_samples, 10.0f, static_cast<float>(kBufferSize) * 0.5f);

    // Tiny constant to keep the ODF away from zero — BTrack's trick to
    // avoid the cumulative score collapsing to 0 forever during silence
    // (which would make all subsequent max-in-window operations return 0).
    const float odf = std::abs(odf_value) + 1e-4f;

    --time_to_next_prediction_;
    --time_to_next_beat_;
    ++beat_phase_counter_;

    // Linearise the cumulative score into a contiguous view so the
    // window math reads naturally. (Could be done with virtual indexing,
    // but it's only kBufferSize floats and predict_beat needs the same
    // layout anyway.)
    std::vector<float> cs_linear(kBufferSize);
    for (size_t i = 0; i < kBufferSize; ++i) {
        cs_linear[i] = cumulative_score_[(cs_head_ + i) % kBufferSize];
    }

    // Compute the new cumulative-score sample.
    const int window_start = static_cast<int>(kBufferSize) - static_cast<int>(std::round(2.0f * beat_period_));
    const int window_end   = static_cast<int>(kBufferSize) - static_cast<int>(std::round(0.5f * beat_period_));
    const int window_size  = std::max(1, window_end - window_start + 1);

    if (window_size > static_cast<int>(w1_window_.size())) {
        w1_window_.resize(static_cast<size_t>(window_size), 0.0f);
    }
    log_gaussian_window(std::span<float>(w1_window_.data(), static_cast<size_t>(window_size)),
                        beat_period_);

    const float weighted_max = weighted_max_in_window(
        cs_linear, window_start, window_end,
        std::span<const float>(w1_window_.data(), static_cast<size_t>(window_size)));
    const float new_cs = (1.0f - kAlpha) * odf + kAlpha * weighted_max;

    // Push new_cs at the end (overwrite oldest, advance head).
    cumulative_score_[cs_head_] = new_cs;
    cs_head_ = (cs_head_ + 1) % kBufferSize;

    // If we've reached the offbeat midpoint, re-predict the next beat.
    if (time_to_next_prediction_ <= 0) {
        predict_beat();
    }

    // If we've reached the predicted beat, fire and re-prime the phase counter.
    bool beat_due = false;
    if (time_to_next_beat_ <= 0) {
        beat_due = true;
        beat_phase_counter_ = 0;
        // The post-beat prediction window is at +β/2. predict_beat()
        // updates time_to_next_beat for the FOLLOWING beat; until that
        // happens, set a placeholder so we don't loop-fire.
        time_to_next_beat_ = static_cast<int>(std::round(beat_period_));
    }

    return beat_due;
}

void BeatTracker::predict_beat() {
    const int beat_window = std::max(2, static_cast<int>(std::round(beat_period_)));

    // Ensure working buffers are big enough.
    const size_t need_future = kBufferSize + static_cast<size_t>(beat_window);
    if (future_cs_.size() < need_future) future_cs_.resize(need_future, 0.0f);
    if (w2_window_.size()  < static_cast<size_t>(beat_window)) {
        w2_window_.resize(static_cast<size_t>(beat_window), 0.0f);
    }

    // Linearise current cumulative score into the front of future_cs_.
    for (size_t i = 0; i < kBufferSize; ++i) {
        future_cs_[i] = cumulative_score_[(cs_head_ + i) % kBufferSize];
    }
    std::fill(future_cs_.begin() + kBufferSize, future_cs_.end(), 0.0f);

    // Build W2 (Gaussian beat-expectation), centred at β/2 ahead.
    const float half_beat = beat_period_ * 0.5f;
    const float two_sigma_sq = 2.0f * half_beat * half_beat;
    for (int i = 0; i < beat_window; ++i) {
        const float dx = static_cast<float>(i + 1) - half_beat;
        w2_window_[static_cast<size_t>(i)] = std::exp(-(dx * dx) / two_sigma_sq);
    }

    // Build W1 once for the window size we'll use across the future projection.
    const int window_start_init = static_cast<int>(kBufferSize) - static_cast<int>(std::round(2.0f * beat_period_));
    const int window_end_init   = static_cast<int>(kBufferSize) - static_cast<int>(std::round(0.5f * beat_period_));
    const int window_size       = std::max(1, window_end_init - window_start_init + 1);
    if (w1_window_.size() < static_cast<size_t>(window_size)) {
        w1_window_.resize(static_cast<size_t>(window_size), 0.0f);
    }
    log_gaussian_window(std::span<float>(w1_window_.data(), static_cast<size_t>(window_size)),
                        beat_period_);

    // Synthesise the cumulative score forward by `beat_window` samples,
    // sliding the (window_start, window_end) bracket forward each step.
    // ODF input is 0 and α = 1: pure momentum.
    int ws = window_start_init;
    int we = window_end_init;
    for (int i = static_cast<int>(kBufferSize); i < static_cast<int>(kBufferSize) + beat_window; ++i) {
        const float wmax = weighted_max_in_window(
            std::span<const float>(future_cs_.data(), future_cs_.size()),
            ws, we,
            std::span<const float>(w1_window_.data(), static_cast<size_t>(window_size)));
        future_cs_[static_cast<size_t>(i)] = wmax;  // (1-α)·0 + 1.0·wmax
        ++ws;
        ++we;
    }

    // Argmax of (future_cs · W2) over the next beat window gives the
    // predicted time to the next beat (in ODF samples).
    float best = 0.0f;
    int   best_i = 0;
    for (int i = 0; i < beat_window; ++i) {
        const float v = future_cs_[static_cast<size_t>(static_cast<int>(kBufferSize) + i)]
                       * w2_window_[static_cast<size_t>(i)];
        if (v > best) { best = v; best_i = i; }
    }

    time_to_next_beat_       = best_i;
    time_to_next_prediction_ = best_i + static_cast<int>(std::round(beat_period_ * 0.5f));
}

float BeatTracker::beat_phase() const noexcept {
    if (beat_period_ <= 0.0f) return 0.0f;
    const float p = static_cast<float>(beat_phase_counter_) / beat_period_;
    return std::clamp(p - std::floor(p), 0.0f, 0.999999f);
}

}  // namespace lw::dsp::beat
