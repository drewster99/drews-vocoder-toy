// Written by Andrew Benson — https://github.com/drewster99
// Copyright (C) 2026 Nuclear Cyborg Corp. MIT License — see LICENSE file.

#pragma once

#include "vocoder_constants.h"
#include "vocoder_carrier_table.h"

// ============================================================================
// Render Callback Helpers
// ============================================================================
//
// Static inline helper functions for the audio render callback.
// All are inlined by the compiler at -O2 (single compilation unit) — zero overhead.

/// Detect DLS note source transitions and trigger reconfigure on mode change.
static inline void renderUpdateDLSMode(RenderContext* ctx, SharedState* shared) {
    bool ptOn = shared->pitchTrackEnabled.load(std::memory_order_relaxed);
    bool mActive = (ctx->midi != nullptr &&
                    (ctx->midi->hasActiveNotes.load(std::memory_order_relaxed) ||
                     ctx->midi->hasSustainedNotes.load(std::memory_order_relaxed)));
    bool aChord = shared->autoChordEnabled.load(std::memory_order_relaxed);

    RenderContext::DLSNoteMode curMode;
    if (mActive)       curMode = RenderContext::kDLSMidi;
    else if (ptOn)     curMode = RenderContext::kDLSPitchTrack;
    else if (aChord)   curMode = RenderContext::kDLSChord;
    else               curMode = RenderContext::kDLSNone;

    if (curMode != ctx->lastDLSMode) {
        shared->synthNeedsReconfigure.store(true, std::memory_order_relaxed);
    }
    ctx->lastDLSMode = curMode;
}

/// Mode-aware synth reconfiguration (handles chord/transpose/harmony/mode changes).
static inline void renderHandleSynthReconfigure(RenderContext* ctx, SharedState* shared) {
    if (!shared->synthNeedsReconfigure.load(std::memory_order_relaxed)) return;
    shared->synthNeedsReconfigure.store(false, std::memory_order_relaxed);

    RenderContext::DLSNoteMode mode = ctx->lastDLSMode;
    if (mode == RenderContext::kDLSChord) {
        ctx->reconfigureSynth();
    } else if (mode == RenderContext::kDLSPitchTrack) {
        if (ctx->lastPitchFreq > 0.0f) {
            // Regenerate harmony from last pitch + current harmony mode
            HarmonyMode hmode = static_cast<HarmonyMode>(
                shared->harmonyMode.load(std::memory_order_relaxed)
                % static_cast<int>(HarmonyMode::Count));
            ctx->harmonyGen.mode = hmode;
            if (hmode != HarmonyMode::Off) {
                float harmFreqs[4];
                int harmCount = 0;
                int baseMidi = static_cast<int>(roundf(
                    69.0f + 12.0f * log2f(ctx->lastPitchFreq / 440.0f)));
                ctx->harmonyGen.generateNotes(ctx->lastPitchFreq, baseMidi,
                    ctx->scaleQuantizer, harmFreqs, harmCount);
                if (harmCount > 0) {
                    ctx->carrierFreqCount = harmCount;
                    for (int i = 0; i < harmCount; i++)
                        ctx->carrierFreqs[i] = harmFreqs[i];
                    ctx->reconfigureSynthFromFreqs(harmFreqs, harmCount);
                }
            } else {
                ctx->carrierFreqCount = 1;
                ctx->carrierFreqs[0] = ctx->lastPitchFreq;
                ctx->reconfigureSynthFromFreqs(&ctx->lastPitchFreq, 1);
            }
        } else {
            // No pitch detected yet — silence DLS (don't leave stale chord notes)
            ctx->dlsSynth.allNotesOff();
        }
    } else if (mode == RenderContext::kDLSMidi) {
        if (ctx->carrierFreqCount > 0) {
            ctx->reconfigureSynthFromFreqs(ctx->carrierFreqs, ctx->carrierFreqCount);
        }
    } else {
        // kDLSNone — silence
        ctx->dlsSynth.allNotesOff();
        ctx->carrierFreqCount = 0;
    }
}

