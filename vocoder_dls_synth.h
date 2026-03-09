// Written by Andrew Benson — https://github.com/drewster99
// Copyright (C) 2026 Nuclear Cyborg Corp. MIT License — see LICENSE file.

#pragma once

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <algorithm>

#include <AudioToolbox/AudioToolbox.h>

#include "vocoder_carriers.h"
#include "vocoder_constants.h"

// ============================================================================
// DLS (Apple MIDISynth) carrier via AUGraph pull-render
// ============================================================================

struct DLSSynthState {
    DLSSynthState() = default;
    ~DLSSynthState() { teardown(); }
    DLSSynthState(const DLSSynthState&) = delete;
    DLSSynthState& operator=(const DLSSynthState&) = delete;
    DLSSynthState(DLSSynthState&&) = delete;
    DLSSynthState& operator=(DLSSynthState&&) = delete;

    AUGraph graph = nullptr;
    AudioUnit dlsUnit = nullptr;
    AudioUnit genOutUnit = nullptr;

    float* renderBuffer = nullptr;   // mono pre-rendered buffer
    float* renderBufL = nullptr;     // stereo left
    float* renderBufR = nullptr;     // stereo right
    int renderBufSize = 0;
    int readPos = 0;

    static constexpr int kNumChannels = 9;
    static constexpr int kMaxDLSNotes = 8;

    // Per-channel state (one MIDI channel per instrument)
    int currentPrograms[kNumChannels];
    int activeNotes[kNumChannels][kMaxDLSNotes];
    int activeNoteCounts[kNumChannels];
    uint8_t lastCC7[kNumChannels];

    // Pitch bend tracking (smooth portamento without retrigger)
    int currentBaseMidi[kMaxDLSNotes] = {};  // un-transposed base MIDI notes currently Note-On'd
    int currentBaseCount = 0;
    float currentBendSemitones = 0.0f;
    static constexpr float kBendRetriggerThreshold = 11.0f;  // retrigger when |bend| > this

    float sampleRate = 48000.0f;
    bool initialized = false;

