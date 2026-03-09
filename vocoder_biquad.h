// Written by Andrew Benson — https://github.com/drewster99
// Copyright (C) 2026 Nuclear Cyborg Corp. MIT License — see LICENSE file.

#pragma once

#include <cmath>
#include <algorithm>

// ============================================================================
// Biquad Filter (from vocoder_test.cpp)
// ============================================================================

struct Biquad {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void reset() { z1 = z2 = 0.0f; }

    void designBandpass(float centerFreq, float Q, float sampleRate) {
        if (sampleRate <= 0.0f) return;
        centerFreq = std::max(1.0f, std::min(centerFreq, sampleRate * 0.499f));
        Q = std::max(0.1f, Q);

        float w0 = 2.0f * static_cast<float>(M_PI) * centerFreq / sampleRate;
        float sinW0 = sinf(w0);
        float cosW0 = cosf(w0);
        float alpha = sinW0 / (2.0f * Q);

        float a0 = 1.0f + alpha;
        b0 = alpha / a0;
        b1 = 0.0f;
        b2 = -alpha / a0;
        a1 = (-2.0f * cosW0) / a0;
        a2 = (1.0f - alpha) / a0;
    }

    void designLowShelf(float freq, float gainDb, float sampleRate, float S = 1.0f) {
        if (sampleRate <= 0.0f) return;
        freq = std::max(1.0f, std::min(freq, sampleRate * 0.499f));
        gainDb = std::max(-60.0f, std::min(60.0f, gainDb));

        float A = powf(10.0f, gainDb / 40.0f);
        float w0 = 2.0f * static_cast<float>(M_PI) * freq / sampleRate;
        float sinW0 = sinf(w0);
        float cosW0 = cosf(w0);
        float alpha = sinW0 / 2.0f * sqrtf((A + 1.0f / A) * (1.0f / S - 1.0f) + 2.0f);
        float twoSqrtAalpha = 2.0f * sqrtf(A) * alpha;

        float a0 = (A + 1.0f) + (A - 1.0f) * cosW0 + twoSqrtAalpha;
        b0 = (A * ((A + 1.0f) - (A - 1.0f) * cosW0 + twoSqrtAalpha)) / a0;
        b1 = (2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosW0)) / a0;
        b2 = (A * ((A + 1.0f) - (A - 1.0f) * cosW0 - twoSqrtAalpha)) / a0;
        a1 = (-2.0f * ((A - 1.0f) + (A + 1.0f) * cosW0)) / a0;
        a2 = ((A + 1.0f) + (A - 1.0f) * cosW0 - twoSqrtAalpha) / a0;
    }

    void designPeakEQ(float freq, float Q, float gainDb, float sampleRate) {
        if (sampleRate <= 0.0f) return;
        freq = std::max(1.0f, std::min(freq, sampleRate * 0.499f));
        Q = std::max(0.1f, Q);
        gainDb = std::max(-60.0f, std::min(60.0f, gainDb));

        float A = powf(10.0f, gainDb / 40.0f);
        float w0 = 2.0f * static_cast<float>(M_PI) * freq / sampleRate;
        float sinW0 = sinf(w0);
        float cosW0 = cosf(w0);
        float alpha = sinW0 / (2.0f * Q);

        float a0 = 1.0f + alpha / A;
        b0 = (1.0f + alpha * A) / a0;
        b1 = (-2.0f * cosW0) / a0;
        b2 = (1.0f - alpha * A) / a0;
        a1 = (-2.0f * cosW0) / a0;
        a2 = (1.0f - alpha / A) / a0;
    }

    void designHighPass(float freq, float Q, float sampleRate) {
        if (sampleRate <= 0.0f) return;
        freq = std::max(1.0f, std::min(freq, sampleRate * 0.499f));
        Q = std::max(0.1f, Q);

        float w0 = 2.0f * static_cast<float>(M_PI) * freq / sampleRate;
        float sinW0 = sinf(w0);
        float cosW0 = cosf(w0);
        float alpha = sinW0 / (2.0f * Q);

        float a0 = 1.0f + alpha;
        b0 = ((1.0f + cosW0) / 2.0f) / a0;
        b1 = (-(1.0f + cosW0)) / a0;
        b2 = ((1.0f + cosW0) / 2.0f) / a0;
        a1 = (-2.0f * cosW0) / a0;
        a2 = (1.0f - alpha) / a0;
    }

    void designHighShelf(float freq, float gainDb, float sampleRate, float S = 1.0f) {
        if (sampleRate <= 0.0f) return;
        freq = std::max(1.0f, std::min(freq, sampleRate * 0.499f));
        gainDb = std::max(-60.0f, std::min(60.0f, gainDb));

        float A = powf(10.0f, gainDb / 40.0f);
        float w0 = 2.0f * static_cast<float>(M_PI) * freq / sampleRate;
        float sinW0 = sinf(w0);
        float cosW0 = cosf(w0);
        float alpha = sinW0 / 2.0f * sqrtf((A + 1.0f / A) * (1.0f / S - 1.0f) + 2.0f);
        float twoSqrtAalpha = 2.0f * sqrtf(A) * alpha;

        float a0 = (A + 1.0f) - (A - 1.0f) * cosW0 + twoSqrtAalpha;
        b0 = (A * ((A + 1.0f) + (A - 1.0f) * cosW0 + twoSqrtAalpha)) / a0;
        b1 = (-2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosW0)) / a0;
        b2 = (A * ((A + 1.0f) + (A - 1.0f) * cosW0 - twoSqrtAalpha)) / a0;
        a1 = (2.0f * ((A - 1.0f) - (A + 1.0f) * cosW0)) / a0;
        a2 = ((A + 1.0f) - (A - 1.0f) * cosW0 - twoSqrtAalpha) / a0;
    }

    float process(float x) {
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        // Flush denormals, NaN, and Inf to prevent cascading instability
        if (!(fabsf(z1) > 1e-30f && fabsf(z1) < 1e15f)) z1 = 0.0f;
        if (!(fabsf(z2) > 1e-30f && fabsf(z2) < 1e15f)) z2 = 0.0f;
        // Guard output against NaN/Inf from degenerate coefficients
        if (!(fabsf(y) < 1e15f)) y = 0.0f;
        return y;
    }
};