/// Update carrier brightness filters (Biquad for saw/noise/buzz, CC74 for DLS).
static inline void renderUpdateBrightness(RenderContext* ctx, SharedState* shared) {
    if (!shared->carrierBrightnessNeedsRedesign.load(std::memory_order_relaxed)) return;

    ctx->sawBrightFilter.designHighShelf(kBrightnessShelfFreq,
        shared->carrierBrightness[kCarrierSaw].load(std::memory_order_relaxed), ctx->sampleRate);
    ctx->noiseBrightFilter.designHighShelf(kBrightnessShelfFreq,
        shared->carrierBrightness[kCarrierNoise].load(std::memory_order_relaxed), ctx->sampleRate);
    ctx->buzzBrightFilter.designHighShelf(kBrightnessShelfFreq,
        shared->carrierBrightness[kCarrierBuzz].load(std::memory_order_relaxed), ctx->sampleRate);

    float brightnesses[kNumDLSCarriers];
    for (int i = 0; i < kNumDLSCarriers; i++)
        brightnesses[i] = shared->carrierBrightness[kCarrierFirstDLS + i].load(std::memory_order_relaxed);
    ctx->dlsSynth.updateBrightness(brightnesses);

    shared->carrierBrightnessNeedsRedesign.store(false, std::memory_order_relaxed);
}

/// Update EQ and compressor parameters from shared state.
static inline void renderUpdateProcessingParams(RenderContext* ctx, SharedState* shared) {
    if (shared->modEqNeedsRedesign.load(std::memory_order_relaxed)) {
        float lo = shared->modEqLow.load(std::memory_order_relaxed);
        float mi = shared->modEqMid.load(std::memory_order_relaxed);
        float hi = shared->modEqHigh.load(std::memory_order_relaxed);
        ctx->modEqLow.designLowShelf(kModEqLowFreq, lo, ctx->sampleRate);
        ctx->modEqMid.designPeakEQ(kModEqMidFreq, 1.0f, mi, ctx->sampleRate);
        ctx->modEqHigh.designHighShelf(kModEqHighFreq, hi, ctx->sampleRate);
        shared->modEqNeedsRedesign.store(false, std::memory_order_relaxed);
    }
    if (shared->outEqNeedsRedesign.load(std::memory_order_relaxed)) {
        float lo = shared->outEqLow.load(std::memory_order_relaxed);
        float mi = shared->outEqMid.load(std::memory_order_relaxed);
        float hi = shared->outEqHigh.load(std::memory_order_relaxed);
        ctx->outEqLowFilter.designLowShelf(kOutEqLowFreq, lo, ctx->sampleRate);
        ctx->outEqMidFilter.designPeakEQ(kOutEqMidFreq, 1.0f, mi, ctx->sampleRate);
        ctx->outEqHighFilter.designHighShelf(kOutEqHighFreq, hi, ctx->sampleRate);
        shared->outEqNeedsRedesign.store(false, std::memory_order_relaxed);
    }
    ctx->modCompressor.setParams(
        shared->modCompThreshold.load(std::memory_order_relaxed),
        shared->modCompRatio.load(std::memory_order_relaxed));
    ctx->outCompressor.setParams(
        shared->outCompThreshold.load(std::memory_order_relaxed),
        shared->outCompRatio.load(std::memory_order_relaxed));
}