    bool init(float sr) {
        if (initialized) teardown();
        sampleRate = sr;

        // Initialize per-channel arrays
        std::fill(currentPrograms, currentPrograms + kNumChannels, -1);
        memset(activeNotes, 0, sizeof(activeNotes));
        memset(activeNoteCounts, 0, sizeof(activeNoteCounts));
        memset(lastCC7, 0xFF, sizeof(lastCC7));

        OSStatus status = NewAUGraph(&graph);
        if (status != noErr) {
            fprintf(stderr, "DLS: NewAUGraph failed (%d)\n", static_cast<int>(status));
            return false;
        }

        // DLS synth node
        AudioComponentDescription dlsDesc = {};
        dlsDesc.componentType = kAudioUnitType_MusicDevice;
        dlsDesc.componentSubType = kAudioUnitSubType_DLSSynth;
        dlsDesc.componentManufacturer = kAudioUnitManufacturer_Apple;

        AUNode dlsNode;
        status = AUGraphAddNode(graph, &dlsDesc, &dlsNode);
        if (status != noErr) {
            fprintf(stderr, "DLS: AddNode(DLS) failed (%d)\n", static_cast<int>(status));
            DisposeAUGraph(graph); graph = nullptr;
            return false;
        }

        // Generic output node (pull-render, no hardware)
        AudioComponentDescription outDesc = {};
        outDesc.componentType = kAudioUnitType_Output;
        outDesc.componentSubType = kAudioUnitSubType_GenericOutput;
        outDesc.componentManufacturer = kAudioUnitManufacturer_Apple;

        AUNode outNode;
        status = AUGraphAddNode(graph, &outDesc, &outNode);
        if (status != noErr) {
            fprintf(stderr, "DLS: AddNode(Out) failed (%d)\n", static_cast<int>(status));
            DisposeAUGraph(graph); graph = nullptr;
            return false;
        }

        // Connect DLS → Output
        status = AUGraphConnectNodeInput(graph, dlsNode, 0, outNode, 0);
        if (status != noErr) {
            fprintf(stderr, "DLS: Connect failed (%d)\n", static_cast<int>(status));
            DisposeAUGraph(graph); graph = nullptr;
            return false;
        }

        // Open graph (instantiates AU units)
        status = AUGraphOpen(graph);
        if (status != noErr) {
            fprintf(stderr, "DLS: Open failed (%d)\n", static_cast<int>(status));
            DisposeAUGraph(graph); graph = nullptr;
            return false;
        }

        // Obtain AU references
        status = AUGraphNodeInfo(graph, dlsNode, nullptr, &dlsUnit);
        if (status != noErr) {
            fprintf(stderr, "DLS: NodeInfo(DLS) failed (%d)\n", static_cast<int>(status));
            DisposeAUGraph(graph); graph = nullptr;
            return false;
        }
        status = AUGraphNodeInfo(graph, outNode, nullptr, &genOutUnit);
        if (status != noErr) {
            fprintf(stderr, "DLS: NodeInfo(Out) failed (%d)\n", static_cast<int>(status));
            DisposeAUGraph(graph); graph = nullptr;
            return false;
        }

        // Set output format: stereo float non-interleaved at vocoder sample rate
        AudioStreamBasicDescription fmt = {};
        fmt.mSampleRate = sr;
        fmt.mFormatID = kAudioFormatLinearPCM;
        fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked
                         | kAudioFormatFlagIsNonInterleaved;
        fmt.mBytesPerPacket = sizeof(float);
        fmt.mFramesPerPacket = 1;
        fmt.mBytesPerFrame = sizeof(float);
        fmt.mChannelsPerFrame = 2;
        fmt.mBitsPerChannel = 32;

        status = AudioUnitSetProperty(genOutUnit, kAudioUnitProperty_StreamFormat,
                                      kAudioUnitScope_Output, 0, &fmt, sizeof(fmt));
        if (status != noErr) {
            fprintf(stderr, "DLS: SetFormat failed (%d)\n", static_cast<int>(status));
            DisposeAUGraph(graph); graph = nullptr;
            return false;
        }

        // Initialize (but do NOT start — pull-render only)
        status = AUGraphInitialize(graph);
        if (status != noErr) {
            fprintf(stderr, "DLS: Initialize failed (%d)\n", static_cast<int>(status));
            DisposeAUGraph(graph); graph = nullptr;
            return false;
        }

        renderBufSize = kMaxCallbackFrames;
        renderBufL   = new float[renderBufSize];
        renderBufR   = new float[renderBufSize];
        renderBuffer = new float[renderBufSize];
        memset(renderBufL,   0, renderBufSize * sizeof(float));
        memset(renderBufR,   0, renderBufSize * sizeof(float));
        memset(renderBuffer, 0, renderBufSize * sizeof(float));

        // Set up all 9 instrument programs on their respective channels
        initialized = true;
        initPrograms();

        // Warmup: render 512 silent frames to prime the graph
        renderBlock(512);
        readPos = 512;

        fprintf(stderr, "DLS MIDISynth initialized (%.0f Hz)\n", sr);
        return true;
    }

    void renderBlock(int numFrames) {
        if (!initialized || numFrames <= 0) return;
        if (numFrames > renderBufSize) numFrames = renderBufSize;

        // Build a 2-buffer AudioBufferList for non-interleaved stereo
        uint8_t storage[sizeof(AudioBufferList) + sizeof(AudioBuffer)];
        auto* bufList = reinterpret_cast<AudioBufferList*>(storage);
        bufList->mNumberBuffers = 2;
        bufList->mBuffers[0].mNumberChannels = 1;
        bufList->mBuffers[0].mDataByteSize   = static_cast<UInt32>(numFrames) * sizeof(float);
        bufList->mBuffers[0].mData           = renderBufL;
        bufList->mBuffers[1].mNumberChannels = 1;
        bufList->mBuffers[1].mDataByteSize   = static_cast<UInt32>(numFrames) * sizeof(float);
        bufList->mBuffers[1].mData           = renderBufR;

        AudioTimeStamp ts = {};
        ts.mFlags = kAudioTimeStampSampleTimeValid;
        ts.mSampleTime = 0;

        AudioUnitRenderActionFlags flags = 0;
        OSStatus status = AudioUnitRender(genOutUnit, &flags, &ts, 0,
                                          static_cast<UInt32>(numFrames), bufList);
        if (status != noErr) {
            memset(renderBuffer, 0, static_cast<size_t>(numFrames) * sizeof(float));
            readPos = 0;
            return;
        }

        // Stereo → mono downmix
        for (int i = 0; i < numFrames; i++) {
            renderBuffer[i] = (renderBufL[i] + renderBufR[i]) * 0.5f;
        }
        readPos = 0;
    }

