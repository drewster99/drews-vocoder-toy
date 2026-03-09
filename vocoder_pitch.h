// Written by Andrew Benson — https://github.com/drewster99
// Copyright (C) 2026 Nuclear Cyborg Corp. MIT License — see LICENSE file.

#pragma once

#include <cmath>
#include <algorithm>

// ============================================================================
// YIN Pitch Detector (de Cheveigné & Kawahara, 2002)
// ============================================================================

struct PitchDetector {
    // Target time constants (sample-rate-independent)
    static constexpr float kTargetBufferMs = 21.3f;  // analysis window length
    static constexpr float kTargetHopMs    = 5.3f;   // analysis hop interval
    static constexpr float kMinDetectHz    = 80.0f;   // lowest detectable pitch
    static constexpr float kThreshold = 0.15f; // confidence threshold
    static constexpr float kSilenceThresholdDb = -50.0f; // power gate threshold

    float* buffer = nullptr;
    int bufferSize = 0;
    int hopSize = 0;
    int maxLag = 0;
    int writePos = 0;
    int hopCounter = 0;
    int samplesWritten = 0;          // total samples written (for initial fill)
    float detectedFreq = 0.0f;      // Hz, 0 = no pitch
    float confidence = 0.0f;        // 0-1
    bool newPitchAvailable = false;
    float lastValidFreq = 0.0f;     // held pitch for unvoiced segments

    // Median filter for outlier rejection (window of 3)
    static constexpr int kMedianSize = 3;
    float recentMidi[kMedianSize] = {};
    int medianIdx = 0;
    int medianCount = 0;            // how many valid entries we have

    float cachedSampleRate = 0.0f;

    // Heap-allocated analysis buffers (sized at init)
    float* analysisBuf = nullptr;
    float* diffFunc = nullptr;
    float* cmndf = nullptr;

    PitchDetector() = default;
    ~PitchDetector() { freeBuffers(); }
    PitchDetector(const PitchDetector&) = delete;
    PitchDetector& operator=(const PitchDetector&) = delete;
    PitchDetector(PitchDetector&&) = delete;
    PitchDetector& operator=(PitchDetector&&) = delete;

    /// Compute buffer sizes from sample rate and allocate.
    void init(float sampleRate) {
        freeBuffers();
        cachedSampleRate = sampleRate;
        bufferSize = static_cast<int>(kTargetBufferMs * 0.001f * sampleRate);
        hopSize    = static_cast<int>(kTargetHopMs * 0.001f * sampleRate);
        maxLag     = static_cast<int>(sampleRate / kMinDetectHz);

        // Ensure bufferSize > maxLag (needed for difference function window)
        if (bufferSize <= maxLag) bufferSize = maxLag + 1;

        buffer      = new float[bufferSize]();
        analysisBuf = new float[bufferSize]();
        diffFunc    = new float[maxLag]();
        cmndf       = new float[maxLag]();

        writePos = 0;
        hopCounter = 0;
        samplesWritten = 0;
        detectedFreq = 0.0f;
        confidence = 0.0f;
        newPitchAvailable = false;
        lastValidFreq = 0.0f;
        medianIdx = 0;
        medianCount = 0;
    }

    void pushSample(float sample) {
        if (bufferSize == 0) return;
        buffer[writePos] = sample;
        writePos = (writePos + 1) % bufferSize;
        hopCounter++;
        if (samplesWritten < bufferSize) samplesWritten++;

        // Analyze every hopSize samples, once we have a full buffer
        if (hopCounter >= hopSize && samplesWritten >= bufferSize) {
            hopCounter = 0;
            analyze(cachedSampleRate);
        }
    }

private:
    void freeBuffers() {
        delete[] buffer;      buffer = nullptr;
        delete[] analysisBuf; analysisBuf = nullptr;
        delete[] diffFunc;    diffFunc = nullptr;
        delete[] cmndf;       cmndf = nullptr;
        bufferSize = 0;
        hopSize = 0;
        maxLag = 0;
    }
public:

    /// Returns median of the valid entries in the median filter buffer.
    float medianMidi() const {
        if (medianCount <= 0) return 0.0f;
        if (medianCount == 1) return recentMidi[0];
        if (medianCount == 2) return (recentMidi[0] + recentMidi[1]) * 0.5f;

        // Median of 3
        float a = recentMidi[0], b = recentMidi[1], c = recentMidi[2];
        if (a > b) std::swap(a, b);
        if (b > c) std::swap(b, c);
        if (a > b) std::swap(a, b);
        return b;
    }