/// Process MIDI note input — build carrier frequencies from active + sustained MIDI notes.
static inline void renderProcessMIDINotes(RenderContext* ctx) {
    if (ctx->midi == nullptr) return;
    bool hasActive = ctx->midi->hasActiveNotes.load(std::memory_order_relaxed);
    bool hasSustained = ctx->midi->hasSustainedNotes.load(std::memory_order_relaxed);
    if (!hasActive && !hasSustained) return;

    float midiBaseFreqs[MIDIInput::kMaxNotes];
    int midiCount = 0;

    // Gather active notes
    for (int n = 0; n < MIDIInput::kMaxNotes && midiCount < kMaxChordNotes; n++) {
        int note = ctx->midi->activeNotes[n].load(std::memory_order_relaxed);
        if (note >= 0) {
            midiBaseFreqs[midiCount] = 440.0f * powf(2.0f, (note - 69) / 12.0f);
            midiCount++;
        }
    }

    // Gather sustained notes (skip duplicates of active notes)
    for (int n = 0; n < MIDIInput::kMaxNotes && midiCount < kMaxChordNotes; n++) {
        int note = ctx->midi->sustainedNotes[n].load(std::memory_order_relaxed);
        if (note < 0) continue;
        float freq = 440.0f * powf(2.0f, (note - 69) / 12.0f);
        bool duplicate = false;
        for (int e = 0; e < midiCount; e++) {
            if (fabsf(midiBaseFreqs[e] - freq) < 0.01f) { duplicate = true; break; }
        }
        if (!duplicate) {
            midiBaseFreqs[midiCount] = freq;
            midiCount++;
        }
    }

    if (midiCount > 0) {
        // Check if notes actually changed before retriggering DLS
        bool notesChanged = (midiCount != ctx->carrierFreqCount);
        if (!notesChanged) {
            for (int i = 0; i < midiCount; i++) {
                if (fabsf(midiBaseFreqs[i] - ctx->carrierFreqs[i]) > 0.01f) {
                    notesChanged = true;
                    break;
                }
            }
        }

        // Always update carrier state (saw/buzz use these directly)
        ctx->carrierFreq = midiBaseFreqs[0];
        ctx->carrierFreqCount = midiCount;
        for (int i = 0; i < midiCount; i++)
            ctx->carrierFreqs[i] = midiBaseFreqs[i];

        // Only send DLS note-off/note-on when note set changed
        if (notesChanged)
            ctx->reconfigureSynthFromFreqs(midiBaseFreqs, midiCount);
    }
}

/// Drain excess ring buffer data to cap mic latency (~4ms target).
static inline void renderDrainMicLatency(RenderContext* ctx, SharedState* shared,
                                          bool useMic, UInt32 inNumberFrames) {
    if (!useMic || ctx->micBuffer == nullptr || gMicDeviceSampleRate <= 0.0f) return;

    uint32_t avail = ctx->micBuffer->available();
    uint32_t targetSamples = static_cast<uint32_t>(gMicDeviceSampleRate * kMicTargetLatencySeconds);
    uint32_t maxBuffered = targetSamples + inNumberFrames * 2;
    if (avail > maxBuffered) {
        uint32_t skip = avail - targetSamples;
        ctx->micBuffer->skip(skip);
    }
    float latMs = static_cast<float>(ctx->micBuffer->available()) /
                  gMicDeviceSampleRate * 1000.0f;
    shared->micBufferLatencyMs.store(latMs, std::memory_order_relaxed);
}

/// Get one modulator sample from mic or file source.
static inline float renderGetModulatorSample(RenderContext* ctx, bool useMic) {
    if (useMic && ctx->micBuffer != nullptr) {
        if (ctx->micRateRatio >= 0.999f && ctx->micRateRatio <= 1.001f) {
            return ctx->micBuffer->read();
        } else {
            ctx->micPhaseFrac += ctx->micRateRatio;
            while (ctx->micPhaseFrac >= 1.0f) {
                ctx->micPrevSample = ctx->micCurrSample;
                ctx->micCurrSample = ctx->micBuffer->read();
                ctx->micPhaseFrac -= 1.0f;
            }
            return ctx->micPrevSample +
                (ctx->micCurrSample - ctx->micPrevSample) * ctx->micPhaseFrac;
        }
    } else if (ctx->modulatorFile != nullptr && ctx->modulatorFile->numFrames > 0) {
        float sample = ctx->modulatorFile->samples[ctx->filePlayPos];
        ctx->filePlayPos++;
        if (ctx->filePlayPos >= ctx->modulatorFile->numFrames) {
            ctx->filePlayPos = 0;
        }
        return sample;
    }
    return 0.0f;
}

