// Written by Andrew Benson — https://github.com/drewster99
// Copyright (C) 2026 Nuclear Cyborg Corp. MIT License — see LICENSE file.

#pragma once

#include <cmath>
#include <algorithm>

// ============================================================================
// Simple Compressor (feed-forward, fixed attack/release)
// ============================================================================

struct SimpleCompressor {
    float threshold = 0.0f;   // dB (0 = off)
    float ratio = 4.0f;       // 1:1 to 20:1
    float envDb = -96.0f;     // envelope state
    float attackCoeff = 0.0f;
    float releaseCoeff = 0.0f;
    float kneeWidth = 6.0f;   // dB

    void init(float sampleRate) {
        float attackMs = 5.0f;
        float releaseMs = 50.0f;
        attackCoeff = expf(-1.0f / (attackMs * 0.001f * sampleRate));
        releaseCoeff = expf(-1.0f / (releaseMs * 0.001f * sampleRate));
        envDb = -96.0f;
    }

    void setParams(float threshDb, float rat) {
        threshold = threshDb;
        ratio = std::max(1.0f, std::min(20.0f, rat));
    }

    float lastGainDb = 0.0f;  // last computed gain reduction (for GR metering)

    float process(float x) {
        if (threshold >= 0.0f) { lastGainDb = 0.0f; return x; } // effectively off

        float inputDb = 20.0f * log10f(fabsf(x) + 1e-30f);

        // Smooth envelope
        float coeff = (inputDb > envDb) ? attackCoeff : releaseCoeff;
        envDb = coeff * envDb + (1.0f - coeff) * inputDb;

        // Soft-knee gain computation
        float gainDb = 0.0f;
        float halfKnee = kneeWidth * 0.5f;
        if (envDb <= threshold - halfKnee) {
            gainDb = 0.0f;
        } else if (envDb >= threshold + halfKnee) {
            gainDb = (threshold + (envDb - threshold) / ratio) - envDb;
        } else {
            float delta = envDb - (threshold - halfKnee);
            gainDb = ((1.0f / ratio - 1.0f) * delta * delta) / (2.0f * kneeWidth);
        }

        lastGainDb = gainDb;
        return x * expf(gainDb * 0.11512925464970229f); // ln(10)/20 — equivalent to powf(10, dB/20)
    }
};

// ============================================================================
// Noise Gate (cut background noise from modulator input)
// ============================================================================

struct NoiseGate {
    float thresholdDb = -40.0f; // dB, 0 = off
    float envDb = -96.0f;
    float gain = 0.0f;         // 0 = closed, 1 = open
    float attackCoeff = 0.0f;  // fast open
    float releaseCoeff = 0.0f; // slower close
    float holdSamples = 0.0f;
    float holdCounter = 0.0f;
    float gainAttack = 0.0f;   // smooth gain ramp-up
    float gainRelease = 0.0f;  // smooth gain ramp-down
    static constexpr float kHysteresisDb = 3.0f;

    void init(float sampleRate) {
        attackCoeff = expf(-1.0f / (0.0005f * sampleRate));  // 0.5ms
        releaseCoeff = expf(-1.0f / (0.100f * sampleRate));  // 100ms
        holdSamples = 0.050f * sampleRate;                   // 50ms hold
        holdCounter = 0.0f;
        gainAttack = expf(-1.0f / (0.001f * sampleRate));    // 1ms gain ramp
        gainRelease = expf(-1.0f / (0.010f * sampleRate));   // 10ms gain ramp
        envDb = -96.0f;
        gain = 0.0f;
    }

    float process(float x) {
        if (thresholdDb >= 0.0f) return x; // gate off

        float inputDb = 20.0f * log10f(fabsf(x) + 1e-30f);

        // Smooth envelope
        float coeff = (inputDb > envDb) ? attackCoeff : releaseCoeff;
        envDb = coeff * envDb + (1.0f - coeff) * inputDb;

        // Gate logic with hysteresis
        float targetGain;
        if (envDb >= thresholdDb) {
            // Above threshold — open
            holdCounter = holdSamples;
            targetGain = 1.0f;
        } else if (holdCounter > 0.0f) {
            // In hold period — stay open
            holdCounter -= 1.0f;
            targetGain = 1.0f;
        } else if (envDb < thresholdDb - kHysteresisDb) {
            // Below threshold - hysteresis — close
            targetGain = 0.0f;
        } else {
            // In hysteresis zone — maintain current state
            targetGain = (gain > 0.5f) ? 1.0f : 0.0f;
        }

        // Smooth gain ramp
        float gc = (targetGain > gain) ? gainAttack : gainRelease;
        gain = gc * gain + (1.0f - gc) * targetGain;

        return x * gain;
    }
};
