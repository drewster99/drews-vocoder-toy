// Drew's Vocoder Toy
// 
// Interactive macOS terminal tool for mixing carrier types and adjusting vocoder parameters live.
//
// Copyright (C) 2026 Nuclear Cyborg Corp. MIT License — see LICENSE file.
// 
// Written by Andrew Benson
// https://github.com/drewster99
// https://www.linkedin.com/in/thedrewbenson/
// 

// Compile: make
// 
// Run: ./drews_vocoder_toy
//
// References:
//   Vocoder deep-dive (carrier design, band analysis, formant preservation):
//   https://www.youtube.com/watch?v=fXhagx4NhwM&t=1s

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdint>
#include <climits>
#include <csignal>
#include <ctime>
#include <atomic>
#include <unistd.h>
#include <termios.h>
#include <sys/time.h>
#include <fcntl.h>
#include <dirent.h>

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreMIDI/CoreMIDI.h>

#include "vocoder_wav_io.h"
#include "vocoder_biquad.h"
#include "vocoder_core.h"
#include "vocoder_pitch.h"
#include "vocoder_carriers.h"
#include "vocoder_dynamics.h"
#include "vocoder_ring_buffer.h"
#include "vocoder_dls_synth.h"
#include "vocoder_midi_input.h"
#include "vocoder_terminal.h"
#include "vocoder_carrier_table.h"
#include "vocoder_constants.h"

// ============================================================================
// Shared State (atomics for lock-free audio<->main thread communication)
// ============================================================================

struct SharedState {
    // Carrier levels (0.0 - 1.0), indexed by kCarriers[] order
    std::atomic<float> carrierLevels[kNumCarriers];

    // Per-carrier brightness (dB, +/-12), indexed same as levels (dry slot unused)
    std::atomic<float> carrierBrightness[kNumCarriers];

    // Per-carrier transpose (semitones), indexed same as levels (dry slot unused)
    std::atomic<int> carrierTranspose[kNumCarriers];

    // Noise gate
    std::atomic<float> micGateThreshold{-40.0f}; // dB, 0 = off

    // Auto-sustain (holds MIDI notes after note-off until new note-on or voice silence)
    std::atomic<bool> autoSustainEnabled{false};
    std::atomic<float> autoSustainThresholdDb{-30.0f};
    std::atomic<float> autoSustainMinTimeMs{10.0f};

    // Chord control
    std::atomic<bool> autoChordEnabled{true};
    std::atomic<int> instrumentIndex{0};  // 0=Organ .. 8=PadPolysynth

    // Flags
    std::atomic<bool> useMic{false};
    std::atomic<AudioFileBuffer*> pendingModulatorFile{nullptr}; // set by main thread, consumed by audio thread
    std::atomic<bool> eqEnabled{true};
    std::atomic<bool> recording{false};
    std::atomic<bool> running{true};

    // Solo mode
    std::atomic<bool> soloActive{false};
    std::atomic<int> soloCarrierIndex{0};

    // Carrier mute
    std::atomic<bool> carrierMuted[kNumCarriers];

    // Monitor mode (hear raw carrier without vocoder)
    std::atomic<bool> monitorMode{false};

    // Dry signal high-pass filter cutoff (Hz), 0 = bypass
    std::atomic<float> dryHPFFreq{kDefaultDryHPFFreq};
    std::atomic<bool> dryHPFNeedsRedesign{true};

    // Compressor gain reduction metering (written by audio thread)
    std::atomic<float> modCompGainReduction{0.0f};
    std::atomic<float> outCompGainReduction{0.0f};

    // Pitch correction metering (written by audio thread)
    std::atomic<float> pitchCorrectionCents{0.0f};

    // Metering (written by audio thread, read by display)
    std::atomic<float> inputPeak{0.0f};
    std::atomic<float> outputPeak{0.0f};
    std::atomic<float> micBufferLatencyMs{0.0f}; // ring buffer latency in ms

    // Audio callback diagnostics (written once by audio thread, read by main thread)
    std::atomic<uint32_t> audioCallbackFrames{0};

    // Dirty flag: set by main thread when chord/instrument changed
    std::atomic<bool> synthNeedsReconfigure{true};

    // --- Pitch tracking ---
    std::atomic<bool> pitchTrackEnabled{false};
    std::atomic<int> harmonyMode{0};          // HarmonyMode enum
    std::atomic<int> scaleType{0};            // ScaleQuantizer::Scale enum
    std::atomic<int> scaleKey{0};             // 0=C through 11=B

    // Unvoiced noise blend (for pitch-tracked consonants)
    std::atomic<float> unvoicedNoiseGain{0.15f};      // 0.0-0.50
    std::atomic<float> unvoicedConfThreshold{0.30f};   // below this confidence → inject noise
    std::atomic<float> strongConfThreshold{0.50f};     // above this confidence → accept pitch

    // Pitch frequency bounds (configurable from Page 4)
    std::atomic<float> pitchMinFreq{50.0f};
    std::atomic<float> pitchMaxFreq{2000.0f};

    // Pitch snap mode: 0=Off (raw pitch), 1=Soft (partial correction), 2=Hard (full snap)
    std::atomic<int> pitchSnapMode{2};          // default: Hard (current behavior)
    std::atomic<float> pitchCorrThreshold{25.0f};  // cents: activation distance
    std::atomic<float> pitchCorrAmount{0.50f};     // 0.0-1.0: correction strength

    // Noise injection attack/release (asymmetric smoothing)
    std::atomic<float> noiseAttackMs{5.0f};    // 1-100ms: how fast noise fades in
    std::atomic<float> noiseReleaseMs{5.0f};   // 1-100ms: how fast noise fades out

    // Pitch detector output (written by audio thread, read by display)
    std::atomic<float> detectedPitch{0.0f};
    std::atomic<float> pitchConfidence{0.0f};

    // --- Modulator processing ---
    std::atomic<float> modCompThreshold{0.0f};  // dB, 0 = off
    std::atomic<float> modCompRatio{4.0f};
    std::atomic<float> modEqLow{0.0f};          // dB
    std::atomic<float> modEqMid{0.0f};
    std::atomic<float> modEqHigh{0.0f};

    // --- Output processing ---
    std::atomic<float> outCompThreshold{0.0f};
    std::atomic<float> outCompRatio{4.0f};
    std::atomic<float> outEqLow{kDefaultOutEqLow};
    std::atomic<float> outEqMid{kDefaultOutEqMid};
    std::atomic<float> outEqHigh{kDefaultOutEqHigh};

    // --- Dirty flags ---
    std::atomic<bool> modEqNeedsRedesign{true};
    std::atomic<bool> outEqNeedsRedesign{true};
    std::atomic<bool> carrierBrightnessNeedsRedesign{true};

    // --- Display-only: current MIDI notes per carrier, updated by audio callback ---
    static constexpr int kMaxDisplayNotes = 8;
    std::atomic<int8_t> displayNotes[kNumCarriers][kMaxDisplayNotes];
    std::atomic<int8_t> displayNoteCounts[kNumCarriers];

    SharedState() {
        // Initialize carrier arrays from descriptor table
        for (int i = 0; i < kNumCarriers; i++) {
            carrierLevels[i].store(kCarriers[i].defaultLevel, std::memory_order_relaxed);
            carrierBrightness[i].store(0.0f, std::memory_order_relaxed);
            carrierTranspose[i].store(0, std::memory_order_relaxed);
            carrierMuted[i].store(false, std::memory_order_relaxed);
            displayNoteCounts[i].store(0, std::memory_order_relaxed);
            for (int n = 0; n < kMaxDisplayNotes; n++)
                displayNotes[i][n].store(-1, std::memory_order_relaxed);
        }
    }
};

// ============================================================================
// Audio Render Context (all DSP state owned by the audio thread)
// ============================================================================

struct RenderContext {
    // Input
    AudioFileBuffer* modulatorFile = nullptr;
    size_t filePlayPos = 0;

    // DSP
    ChannelVocoder vocoder;
    SimpleCarrier carrier;
    DLSSynthState dlsSynth;

    // Pitch tracking
    PitchDetector pitchDetector;
    ScaleQuantizer scaleQuantizer;
    HarmonyGenerator harmonyGen;
    float carrierFreq = 110.0f;         // primary carrier frequency (for display/reference)
    float carrierFreqs[kMaxChordNotes]; // un-transposed base carrier frequencies
    float smoothedCarrierFreqs[kMaxChordNotes]; // portamento-smoothed base frequencies
    int carrierFreqCount = 1;           // number of active carrier frequencies
    int prevCarrierFreqCount = 0;       // previous count (for detecting harmony mode changes)
    float lastPitchFreq = 0.0f;         // last detected pitch, un-transposed (held across unvoiced)
    int lastQuantizedMidi = -1;          // hysteresis: last snapped MIDI note (-1 = none)
    float smoothedPitchFreq = 0.0f;     // portamento: smoothed primary carrier frequency (un-transposed)
    float unvoicedNoiseLevel = 0.0f;    // smoothed noise blend for unvoiced segments
    float unvoicedNoiseTarget = 0.0f;

    // DLS note source tracking (for mode transition detection)
    enum DLSNoteMode { kDLSChord, kDLSPitchTrack, kDLSMidi, kDLSNone };
    DLSNoteMode lastDLSMode = kDLSNone;

    // MIDI
    MIDIInput* midi = nullptr;

    // Modulator processing
    NoiseGate micGate;
    SimpleCompressor modCompressor;
    Biquad modEqLow, modEqMid, modEqHigh;

    // Per-carrier brightness filters (high-shelf) — saw/noise/buzz only
    // DLS instrument brightness is handled via MIDI CC74 per channel
    Biquad sawBrightFilter, noiseBrightFilter, buzzBrightFilter;

    // Dry signal HPF
    Biquad dryHPF;
    float dryHPFCurrentFreq = 0.0f;  // track for redesign detection

    // Output processing
    SimpleCompressor outCompressor;
    Biquad outEqLowFilter, outEqMidFilter, outEqHighFilter;

    // State
    SharedState* shared = nullptr;
    RingBuffer* micBuffer = nullptr;

    // Mic sample rate conversion (linear interpolation resampler)
    float micRateRatio = 1.0f;      // deviceSR / outputSR (< 1 means upsampling)
    float micPhaseFrac = 0.0f;      // fractional position between input samples
    float micPrevSample = 0.0f;     // previous input sample
    float micCurrSample = 0.0f;     // current input sample

    // Cached values
    float sampleRate = 44100.0f;

    // Streaming recording (audio thread → main thread via ring buffer → disk)
    RecordRingBuffer recordRing;
    FILE* recordFile = nullptr;           // opened WAV file (main thread only)
    char recordFilename[64] = {};         // current recording filename
    std::atomic<int64_t> recordSampleCount{0}; // total samples written (by audio thread)
    std::atomic<bool> recordingOverflow{false}; // set if ring buffer overflows

    // Auto-sustain threshold monitoring (audio-thread-only, no atomics needed)
    float sustainModEnvelope = 0.0f;
    float sustainModEnvCoeff = 0.0f;       // ~2ms release coefficient
    int sustainBelowThresholdCount = 0;
    int sustainBelowThresholdTarget = 0;   // minTimeMs * sr / 1000
    bool prevAutoSustainEnabled = false;    // for edge detection on disable

