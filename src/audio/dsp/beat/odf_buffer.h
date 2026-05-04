// odf_buffer — circular history of ODF samples + linear resample to a
// fixed canonical length (512) for downstream tempo estimation.
//
// The comb-filterbank and beat-tracker constants from BTrack
// (Rayleigh peak β=43, transition matrix σ=41/8, log-Gaussian γ=5)
// are derived for a 512-sample ODF window representing ≈6 s of audio
// at BTrack's hop rate (44.1 kHz / 512 = 86 Hz). Listeningway's hop
// rate varies with FFT size; resampling the buffer to a fixed 512
// keeps every downstream constant valid regardless of host hop rate.
//
// Linear interpolation (rather than libsamplerate's SINC) is fine here:
// the ODF is already a slow, low-bandwidth signal — there's no aliasing
// budget to spend on fancy resampling.
//
// References:
//   - Stark thesis 2011, §3.3 (BTrack uses libsamplerate's SINC; we
//     trade quality for one less dependency).
//   - Davies & Plumbley 2007, §III-A (the fixed 512 buffer convention).
#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace lw::dsp::beat {

class OdfBuffer {
public:
    /// Capacity covers ~10 s at any reasonable hop rate; the snapshot
    /// only ever asks for the most recent kSnapshotSeconds of history.
    static constexpr size_t kCapacity        = 1024;
    static constexpr size_t kSnapshotLength  = 512;     // canonical downstream length
    static constexpr float  kSnapshotSeconds = 5.95f;   // matches BTrack's 512/86 Hz window

    void push(float odf_value);
    void reset();

    /// Linear-resample the most-recent `kSnapshotSeconds` of ODF
    /// history to exactly kSnapshotLength samples. `hop_rate_hz` is
    /// the rate at which push() is being called (used to select how
    /// many recent samples to read).
    /// Returns false if not enough history yet (output filled with zeros).
    bool snapshot_to(std::span<float> out, float hop_rate_hz) const;

    size_t size() const noexcept { return size_; }

private:
    std::vector<float> ring_ = std::vector<float>(kCapacity, 0.0f);
    size_t head_ = 0;   ///< next write index
    size_t size_ = 0;   ///< number of valid samples (≤ kCapacity)
};

}  // namespace lw::dsp::beat
