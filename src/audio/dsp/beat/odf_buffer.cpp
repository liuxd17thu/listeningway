#include "odf_buffer.h"

#include <algorithm>
#include <cmath>

namespace lw::dsp::beat {

void OdfBuffer::push(float odf_value) {
    ring_[head_] = odf_value;
    head_ = (head_ + 1) % kCapacity;
    if (size_ < kCapacity) ++size_;
}

void OdfBuffer::reset() {
    std::fill(ring_.begin(), ring_.end(), 0.0f);
    head_ = 0;
    size_ = 0;
}

bool OdfBuffer::snapshot_to(std::span<float> out, float hop_rate_hz) const {
    if (out.size() != kSnapshotLength) {
        // Misuse: the downstream tempo / beat constants assume exactly
        // kSnapshotLength samples. Refuse to silently lie.
        std::fill(out.begin(), out.end(), 0.0f);
        return false;
    }
    if (hop_rate_hz <= 0.0f) {
        std::fill(out.begin(), out.end(), 0.0f);
        return false;
    }

    // How many of our actual-rate samples cover the canonical window.
    const size_t want_in = static_cast<size_t>(
        std::round(kSnapshotSeconds * hop_rate_hz));
    if (want_in == 0 || size_ == 0) {
        std::fill(out.begin(), out.end(), 0.0f);
        return false;
    }
    const size_t avail_in = std::min(want_in, std::min(size_, kCapacity));

    // Linearise the most-recent `avail_in` samples into a contiguous view
    // for cheap interpolation. The ring's most recent sample is at
    // (head_ - 1) mod kCapacity; samples [head_ - avail_in, head_) are
    // the window we want, in chronological order.
    std::vector<float> linear(avail_in);
    const size_t start = (head_ + kCapacity - avail_in) % kCapacity;
    for (size_t i = 0; i < avail_in; ++i) {
        linear[i] = ring_[(start + i) % kCapacity];
    }

    // Linear resample `linear` (length avail_in) onto `out` (length 512).
    // For each output index j, source position = j · (avail_in - 1) / (out_len - 1).
    if (avail_in == 1) {
        std::fill(out.begin(), out.end(), linear[0]);
        return true;
    }
    const float scale = static_cast<float>(avail_in - 1) /
                         static_cast<float>(out.size() - 1);
    for (size_t j = 0; j < out.size(); ++j) {
        const float src   = static_cast<float>(j) * scale;
        const size_t src0 = static_cast<size_t>(std::floor(src));
        const size_t src1 = std::min(src0 + 1, avail_in - 1);
        const float frac  = src - static_cast<float>(src0);
        out[j] = linear[src0] * (1.0f - frac) + linear[src1] * frac;
    }
    return avail_in >= kSnapshotLength / 2;  // "warmed up" once we have at least half the window
}

}  // namespace lw::dsp::beat