    float readSample() {
        if (!initialized || readPos >= renderBufSize) return 0.0f;
        return renderBuffer[readPos++];
    }

    /// Set up all 9 instrument programs on their respective MIDI channels.
    void initPrograms() {
        if (!initialized) return;
        for (int ch = 0; ch < kNumChannels; ch++) {
            int pgm = gmProgramNumber(static_cast<InstrumentType>(ch));
            MusicDeviceMIDIEvent(dlsUnit, 0xC0 | ch, static_cast<UInt32>(pgm & 0x7F), 0, 0);
            currentPrograms[ch] = pgm;
            // Start muted — volumes applied via CC7 before each render
            MusicDeviceMIDIEvent(dlsUnit, 0xB0 | ch, 7, 0, 0);
            lastCC7[ch] = 0;
            // Set pitch bend range to +/-12 semitones via RPN 0
            MusicDeviceMIDIEvent(dlsUnit, 0xB0 | ch, 101, 0, 0);  // RPN MSB = 0
            MusicDeviceMIDIEvent(dlsUnit, 0xB0 | ch, 100, 0, 0);  // RPN LSB = 0
            MusicDeviceMIDIEvent(dlsUnit, 0xB0 | ch, 6, 12, 0);   // Data Entry = 12 semitones
        }
    }

    /// Send notes to all 9 channels, each with its own transpose.
    void sendNotesToAllChannels(const int* baseMidiNotes, int count, const int* transposes) {
        if (!initialized) return;
        count = std::min(count, kMaxDLSNotes);
        for (int ch = 0; ch < kNumChannels; ch++) {
            // Note Off old notes
            for (int i = 0; i < activeNoteCounts[ch]; i++) {
                MusicDeviceMIDIEvent(dlsUnit, 0x80 | ch,
                                     static_cast<UInt32>(activeNotes[ch][i] & 0x7F), 0, 0);
            }
            // Note On new notes with per-channel transpose
            for (int i = 0; i < count; i++) {
                int note = std::max(0, std::min(127, baseMidiNotes[i] + transposes[ch]));
                activeNotes[ch][i] = note;
                MusicDeviceMIDIEvent(dlsUnit, 0x90 | ch, static_cast<UInt32>(note), 100, 0);
            }
            activeNoteCounts[ch] = count;
        }
    }

    /// Update per-channel volumes via CC7. Only sends when value changed.
    void updateVolumes(const float* levels) {
        if (!initialized) return;
        for (int ch = 0; ch < kNumChannels; ch++) {
            uint8_t vol = static_cast<uint8_t>(std::max(0, std::min(127,
                static_cast<int>(levels[ch] * 127.0f))));
            if (vol != lastCC7[ch]) {
                MusicDeviceMIDIEvent(dlsUnit, 0xB0 | ch, 7, vol, 0);
                lastCC7[ch] = vol;
            }
        }
    }

    /// Update per-channel brightness via CC74.
    void updateBrightness(const float* dbs) {
        if (!initialized) return;
        for (int ch = 0; ch < kNumChannels; ch++) {
            // Map dB (-12..+12) to CC74 (0..127), 64 = neutral
            int cc74 = static_cast<int>(64.0f + dbs[ch] * (64.0f / 12.0f));
            cc74 = std::max(0, std::min(127, cc74));
            MusicDeviceMIDIEvent(dlsUnit, 0xB0 | ch, 74, cc74, 0);
        }
    }

