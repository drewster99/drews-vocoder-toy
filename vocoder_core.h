// Written by Andrew Benson — https://github.com/drewster99
// Copyright (C) 2026 Nuclear Cyborg Corp. MIT License — see LICENSE file.

#pragma once

#include "vocoder_constants.h"
#include "vocoder_biquad.h"
#include <cmath>
#include <vector>
#include <algorithm>

// ============================================================================
// Bandpass, EnvelopeFollower, VocoderBand, ChannelVocoder (from vocoder_test.cpp)
// ============================================================================

struct BandpassFilter {
    Biquad stage1, stage2;
    bool fourthOrder = true;

    void design(float centerFreq, float Q, float sampleRate, bool use4thOrder) {
        fourthOrder = use4thOrder;
        stage1.designBandpass(centerFreq, Q, sampleRate);
        if (fourthOrder) {
            stage2.designBandpass(centerFreq, Q, sampleRate);
        }
    }

    void reset() { stage1.reset(); stage2.reset(); }

    float process(float x) {
        float out = stage1.process(x);
        return fourthOrder ? stage2.process(out) : out;
    }
};

struct EnvelopeFollower {
    float envelope = 0.0f;
    float attackCoeff = 0.0f;
    float releaseCoeff = 0.0f;

    void init(float attackMs, float releaseMs, float sampleRate) {
        attackCoeff = expf(-1.0f / (attackMs * 0.001f * sampleRate));
        releaseCoeff = expf(-1.0f / (releaseMs * 0.001f * sampleRate));
        envelope = 0.0f;
    }

    void reset() { envelope = 0.0f; }

    float process(float x) {
        float rectified = fabsf(x);
        float coeff = (rectified > envelope) ? attackCoeff : releaseCoeff;
        envelope = coeff * envelope + (1.0f - coeff) * rectified;
        return envelope;
    }
};

struct VocoderBand {
    float frequency = 0.0f;
    BandpassFilter modFilter;
    BandpassFilter carFilter;
    EnvelopeFollower envFollower;

    float hetSin = 0.0f, hetCos = 1.0f;
    float hetSinInc = 0.0f, hetCosInc = 1.0f;
    int hetRenormCounter = 0;

    void initHeterodyne(float centerFreq, float sampleRate) {
        float w = 2.0f * static_cast<float>(M_PI) * centerFreq / sampleRate;
        hetSinInc = sinf(w);
        hetCosInc = cosf(w);
        hetSin = 0.0f;
        hetCos = 1.0f;
        hetRenormCounter = 0;
    }

    float heterodyne(float x) {
        float result = x * hetSin * 2.0f;
        float newSin = hetSin * hetCosInc + hetCos * hetSinInc;
        float newCos = hetCos * hetCosInc - hetSin * hetSinInc;
        hetSin = newSin;
        hetCos = newCos;
        // Renormalize every 1024 samples to prevent amplitude drift
        if (++hetRenormCounter >= 1024) {
            hetRenormCounter = 0;
            float mag = hetSin * hetSin + hetCos * hetCos;
            if (mag > 0.0f) {
                float invMag = 1.0f / sqrtf(mag);
                hetSin *= invMag;
                hetCos *= invMag;
            }
        }
        return result;
    }

    void reset() {
        modFilter.reset();
        carFilter.reset();
        envFollower.reset();
        hetSin = 0.0f;
        hetCos = 1.0f;
        hetRenormCounter = 0;
    }
};

struct ChannelVocoder {
    std::vector<VocoderBand> bands;
    int numBands = kDefaultNumBands;
    float sampleRate = 0.0f;

    float filterQ = kDefaultFilterQ;
    float envelopeAttackMs = 2.0f;
    float envelopeReleaseMs = 15.0f;
    float startFreq = kDefaultStartFreq;
    float endFreq = kDefaultEndFreq;
    float outputGain = 1.0f;
    float postFilterGain = 6.0f;
    float carrierGain = 10.0f;
    bool use4thOrder = true;
    bool useHeterodyne = true;

    void init(int nBands, float sr) {
        numBands = nBands;
        sampleRate = sr;
        bands.resize(numBands);

        float totalRangeInCents = 1200.0f * logf(endFreq / startFreq) / logf(2.0f);
        float centsPerBand = totalRangeInCents / numBands;
        float scale = powf(2.0f, centsPerBand / 1200.0f);

        float currentFreq = startFreq;
        for (int i = 0; i < numBands; i++) {
            bands[i].frequency = currentFreq;
            bands[i].modFilter.design(currentFreq, filterQ, sampleRate, use4thOrder);
            bands[i].carFilter.design(currentFreq, filterQ, sampleRate, use4thOrder);
            bands[i].envFollower.init(envelopeAttackMs, envelopeReleaseMs, sampleRate);
            bands[i].initHeterodyne(currentFreq, sampleRate);
            currentFreq *= scale;
        }
    }

    void reset() {
        for (auto& band : bands) {
            band.reset();
        }
    }

    float processSample(float modSample, float carSample) {
        float output = 0.0f;
        for (int i = 0; i < numBands; i++) {
            float modFiltered = bands[i].modFilter.process(modSample);
            float boosted = modFiltered * postFilterGain;
            float envelopeInput = useHeterodyne ? bands[i].heterodyne(boosted) : boosted;
            float env = bands[i].envFollower.process(envelopeInput);
            float carFiltered = bands[i].carFilter.process(carSample) * carrierGain;
            output += carFiltered * env;
        }
        return output * outputGain;
    }
};

// ============================================================================
// Soft Clipper (from DSPKernel)
// ============================================================================

static float softClip(float x) {
    const float threshold = 0.95f;
    if (x > threshold) {
        float excess = x - threshold;
        float clampedExcess = std::min(10.0f, excess * 4.0f);
        return threshold + (1.0f - threshold) * (1.0f - expf(-clampedExcess));
    } else if (x < -threshold) {
        float excess = -x - threshold;
        float clampedExcess = std::min(10.0f, excess * 4.0f);
        return -threshold - (1.0f - threshold) * (1.0f - expf(-clampedExcess));
    }
    return x;
}
