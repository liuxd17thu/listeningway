# ADR-0013: Beat tracker — clean-room implementation of the Davies / Plumbley / Stark algorithm chain

## Status

Accepted, 2026-05-02

## Context

The v1 beat detector (carried into v2.0.0-beta.2 unchanged) used a single-band amplitude flux as the onset signal, naive autocorrelation with a log-Gaussian tempo prior at 120 BPM as the tempo estimator, and a `1 - second_best/best` confidence formula on the autocorrelation peaks. Two failure modes accumulated in user reports:

1. **Tempo confidence saturated at ~20%** even on metronomic dance music. Root cause: the autocorrelation function for a strong rhythm has wide peaks that span several lag samples; `second_best` was just the lag adjacent to `best`, dropping the ratio close to 1 and pinning confidence near 0. The "second peak" math counted the same physical peak twice.
2. **Octave errors with no recovery.** A single tempo prior at 120 BPM with no harmonic-summing meant true tempo and its half / double aliases scored comparably; the Viterbi-equivalent layer was absent.

A pre-rewrite interim added an Auto / Profile / Custom UI shape (preserved in this ADR) and replaced the threshold formula with multi-band onset aggregation against per-band EMA baselines. That helped the false-trigger story on sustained loud passages but did not address the tempo-confidence or octave-error issues. The user pushed back: *"this is hacky — download a battle-tested repo, analyze the code, understand how they do it, and implement something native."*

Two reference implementations were studied end-to-end (cloned to `_listeningway-research/`, sibling to the repo):

- **Adam Stark's BTrack** (GPLv3) — C++ realisation of Stark's PhD thesis, four stages: ODF, ODF buffer, comb-filterbank tempo with Viterbi smoothing, cumulative-score beat tracker with forward beat prediction.
- **Paul Brossier's aubio** (BSD-3) — same Davies / Plumbley algorithm chain in C with multiple selectable ODFs, a discrete two-state context-dependent switch instead of Viterbi, and an honest peak/sum tempo confidence measure.

Findings consolidated in [research-notes-beat.md](research-notes-beat.md). License posture: BTrack is studied algorithmically and reimplemented from the published papers (no source copied); aubio's peak/sum confidence formula is borrowed with attribution.

## Decision

The BeatStage is rewritten clean-room as four composed submodules, each one published primitive from the Davies / Plumbley / Stark literature:

```
src/audio/dsp/beat/
├── odf_csd.{h,cpp}        ──→  CSD-HWR onset detection function
├── odf_buffer.{h,cpp}     ──→  ring buffer + linear resample to canonical 512
├── comb_tempo.{h,cpp}     ──→  balanced FFT-ACF + comb filterbank + Viterbi
└── beat_tracker.{h,cpp}   ──→  cumulative score + W1/W2 + forward predictBeat
```

`src/audio/dsp/stages/beat_stage.{h,cpp}` becomes a thin assembly:

```
per hop:
    odf      = csd.process(magnitudes, phases)
    odf_buf.push(odf)
    every kTempoUpdateHops (~25 hops, ~250 ms):
        snap = odf_buf.snapshot_to(512, est_hop_rate_hz)
        comb_tempo.estimate(snap)         // updates beat_period + confidence
    beat_due = beat_tracker.process(odf, native_beat_period)
    if beat_due:
        beat_value = pulse_strength       // mode-dependent
    beat_value *= exp(-dt / decay_tau)    // mode-dependent
    publish(beat, beat_phase, tempo_bpm, tempo_confidence, ...)
```

### A. Onset detection: CSD-HWR