    // Peak metering (exponential decay)
    float inputPeakSmooth = 0.0f;
    float outputPeakSmooth = 0.0f;
    float peakDecay = 0.0f;
    float unvoicedNoiseSmoothAttack = 0.0f;   // coeff when target > current (noise in)
    float unvoicedNoiseSmoothRelease = 0.0f;  // coeff when target < current (noise out)
    int meterCounter = 0;

    void init(float sr) {
        sampleRate = sr;
        for (int i = 0; i < kMaxChordNotes; i++) {
            carrierFreqs[i] = 110.0f;
            smoothedCarrierFreqs[i] = 110.0f;
        }
        vocoder.init(kDefaultNumBands, sr);
        peakDecay = expf(-1.0f / (kPeakDecayCoeff * sr)); // 100ms decay
        // Default 5ms symmetric smoothing (will be overridden per-block from SharedState)
        unvoicedNoiseSmoothAttack = expf(-1.0f / (kUnvoicedNoiseSmoothCoeff * sr));
        unvoicedNoiseSmoothRelease = unvoicedNoiseSmoothAttack;

        // Auto-sustain envelope follower (~2ms release)
        sustainModEnvCoeff = expf(-1.0f / (0.002f * sr));
        sustainBelowThresholdTarget = static_cast<int>(0.010f * sr);

        // Init pitch detector with runtime-sized buffers for this sample rate
        pitchDetector.init(sr);

        // Init noise gate and compressors
        micGate.init(sr);
        modCompressor.init(sr);
        outCompressor.init(sr);

        // Init modulator EQ (flat)
        modEqLow.designLowShelf(kModEqLowFreq, 0.0f, sr);
        modEqMid.designPeakEQ(kModEqMidFreq, 1.0f, 0.0f, sr);
        modEqHigh.designHighShelf(kModEqHighFreq, 0.0f, sr);

        // Init output EQ with defaults
        outEqLowFilter.designLowShelf(kOutEqLowFreq, kDefaultOutEqLow, sr);
        outEqMidFilter.designPeakEQ(kOutEqMidFreq, 1.0f, kDefaultOutEqMid, sr);
        outEqHighFilter.designHighShelf(kOutEqHighFreq, kDefaultOutEqHigh, sr);

        // Init brightness filters (flat) — saw/noise/buzz only
        // DLS instrument brightness handled via MIDI CC74 per channel
        sawBrightFilter.designHighShelf(2000.0f, 0.0f, sr);
        noiseBrightFilter.designHighShelf(2000.0f, 0.0f, sr);
        buzzBrightFilter.designHighShelf(2000.0f, 0.0f, sr);

        // Init dry HPF
        dryHPF.designHighPass(kDefaultDryHPFFreq, 0.707f, sr);
        dryHPFCurrentFreq = kDefaultDryHPFFreq;
    }

    void reconfigureSynth() {
        // Populate carrierFreqs[] as single source of truth for ALL carriers
        static const int baseNotes[3] = {48, 52, 55};  // C3-E3-G3
        carrierFreqCount = 3;
        for (int i = 0; i < 3; i++)
            carrierFreqs[i] = 440.0f * powf(2.0f, (baseNotes[i] - 69) / 12.0f);
        reconfigureSynthFromFreqs(carrierFreqs, 3);
    }

    void reconfigureSynthFromFreqs(const float* baseFreqs, int numFreqs) {
        int dlsTransposes[kNumDLSCarriers];
        for (int i = 0; i < kNumDLSCarriers; i++)
            dlsTransposes[i] = shared->carrierTranspose[kCarrierFirstDLS + i].load(std::memory_order_relaxed);
        // Force retrigger: this function is called on explicit reconfigure events
        // (transpose change, note change, mode change), not on smooth per-block updates.
        // Without this, updatePitchFromFreqs would skip retrigger when only transpose changed
        // (since base frequencies are identical, it would see no pitch deviation).
        dlsSynth.currentBaseCount = 0;
        dlsSynth.updatePitchFromFreqs(baseFreqs, numFreqs, dlsTransposes);
    }
};

// ============================================================================
// CoreAudio Output Render Callback
// ============================================================================

// Mic device sample rate (set during input setup, used by output callback for latency drain)
static float gMicDeviceSampleRate = 0.0f;

#include "vocoder_render.h"

static OSStatus outputRenderCallback(
    void* inRefCon,
    AudioUnitRenderActionFlags* /*ioActionFlags*/,
    const AudioTimeStamp* /*inTimeStamp*/,
    UInt32 /*inBusNumber*/,
    UInt32 inNumberFrames,
    AudioBufferList* ioData)
{
    auto* ctx = static_cast<RenderContext*>(inRefCon);
    auto* shared = ctx->shared;

    // Report callback buffer size to main thread (once, lock-free)
    if (shared->audioCallbackFrames.load(std::memory_order_relaxed) == 0) {
        shared->audioCallbackFrames.store(inNumberFrames, std::memory_order_relaxed);
    }

    float* outBuf = static_cast<float*>(ioData->mBuffers[0].mData);

    // --- Per-block setup ---
    renderUpdateDLSMode(ctx, shared);
    renderHandleSynthReconfigure(ctx, shared);

    // --- Load all carrier levels into unified array, apply solo/mute ---
    float levels[kNumCarriers];
    for (int i = 0; i < kNumCarriers; i++)
        levels[i] = shared->carrierLevels[i].load(std::memory_order_relaxed);

    // Solo mode: zero out all carriers except the solo'd one
    bool soloOn = shared->soloActive.load(std::memory_order_relaxed);
    int soloIdx = shared->soloCarrierIndex.load(std::memory_order_relaxed);
    if (soloOn) {
        for (int i = 0; i < kNumCarriers; i++)
            if (i != soloIdx) levels[i] = 0.0f;
    }

    // Mute: zero out muted carriers
    for (int i = 0; i < kNumCarriers; i++) {
        if (shared->carrierMuted[i].load(std::memory_order_relaxed))
            levels[i] = 0.0f;
    }

    // Monitor mode: bypass vocoder so you hear raw carrier signal.
    // Does NOT affect dry level — dry is already post-vocoder by nature.
    bool monitorOn = shared->monitorMode.load(std::memory_order_relaxed);

    // Send final (solo/mute-adjusted) DLS volumes, then render
    ctx->dlsSynth.updateVolumes(&levels[kCarrierFirstDLS]);
    renderUpdateBrightness(ctx, shared);
    ctx->dlsSynth.renderBlock(static_cast<int>(inNumberFrames));

    renderUpdateProcessingParams(ctx, shared);

    bool useEq = shared->eqEnabled.load(std::memory_order_relaxed);
    bool useMic = shared->useMic.load(std::memory_order_relaxed);
    float gateThreshold = shared->micGateThreshold.load(std::memory_order_relaxed);

    // Check for pending modulator file swap (set by main thread, consumed here)
    AudioFileBuffer* pending = shared->pendingModulatorFile.load(std::memory_order_acquire);
    if (pending != nullptr) {
        ctx->modulatorFile = pending;
        ctx->filePlayPos = 0;
        shared->pendingModulatorFile.store(nullptr, std::memory_order_release);
    }

    // Dry HPF redesign check
    float dryHPFFreq = shared->dryHPFFreq.load(std::memory_order_relaxed);
    if (shared->dryHPFNeedsRedesign.load(std::memory_order_relaxed) ||
        fabsf(dryHPFFreq - ctx->dryHPFCurrentFreq) > 0.5f) {
        if (dryHPFFreq > 1.0f) {
            ctx->dryHPF.designHighPass(dryHPFFreq, 0.707f, ctx->sampleRate);
        }
        ctx->dryHPFCurrentFreq = dryHPFFreq;
        shared->dryHPFNeedsRedesign.store(false, std::memory_order_relaxed);
    }

    bool anyCarrierActive = false;
    for (int i = 0; i < kNumPitchedCarriers; i++) {
        if (levels[i] > 0.001f) { anyCarrierActive = true; break; }
    }

    bool pitchTrackOn = shared->pitchTrackEnabled.load(std::memory_order_relaxed);
    ctx->scaleQuantizer.scale = static_cast<ScaleQuantizer::Scale>(
        shared->scaleType.load(std::memory_order_relaxed) % ScaleQuantizer::Count);
    ctx->scaleQuantizer.keyNote = shared->scaleKey.load(std::memory_order_relaxed) % 12;
    ctx->harmonyGen.mode = static_cast<HarmonyMode>(
        shared->harmonyMode.load(std::memory_order_relaxed) % static_cast<int>(HarmonyMode::Count));
    float unvoicedGain = shared->unvoicedNoiseGain.load(std::memory_order_relaxed);
    float unvoicedConfThresh = shared->unvoicedConfThreshold.load(std::memory_order_relaxed);
    float strongConfThresh = shared->strongConfThreshold.load(std::memory_order_relaxed);
    float pitchMinHz = shared->pitchMinFreq.load(std::memory_order_relaxed);
    float pitchMaxHz = shared->pitchMaxFreq.load(std::memory_order_relaxed);

    int snapMode = shared->pitchSnapMode.load(std::memory_order_relaxed);
    float corrThreshold = shared->pitchCorrThreshold.load(std::memory_order_relaxed);
    float corrAmount = shared->pitchCorrAmount.load(std::memory_order_relaxed);

    // --- Auto-sustain per-block setup ---
    bool autoSustainOn = shared->autoSustainEnabled.load(std::memory_order_relaxed);
    if (ctx->midi != nullptr) {
        ctx->midi->autoSustainEnabled.store(autoSustainOn, std::memory_order_relaxed);

        // Edge: auto-sustain just disabled while notes are sustained → release immediately
        if (!autoSustainOn && ctx->prevAutoSustainEnabled &&
            ctx->midi->hasSustainedNotes.load(std::memory_order_relaxed)) {
            ctx->midi->releaseSustainedNotes();
        }
        ctx->prevAutoSustainEnabled = autoSustainOn;

        // Recompute threshold target from ms setting (infinity = never release on silence)
        float sustainMinMs = shared->autoSustainMinTimeMs.load(std::memory_order_relaxed);
        if (sustainMinMs >= kSustainTimeInfinityMs) {
            ctx->sustainBelowThresholdTarget = INT_MAX;
        } else {
            ctx->sustainBelowThresholdTarget = static_cast<int>(sustainMinMs * 0.001f * ctx->sampleRate);
        }
    }
    float sustainThreshLinear = 0.0f;
    if (autoSustainOn) {
        float threshDb = shared->autoSustainThresholdDb.load(std::memory_order_relaxed);
        sustainThreshLinear = powf(10.0f, threshDb / 20.0f);
    }

    bool midiActive = (ctx->midi != nullptr &&
                       (ctx->midi->hasActiveNotes.load(std::memory_order_relaxed) ||
                        ctx->midi->hasSustainedNotes.load(std::memory_order_relaxed)));
    bool autoChord = shared->autoChordEnabled.load(std::memory_order_relaxed);
    if (midiActive && autoChord) {
        shared->autoChordEnabled.store(false, std::memory_order_relaxed);
        autoChord = false;
    }

    int sawTranspose = shared->carrierTranspose[kCarrierSaw].load(std::memory_order_relaxed);
    int buzzTranspose = shared->carrierTranspose[kCarrierBuzz].load(std::memory_order_relaxed);

    if (midiActive) renderProcessMIDINotes(ctx);

    float sawScale = powf(2.0f, sawTranspose / 12.0f);
    float buzzScale = powf(2.0f, buzzTranspose / 12.0f);

    // Compute asymmetric noise smoothing coefficients per-block
    float sr = ctx->sampleRate;
    {
        float attackMs  = shared->noiseAttackMs.load(std::memory_order_relaxed);
        float releaseMs = shared->noiseReleaseMs.load(std::memory_order_relaxed);
        ctx->unvoicedNoiseSmoothAttack  = expf(-1.0f / (attackMs  * 0.001f * sr));
        ctx->unvoicedNoiseSmoothRelease = expf(-1.0f / (releaseMs * 0.001f * sr));
    }
    bool isRecording = shared->recording.load(std::memory_order_acquire);

    renderDrainMicLatency(ctx, shared, useMic, inNumberFrames);

    // --- Per-sample loop ---
    for (UInt32 i = 0; i < inNumberFrames; i++) {
        float modSample = renderGetModulatorSample(ctx, useMic);
        float rawModSample = modSample;
        modSample = renderProcessModulatorChain(ctx, modSample, gateThreshold);

        // Auto-sustain: monitor modulator level to release sustained notes on silence
        if (autoSustainOn && ctx->midi != nullptr &&
            ctx->midi->hasSustainedNotes.load(std::memory_order_relaxed)) {
            float absmod = fabsf(modSample);
            // Envelope follower: instant attack, ~2ms release
            if (absmod > ctx->sustainModEnvelope)
                ctx->sustainModEnvelope = absmod;
            else
                ctx->sustainModEnvelope = ctx->sustainModEnvCoeff * ctx->sustainModEnvelope +
                                          (1.0f - ctx->sustainModEnvCoeff) * absmod;

            if (ctx->sustainModEnvelope < sustainThreshLinear) {
                ctx->sustainBelowThresholdCount++;
                if (ctx->sustainBelowThresholdCount >= ctx->sustainBelowThresholdTarget) {
                    ctx->midi->releaseSustainedNotes();
                    ctx->sustainBelowThresholdCount = 0;
                }
            } else {
                ctx->sustainBelowThresholdCount = 0;
            }
        }

        renderUpdatePitchTracking(ctx, shared, modSample, pitchTrackOn, midiActive,
                                  strongConfThresh, unvoicedConfThresh, unvoicedGain,
                                  pitchMinHz, pitchMaxHz,
                                  snapMode, corrThreshold, corrAmount);
        renderApplyPortamento(ctx, pitchTrackOn, midiActive);

        float compositeCarrier = renderGenerateCompositeCarrier(
            ctx, pitchTrackOn, midiActive, anyCarrierActive,
            levels, sawScale, buzzScale);

        // --- Output processing ---
        float vocoded;
        if (monitorOn) {
            vocoded = compositeCarrier;  // bypass vocoder, hear raw carrier
        } else {
            vocoded = ctx->vocoder.processSample(modSample, compositeCarrier);
            // Suppress vocoder output when no pitched carrier is active — prevents
            // DLS residual / filter ringing from leaking modulator-shaped signal
            if (!anyCarrierActive) vocoded = 0.0f;
        }
        if (useEq) {
            vocoded = ctx->outEqLowFilter.process(vocoded);
            vocoded = ctx->outEqMidFilter.process(vocoded);
            vocoded = ctx->outEqHighFilter.process(vocoded);
        }
        vocoded = ctx->outCompressor.process(vocoded);

        // Apply dry HPF before mixing (always process to keep filter state warm)
        float drySignal = rawModSample;
        if (dryHPFFreq > 1.0f) {
            drySignal = ctx->dryHPF.process(drySignal);
        }

        float output = vocoded + drySignal * levels[kCarrierDry];
        output = softClip(output);
        outBuf[i] = output;

        if (isRecording) {
            if (!ctx->recordRing.writeSingle(output)) {
                ctx->recordingOverflow.store(true, std::memory_order_relaxed);
            }
        }

        float inAbs = fabsf(modSample);
        float outAbs = fabsf(output);
        ctx->inputPeakSmooth = std::max(inAbs, ctx->inputPeakSmooth * ctx->peakDecay);
        ctx->outputPeakSmooth = std::max(outAbs, ctx->outputPeakSmooth * ctx->peakDecay);
    }

    // --- Post-loop ---
    shared->inputPeak.store(ctx->inputPeakSmooth, std::memory_order_relaxed);
    shared->outputPeak.store(ctx->outputPeakSmooth, std::memory_order_relaxed);

    // Compressor GR metering
    shared->modCompGainReduction.store(ctx->modCompressor.lastGainDb, std::memory_order_relaxed);
    shared->outCompGainReduction.store(ctx->outCompressor.lastGainDb, std::memory_order_relaxed);

    // DLS pitch bend update (once per block, after portamento smoothing)
    renderUpdateDLSPitchBend(ctx, shared, pitchTrackOn, midiActive);

    renderUpdateDisplayNotes(ctx, shared, sawScale, buzzScale,
                             pitchTrackOn, midiActive);

    return noErr;
}