/// Process modulator chain: noise gate, compressor, 3-band EQ.
static inline float renderProcessModulatorChain(RenderContext* ctx, float modSample,
                                                 float gateThreshold) {
    ctx->micGate.thresholdDb = gateThreshold;
    modSample = ctx->micGate.process(modSample);
    modSample = ctx->modCompressor.process(modSample);
    modSample = ctx->modEqLow.process(modSample);
    modSample = ctx->modEqMid.process(modSample);
    modSample = ctx->modEqHigh.process(modSample);
    return modSample;
}

/// Soft pitch correction: partially corrects deviation from scale notes.
/// snapMode: 0=Off (raw pitch), 1=Soft (partial), 2=Hard (full snap)
/// correctionCentsOut: the amount of correction applied (in cents, signed)
static inline float applySoftPitchCorrection(
    float rawFreq, const ScaleQuantizer& sq,
    int snapMode, float thresholdCents, float amount,
    float& correctionCentsOut)
{
    if (snapMode == 0) { correctionCentsOut = 0.0f; return rawFreq; }  // Off: raw pitch through

    float snappedFreq = sq.quantize(rawFreq);
    float rawCents = 1200.0f * log2f(rawFreq / snappedFreq);  // deviation in cents

    if (snapMode == 2) {
        correctionCentsOut = -rawCents;  // full snap: correct by entire deviation
        return snappedFreq;
    }

    // Soft: partial correction (compressor model)
    if (fabsf(rawCents) > thresholdCents) {
        correctionCentsOut = 0.0f;
        return rawFreq;  // beyond threshold: no correction
    }

    float correctedCents = rawCents * (1.0f - amount);
    correctionCentsOut = -(rawCents * amount);  // amount of correction applied
    return snappedFreq * powf(2.0f, correctedCents / 1200.0f);
}