    /// Send pitch bend to all DLS channels. Range is +/-12 semitones.
    void sendPitchBend(float semitoneOffset) {
        if (!initialized) return;
        semitoneOffset = std::max(-12.0f, std::min(12.0f, semitoneOffset));
        float normalized = (semitoneOffset + 12.0f) / 24.0f;  // 0..1
        int bend14 = std::max(0, std::min(16383, static_cast<int>(normalized * 16383.0f)));
        uint8_t lsb = bend14 & 0x7F;
        uint8_t msb = (bend14 >> 7) & 0x7F;
        for (int ch = 0; ch < kNumChannels; ch++)
            MusicDeviceMIDIEvent(dlsUnit, 0xE0 | ch, lsb, msb, 0);
        currentBendSemitones = semitoneOffset;
    }

    /// Per-block pitch update: track desired frequencies via pitch bend,
    /// only retrigger notes when bend exceeds threshold.
    void updatePitchFromFreqs(const float* desiredFreqs, int count, const int* transposes) {
        if (!initialized || count <= 0) return;
        count = std::min(count, static_cast<int>(kMaxDLSNotes));

        // Guard: skip if any frequency is invalid
        // Negated > catches 0, negative, and NaN (NaN comparisons return false).
        // Upper bound prevents log2f(Inf) → static_cast<int>(Inf) UB.
        for (int i = 0; i < count; i++) {
            if (!(desiredFreqs[i] > 0.0f && desiredFreqs[i] < 100000.0f)) return;
        }

        // Compute desired bend from current base notes
        float totalBend = 0.0f;
        bool needRetrigger = (count != currentBaseCount);

        if (!needRetrigger) {
            for (int i = 0; i < count; i++) {
                float exactMidi = 69.0f + 12.0f * log2f(desiredFreqs[i] / 440.0f);
                float bend = exactMidi - static_cast<float>(currentBaseMidi[i]);
                totalBend += bend;
                if (fabsf(bend) > kBendRetriggerThreshold)
                    needRetrigger = true;
            }
        }

        if (needRetrigger) {
            // Round to nearest MIDI, send Note Off/On, reset bend to residual
            int newBaseMidi[kMaxDLSNotes];
            float residualBend = 0.0f;
            for (int i = 0; i < count; i++) {
                float exactMidi = 69.0f + 12.0f * log2f(desiredFreqs[i] / 440.0f);
                newBaseMidi[i] = static_cast<int>(roundf(exactMidi));
                residualBend += (exactMidi - static_cast<float>(newBaseMidi[i]));
            }
            sendNotesToAllChannels(newBaseMidi, count, transposes);
            for (int i = 0; i < count; i++)
                currentBaseMidi[i] = newBaseMidi[i];
            currentBaseCount = count;
            sendPitchBend(residualBend / static_cast<float>(count));
        } else {
            // Just update pitch bend — no retrigger
            sendPitchBend(totalBend / static_cast<float>(count));
        }
    }

    void allNotesOff() {
        if (!initialized) return;
        for (int ch = 0; ch < kNumChannels; ch++) {
            for (int i = 0; i < activeNoteCounts[ch]; i++) {
                MusicDeviceMIDIEvent(dlsUnit, 0x80 | ch,
                                     static_cast<UInt32>(activeNotes[ch][i] & 0x7F), 0, 0);
            }
            activeNoteCounts[ch] = 0;
        }
        currentBaseCount = 0;
        sendPitchBend(0.0f);
    }

    void teardown() {
        if (graph) {
            allNotesOff();
            AUGraphUninitialize(graph);
            AUGraphClose(graph);
            DisposeAUGraph(graph);
            graph = nullptr;
        }
        delete[] renderBuffer;  renderBuffer = nullptr;
        delete[] renderBufL;    renderBufL = nullptr;
        delete[] renderBufR;    renderBufR = nullptr;
        initialized = false;
    }
};