    /// Push a raw MIDI note into the median filter and return the filtered value.
    float pushAndMedianFilter(float rawMidi) {
        recentMidi[medianIdx % kMedianSize] = rawMidi;
        medianIdx++;
        if (medianCount < kMedianSize) medianCount++;
        return medianMidi();
    }

    void analyze(float sampleRate) {
        if (bufferSize == 0 || maxLag < 3) return;

        // Power gating: skip analysis when signal is too quiet
        float energy = 0.0f;
        for (int i = 0; i < bufferSize; i++) {
            energy += buffer[i] * buffer[i];
        }
        float rmsDb = 10.0f * log10f(energy / bufferSize + 1e-10f);
        if (rmsDb < kSilenceThresholdDb) {
            confidence = 0.0f;
            detectedFreq = 0.0f;
            newPitchAvailable = true;
            return;
        }

        // Build a linear analysis buffer from the circular buffer
        // (read bufferSize samples ending at writePos)
        for (int i = 0; i < bufferSize; i++) {
            analysisBuf[i] = buffer[(writePos + i) % bufferSize];
        }

        // Step 1: Difference function d(tau)
        diffFunc[0] = 1.0f; // not used, but set for cumulative mean
        for (int tau = 1; tau < maxLag; tau++) {
            float sum = 0.0f;
            for (int j = 0; j < bufferSize - maxLag; j++) {
                float diff = analysisBuf[j] - analysisBuf[j + tau];
                sum += diff * diff;
            }
            diffFunc[tau] = sum;
        }

        // Step 2: Cumulative mean normalized difference d'(tau)
        cmndf[0] = 1.0f;
        float runningSum = 0.0f;
        for (int tau = 1; tau < maxLag; tau++) {
            runningSum += diffFunc[tau];
            if (runningSum < 1e-10f) {
                cmndf[tau] = 1.0f;
            } else {
                cmndf[tau] = diffFunc[tau] * tau / runningSum;
            }
        }

        // Step 3: Find first tau below threshold (absolute threshold method)
        int bestTau = -1;
        for (int tau = 2; tau < maxLag; tau++) {
            if (cmndf[tau] < kThreshold) {
                // Find the local minimum from here
                while (tau + 1 < maxLag && cmndf[tau + 1] < cmndf[tau]) {
                    tau++;
                }
                bestTau = tau;
                break;
            }
        }

        if (bestTau < 2) {
            // No confident pitch detected
            confidence = 0.0f;
            detectedFreq = 0.0f;
            newPitchAvailable = true;
            return;
        }

        // Step 4: Parabolic interpolation around the minimum
        float tauEstimate = static_cast<float>(bestTau);
        if (bestTau > 0 && bestTau < maxLag - 1) {
            float s0 = cmndf[bestTau - 1];
            float s1 = cmndf[bestTau];
            float s2 = cmndf[bestTau + 1];
            float denom = 2.0f * (2.0f * s1 - s2 - s0);
            if (fabsf(denom) > 1e-10f) {
                tauEstimate = bestTau + (s0 - s2) / denom;
            }
        }

        // Step 5: Convert to frequency
        if (tauEstimate > 0.0f) {
            detectedFreq = sampleRate / tauEstimate;
            confidence = 1.0f - cmndf[bestTau];
            confidence = std::max(0.0f, std::min(1.0f, confidence));
            if (confidence > 0.3f) {
                lastValidFreq = detectedFreq;
            }
        } else {
            detectedFreq = 0.0f;
            confidence = 0.0f;
        }

        newPitchAvailable = true;
    }
};

// ============================================================================
// Scale Quantizer
// ============================================================================

struct ScaleQuantizer {
    enum Scale : int { Chromatic = 0, Major = 1, Minor = 2, Count = 3 };
    Scale scale = Scale::Chromatic;
    int keyNote = 0; // 0=C, 1=C#, ..., 11=B

    static constexpr int majorDegrees[7] = {0, 2, 4, 5, 7, 9, 11};
    static constexpr int minorDegrees[7] = {0, 2, 3, 5, 7, 8, 10};