/// Pitch detection: YIN, median filter, hysteresis, scale quantization, harmony.
static inline void renderUpdatePitchTracking(
    RenderContext* ctx, SharedState* shared, float modSample,
    bool pitchTrackOn, bool midiActive,
    float strongConfThresh, float unvoicedConfThresh, float unvoicedGain,
    float pitchMinHz, float pitchMaxHz,
    int pitchSnapMode, float pitchCorrThreshold, float pitchCorrAmount)
{
    if (!pitchTrackOn || midiActive) return;

    ctx->pitchDetector.pushSample(modSample);
    if (!ctx->pitchDetector.newPitchAvailable) return;
    ctx->pitchDetector.newPitchAvailable = false;

    float conf = ctx->pitchDetector.confidence;
    float freq = ctx->pitchDetector.detectedFreq;
    shared->pitchConfidence.store(conf, std::memory_order_relaxed);

    // Clamp detected frequency to configured pitch range
    freq = std::max(pitchMinHz, std::min(pitchMaxHz, freq));

    if (conf > strongConfThresh) {
        // Strong detection: median-filter the MIDI note, then hysteresis
        float rawMidi = 69.0f + 12.0f * log2f(freq / 440.0f);
        float filteredMidi = ctx->pitchDetector.pushAndMedianFilter(rawMidi);

        bool shouldUpdate = true;
        if (ctx->lastQuantizedMidi >= 0) {
            float dist = fabsf(filteredMidi - static_cast<float>(ctx->lastQuantizedMidi));
            if (dist < kPitchHysteresisSemitones) {
                shouldUpdate = false;
            }
        }

        if (shouldUpdate) {
            float filteredFreq = 440.0f * powf(2.0f, (filteredMidi - 69.0f) / 12.0f);
            float corrCents = 0.0f;
            float quantized = applySoftPitchCorrection(
                filteredFreq, ctx->scaleQuantizer,
                pitchSnapMode, pitchCorrThreshold, pitchCorrAmount,
                corrCents);
            shared->pitchCorrectionCents.store(corrCents, std::memory_order_relaxed);
            ctx->lastPitchFreq = quantized;
            ctx->lastQuantizedMidi = static_cast<int>(roundf(
                69.0f + 12.0f * log2f(quantized / 440.0f)));

            shared->detectedPitch.store(quantized, std::memory_order_relaxed);

            if (ctx->smoothedPitchFreq <= 0.0f) {
                ctx->smoothedPitchFreq = quantized;
            }

            // Update synth from harmony generator
            if (ctx->harmonyGen.mode != HarmonyMode::Off) {
                float baseHarmFreqs[4];
                int harmCount = 0;
                int baseMidiNote = static_cast<int>(roundf(
                    69.0f + 12.0f * log2f(quantized / 440.0f)));
                ctx->harmonyGen.generateNotes(quantized, baseMidiNote,
                    ctx->scaleQuantizer, baseHarmFreqs, harmCount);
                if (harmCount > 0) {
                    ctx->carrierFreqCount = harmCount;
                    for (int hf = 0; hf < harmCount; hf++)
                        ctx->carrierFreqs[hf] = baseHarmFreqs[hf];
                    ctx->reconfigureSynthFromFreqs(baseHarmFreqs, harmCount);
                }
            } else {
                ctx->carrierFreqs[0] = quantized;
                ctx->carrierFreqCount = 1;
                ctx->reconfigureSynthFromFreqs(&quantized, 1);
            }
        }

        ctx->unvoicedNoiseTarget = 0.0f;
    } else if (conf > unvoicedConfThresh && conf <= strongConfThresh) {
        // Weak detection: hold previous note
        ctx->unvoicedNoiseTarget = 0.0f;
    } else {
        // Unvoiced — hold last pitch, blend noise
        if (ctx->lastPitchFreq > 0.0f) {
            ctx->carrierFreq = ctx->lastPitchFreq;
        }
        ctx->unvoicedNoiseTarget = unvoicedGain;
    }
}

/// Per-sample portamento: smooth pitch transitions in log space (~15ms glide).
static inline void renderApplyPortamento(RenderContext* ctx,
                                          bool pitchTrackOn, bool midiActive) {
    // Smooth unvoiced noise level with asymmetric attack/release
    float noiseCoeff = (ctx->unvoicedNoiseTarget > ctx->unvoicedNoiseLevel)
        ? ctx->unvoicedNoiseSmoothAttack    // noise fading in
        : ctx->unvoicedNoiseSmoothRelease;  // noise fading out
    ctx->unvoicedNoiseLevel = noiseCoeff * ctx->unvoicedNoiseLevel +
                              (1.0f - noiseCoeff) * ctx->unvoicedNoiseTarget;

    if (!pitchTrackOn || midiActive || ctx->lastPitchFreq <= 0.0f ||
        ctx->smoothedPitchFreq <= 0.0f) return;

    // Reset smoothed freqs when harmony voice count changes
    if (ctx->carrierFreqCount != ctx->prevCarrierFreqCount) {
        for (int f = 0; f < ctx->carrierFreqCount; f++)
            ctx->smoothedCarrierFreqs[f] = ctx->carrierFreqs[f];
        ctx->prevCarrierFreqCount = ctx->carrierFreqCount;
    }

    // Smooth primary frequency
    float logTarget = log2f(ctx->lastPitchFreq);
    float logSmoothed = log2f(ctx->smoothedPitchFreq);
    logSmoothed += (logTarget - logSmoothed) * kPortamentoAlpha;
    ctx->smoothedPitchFreq = exp2f(logSmoothed);
    ctx->carrierFreq = ctx->smoothedPitchFreq;

    // Smooth all harmony voices independently
    for (int f = 0; f < ctx->carrierFreqCount; f++) {
        if (ctx->smoothedCarrierFreqs[f] <= 0.0f)
            ctx->smoothedCarrierFreqs[f] = ctx->carrierFreqs[f];
        float lt = log2f(ctx->carrierFreqs[f]);
        float ls = log2f(ctx->smoothedCarrierFreqs[f]);
        ls += (lt - ls) * kPortamentoAlpha;
        ctx->smoothedCarrierFreqs[f] = exp2f(ls);
    }
}

