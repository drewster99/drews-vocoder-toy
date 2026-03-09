// Written by Andrew Benson — https://github.com/drewster99
// Copyright (C) 2026 Nuclear Cyborg Corp. MIT License — see LICENSE file.

#pragma once

#include <cmath>
#include <algorithm>

// ============================================================================
// Per-sample Additive Synth (organ and cello with persistent phase state)
// ============================================================================

static constexpr int kMaxChordNotes = 6;

enum class InstrumentType : int {
    Organ = 0, Cello = 1, SynthBrass = 2,
    StringEnsemble2 = 3, PadNewAge = 4, FXSciFi = 5,
    Bagpipe = 6, LeadFifths = 7, PadPolysynth = 8,
    Count = 9
};

/// Returns 0-indexed GM program number for each instrument.
static int gmProgramNumber(InstrumentType t) {
    static const int programs[] = { 19, 42, 62, 49, 88, 103, 109, 86, 90 };
    int idx = static_cast<int>(t);
    return (idx >= 0 && idx < 9) ? programs[idx] : 0;
}

static const char* instrumentName(InstrumentType t) {
    switch (t) {
        case InstrumentType::Organ:           return "Organ";
        case InstrumentType::Cello:           return "Cello";
        case InstrumentType::SynthBrass:      return "Syn Brass";
        case InstrumentType::StringEnsemble2: return "Str Ens 2";
        case InstrumentType::PadNewAge:       return "Pad NewAg";
        case InstrumentType::FXSciFi:         return "FX Sci-Fi";
        case InstrumentType::Bagpipe:         return "Bagpipe";
        case InstrumentType::LeadFifths:      return "Ld Fifths";
        case InstrumentType::PadPolysynth:    return "Pad Poly";
        default: return "?";
    }
}

// ============================================================================
// Per-sample Simple Carriers (saw, noise, buzz with persistent state)
// ============================================================================

struct SimpleCarrier {
    static constexpr int kMaxVoices = 8;
    static constexpr int kBuzzTableSize = 2048;

    double sawPhases[kMaxVoices] = {};
    double buzzPhases[kMaxVoices] = {};
    uint32_t noiseSeed = 12345;

    // Buzz wavetable: one precomputed cycle per voice
    float buzzTable[kMaxVoices][kBuzzTableSize] = {};
    int buzzTableMaxH[kMaxVoices] = {};  // maxH used when table was built

    float generateSawtooth(const float* freqs, int numVoices, float sampleRate) {
        float sum = 0.0f;
        int n = std::min(numVoices, kMaxVoices);
        for (int v = 0; v < n; v++) {
            sum += static_cast<float>(2.0 * sawPhases[v] - 1.0);
            sawPhases[v] += freqs[v] / sampleRate;
            if (!(sawPhases[v] < 10.0)) sawPhases[v] = 0.0;  // Inf/NaN guard
            while (sawPhases[v] >= 1.0) sawPhases[v] -= 1.0;
        }
        return (n > 1) ? sum / sqrtf(static_cast<float>(n)) : sum;
    }

    float generateNoise() {
        noiseSeed = noiseSeed * 1664525u + 1013904223u;
        return (static_cast<float>(noiseSeed) / 2147483648.0f) - 1.0f;
    }

    /// Precompute one cycle of band-limited buzz waveform for a voice.
    /// Only called when maxH changes (i.e., on note changes).
    /// Normalized so peak amplitude is consistent regardless of harmonic count.
    void rebuildBuzzTable(int voice, float freq, float sampleRate) {
        int maxH = std::min(64, static_cast<int>(sampleRate / (2.0f * freq)));
        if (maxH < 1) maxH = 1;
        if (maxH == buzzTableMaxH[voice]) return;  // no change needed
        buzzTableMaxH[voice] = maxH;
        float invTable = (2.0f * static_cast<float>(M_PI)) / static_cast<float>(kBuzzTableSize);
        float peak = 0.0f;
        for (int s = 0; s < kBuzzTableSize; s++) {
            float t = static_cast<float>(s) * invTable;
            float val = 0.0f;
            for (int h = 1; h <= maxH; h++)
                val += sinf(t * h) / static_cast<float>(h);
            buzzTable[voice][s] = val;
            float absVal = fabsf(val);
            if (absVal > peak) peak = absVal;
        }
        // Normalize to consistent peak of 0.5 (matches single-harmonic amplitude)
        if (peak > 0.0f) {
            float scale = 0.5f / peak;
            for (int s = 0; s < kBuzzTableSize; s++)
                buzzTable[voice][s] *= scale;
        }
    }

    float generateBuzz(const float* freqs, int numVoices, float sampleRate) {
        float sum = 0.0f;
        int n = std::min(numVoices, kMaxVoices);
        for (int v = 0; v < n; v++) {
            // Rebuild table if harmonic count changed
            int maxH = std::min(64, static_cast<int>(sampleRate / (2.0f * freqs[v])));
            if (maxH < 1) maxH = 1;
            if (maxH != buzzTableMaxH[v])
                rebuildBuzzTable(v, freqs[v], sampleRate);

            // Table lookup with linear interpolation
            float tablePos = static_cast<float>(buzzPhases[v]) * static_cast<float>(kBuzzTableSize);
            int idx = static_cast<int>(tablePos);
            float frac = tablePos - static_cast<float>(idx);
            idx &= (kBuzzTableSize - 1);
            int nextIdx = (idx + 1) & (kBuzzTableSize - 1);
            sum += buzzTable[v][idx] + (buzzTable[v][nextIdx] - buzzTable[v][idx]) * frac;

            buzzPhases[v] += static_cast<double>(freqs[v]) / static_cast<double>(sampleRate);
            if (!(buzzPhases[v] < 10.0)) buzzPhases[v] = 0.0;  // Inf/NaN guard
            while (buzzPhases[v] >= 1.0) buzzPhases[v] -= 1.0;
        }
        return (n > 1) ? sum / sqrtf(static_cast<float>(n)) : sum;
    }
};