// ============================================================================
// CoreAudio Mic Input Callback
// ============================================================================

static AudioComponentInstance gInputUnit = nullptr;
static RingBuffer gMicRingBuffer;
static std::atomic<uint32_t> gMicCallbackCount{0};
static std::atomic<int32_t> gMicLastError{0};
static char gMicDeviceName[128] = {};

static OSStatus inputRenderCallback(
    void* inRefCon,
    AudioUnitRenderActionFlags* ioActionFlags,
    const AudioTimeStamp* inTimeStamp,
    UInt32 /*inBusNumber*/,
    UInt32 inNumberFrames,
    AudioBufferList* /*ioData*/)
{
    auto* ringBuf = static_cast<RingBuffer*>(inRefCon);

    // Allocate a temporary buffer to receive input
    float tempBuf[kMaxCallbackFrames];
    if (inNumberFrames > kMaxCallbackFrames) inNumberFrames = kMaxCallbackFrames;

    AudioBufferList bufList;
    bufList.mNumberBuffers = 1;
    bufList.mBuffers[0].mNumberChannels = 1;
    bufList.mBuffers[0].mDataByteSize = inNumberFrames * sizeof(float);
    bufList.mBuffers[0].mData = tempBuf;

    // Always use bus 1 for input (inBusNumber may be 0 from the callback registration)
    OSStatus status = AudioUnitRender(gInputUnit, ioActionFlags, inTimeStamp,
                                       1, inNumberFrames, &bufList);
    gMicCallbackCount.fetch_add(1, std::memory_order_relaxed);
    if (status == noErr) {
        ringBuf->write(tempBuf, static_cast<int>(inNumberFrames));
    } else {
        gMicLastError.store(static_cast<int32_t>(status), std::memory_order_relaxed);
    }

    return noErr;
}

// ============================================================================
// CoreAudio Setup
// ============================================================================

static AudioComponentInstance gOutputUnit = nullptr;

static bool setupOutputUnit(RenderContext* ctx, float sampleRate) {
    AudioComponentDescription desc = {};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_DefaultOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (!comp) {
        fprintf(stderr, "ERROR: Cannot find default output audio component\n");
        return false;
    }

    OSStatus status = AudioComponentInstanceNew(comp, &gOutputUnit);
    if (status != noErr) {
        fprintf(stderr, "ERROR: Cannot create output audio unit (status %d)\n", static_cast<int>(status));
        return false;
    }

    // Set format: mono float32 at desired sample rate
    AudioStreamBasicDescription fmt = {};
    fmt.mSampleRate = sampleRate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
    fmt.mBytesPerPacket = sizeof(float);
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = sizeof(float);
    fmt.mChannelsPerFrame = 1;
    fmt.mBitsPerChannel = 32;

    status = AudioUnitSetProperty(gOutputUnit, kAudioUnitProperty_StreamFormat,
                                   kAudioUnitScope_Input, 0, &fmt, sizeof(fmt));
    if (status != noErr) {
        fprintf(stderr, "ERROR: Cannot set output format (status %d)\n", static_cast<int>(status));
        AudioComponentInstanceDispose(gOutputUnit);
        gOutputUnit = nullptr;
        return false;
    }

    // Set render callback
    AURenderCallbackStruct cb = {};
    cb.inputProc = outputRenderCallback;
    cb.inputProcRefCon = ctx;

    status = AudioUnitSetProperty(gOutputUnit, kAudioUnitProperty_SetRenderCallback,
                                   kAudioUnitScope_Input, 0, &cb, sizeof(cb));
    if (status != noErr) {
        fprintf(stderr, "ERROR: Cannot set render callback (status %d)\n", static_cast<int>(status));
        AudioComponentInstanceDispose(gOutputUnit);
        gOutputUnit = nullptr;
        return false;
    }

    // Request small buffer for low latency
    {
        UInt32 desiredFrames = kDesiredAudioBufferFrames;
        AudioObjectPropertyAddress prop = {
            kAudioDevicePropertyBufferFrameSize,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        AudioDeviceID outputDevice = 0;
        UInt32 devSize = sizeof(outputDevice);
        AudioObjectPropertyAddress defaultProp = {
            kAudioHardwarePropertyDefaultOutputDevice,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &defaultProp,
                                        0, nullptr, &devSize, &outputDevice) == noErr) {
            // Read current (default) size first
            UInt32 currentFrames = 0;
            UInt32 frameSize = sizeof(currentFrames);
            OSStatus readStatus = AudioObjectGetPropertyData(outputDevice, &prop, 0, nullptr, &frameSize, &currentFrames);
            if (readStatus != noErr) {
                fprintf(stderr, "WARNING: Could not read output buffer size (status %d)\n", static_cast<int>(readStatus));
            }
            printf("Audio buffer: default = %u frames (%.1f ms)\n",
                   currentFrames, static_cast<float>(currentFrames) / sampleRate * 1000.0f);

            // Now request smaller buffer
            OSStatus bufStatus = AudioObjectSetPropertyData(
                outputDevice, &prop, 0, nullptr, sizeof(desiredFrames), &desiredFrames);
            UInt32 actualFrames = 0;
            frameSize = sizeof(actualFrames);
            readStatus = AudioObjectGetPropertyData(outputDevice, &prop, 0, nullptr, &frameSize, &actualFrames);
            if (readStatus != noErr) {
                fprintf(stderr, "WARNING: Could not read actual buffer size (status %d)\n", static_cast<int>(readStatus));
            }
            printf("Audio buffer: requested %u, got %u frames (%.1f ms)\n",
                   desiredFrames, actualFrames,
                   static_cast<float>(actualFrames) / sampleRate * 1000.0f);
            if (bufStatus != noErr) {
                printf("  (buffer size request returned status %d)\n", static_cast<int>(bufStatus));
            }
        }
    }

    status = AudioUnitInitialize(gOutputUnit);
    if (status != noErr) {
        fprintf(stderr, "ERROR: Cannot initialize output unit (status %d)\n", static_cast<int>(status));
        AudioComponentInstanceDispose(gOutputUnit);
        gOutputUnit = nullptr;
        return false;
    }

    return true;
}