    float quantize(float freqHz) const {
        if (freqHz <= 0.0f) return freqHz;
        // Convert to MIDI note number (continuous)
        float midiNote = 69.0f + 12.0f * log2f(freqHz / 440.0f);
        int rounded = static_cast<int>(roundf(midiNote));

        if (scale == Scale::Chromatic) {
            return 440.0f * powf(2.0f, (rounded - 69) / 12.0f);
        }

        const int* degrees = (scale == Scale::Major) ? majorDegrees : minorDegrees;
        int numDegrees = 7;

        // Find note relative to key
        int noteInOctave = ((rounded % 12) - keyNote + 12) % 12;
        int octave = (rounded - keyNote) / 12;
        if (rounded < keyNote) octave--;

        // Find nearest scale degree
        int bestDegree = 0;
        int bestDist = 12;
        for (int i = 0; i < numDegrees; i++) {
            int dist = abs(noteInOctave - degrees[i]);
            int wrapDist = 12 - dist;
            int minDist = std::min(dist, wrapDist);
            if (minDist < bestDist) {
                bestDist = minDist;
                bestDegree = degrees[i];
            }
        }

        int quantizedMidi = keyNote + octave * 12 + bestDegree;
        // Adjust octave if wrapping caused issues
        if (quantizedMidi < 0) quantizedMidi += 12;

        return 440.0f * powf(2.0f, (quantizedMidi - 69) / 12.0f);
    }

    /// Returns the diatonic interval (in semitones) for scale degree above root
    int diatonicInterval(int rootNoteInOctave, int degreesAbove) const {
        if (scale == Scale::Chromatic) {
            // For chromatic, use major intervals
            static constexpr int chromaticIntervals[] = {0, 2, 4, 5, 7, 9, 11};
            return chromaticIntervals[degreesAbove % 7];
        }

        const int* degrees = (scale == Scale::Major) ? majorDegrees : minorDegrees;
        int numDegrees = 7;

        // Find which degree the root is
        int rootDegreeIdx = 0;
        int rootRelative = ((rootNoteInOctave % 12) - keyNote + 12) % 12;
        int bestDist = 12;
        for (int i = 0; i < numDegrees; i++) {
            int dist = abs(rootRelative - degrees[i]);
            if (dist < bestDist) {
                bestDist = dist;
                rootDegreeIdx = i;
            }
        }

        int targetIdx = (rootDegreeIdx + degreesAbove) % numDegrees;
        int interval = degrees[targetIdx] - degrees[rootDegreeIdx];
        if (interval < 0) interval += 12;
        return interval;
    }
};

constexpr int ScaleQuantizer::majorDegrees[7];
constexpr int ScaleQuantizer::minorDegrees[7];

// ============================================================================
// Harmony Generator
// ============================================================================

enum class HarmonyMode : int { Off = 0, Unison = 1, UnisonOctave = 2, Diatonic = 3, Count = 4 };

static const char* harmonyModeName(HarmonyMode m) {
    switch (m) {
        case HarmonyMode::Off: return "Off";
        case HarmonyMode::Unison: return "Unison";
        case HarmonyMode::UnisonOctave: return "Unison+Oct";
        case HarmonyMode::Diatonic: return "Diatonic";
        default: return "?";
    }
}

struct HarmonyGenerator {
    HarmonyMode mode = HarmonyMode::Off;

    /// Generate carrier frequencies from a detected root pitch.
    /// outFreqs: array of at least 4 floats for output frequencies.
    /// noteCount: set to number of valid entries.
    void generateNotes(float rootFreqHz, int rootMidiNote,
                       const ScaleQuantizer& quantizer,
                       float outFreqs[4], int& noteCount) const {
        noteCount = 0;
        if (rootFreqHz <= 0.0f) return;

        switch (mode) {
            case HarmonyMode::Off:
                break;

            case HarmonyMode::Unison:
                outFreqs[0] = rootFreqHz;
                noteCount = 1;
                break;

            case HarmonyMode::UnisonOctave:
                outFreqs[0] = rootFreqHz;
                outFreqs[1] = rootFreqHz * 0.5f; // octave below
                noteCount = 2;
                break;

            case HarmonyMode::Diatonic: {
                outFreqs[0] = rootFreqHz;
                // Diatonic 3rd (2 scale degrees up)
                int interval3 = quantizer.diatonicInterval(rootMidiNote % 12, 2);
                outFreqs[1] = rootFreqHz * powf(2.0f, interval3 / 12.0f);
                // Diatonic 5th (4 scale degrees up)
                int interval5 = quantizer.diatonicInterval(rootMidiNote % 12, 4);
                outFreqs[2] = rootFreqHz * powf(2.0f, interval5 / 12.0f);
                noteCount = 3;
                break;
            }

            default:
                break;
        }
    }
};