/// Generate composite carrier signal from all active carriers.
/// levels[] is indexed by kCarrier* constants (kNumCarriers entries, solo/mute already applied).
static inline float renderGenerateCompositeCarrier(
    RenderContext* ctx, bool pitchTrackOn, bool midiActive,
    bool anyCarrierActive, const float* levels,
    float sawScale, float buzzScale)
{
    float compositeCarrier = 0.0f;

    // Determine un-transposed base carrier frequencies (carrierFreqs[] is single source of truth)
    float baseFreqs[kMaxChordNotes];
    int cFreqCount = ctx->carrierFreqCount;
    if (pitchTrackOn && !midiActive && ctx->smoothedPitchFreq > 0.0f) {
        for (int f = 0; f < cFreqCount; f++)
            baseFreqs[f] = ctx->smoothedCarrierFreqs[f];
    } else {
        for (int f = 0; f < cFreqCount; f++)
            baseFreqs[f] = ctx->carrierFreqs[f];
    }

    // Apply per-carrier transpose at generation time
    if (levels[kCarrierSaw] > 0.0f) {
        float sawFreqs[kMaxChordNotes];
        for (int f = 0; f < cFreqCount; f++)
            sawFreqs[f] = baseFreqs[f] * sawScale;
        float saw = ctx->carrier.generateSawtooth(sawFreqs, cFreqCount, ctx->sampleRate);
        saw = ctx->sawBrightFilter.process(saw);
        compositeCarrier += saw * levels[kCarrierSaw];
    }
    if (levels[kCarrierNoise] > 0.0f && cFreqCount > 0) {
        float noise = ctx->carrier.generateNoise();
        noise = ctx->noiseBrightFilter.process(noise);
        compositeCarrier += noise * levels[kCarrierNoise];
    }
    if (levels[kCarrierBuzz] > 0.0f) {
        float buzzFreqs[kMaxChordNotes];
        for (int f = 0; f < cFreqCount; f++)
            buzzFreqs[f] = baseFreqs[f] * buzzScale;
        float buzz = ctx->carrier.generateBuzz(buzzFreqs, cFreqCount, ctx->sampleRate);
        buzz = ctx->buzzBrightFilter.process(buzz);
        compositeCarrier += buzz * levels[kCarrierBuzz];
    }

    // DLS synth carrier — always read to keep buffer in sync
    {
        float dlsSample = ctx->dlsSynth.readSample();
        if (cFreqCount > 0) {
            compositeCarrier += dlsSample;
        }
    }

    // Unvoiced noise blend (for pitch tracking consonants)
    if (pitchTrackOn && anyCarrierActive && ctx->unvoicedNoiseLevel > 0.001f) {
        compositeCarrier += ctx->carrier.generateNoise() * ctx->unvoicedNoiseLevel;
    }

    return compositeCarrier;
}

/// Per-block DLS pitch bend update — smooth portamento without retrigger.
static inline void renderUpdateDLSPitchBend(RenderContext* ctx, SharedState* shared,
                                              bool pitchTrackOn, bool midiActive) {
    if (!ctx->dlsSynth.initialized || ctx->dlsSynth.currentBaseCount == 0) return;

    // Determine current desired frequencies (same logic as carrier generation)
    const float* desiredFreqs;
    int count;
    if (pitchTrackOn && !midiActive && ctx->smoothedPitchFreq > 0.0f) {
        desiredFreqs = ctx->smoothedCarrierFreqs;
        count = ctx->carrierFreqCount;
    } else {
        desiredFreqs = ctx->carrierFreqs;
        count = ctx->carrierFreqCount;
    }
    if (count <= 0) return;

    int dlsTransposes[kNumDLSCarriers];
    for (int i = 0; i < kNumDLSCarriers; i++)
        dlsTransposes[i] = shared->carrierTranspose[kCarrierFirstDLS + i].load(std::memory_order_relaxed);

    ctx->dlsSynth.updatePitchFromFreqs(desiredFreqs, count, dlsTransposes);
}