static bool setupInputUnit(RingBuffer* ringBuf, float sampleRate) {
    AudioComponentDescription desc = {};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (!comp) {
        fprintf(stderr, "WARNING: Cannot find HAL output component for mic input\n");
        return false;
    }

    OSStatus status = AudioComponentInstanceNew(comp, &gInputUnit);
    if (status != noErr) {
        fprintf(stderr, "WARNING: Cannot create input audio unit (status %d)\n", static_cast<int>(status));
        return false;
    }

    // Enable input on bus 1 (must be done before setting device)
    UInt32 enableIO = 1;
    status = AudioUnitSetProperty(gInputUnit, kAudioOutputUnitProperty_EnableIO,
                                   kAudioUnitScope_Input, 1, &enableIO, sizeof(enableIO));
    if (status != noErr) {
        fprintf(stderr, "WARNING: Cannot enable mic input (status %d)\n", static_cast<int>(status));
        AudioComponentInstanceDispose(gInputUnit);
        gInputUnit = nullptr;
        return false;
    }

    // Disable output on bus 0
    UInt32 disableIO = 0;
    status = AudioUnitSetProperty(gInputUnit, kAudioOutputUnitProperty_EnableIO,
                                   kAudioUnitScope_Output, 0, &disableIO, sizeof(disableIO));
    if (status != noErr) {
        fprintf(stderr, "WARNING: Cannot disable output on input unit (status %d)\n", static_cast<int>(status));
        AudioComponentInstanceDispose(gInputUnit);
        gInputUnit = nullptr;
        return false;
    }

    // Set the default input device explicitly (HAL unit defaults to output device)
    AudioObjectPropertyAddress propAddr = {
        kAudioHardwarePropertyDefaultInputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioDeviceID inputDevice = 0;
    UInt32 propSize = sizeof(AudioDeviceID);
    status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &propAddr,
                                         0, nullptr, &propSize, &inputDevice);
    if (status != noErr || inputDevice == 0) {
        fprintf(stderr, "WARNING: Cannot get default input device (status %d)\n", static_cast<int>(status));
        AudioComponentInstanceDispose(gInputUnit);
        gInputUnit = nullptr;
        return false;
    }

    status = AudioUnitSetProperty(gInputUnit, kAudioOutputUnitProperty_CurrentDevice,
                                   kAudioUnitScope_Global, 0, &inputDevice, sizeof(inputDevice));
    if (status != noErr) {
        fprintf(stderr, "WARNING: Cannot set input device (status %d)\n", static_cast<int>(status));
        AudioComponentInstanceDispose(gInputUnit);
        gInputUnit = nullptr;
        return false;
    }

    // Request small input buffer to reduce latency
    {
        AudioObjectPropertyAddress bufProp = {
            kAudioDevicePropertyBufferFrameSize,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 desiredInputFrames = kDesiredAudioBufferFrames;
        AudioObjectSetPropertyData(inputDevice, &bufProp, 0, nullptr,
                                   sizeof(desiredInputFrames), &desiredInputFrames);
        UInt32 actualInputFrames = 0;
        UInt32 bufSz = sizeof(actualInputFrames);
        AudioObjectGetPropertyData(inputDevice, &bufProp, 0, nullptr,
                                   &bufSz, &actualInputFrames);
        printf("  Input buffer: requested %u, got %u frames\n",
               static_cast<unsigned>(kDesiredAudioBufferFrames), static_cast<unsigned>(actualInputFrames));
    }

    // Query device native sample rate (needed for format setup)
    Float64 deviceSR = sampleRate;
    propSize = sizeof(Float64);
    AudioObjectPropertyAddress srAddr = {
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    {
        OSStatus srStatus = AudioObjectGetPropertyData(inputDevice, &srAddr, 0, nullptr, &propSize, &deviceSR);
        if (srStatus != noErr) {
            fprintf(stderr, "WARNING: Could not read mic sample rate (status %d), assuming %.0f Hz\n",
                    static_cast<int>(srStatus), sampleRate);
            deviceSR = sampleRate;  // ratio becomes 1.0 — resampling effectively disabled
        }
    }

    // Print device diagnostics
    {
        // Device name
        CFStringRef deviceName = nullptr;
        propSize = sizeof(CFStringRef);
        AudioObjectPropertyAddress nameAddr = {
            kAudioDevicePropertyDeviceNameCFString,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        if (AudioObjectGetPropertyData(inputDevice, &nameAddr, 0, nullptr, &propSize, &deviceName) == noErr
            && deviceName != nullptr) {
            char nameBuf[256];
            CFStringGetCString(deviceName, nameBuf, sizeof(nameBuf), kCFStringEncodingUTF8);
            printf("  Input device: \"%s\" (ID %u)\n", nameBuf, static_cast<unsigned>(inputDevice));
            snprintf(gMicDeviceName, sizeof(gMicDeviceName), "%s", nameBuf);
            CFRelease(deviceName);
        } else {
            printf("  Input device ID: %u (name unavailable)\n", static_cast<unsigned>(inputDevice));
            snprintf(gMicDeviceName, sizeof(gMicDeviceName), "Device %u", static_cast<unsigned>(inputDevice));
        }

        gMicDeviceSampleRate = static_cast<float>(deviceSR);
        printf("  Device sample rate: %.0f Hz (requested: %.0f Hz)\n", deviceSR, static_cast<double>(sampleRate));

        // Number of input channels (input scope)
        AudioObjectPropertyAddress chAddr = {
            kAudioDevicePropertyStreamConfiguration,
            kAudioObjectPropertyScopeInput,
            kAudioObjectPropertyElementMain
        };
        propSize = 0;
        if (AudioObjectGetPropertyDataSize(inputDevice, &chAddr, 0, nullptr, &propSize) == noErr && propSize > 0) {
            std::vector<uint8_t> buf(propSize);
            auto* bufList = reinterpret_cast<AudioBufferList*>(buf.data());
            if (AudioObjectGetPropertyData(inputDevice, &chAddr, 0, nullptr, &propSize, bufList) == noErr) {
                int totalCh = 0;
                for (UInt32 b = 0; b < bufList->mNumberBuffers; b++) {
                    totalCh += bufList->mBuffers[b].mNumberChannels;
                }
                printf("  Input channels: %d\n", totalCh);
                if (totalCh == 0) {
                    fprintf(stderr, "WARNING: Input device has 0 input channels!\n");
                }
            }
        }

        // Check hardware format on input scope of bus 1
        AudioStreamBasicDescription hwFmt = {};
        propSize = sizeof(hwFmt);
        if (AudioUnitGetProperty(gInputUnit, kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Input, 1, &hwFmt, &propSize) == noErr) {
            printf("  Hardware format: %.0f Hz, %u ch, %u bits\n",
                   hwFmt.mSampleRate,
                   static_cast<unsigned>(hwFmt.mChannelsPerFrame),
                   static_cast<unsigned>(hwFmt.mBitsPerChannel));
        }
    }

    // Set format on output scope of bus 1 (application side)
    // Use the device's native sample rate to avoid AUHAL sample rate conversion issues
    AudioStreamBasicDescription fmt = {};
    fmt.mSampleRate = deviceSR;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
    fmt.mBytesPerPacket = sizeof(float);
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = sizeof(float);
    fmt.mChannelsPerFrame = 1;
    fmt.mBitsPerChannel = 32;

    status = AudioUnitSetProperty(gInputUnit, kAudioUnitProperty_StreamFormat,
                                   kAudioUnitScope_Output, 1, &fmt, sizeof(fmt));
    if (status != noErr) {
        fprintf(stderr, "WARNING: Cannot set input format (status %d)\n", static_cast<int>(status));
        AudioComponentInstanceDispose(gInputUnit);
        gInputUnit = nullptr;
        return false;
    }

    // Set input callback
    AURenderCallbackStruct cb = {};
    cb.inputProc = inputRenderCallback;
    cb.inputProcRefCon = ringBuf;

    status = AudioUnitSetProperty(gInputUnit, kAudioOutputUnitProperty_SetInputCallback,
                                   kAudioUnitScope_Global, 0, &cb, sizeof(cb));
    if (status != noErr) {
        fprintf(stderr, "WARNING: Cannot set input callback (status %d)\n", static_cast<int>(status));
        AudioComponentInstanceDispose(gInputUnit);
        gInputUnit = nullptr;
        return false;
    }

    status = AudioUnitInitialize(gInputUnit);
    if (status != noErr) {
        fprintf(stderr, "WARNING: Cannot initialize input unit (status %d)\n", static_cast<int>(status));
        AudioComponentInstanceDispose(gInputUnit);
        gInputUnit = nullptr;
        return false;
    }

    status = AudioOutputUnitStart(gInputUnit);
    if (status != noErr) {
        fprintf(stderr, "WARNING: Cannot start input unit (status %d)\n", static_cast<int>(status));
        AudioComponentInstanceDispose(gInputUnit);
        gInputUnit = nullptr;
        return false;
    }

    return true;
}


#include "vocoder_ui.h"
#include "vocoder_settings.h"

/// Drain recording ring buffer to disk file. Called from main thread (~50ms interval).
/// Returns number of samples written.
static int64_t drainRecordingBuffer(RenderContext& ctx) {
    if (ctx.recordFile == nullptr) return 0;
    uint32_t avail = ctx.recordRing.available();
    if (avail == 0) return 0;

    int64_t totalWritten = 0;
    constexpr int kDrainChunk = 4096;
    int16_t chunk[kDrainChunk];
    while (avail > 0) {
        int count = std::min(static_cast<int>(avail), kDrainChunk);
        for (int j = 0; j < count; j++) {
            float s = ctx.recordRing.read();
            float clamped = std::max(-1.0f, std::min(1.0f, s));
            chunk[j] = static_cast<int16_t>(clamped * 32767.0f);
        }
        fwrite(chunk, sizeof(int16_t), static_cast<size_t>(count), ctx.recordFile);
        totalWritten += count;
        avail -= count;
    }
    ctx.recordSampleCount.fetch_add(totalWritten, std::memory_order_relaxed);
    return totalWritten;
}

/// Finalize a WAV recording: drain remaining samples, patch header sizes, close file.
static void finalizeRecordingWAV(RenderContext& ctx) {
    if (ctx.recordFile == nullptr) return;
    drainRecordingBuffer(ctx);

    int64_t totalSamples = ctx.recordSampleCount.load(std::memory_order_relaxed);
    int64_t dataBytes = totalSamples * static_cast<int64_t>(sizeof(int16_t));
    constexpr int64_t kWAVHeaderOverhead = sizeof(WAVHeader) + sizeof(WAVDataChunk) - 8;
    int64_t fileSizeBytes = kWAVHeaderOverhead + dataBytes;
    if (fileSizeBytes > UINT32_MAX) {
        fprintf(stderr, "Warning: recording exceeds WAV 4GB limit — header sizes will be clamped\n");
        fileSizeBytes = UINT32_MAX;
        dataBytes = fileSizeBytes - kWAVHeaderOverhead;
    }
    uint32_t dataSize = static_cast<uint32_t>(dataBytes);
    uint32_t fileSize = static_cast<uint32_t>(fileSizeBytes);

    fseek(ctx.recordFile, 4, SEEK_SET);
    fwrite(&fileSize, sizeof(uint32_t), 1, ctx.recordFile);
    fseek(ctx.recordFile, sizeof(WAVHeader) + 4, SEEK_SET);
    fwrite(&dataSize, sizeof(uint32_t), 1, ctx.recordFile);
    fclose(ctx.recordFile);
    ctx.recordFile = nullptr;
}

// ============================================================================
// Main
// ============================================================================

int main(int /*argc*/, char* /*argv*/[]) {
    printf("=== Drew's Vocoder Toy ===\n\n");

    const float SAMPLE_RATE = 48000.0f;

    // --- Scan test_audio/ for modulator files ---
    std::vector<std::string> modulatorPaths;
    {
        DIR* dir = opendir("test_audio");
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                if (entry->d_type != DT_REG && entry->d_type != DT_UNKNOWN) continue;
                const char* name = entry->d_name;
                size_t len = strlen(name);
                bool isAudio = false;
                if (len > 4) {
                    const char* ext = name + len - 4;
                    if (strcasecmp(ext, ".wav") == 0 || strcasecmp(ext, ".mp3") == 0 ||
                        strcasecmp(ext, ".m4a") == 0 || strcasecmp(ext, ".aac") == 0 ||
                        strcasecmp(ext, ".aif") == 0) {
                        isAudio = true;
                    }
                }
                if (!isAudio && len > 5) {
                    const char* ext = name + len - 5;
                    if (strcasecmp(ext, ".aiff") == 0 || strcasecmp(ext, ".flac") == 0) {
                        isAudio = true;
                    }
                }
                if (isAudio) {
                    modulatorPaths.push_back(std::string("test_audio/") + name);
                }
            }
            closedir(dir);
        }
        std::sort(modulatorPaths.begin(), modulatorPaths.end());
    }

    if (modulatorPaths.empty()) {
        fprintf(stderr, "WARNING: No audio files found in test_audio/\n");
    } else {
        printf("Found %zu modulator files in test_audio/:\n", modulatorPaths.size());
        for (size_t i = 0; i < modulatorPaths.size(); i++) {
            const char* name = strrchr(modulatorPaths[i].c_str(), '/');
            printf("  [%zu] %s\n", i + 1, name ? name + 1 : modulatorPaths[i].c_str());
        }
    }

    // --- Pre-load all modulator files ---
    // Stored in a vector so pointers remain stable (audio thread safety).
    std::vector<AudioFileBuffer> modulatorFiles(modulatorPaths.size());
    std::vector<std::string> modulatorDisplayNames;
    {
        size_t validCount = 0;
        for (size_t i = 0; i < modulatorPaths.size(); i++) {
            const char* path = modulatorPaths[i].c_str();
            bool loaded = readAudioFileAT(path, modulatorFiles[i]);
            if (!loaded) {
                fprintf(stderr, "WARNING: Failed to load '%s', skipping\n", path);
                continue;
            }

            // Resample if needed
            if (modulatorFiles[i].sampleRate != static_cast<uint32_t>(SAMPLE_RATE)) {
                printf("Resampling '%s' from %u Hz to %.0f Hz...\n", path,
                       modulatorFiles[i].sampleRate, SAMPLE_RATE);
                AudioFileBuffer resampled;
                double ratio = static_cast<double>(modulatorFiles[i].sampleRate) / SAMPLE_RATE;
                size_t newFrames = static_cast<size_t>(modulatorFiles[i].numFrames / ratio);
                resampled.sampleRate = static_cast<uint32_t>(SAMPLE_RATE);
                resampled.numChannels = 1;
                resampled.numFrames = newFrames;
                resampled.samples.resize(newFrames);
                for (size_t j = 0; j < newFrames; j++) {
                    double srcPos = j * ratio;
                    size_t idx = static_cast<size_t>(srcPos);
                    float frac = static_cast<float>(srcPos - idx);
                    if (idx + 1 < modulatorFiles[i].numFrames) {
                        resampled.samples[j] = modulatorFiles[i].samples[idx] * (1.0f - frac) +
                                                modulatorFiles[i].samples[idx + 1] * frac;
                    } else if (idx < modulatorFiles[i].numFrames) {
                        resampled.samples[j] = modulatorFiles[i].samples[idx];
                    }
                }
                modulatorFiles[i] = std::move(resampled);
            }

            const char* name = strrchr(path, '/');
            modulatorDisplayNames.push_back(name ? name + 1 : path);
            printf("Loaded: %s (%.1f sec, %zu frames)\n",
                   modulatorDisplayNames.back().c_str(),
                   static_cast<float>(modulatorFiles[i].numFrames) / SAMPLE_RATE,
                   modulatorFiles[i].numFrames);

            // Pack valid entries to front if needed
            if (validCount < i) {
                modulatorFiles[validCount] = std::move(modulatorFiles[i]);
            }
            validCount++;
        }
        modulatorFiles.resize(validCount);
        modulatorPaths.resize(validCount);
    }

    // modulatorFileIndex: 0..N-1 = file, N = mic
    size_t modulatorFileIndex = modulatorFiles.empty() ? modulatorPaths.size() : 0;

    // --- Set up MIDI input (optional) ---
    MIDIInput midiInput;
    bool midiAvailable = midiInput.init();
    if (midiAvailable) {
        printf("MIDI input available (%d sources)\n",
               midiInput.sourceCount.load(std::memory_order_relaxed));
    } else {
        printf("MIDI input unavailable\n");
    }

    // --- Set up shared state and render context ---
    SharedState shared;

    // Load last saved settings (before audio starts)
    char lastProfileName[128] = {};
    int loadedCCNumbers[kNumCCMappings];
    for (int i = 0; i < kNumCCMappings; i++) loadedCCNumbers[i] = -1;
    bool settingsLoaded = loadLastSettings(shared, loadedCCNumbers, kNumCCMappings,
                                           lastProfileName, sizeof(lastProfileName));
    if (!settingsLoaded) {
        printf("No saved settings found, using defaults\n");
    }

    // Set initial modulator source based on file availability
    bool startWithMic = modulatorFileIndex >= modulatorFiles.size();
    shared.useMic.store(startWithMic, std::memory_order_relaxed);

    RenderContext ctx;
    ctx.shared = &shared;
    ctx.modulatorFile = modulatorFiles.empty() ? nullptr : &modulatorFiles[modulatorFileIndex];
    ctx.micBuffer = &gMicRingBuffer;
    ctx.midi = midiAvailable ? &midiInput : nullptr;
    ctx.init(SAMPLE_RATE);

    // --- Initialize DLS MIDISynth (non-fatal if it fails) ---
    if (!ctx.dlsSynth.init(SAMPLE_RATE)) {
        fprintf(stderr, "WARNING: DLS MIDISynth init failed — instruments will be silent\n");
    }

    // --- Set up CoreAudio output ---
    printf("Setting up CoreAudio output...\n");
    if (!setupOutputUnit(&ctx, SAMPLE_RATE)) {
        fprintf(stderr, "FATAL: Cannot set up audio output\n");
        midiInput.shutdown();
        return 1;
    }

    // --- Set up CoreAudio mic input (optional) ---
    printf("Setting up microphone input...\n");
    bool micAvailable = setupInputUnit(&gMicRingBuffer, SAMPLE_RATE);
    if (micAvailable) {
        printf("Microphone input available (press M to toggle)\n");
        if (gMicDeviceSampleRate > 0.0f) {
            ctx.micRateRatio = gMicDeviceSampleRate / SAMPLE_RATE;
            printf("  Mic resample ratio: %.4f (%s)\n", ctx.micRateRatio,
                   (ctx.micRateRatio >= 0.999f && ctx.micRateRatio <= 1.001f)
                       ? "no conversion needed" : "resampling enabled");
        }
    } else {
        printf("Microphone input unavailable (file-only mode)\n");
    }

    // --- Start audio ---
    OSStatus status = AudioOutputUnitStart(gOutputUnit);
    if (status != noErr) {
        fprintf(stderr, "FATAL: Cannot start audio output (status %d)\n", static_cast<int>(status));
        midiInput.shutdown();
        return 1;
    }
    printf("Audio output started.\n");
    // Countdown before entering interactive mode (skippable with any key)
    {
        struct termios oldTerm, newTerm;
        tcgetattr(STDIN_FILENO, &oldTerm);
        newTerm = oldTerm;
        newTerm.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
        newTerm.c_cc[VMIN] = 0;
        newTerm.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &newTerm);
        int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

        for (int sec = 5; sec > 0; sec--) {
            printf("\rStarting in %d... (press any key to skip)", sec);
            fflush(stdout);
            // Poll for keypress 10 times per second
            for (int tick = 0; tick < 10; tick++) {
                char ch;
                if (read(STDIN_FILENO, &ch, 1) > 0) {
                    sec = 0; // break outer loop
                    break;
                }
                usleep(100000); // 100ms
            }
        }
        printf("\r\033[K"); // clear countdown line
        fflush(stdout);

        fcntl(STDIN_FILENO, F_SETFL, flags);
        tcsetattr(STDIN_FILENO, TCSANOW, &oldTerm);
    }

    // Print audio callback buffer size (captured by first callback, printed from main thread)
    uint32_t cbFrames = shared.audioCallbackFrames.load(std::memory_order_relaxed);
    if (cbFrames > 0) {
        printf("Audio callback buffer: %u frames (%.1f ms)\n",
               cbFrames, static_cast<float>(cbFrames) / SAMPLE_RATE * 1000.0f);
    }

    // --- Enter raw terminal mode ---
    if (!enableRawMode()) {
        fprintf(stderr, "FATAL: Cannot enter raw terminal mode\n");
        AudioOutputUnitStop(gOutputUnit);
        midiInput.shutdown();
        return 1;
    }

    // Clear screen
    printf("\033[2J");
    fflush(stdout);

    // --- Main loop: keyboard + display ---
    int currentPage = 0;
    int selectedItems[kNumPages] = {0, 0, 0, 0, 0}; // per-page selected item

    // Max items per page (Page 5 MIDI is informational, 0 selectable items)
    int maxItems[kNumPages] = {kNumCarriers, 6, 5, kPage4Items, 0};

    // CC mapping setup — carrier levels from table, plus gate
    CCMapping ccMappings[kNumCCMappings];
    for (int i = 0; i < kNumCarriers; i++) {
        ccMappings[i] = {-1, &shared.carrierLevels[i], 0.0f, 1.0f, kCarriers[i].ccName};
    }
    ccMappings[kNumCarriers] = {-1, &shared.micGateThreshold, -60.0f, 0.0f, "Gate"};

    // Apply loaded CC mappings
    if (settingsLoaded) {
        for (int i = 0; i < kNumCCMappings; i++) {
            ccMappings[i].ccNumber = loadedCCNumbers[i];
        }
    }

    // Configure MIDIInput's direct-apply CC slots
    if (midiAvailable) {
        for (int i = 0; i < kNumCCMappings; i++) {
            // Auto-switch instrument index when CC adjusts a DLS carrier level
            int instrumentIdx = (i < kNumCarriers) ? kCarriers[i].dlsChannelIndex : -1;
            midiInput.ccSlots[i] = {
                ccMappings[i].target, ccMappings[i].minVal, ccMappings[i].maxVal,
                instrumentIdx
            };
            if (ccMappings[i].ccNumber >= 0) {
                midiInput.ccToSlot[ccMappings[i].ccNumber].store(i, std::memory_order_relaxed);
            }
        }
        midiInput.instrumentIndex = &shared.instrumentIndex;
        midiInput.synthNeedsReconfigure = &shared.synthNeedsReconfigure;
        midiInput.ccReady.store(true, std::memory_order_release);
    }

    // MIDI learn state
    bool midiLearnMode = false;
    int midiLearnWaitCC = -1;  // -1 = waiting for knob, >= 0 = CC detected, waiting for slot

    // Save mode state
    bool savingMode = false;
    char saveComment[256] = {};
    int saveCommentLen = 0;

    // Load mode state
    bool loadingMode = false;
    static char loadProfiles[kMaxProfiles][4096];
    int loadProfileCount = 0;
    int loadPage = 0;

    // Profile tracking (lastProfileName populated by loadLastSettings above)
    bool profileDirty = false;

    // Display name for current modulator source
    const char* displayName = (modulatorFileIndex < modulatorDisplayNames.size())
        ? modulatorDisplayNames[modulatorFileIndex].c_str()
        : "mic";

    while (shared.running.load(std::memory_order_relaxed)) {
        // Drain recording ring buffer to disk (~50ms interval)
        drainRecordingBuffer(ctx);

        int key = readRawKey();

        // --- Save mode: capture profile name ---
        if (savingMode) {
            if (key == '\n' || key == '\r') {
                saveComment[saveCommentLen] = '\0';
                saveSettings(shared, saveComment, midiAvailable ? &midiInput : nullptr,
                             ccMappings);
                strncpy(lastProfileName, saveComment, sizeof(lastProfileName) - 1);
                lastProfileName[sizeof(lastProfileName) - 1] = '\0';
                profileDirty = false;
                savingMode = false;
                saveCommentLen = 0;
                saveComment[0] = '\0';
            } else if (key == 27) { // Esc cancels
                savingMode = false;
                saveCommentLen = 0;
                saveComment[0] = '\0';
            } else if (key == 127 || key == 8) { // Backspace
                if (saveCommentLen > 0) {
                    saveCommentLen--;
                    saveComment[saveCommentLen] = '\0';
                }
            } else if (key >= 32 && key < 127 && saveCommentLen < 254) {
                saveComment[saveCommentLen] = static_cast<char>(key);
                saveCommentLen++;
                saveComment[saveCommentLen] = '\0';
            }
            drawDisplay(shared, currentPage, selectedItems[currentPage],
                        displayName, micAvailable,
                        midiAvailable ? &midiInput : nullptr, ccMappings,
                        savingMode, saveComment, ctx.recordSampleCount.load(std::memory_order_relaxed), SAMPLE_RATE,
                        lastProfileName, profileDirty);
            usleep(kDisplayRefreshUs);
            continue;
        }

        // --- Load mode: show profile list and pick one ---
        if (loadingMode) {
            if (key == 27) {
                loadingMode = false;
            } else if (key == kKeyUp || key == '[') {
                loadPage = std::max(0, loadPage - 1);
            } else if (key == kKeyDown || key == ']') {
                int totalPages = (loadProfileCount + 8) / 9;
                loadPage = std::min(totalPages - 1, loadPage + 1);
            } else if (key >= '1' && key <= '9') {
                int idx = (key - '1') + loadPage * 9;
                if (idx < loadProfileCount) {
                    int lineIdx = loadProfileCount - 1 - idx; // most recent first
                    int loadedCC[kNumCCMappings];
                    for (int i = 0; i < kNumCCMappings; i++) loadedCC[i] = -1;
                    if (applySettingsLine(loadProfiles[lineIdx], shared, loadedCC, kNumCCMappings)) {
                        for (int i = 0; i < kNumCCMappings; i++) {
                            ccMappings[i].ccNumber = loadedCC[i];
                        }
                        // Re-wire MIDI CC slots
                        if (midiAvailable) {
                            for (int cc = 0; cc < 128; cc++) {
                                midiInput.ccToSlot[cc].store(-1, std::memory_order_relaxed);
                            }
                            for (int i = 0; i < kNumCCMappings; i++) {
                                if (ccMappings[i].ccNumber >= 0) {
                                    midiInput.ccToSlot[ccMappings[i].ccNumber].store(
                                        i, std::memory_order_relaxed);
                                }
                            }
                        }
                        // Track loaded profile name
                        jsonProfileName(loadProfiles[lineIdx], lastProfileName, sizeof(lastProfileName));
                        profileDirty = false;
                    }
                    loadingMode = false;
                }
            }

            if (loadingMode) {
                // Draw profile list overlay
                printf("\033[H");
                drawHeader(shared, currentPage, displayName, micAvailable, lastProfileName, profileDirty);
                int totalPages = (loadProfileCount + 8) / 9;
                loadPage = std::max(0, std::min(totalPages - 1, loadPage));
                int pageStart = loadPage * 9;
                int pageEnd = std::min(pageStart + 9, loadProfileCount);
                printf("\033[1mLoad Profile (1-9 select, \xe2\x86\x91\xe2\x86\x93:Page, Esc cancel):   [Page %d/%d]\033[0m\033[K\n\033[K\n",
                       loadPage + 1, totalPages);
                for (int i = pageStart; i < pageEnd; i++) {
                    int lineIdx = loadProfileCount - 1 - i; // most recent first
                    char name[128];
                    jsonProfileName(loadProfiles[lineIdx], name, sizeof(name));
                    char ts[64];
                    jsonTimestamp(loadProfiles[lineIdx], ts, sizeof(ts));
                    int displayNum = (i - pageStart) + 1;
                    if (name[0] != '\0') {
                        printf("  [%d] \033[33m%-20s\033[0m  %s\033[K\n", displayNum, name, ts);
                    } else {
                        printf("  [%d] \033[90m(unnamed)\033[0m            %s\033[K\n", displayNum, ts);
                    }
                }
                printf("\033[J");
                fflush(stdout);
            }

            usleep(kDisplayRefreshUs);
            continue;
        }

        // --- MIDI Learn mode ---
        if (midiLearnMode) {
            if (key == 27) {
                // Esc cancels learn mode
                midiLearnMode = false;
                midiLearnWaitCC = -1;
            } else if (midiLearnWaitCC < 0) {
                // Waiting for a CC knob turn
                int cc = midiInput.lastCCNumber.load(std::memory_order_relaxed);
                if (cc >= 0) {
                    midiLearnWaitCC = cc;
                }
            } else {
                // CC detected, waiting for slot assignment (1-8)
                if (key >= '1' && key <= '8') {
                    int slot = key - '1';
                    // Clear old CC->slot mapping if this slot was previously mapped
                    int oldCC = ccMappings[slot].ccNumber;
                    if (oldCC >= 0) {
                        midiInput.ccToSlot[oldCC].store(-1, std::memory_order_relaxed);
                    }
                    // Set new mapping
                    ccMappings[slot].ccNumber = midiLearnWaitCC;
                    midiInput.ccToSlot[midiLearnWaitCC].store(static_cast<int8_t>(slot), std::memory_order_relaxed);
                    profileDirty = true;
                    midiLearnMode = false;
                    midiLearnWaitCC = -1;
                }
            }

            if (midiLearnMode) {
                // Draw learn mode overlay (only if still in learn mode)
                printf("\033[H");
                drawHeader(shared, currentPage, displayName, micAvailable, lastProfileName, profileDirty);
                if (midiLearnWaitCC < 0) {
                    printf("\033[1;33mMIDI Learn: Turn a knob... (Esc cancel)\033[0m\033[K\n");
                } else {
                    printf("\033[1;33mCC %d detected. Assign to slot 1-8? (Esc cancel)\033[0m\033[K\n",
                           midiLearnWaitCC);
                }
                printf("\033[J");
                fflush(stdout);
            }
            usleep(kDisplayRefreshUs);
            continue;
        }

        // Handle terminal resize
        if (gTerminalResized) {
            gTerminalResized = 0;
            printf("\033[2J");  // full clear on resize
        }

        if (key == 0) {
            // No key pressed — just refresh display
            drawDisplay(shared, currentPage, selectedItems[currentPage],
                        displayName, micAvailable,
                        midiAvailable ? &midiInput : nullptr, ccMappings,
                        false, "", ctx.recordSampleCount.load(std::memory_order_relaxed), SAMPLE_RATE,
                        lastProfileName, profileDirty);
            usleep(kDisplayRefreshUs);
            continue;
        }

        // --- Global keys (work from any page) ---
        bool handled = false;

        if (key == 27) { // Esc
            shared.running.store(false, std::memory_order_relaxed);
            handled = true;
        } else if (key == '\t') { // Tab: next page
            currentPage = (currentPage + 1) % kNumPages;
            printf("\033[2J"); // clear screen on page change
            handled = true;
        } else if (key == 't' || key == 'T') { // Toggle pitch tracking
            bool cur = shared.pitchTrackEnabled.load(std::memory_order_relaxed);
            shared.pitchTrackEnabled.store(!cur, std::memory_order_relaxed);
            if (cur) { // turning off — reset hysteresis and portamento
                ctx.lastQuantizedMidi = -1;
                ctx.smoothedPitchFreq = 0.0f;
                shared.synthNeedsReconfigure.store(true, std::memory_order_relaxed);
            }
            handled = true;
        } else if (key == 'c' || key == 'C') { // Toggle auto-chord
            bool cur = shared.autoChordEnabled.load(std::memory_order_relaxed);
            shared.autoChordEnabled.store(!cur, std::memory_order_relaxed);
            shared.synthNeedsReconfigure.store(true, std::memory_order_relaxed);
            handled = true;
        } else if (key == 'h' || key == 'H') { // Cycle harmony mode
            int hm = shared.harmonyMode.load(std::memory_order_relaxed);
            hm = (hm + 1) % static_cast<int>(HarmonyMode::Count);
            shared.harmonyMode.store(hm, std::memory_order_relaxed);
            shared.synthNeedsReconfigure.store(true, std::memory_order_relaxed);
            handled = true;
        } else if (key == 'm' || key == 'M') { // Cycle modulator source
            {
                size_t totalSources = modulatorFiles.size() + (micAvailable ? 1 : 0);
                if (totalSources > 0) {
                    // Advance to next source
                    // Sources: file0, file1, ..., fileN-1, mic (if available)
                    size_t micIndex = modulatorFiles.size(); // index that represents "mic"
                    size_t current = modulatorFileIndex;

                    // Find next valid index
                    if (current < modulatorFiles.size()) {
                        // Currently on a file, go to next file or mic
                        current++;
                    } else {
                        // Currently on mic, wrap to first file
                        current = 0;
                    }
                    // If we landed on micIndex but mic isn't available, wrap to 0
                    if (current == micIndex && !micAvailable) {
                        current = 0;
                    }

                    modulatorFileIndex = current;
                    if (current < modulatorFiles.size()) {
                        // Queue file swap for audio thread to pick up safely
                        shared.pendingModulatorFile.store(&modulatorFiles[current],
                                                          std::memory_order_release);
                        shared.useMic.store(false, std::memory_order_relaxed);
                        displayName = modulatorDisplayNames[current].c_str();
                    } else {
                        // Switch to mic
                        shared.useMic.store(true, std::memory_order_relaxed);
                        displayName = "mic";
                    }
                }
            }
            handled = true;
        } else if (key == 'p' || key == 'P') { // Save profile
            savingMode = true;
            if (lastProfileName[0] != '\0') {
                strncpy(saveComment, lastProfileName, sizeof(saveComment) - 1);
                saveComment[sizeof(saveComment) - 1] = '\0';
                saveCommentLen = static_cast<int>(strlen(saveComment));
            } else {
                saveCommentLen = 0;
                saveComment[0] = '\0';
            }
            handled = true;
        } else if (key == 'f' || key == 'F') { // Load profile
            loadProfileCount = readAllProfiles(loadProfiles, kMaxProfiles);
            if (loadProfileCount > 0) {
                loadingMode = true;
                loadPage = 0;
            }
            handled = true;
        } else if (key == 'l' || key == 'L') { // MIDI Learn
            if (midiAvailable && !midiLearnMode) {
                midiLearnMode = true;
                midiLearnWaitCC = -1;
                // Clear last CC so we detect a fresh one
                midiInput.lastCCNumber.store(-1, std::memory_order_relaxed);
            }
            handled = true;
        } else if (key == 'o' || key == 'O') { // Record to WAV (stream to disk)
            bool wasRecording = shared.recording.load(std::memory_order_relaxed);
            if (!wasRecording) {
                // Generate filename
                time_t now = time(nullptr);
                struct tm t_buf;
                struct tm* t = localtime_r(&now, &t_buf);
                char filename[64];
                snprintf(filename, sizeof(filename),
                         "vocoder_%04d%02d%02d_%02d%02d%02d.wav",
                         t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                         t->tm_hour, t->tm_min, t->tm_sec);

                // Open WAV file and write placeholder header
                FILE* rf = fopen(filename, "wb");
                if (rf != nullptr) {
                    WAVHeader hdr;
                    memcpy(hdr.riff, "RIFF", 4);
                    hdr.fileSize = 0; // placeholder
                    memcpy(hdr.wave, "WAVE", 4);
                    memcpy(hdr.fmt, "fmt ", 4);
                    hdr.fmtSize = 16;
                    hdr.audioFormat = 1;
                    hdr.numChannels = 1;
                    hdr.sampleRate = static_cast<uint32_t>(SAMPLE_RATE);
                    hdr.byteRate = static_cast<uint32_t>(SAMPLE_RATE) * sizeof(int16_t);
                    hdr.blockAlign = sizeof(int16_t);
                    hdr.bitsPerSample = 16;
                    fwrite(&hdr, sizeof(WAVHeader), 1, rf);

                    WAVDataChunk dc;
                    memcpy(dc.data, "data", 4);
                    dc.dataSize = 0; // placeholder
                    fwrite(&dc, sizeof(WAVDataChunk), 1, rf);

                    ctx.recordFile = rf;
                    ctx.recordRing.reset();
                    ctx.recordSampleCount.store(0, std::memory_order_relaxed);
                    ctx.recordingOverflow.store(false, std::memory_order_relaxed);
                    shared.recording.store(true, std::memory_order_release);
                    printf("\033[2K\033[1;31mRecording to %s ...\033[0m\n", filename);
                    snprintf(ctx.recordFilename, sizeof(ctx.recordFilename), "%s", filename);
                } else {
                    printf("\033[2K\033[31mFailed to open recording file.\033[0m\n");
                }
            } else {
                // Stop recording
                shared.recording.store(false, std::memory_order_release);
                // Wait a tiny bit for audio thread to stop pushing
                usleep(5000);

                if (ctx.recordFile != nullptr) {
                    finalizeRecordingWAV(ctx);
                    int64_t totalSamples = ctx.recordSampleCount.load(std::memory_order_relaxed);

                    if (totalSamples > 0) {
                        printf("\033[2K\033[1;32mSaved %s (%.1fs)\033[0m\n",
                               ctx.recordFilename,
                               static_cast<float>(totalSamples) / SAMPLE_RATE);
                    }
                    if (ctx.recordingOverflow.load(std::memory_order_relaxed)) {
                        printf("\033[2K\033[33mWarning: recording ring buffer overflowed — some samples lost.\033[0m\n");
                    }
                }
            }
            handled = true;
        } else if (key == ' ') { // Solo toggle
            bool cur = shared.soloActive.load(std::memory_order_relaxed);
            if (!cur) {
                // Activate solo on current Page 1 selection
                shared.soloCarrierIndex.store(selectedItems[0], std::memory_order_relaxed);
            }
            shared.soloActive.store(!cur, std::memory_order_relaxed);
            handled = true;
        } else if (key == 'd' || key == 'D') { // Monitor mode toggle
            bool cur = shared.monitorMode.load(std::memory_order_relaxed);
            shared.monitorMode.store(!cur, std::memory_order_relaxed);
            handled = true;
        }

        // Enter key: toggle mute on selected carrier (Page 1)
        if (!handled && (key == '\n' || key == '\r')) {
            int sel = selectedItems[0];
            if (sel >= 0 && sel < kNumCarriers) {
                bool cur = shared.carrierMuted[sel].load(std::memory_order_relaxed);
                shared.carrierMuted[sel].store(!cur, std::memory_order_relaxed);
                profileDirty = true;
            }
            handled = true;
        }

        // Mark profile dirty for parameter-changing keys (not navigation/mode keys)
        if (handled && key != 27 && key != '\t' && key != 'p' && key != 'P' &&
            key != 'f' && key != 'F' && key != 'l' && key != 'L' &&
            key != 'o' && key != 'O' && key != ' ' &&
            key != 'd' && key != 'D') {
            profileDirty = true;
        }

        if (handled) {
            drawDisplay(shared, currentPage, selectedItems[currentPage],
                        displayName, micAvailable,
                        midiAvailable ? &midiInput : nullptr, ccMappings,
                        savingMode, saveComment, ctx.recordSampleCount.load(std::memory_order_relaxed), SAMPLE_RATE,
                        lastProfileName, profileDirty);
            usleep(kDisplayRefreshUs);
            continue;
        }

        // --- Number keys: select item on current page ---
        if (maxItems[currentPage] > 0) {
            if (key >= '1' && key <= '9') {
                int idx = key - '1';
                if (idx < maxItems[currentPage]) {
                    selectedItems[currentPage] = idx;
                }
            } else if (key == '0') {
                // '0' selects item 9 (on page 1 this is Bagpipe)
                if (9 < maxItems[currentPage]) {
                    selectedItems[currentPage] = 9;
                }
            }
        }

        // --- Up/Down arrows: navigate selected item ---
        if (maxItems[currentPage] > 0) {
            if (key == kKeyUp) {
                int& sel = selectedItems[currentPage];
                sel = (sel - 1 + maxItems[currentPage]) % maxItems[currentPage];
            } else if (key == kKeyDown) {
                int& sel = selectedItems[currentPage];
                sel = (sel + 1) % maxItems[currentPage];
            }
        }

        // Move solo target to follow selection on Page 1
        if (currentPage == 0 && shared.soloActive.load(std::memory_order_relaxed)) {
            shared.soloCarrierIndex.store(selectedItems[0], std::memory_order_relaxed);
        }

        // --- +/- and Left/Right: adjust selected item ---
        bool isInc = (key == '+' || key == '=' || key == kKeyRight);
        bool isDec = (key == '-' || key == '_' || key == kKeyLeft);

        if (isInc || isDec) {
            profileDirty = true;
            float delta = isInc ? 1.0f : -1.0f;
            int sel = selectedItems[currentPage];

            if (currentPage == 0) {
                // Page 1: Carrier levels
                if (sel >= 0 && sel < kNumCarriers) {
                    float levelDelta = delta * 0.05f;
                    float cur = shared.carrierLevels[sel].load(std::memory_order_relaxed);
                    float newVal = std::max(0.0f, std::min(1.0f, cur + levelDelta));
                    shared.carrierLevels[sel].store(newVal, std::memory_order_relaxed);
                    // Auto-switch UI-selected instrument when adjusting a DLS carrier level
                    if (kCarriers[sel].dlsChannelIndex >= 0) {
                        shared.instrumentIndex.store(kCarriers[sel].dlsChannelIndex, std::memory_order_relaxed);
                    }
                }
            } else if (currentPage == 1) {
                // Page 2: Modulator processing
                if (sel == 0) {
                    // Gate threshold: -60 to 0 dB (0 = off)
                    float cur = shared.micGateThreshold.load(std::memory_order_relaxed);
                    float newVal = std::max(-60.0f, std::min(0.0f, cur + delta * 2.0f));
                    shared.micGateThreshold.store(newVal, std::memory_order_relaxed);
                } else if (sel == 1) {
                    // Compressor threshold: -40 to 0 dB
                    float cur = shared.modCompThreshold.load(std::memory_order_relaxed);
                    float newVal = std::max(-40.0f, std::min(0.0f, cur + delta * 2.0f));
                    shared.modCompThreshold.store(newVal, std::memory_order_relaxed);
                } else if (sel == 2) {
                    // Ratio: 1 to 20
                    float cur = shared.modCompRatio.load(std::memory_order_relaxed);
                    float newVal = std::max(1.0f, std::min(20.0f, cur + delta));
                    shared.modCompRatio.store(newVal, std::memory_order_relaxed);
                } else {
                    // EQ bands: -12 to +12 dB
                    std::atomic<float>* targets[] = {
                        &shared.modEqLow, &shared.modEqMid, &shared.modEqHigh
                    };
                    int eqIdx = sel - 3;
                    if (eqIdx >= 0 && eqIdx < 3) {
                        float cur = targets[eqIdx]->load(std::memory_order_relaxed);
                        float newVal = std::max(-12.0f, std::min(12.0f, cur + delta));
                        targets[eqIdx]->store(newVal, std::memory_order_relaxed);
                        shared.modEqNeedsRedesign.store(true, std::memory_order_relaxed);
                    }
                }
            } else if (currentPage == 2) {
                // Page 3: Output processing
                if (sel == 0) {
                    float cur = shared.outCompThreshold.load(std::memory_order_relaxed);
                    float newVal = std::max(-40.0f, std::min(0.0f, cur + delta * 2.0f));
                    shared.outCompThreshold.store(newVal, std::memory_order_relaxed);
                } else if (sel == 1) {
                    float cur = shared.outCompRatio.load(std::memory_order_relaxed);
                    float newVal = std::max(1.0f, std::min(20.0f, cur + delta));
                    shared.outCompRatio.store(newVal, std::memory_order_relaxed);
                } else {
                    std::atomic<float>* targets[] = {
                        &shared.outEqLow, &shared.outEqMid, &shared.outEqHigh
                    };
                    int eqIdx = sel - 2;
                    if (eqIdx >= 0 && eqIdx < 3) {
                        float cur = targets[eqIdx]->load(std::memory_order_relaxed);
                        float newVal = std::max(-12.0f, std::min(12.0f, cur + delta));
                        targets[eqIdx]->store(newVal, std::memory_order_relaxed);
                        shared.outEqNeedsRedesign.store(true, std::memory_order_relaxed);
                    }
                }
            } else if (currentPage == 3) {
                // Page 4: Pitch tracking — cycle through options
                int intDelta = isInc ? 1 : -1;
                if (sel == 0) {
                    // Pitch track on/off
                    bool cur = shared.pitchTrackEnabled.load(std::memory_order_relaxed);
                    shared.pitchTrackEnabled.store(!cur, std::memory_order_relaxed);
                    if (cur) { // turning off — reset hysteresis and portamento
                        ctx.lastQuantizedMidi = -1;
                        ctx.smoothedPitchFreq = 0.0f;
                        shared.synthNeedsReconfigure.store(true, std::memory_order_relaxed);
                    }
                } else if (sel == 1) {
                    // Harmony mode
                    int hm = shared.harmonyMode.load(std::memory_order_relaxed);
                    hm = (hm + intDelta + static_cast<int>(HarmonyMode::Count))
                         % static_cast<int>(HarmonyMode::Count);
                    shared.harmonyMode.store(hm, std::memory_order_relaxed);
                    shared.synthNeedsReconfigure.store(true, std::memory_order_relaxed);
                } else if (sel == 2) {
                    // Scale type
                    int sc = shared.scaleType.load(std::memory_order_relaxed);
                    sc = (sc + intDelta + ScaleQuantizer::Count) % ScaleQuantizer::Count;
                    shared.scaleType.store(sc, std::memory_order_relaxed);
                } else if (sel == 3) {
                    // Scale key
                    int sk = shared.scaleKey.load(std::memory_order_relaxed);
                    sk = (sk + intDelta + 12) % 12;
                    shared.scaleKey.store(sk, std::memory_order_relaxed);
                } else if (sel == 4) {
                    // Unvoiced noise gain: 0.00 - 0.50
                    float cur = shared.unvoicedNoiseGain.load(std::memory_order_relaxed);
                    float newVal = std::max(0.0f, std::min(0.50f, cur + delta * 0.01f));
                    shared.unvoicedNoiseGain.store(newVal, std::memory_order_relaxed);
                } else if (sel == 5) {
                    // Unvoiced confidence threshold: must stay below strongConfThreshold
                    float strongConf = shared.strongConfThreshold.load(std::memory_order_relaxed);
                    float cur = shared.unvoicedConfThreshold.load(std::memory_order_relaxed);
                    float maxUV = (strongConf > 0.90f) ? (strongConf - 0.01f) : (strongConf - 0.05f);
                    float newVal = std::max(0.10f, std::min(maxUV, cur + delta * 0.05f));
                    shared.unvoicedConfThreshold.store(newVal, std::memory_order_relaxed);
                } else if (sel == 6) {
                    // Strong confidence threshold: must stay above unvoicedConfThreshold
                    float uvThresh = shared.unvoicedConfThreshold.load(std::memory_order_relaxed);
                    float cur = shared.strongConfThreshold.load(std::memory_order_relaxed);
                    float step = (cur >= 0.90f) ? 0.01f : 0.05f;
                    float newVal = std::max(uvThresh + 0.05f, std::min(0.99f, cur + delta * step));
                    shared.strongConfThreshold.store(newVal, std::memory_order_relaxed);
                } else if (sel == 7) {
                    // Pitch Lo Hz: 20..hi-5, variable step, linked push
                    float lo = shared.pitchMinFreq.load(std::memory_order_relaxed);
                    float hi = shared.pitchMaxFreq.load(std::memory_order_relaxed);
                    float step = (lo < 250.0f) ? 1.0f : (lo < 500.0f) ? 5.0f
                               : (lo < 1000.0f) ? 10.0f : 25.0f;
                    float newLo = std::max(20.0f, lo + delta * step);
                    if (newLo > hi - 5.0f) {
                        float newHi = std::min(5000.0f, newLo + 5.0f);
                        newLo = newHi - 5.0f;
                        shared.pitchMaxFreq.store(newHi, std::memory_order_relaxed);
                    }
                    shared.pitchMinFreq.store(newLo, std::memory_order_relaxed);
                } else if (sel == 8) {
                    // Pitch Hi Hz: lo+5..5000, variable step, linked push
                    float lo = shared.pitchMinFreq.load(std::memory_order_relaxed);
                    float hi = shared.pitchMaxFreq.load(std::memory_order_relaxed);
                    float step = (hi < 250.0f) ? 1.0f : (hi < 500.0f) ? 5.0f
                               : (hi < 1000.0f) ? 10.0f : 25.0f;
                    float newHi = std::min(5000.0f, hi + delta * step);
                    if (newHi < lo + 5.0f) {
                        float newLo = std::max(20.0f, newHi - 5.0f);
                        newHi = newLo + 5.0f;
                        shared.pitchMinFreq.store(newLo, std::memory_order_relaxed);
                    }
                    shared.pitchMaxFreq.store(newHi, std::memory_order_relaxed);
                } else if (sel == 9) {
                    // Pitch Snap: Off(0) / Soft(1) / Hard(2)
                    int cur = shared.pitchSnapMode.load(std::memory_order_relaxed);
                    cur = (cur + intDelta + 3) % 3;
                    shared.pitchSnapMode.store(cur, std::memory_order_relaxed);
                } else if (sel == 10) {
                    // Correction Threshold: 5-50 cents
                    float cur = shared.pitchCorrThreshold.load(std::memory_order_relaxed);
                    float newVal = std::max(5.0f, std::min(50.0f, cur + delta * 5.0f));
                    shared.pitchCorrThreshold.store(newVal, std::memory_order_relaxed);
                } else if (sel == 11) {
                    // Correction Amount: 0-100%
                    float cur = shared.pitchCorrAmount.load(std::memory_order_relaxed);
                    float newVal = std::max(0.0f, std::min(1.0f, cur + delta * 0.05f));
                    shared.pitchCorrAmount.store(newVal, std::memory_order_relaxed);
                } else if (sel == 12) {
                    // Noise Attack: 1-100 ms
                    float cur = shared.noiseAttackMs.load(std::memory_order_relaxed);
                    float newVal = std::max(1.0f, std::min(100.0f, cur + delta * 1.0f));
                    shared.noiseAttackMs.store(newVal, std::memory_order_relaxed);
                } else if (sel == 13) {
                    // Noise Release: 1-100 ms
                    float cur = shared.noiseReleaseMs.load(std::memory_order_relaxed);
                    float newVal = std::max(1.0f, std::min(100.0f, cur + delta * 1.0f));
                    shared.noiseReleaseMs.store(newVal, std::memory_order_relaxed);
                } else if (sel == 14) {
                    // Auto-Sustain: on/off toggle
                    bool cur = shared.autoSustainEnabled.load(std::memory_order_relaxed);
                    shared.autoSustainEnabled.store(!cur, std::memory_order_relaxed);
                } else if (sel == 15) {
                    // Sustain Threshold: -60 to 0 dB, step 2
                    float cur = shared.autoSustainThresholdDb.load(std::memory_order_relaxed);
                    float newVal = std::max(-60.0f, std::min(0.0f, cur + delta * 2.0f));
                    shared.autoSustainThresholdDb.store(newVal, std::memory_order_relaxed);
                } else if (sel == 16) {
                    // Sustain Min Time: 1ms..3000ms..INF, variable step
                    float cur = shared.autoSustainMinTimeMs.load(std::memory_order_relaxed);
                    bool isInf = (cur >= kSustainTimeInfinityMs);
                    if (isInf && isDec) {
                        // Step down from infinity to 3000ms
                        cur = 3000.0f;
                    } else if (!isInf && isInc && cur >= 3000.0f) {
                        // Step up from 3000ms to infinity
                        cur = kSustainTimeInfinityMs;
                    } else if (!isInf) {
                        float step = (cur >= 1000.0f) ? 50.0f
                                   : (cur >= 250.0f)  ? 25.0f
                                   : (cur >= 100.0f)  ? 5.0f
                                   : (cur >= 50.0f)   ? 2.0f : 1.0f;
                        cur = std::max(1.0f, std::min(3000.0f, cur + delta * step));
                    }
                    float newVal = cur;
                    shared.autoSustainMinTimeMs.store(newVal, std::memory_order_relaxed);
                }
            }
        }

        // --- Page-specific keys ---
        if (currentPage == 0) {
            // Page 1: Carrier-specific keys
            if (key == 'q' || key == 'Q' || key == 'w' || key == 'W' ||
                key == 'a' || key == 'A' || key == 's' || key == 'S') {
                profileDirty = true;
                // Transpose the carrier at the cursor position (pitched carriers only)
                int sel = selectedItems[0];
                if (sel < kNumPitchedCarriers) {
                    int t = shared.carrierTranspose[sel].load(std::memory_order_relaxed);
                    int delta = (key == 'q' || key == 'Q') ? -1 :
                                (key == 'w' || key == 'W') ?  1 :
                                (key == 'a' || key == 'A') ? -12 : 12;
                    int newT = std::max(-kMaxTransposeSemitones, std::min(kMaxTransposeSemitones, t + delta));
                    shared.carrierTranspose[sel].store(newT, std::memory_order_relaxed);
                    shared.synthNeedsReconfigure.store(true, std::memory_order_relaxed);
                }
            } else if (key == 'i' || key == 'I') {
                // Cycle UI-selected instrument (for Q/W transpose and B/V brightness)
                // No level swapping — all instruments can play simultaneously
                profileDirty = true;
                int oldIdx = shared.instrumentIndex.load(std::memory_order_relaxed);
                int newIdx = (oldIdx + 1) % static_cast<int>(InstrumentType::Count);
                shared.instrumentIndex.store(newIdx, std::memory_order_relaxed);
            } else if (key == 'b' || key == 'B' || key == 'v' || key == 'V') {
                profileDirty = true;
                // Brightness up (B) or down (V)
                // Pitched carriers: adjust brightness. Dry: adjust HPF cutoff.
                int sel = selectedItems[0];
                if (sel < kNumPitchedCarriers) {
                    float brtDelta = (key == 'b' || key == 'B') ? 1.0f : -1.0f;
                    float cur = shared.carrierBrightness[sel].load(std::memory_order_relaxed);
                    float newVal = std::max(-12.0f, std::min(12.0f, cur + brtDelta));
                    shared.carrierBrightness[sel].store(newVal, std::memory_order_relaxed);
                    shared.carrierBrightnessNeedsRedesign.store(true, std::memory_order_relaxed);
                } else if (sel == kCarrierDry) {
                    // Dry HPF cutoff: 0 (bypass), 100-8000 Hz, step 50
                    bool isUp = (key == 'b' || key == 'B');
                    float cur = shared.dryHPFFreq.load(std::memory_order_relaxed);
                    float newVal;
                    if (cur <= 0.0f && isUp) {
                        newVal = 100.0f;
                    } else if (cur <= 100.0f && !isUp) {
                        newVal = 0.0f;
                    } else {
                        float step = 50.0f;
                        newVal = std::max(100.0f, std::min(28000.0f, cur + (isUp ? step : -step)));
                    }
                    if (newVal != cur) {
                        shared.dryHPFFreq.store(newVal, std::memory_order_relaxed);
                        shared.dryHPFNeedsRedesign.store(true, std::memory_order_relaxed);
                    }
                }
            }
        } else if (currentPage == 2) {
            // Page 3: Output EQ toggle
            if (key == 'e' || key == 'E') {
                profileDirty = true;
                bool cur = shared.eqEnabled.load(std::memory_order_relaxed);
                shared.eqEnabled.store(!cur, std::memory_order_relaxed);
            }
        }

        // Draw display at ~20Hz
        drawDisplay(shared, currentPage, selectedItems[currentPage],
                    displayName, micAvailable,
                    midiAvailable ? &midiInput : nullptr, ccMappings,
                    false, "", ctx.recordSampleCount.load(std::memory_order_relaxed), SAMPLE_RATE,
                    lastProfileName, profileDirty);
        usleep(kDisplayRefreshUs); // 50ms = 20Hz
    }

    // --- Cleanup ---
    AudioOutputUnitStop(gOutputUnit);
    AudioUnitUninitialize(gOutputUnit);
    AudioComponentInstanceDispose(gOutputUnit);

    if (gInputUnit) {
        AudioOutputUnitStop(gInputUnit);
        AudioUnitUninitialize(gInputUnit);
        AudioComponentInstanceDispose(gInputUnit);
    }

    ctx.dlsSynth.teardown();
    midiInput.shutdown();

    // Close any in-progress recording
    finalizeRecordingWAV(ctx);

    restoreTerminal();
    printf("Goodbye.\n");

    return 0;
}