Complex spectral difference, half-wave rectified (BTrack's default; canonical in Bello et al. 2005). Per FFT bin: predict next phase as a linear extrapolation of the previous two, compute Euclidean distance between actual and predicted complex values, contribute only when the magnitude has risen between frames. Catches onsets in pitched / sustained content that pure-magnitude flux misses.

Pre-requisite refactor: `FftStage` now also publishes `phases` on the AnalysisFrame (atan2 of the complex output). One `atan2` per bin per hop — negligible.

### B. Tempo: balanced ACF + comb filterbank + Viterbi

Once every ~250 ms the ODF buffer is linearly resampled to a canonical 512 samples representing 5.95 s (BTrack's window — keeps every downstream constant valid regardless of the host's hop rate), then:

1. **Adaptive threshold** via sliding-window mean over [i-8, i+7], in-place subtract, clip ≥0.
2. **Balanced FFT-ACF**: zero-pad 512 → 1024, |X|², IFFT, divide each lag by (N-lag) for the unbiased normalisation.
3. **Shift-invariant comb filterbank**: for each candidate lag τ ∈ [2, 127], sum ACF at integer multiples a·τ + b for a ∈ [1, 4] (harmonics) and b ∈ [1-a, a-1] (a `2a-1`-wide neighbourhood for shift tolerance), multiplied by a precomputed Rayleigh weighting peaked at τ=43 (≈ 120 BPM at 86 Hz envelope rate). The harmonic summing is what mitigates octave errors at this stage.
4. **Adaptive threshold** the comb output again (sharpens peaks).
5. **Tempo observation vector** (length 41): each index k corresponds to (80 + 2k) BPM. Score is the comb output at the candidate lag plus the comb output at the half-tempo lag — the "double-sampling" trick from BTrack that boosts low-tempo accuracy.
6. **41-state Viterbi step** with Gaussian transition matrix σ = 41/8 ≈ 5.1 (≈ 10 BPM tempo drift per call). `delta[j] = max_i(prevDelta[i] · transition[i][j]) · observation[j]`, normalised. Argmax → current tempo.
7. **Confidence**: parabolic-peak-refined peak / sum on the observation vector — aubio's measure. Robust, dimensionless, in [0, 1]. **This is the single biggest fix relative to v1**: where the v1 detector pinned confidence at ~20% on locked dance music because of the second-peak-counting-the-same-peak bug, the new measure reads honestly across content.

Constants are BTrack's published values (Rayleigh β=43, σ=41/8, 41-state lattice 80–160 BPM in 2 BPM steps); they encode the algorithm, not the audio.

### C. Beat tracking: cumulative score + W1 + forward predictBeat (W2)

Per hop: update the 512-sample cumulative-score ring with `C[n] = (1-α)·ODF[n] + α · max_{k ∈ window} W1[k] · C[n-k]`, where α=0.9 and W1 is a log-Gaussian transition window peaked at exactly k=β with tightness γ=5. The look-back window covers k ∈ [β/2, 2β] of past cumulative-score samples.

At each offbeat (`time_to_next_prediction == 0`), `predict_beat()` projects the cumulative score forward by one beat period using α=1 (pure momentum, no new ODF input), builds W2 (Gaussian beat-expectation centred at β/2 ahead), and argmax of `future_cs · W2` gives the predicted `time_to_next_beat` in ODF samples.

The downstream consequence: `listeningway_beat_phase` is now a *forward prediction* — a sub-hop countdown to the next beat — rather than the v1 PLL's "stale value between detections."

### D. Mode / Profile / Custom UX preserved

The Auto / Profile / Custom segmented control from the interim implementation stays at the panel level. What each mode controls narrows because the underlying detection is now self-tuning at the algorithm level (no per-band sensitivity to dial):

| Mode | pulse_strength | decay_tau | Status badge |
|---|---|---|---|
| Auto (default) | 1.0 | 150 ms | "Adapting…" / "Locked" once tempo confidence above threshold for ~3 s |
| Profile · Percussive | 1.00 | 120 ms | — |
| Profile · Melodic | 1.20 | 160 ms | — |
| Profile · Sustained | 1.60 | 230 ms | — |
| Custom | from `cfg.beat.pulse_strength` slider | 150 ms | — |

Switching from Auto / Profile into Custom seeds the slider with the system's current `pulse_strength` so the slider lands where the audio is, not at a default.

### E. Old BeatConfig fields removed

All ten v1 knobs (`threshold_lambda`, `threshold_window_ms`, `refractory_ms`, `phase_kp`, `phase_ki`, `tempo_prior_bpm`, `tempo_prior_sigma`, `tempo_window_sec`, `beat_decay_per_sec`, `algorithm`) are removed from `BeatConfig`. nlohmann's `_WITH_DEFAULT` macro silently ignores unknown fields on load, so old `Listeningway.json` files deserialise cleanly.

## Consequences

### Positive

- Tempo confidence reads honestly (~0.10–0.30 on locked content vs the v1 ~0.20 ceiling). Shaders that gate visuals on `tempo_confidence > 0.05` get reliable lock-detection across genres.
- Octave errors mitigated at the comb filterbank stage (energy at 2τ, 3τ, 4τ contributes to τ's bin) and at the Viterbi smoother (prefers continuity over jumps).
- `listeningway_beat_phase` is a forward prediction with sub-hop accuracy — smooth countdown between beats instead of stale-value-between-PLL-detections.
- Continuous pulse curve (instant attack, exp decay) reads cleanly in shaders; missed onsets just go quiet rather than flicker the wrong direction.
- One user-facing knob (Pulse Strength in Custom mode) instead of ten. Auto mode is the default and needs no tuning.
- Profile presets are differentiated by decay tau, which shapes the visible pulse character — Percussive snappy, Sustained smoother — without changing the underlying detection.
- The four submodules are testable in isolation. Each has a small self-contained API and could be replaced independently.

### Negative

- More files than the previous monolithic BeatStage (5 instead of 1 in the beat path). Acceptable; each file has one job and the ADR documents the assembly.
- Memory footprint is larger. The ODF ring (1024 floats), the comb-tempo Viterbi state (41×41 transition matrix + 41 delta), the cumulative-score ring (512 floats), and the future-projection working buffer (~640 floats) add up to ~30 KB. Negligible at the addon's overall ~1 MB working set.
- CPU per hop is higher. The CSD ODF loop is one extra `atan2` + one `cos` + one `sqrt` per FFT bin (~3 µs at FFT=2048). The cumulative-score update is O(W) per hop where W ≈ 1.5β ≈ 130 (~5 µs). Tempo recomputation is the most expensive op and runs every 25 hops; amortised ~3 µs per hop. Total beat-stage cost increases from ~6 µs to ~13 µs per hop. Trivially absorbed.
- The two `_listeningway-research/` clones (BTrack and aubio) are not part of the repo. Anyone wanting to verify the lineage clones them themselves following the URLs in `research-notes-beat.md`.

### Neutral

- Shader uniform names and ranges unchanged. `listeningway_beat`, `listeningway_beat_phase`, `listeningway_tempo_bpm`, `listeningway_tempo_confidence`, `listeningway_tempo_detected` keep their contracts. Semantics tightened (continuous pulse curve, forward-predicted phase, honest confidence) within the documented ranges, per STABILITY.md's "semantics may be tuned, names and ranges are stable" provision.
- Old configs deserialise cleanly thanks to `nlohmann::DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT` ignoring unknown fields.

## Alternatives considered

### Vendor BTrack directly

**Rejected.** BTrack is GPLv3. Vendoring it would force GPLv3 on Listeningway. The algorithm is published; a clean-room implementation has the same algorithmic lineage with a permissive licence.

### Vendor aubio's beat-tracking subsystem

**Rejected.** aubio is BSD-3, so the licence works. But aubio's `tempo.c` + `beattracking.c` + `peakpicker.c` + `specdesc.c` total ~1700 LOC of dense C with its own complex-FFT and FFTW-or-Ooura conditional paths; integrating it would be a substantial subset to vendor, and the published comb-filter / Viterbi / cumulative-score stack is small enough that a native C++ port is the cleaner long-term answer.

### Use deep-learning beat trackers (madmom DBN, BeatNet, BEAST)

**Rejected.** Best in class on offline benchmarks but the practical cost (PyTorch/Theano weights, multi-millisecond inference) is wrong for an in-process ReShade addon's per-hop budget. Reconsider only if a tiny (< 1 MB weights, < 1 ms inference) RNN beat tracker emerges that's clearly better than CSD + comb + cumulative-score on game-audio content.

### Background thread for the beat tracker

**Considered, deferred.** The architectural cleanliness of "beat off the hot path" is real, but the new tracker's amortised cost is ~13 µs per hop because the heavy comb-filterbank and Viterbi work runs once per 25 hops. Adding a worker thread + SPSC ring + lock-free publisher for that saving would be ~400 LOC of pure plumbing for ~13 µs of avoided per-hop cost. Not worth it now; revisit if someone genuinely needs sub-ms beat latency at 240 Hz audio rates (no current ask).

### Adaptive algorithm constants in Auto mode

**Considered, partially adopted.** The Davies / Plumbley constants (Rayleigh β, Viterbi σ, W1 tightness γ, cumulative-score α) define how the tracker thinks; auto-adjusting them would be like a thermostat that auto-changes its definition of Celsius. The visualisation constants (confidence-lock threshold, pulse decay tau) are reasonable candidates for self-tuning — pursued in a follow-up if desired.

### Per-stage enable toggles

**Deferred.** A useful accessibility feature for low-end hardware but orthogonal to this ADR. Listed as a future task.

## References

- Stark, A. M. *Automatic Real-Time Beat Tracking*. PhD thesis, Queen Mary University of London, 2011.
- Davies, M. E. P., & Plumbley, M. D. *Context-Dependent Beat Tracking of Musical Audio*. IEEE TASLP 15(3):1009-1020, 2007.
- Stark, A. M., Davies, M. E. P., & Plumbley, M. D. *Real-Time Beat-Synchronous Analysis of Musical Audio*. DAFx-09, Como, 2009.
- Bello, J. P., Daudet, L., Abdallah, S., Duxbury, C., Davies, M., & Sandler, M. B. *A Tutorial on Onset Detection in Music Signals*. IEEE TSAP 13(5):1035-1047, 2005.
- Duxbury, C., Bello, J. P., Davies, M., & Sandler, M. B. *Complex Domain Onset Detection for Musical Signals*. DAFx-03, London, 2003.
- Brossier, P., Bello, J. P., & Plumbley, M. D. *Real-Time Temporal Segmentation of Note Objects in Music Signals*. ICMC, 2004.
- [`docs/adr/research-notes-beat.md`](research-notes-beat.md) — full reverse-engineering of BTrack and aubio that informed this ADR.
- BTrack source at `https://github.com/adamstark/BTrack` (GPLv3) — studied algorithmically, no source copied.
- aubio source at `https://github.com/aubio/aubio` (BSD-3) — studied; the peak/sum tempo-confidence formula is borrowed with attribution.
- ADR-0002 places this stage in the five-layer pipeline.
- ADR-0003 explains why BeatStage stays a concrete `IDspStage` rather than gaining an `IBeatTracker` strategy interface.