/// Update display notes for Page 1 carrier note readout (post-loop).
/// Notes always show what the carrier *would play* — level/mute/solo indicators communicate silence.
static inline void renderUpdateDisplayNotes(
    RenderContext* ctx, SharedState* shared,
    float sawScale, float buzzScale,
    bool pitchTrackOn, bool midiActive)
{
    // Compute display base frequencies (un-transposed) from current state
    int dispFreqCount = ctx->carrierFreqCount;
    float dispFreqs[kMaxChordNotes];
    if (pitchTrackOn && ctx->smoothedPitchFreq > 0.0f) {
        for (int f = 0; f < dispFreqCount; f++)
            dispFreqs[f] = ctx->smoothedCarrierFreqs[f];
    } else {
        for (int f = 0; f < dispFreqCount; f++)
            dispFreqs[f] = ctx->carrierFreqs[f];
    }

    // DLS channels: copy from activeNotes (already transposed)
    for (int ch = 0; ch < kNumDLSCarriers; ch++) {
        int count = std::min(ctx->dlsSynth.activeNoteCounts[ch],
                             static_cast<int>(SharedState::kMaxDisplayNotes));
        for (int n = 0; n < count; n++)
            shared->displayNotes[kCarrierFirstDLS + ch][n].store(
                static_cast<int8_t>(ctx->dlsSynth.activeNotes[ch][n]),
                std::memory_order_relaxed);
        shared->displayNoteCounts[kCarrierFirstDLS + ch].store(
            static_cast<int8_t>(count), std::memory_order_relaxed);
    }

    // Saw: freq * sawScale -> MIDI note (always show when frequencies available)
    if (dispFreqCount > 0) {
        int count = std::min(dispFreqCount,
                             static_cast<int>(SharedState::kMaxDisplayNotes));
        for (int n = 0; n < count; n++) {
            float freq = dispFreqs[n] * sawScale;
            int midi = static_cast<int>(
                roundf(69.0f + 12.0f * log2f(std::max(1.0f, freq) / 440.0f)));
            midi = std::max(0, std::min(127, midi));
            shared->displayNotes[kCarrierSaw][n].store(
                static_cast<int8_t>(midi), std::memory_order_relaxed);
        }
        shared->displayNoteCounts[kCarrierSaw].store(
            static_cast<int8_t>(count), std::memory_order_relaxed);
    } else {
        shared->displayNoteCounts[kCarrierSaw].store(0, std::memory_order_relaxed);
    }

    // Noise: no pitch
    shared->displayNoteCounts[kCarrierNoise].store(0, std::memory_order_relaxed);

    // Buzz: freq * buzzScale -> MIDI note (always show when frequencies available)
    if (dispFreqCount > 0) {
        int count = std::min(dispFreqCount,
                             static_cast<int>(SharedState::kMaxDisplayNotes));
        for (int n = 0; n < count; n++) {
            float freq = dispFreqs[n] * buzzScale;
            int midi = static_cast<int>(
                roundf(69.0f + 12.0f * log2f(std::max(1.0f, freq) / 440.0f)));
            midi = std::max(0, std::min(127, midi));
            shared->displayNotes[kCarrierBuzz][n].store(
                static_cast<int8_t>(midi), std::memory_order_relaxed);
        }
        shared->displayNoteCounts[kCarrierBuzz].store(
            static_cast<int8_t>(count), std::memory_order_relaxed);
    } else {
        shared->displayNoteCounts[kCarrierBuzz].store(0, std::memory_order_relaxed);
    }
}
