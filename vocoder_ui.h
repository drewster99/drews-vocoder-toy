// Written by Andrew Benson — https://github.com/drewster99
// Copyright (C) 2026 Nuclear Cyborg Corp. MIT License — see LICENSE file.

#pragma once

#include <cstdio>
#include <cstdint>
#include <cmath>
#include <algorithm>

#include "vocoder_carrier_table.h"
#include "vocoder_constants.h"

static constexpr int kNumPages = 5;
static constexpr int kPage4Items = 17;
static const char* pageNames[] = {"Carriers", "Modulator", "Output", "Pitch", "MIDI"};

static const char* noteNames[] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

static void midiToNoteName(int midi, char* buf, size_t bufSize) {
    int note = ((midi % 12) + 12) % 12;
    int octave = (midi / 12) - 1;
    snprintf(buf, bufSize, "%s%d", noteNames[note], octave);
}

static void drawLevelBar(float level, int width) {
    int filled = static_cast<int>(level * width + 0.5f);
    filled = std::max(0, std::min(width, filled));
    for (int i = 0; i < filled; i++) printf("\033[32m\xe2\x96\x88\033[0m");
    for (int i = filled; i < width; i++) printf("\033[90m\xe2\x96\x91\033[0m");
}

static void drawDbBar(float db, float minDb, float maxDb, int width) {
    float normalized = (db - minDb) / (maxDb - minDb);
    normalized = std::max(0.0f, std::min(1.0f, normalized));
    int filled = static_cast<int>(normalized * width + 0.5f);
    filled = std::max(0, std::min(width, filled));
    for (int i = 0; i < filled; i++) printf("\033[32m\xe2\x96\x88\033[0m");
    for (int i = filled; i < width; i++) printf("\033[90m\xe2\x96\x91\033[0m");
}

static void drawPeakBar(float peak, int width) {
    float db = (peak > 1e-10f) ? 20.0f * log10f(peak) : -60.0f;
    db = std::max(-60.0f, db);
    float normalized = (db + 60.0f) / 60.0f;
    normalized = std::max(0.0f, std::min(1.0f, normalized));
    int filled = static_cast<int>(normalized * width + 0.5f);
    filled = std::max(0, std::min(width, filled));
    for (int i = 0; i < filled; i++) printf("\033[33m#\033[0m");
    for (int i = filled; i < width; i++) printf("\033[90m-\033[0m");
    printf(" (%.0f dB)", db);
}

static void drawGRBar(float grDb, int width) {
    // grDb is negative (0 = no compression, -20 = max compression shown)
    float normalized = -grDb / 20.0f;
    normalized = std::max(0.0f, std::min(1.0f, normalized));
    int filled = static_cast<int>(normalized * width + 0.5f);
    filled = std::max(0, std::min(width, filled));
    for (int i = 0; i < filled; i++) printf("\033[31m\xe2\x96\x88\033[0m");
    for (int i = filled; i < width; i++) printf("\033[90m\xe2\x96\x91\033[0m");
}

static void drawConfidenceBar(float conf, int width) {
    int filled = static_cast<int>(conf * width + 0.5f);
    filled = std::max(0, std::min(width, filled));
    for (int i = 0; i < filled; i++) printf("\033[32m\xe2\x96\x88\033[0m");
    for (int i = filled; i < width; i++) printf("\033[90m\xe2\x96\x91\033[0m");
}

struct CarrierDisplayInfo {
    const char* name;
    std::atomic<float>* level;
    std::atomic<float>* brightness; // nullptr for Dry
};

static void drawHeader(SharedState& shared, int currentPage,
                        const char* modulatorName, bool micAvailable,
                        const char* profileName = nullptr, bool profileDirty = false) {
    printf("\033[H");

    printf("\033[1m=== Drew's Vocoder Toy ===\033[0m");
    printf("%*s[Page %d/%d: %s]", 30, "", currentPage + 1, kNumPages, pageNames[currentPage]);
    if (profileName != nullptr && profileName[0] != '\0') {
        printf("  Profile: \033[33m%s\033[0m%s", profileName, profileDirty ? " \033[31m[Edited]\033[0m" : "");
    } else {
        printf("  Profile: \033[90m(none)\033[0m");
    }
    printf("\033[K\n\n");

    // Modulator source
    bool useMic = shared.useMic.load(std::memory_order_relaxed);
    if (useMic) {
        printf("Modulator: \033[36mMIC\033[0m");
        if (!micAvailable) printf(" (unavailable!)");
        else if (gMicDeviceName[0] != '\0')
            printf(" (%s, %.0f Hz)", gMicDeviceName, gMicDeviceSampleRate);
    } else {
        printf("Modulator: \033[36mFILE\033[0m (%s, looping)", modulatorName);
    }

    // Solo / Monitor / Recording indicators
    bool soloOn = shared.soloActive.load(std::memory_order_relaxed);
    bool monitorOn = shared.monitorMode.load(std::memory_order_relaxed);
    if (soloOn) {
        int si = shared.soloCarrierIndex.load(std::memory_order_relaxed);
        const char* soloName = (si >= 0 && si < kNumCarriers) ? kCarriers[si].displayName : "?";
        printf("  \033[1;33m[SOLO: %s]\033[0m", soloName);
    }
    if (monitorOn) {
        printf("  \033[1;33m[D:MONITOR]\033[0m");
    }

    // Peak meters
    float inPeak = shared.inputPeak.load(std::memory_order_relaxed);
    float outPeak = shared.outputPeak.load(std::memory_order_relaxed);

    printf("\033[K\n");
    printf("  Input:  ");
    drawPeakBar(inPeak, 10);
    printf("\033[K\n");
    printf("  Output: ");
    drawPeakBar(outPeak, 10);
    printf("\033[K\n\n");
}

static void drawPage1(SharedState& shared, int selectedItem, int64_t recordSampleCount,
                       float sampleRate) {
    // Build display info from carrier descriptor table
    CarrierDisplayInfo carriers[kNumCarriers];
    for (int i = 0; i < kNumCarriers; i++) {
        carriers[i].name = kCarriers[i].displayName;
        carriers[i].level = &shared.carrierLevels[i];
        carriers[i].brightness = (kCarriers[i].jsonBrightKey != nullptr)
                                    ? &shared.carrierBrightness[i] : nullptr;
    }

    bool soloOn = shared.soloActive.load(std::memory_order_relaxed);
    int soloIdx = soloOn ? shared.soloCarrierIndex.load(std::memory_order_relaxed) : -1;

    printf("Carrier Mix (1-9,0 select, +/- adjust, B/V:brightness, Enter:mute):\033[K\n");
    for (int i = 0; i < kNumCarriers; i++) {
        float lvl = carriers[i].level->load(std::memory_order_relaxed);
        bool selected = (i == selectedItem);
        bool isMuted = shared.carrierMuted[i].load(std::memory_order_relaxed);
        bool isSoloed = (soloOn && i == soloIdx);

        // Display key: 1-9 for items 0-8, 0 for item 9, arrows for 10-12
        char keyLabel[4];
        if (i < 9)       snprintf(keyLabel, sizeof(keyLabel), "%d", i + 1);
        else if (i == 9)  snprintf(keyLabel, sizeof(keyLabel), "0");
        else              snprintf(keyLabel, sizeof(keyLabel), " ");

        if (selected) {
            printf("\033[1m> [%s] ", keyLabel);
        } else {
            printf("  [%s] ", keyLabel);
        }

        // Muted carriers shown in gray, solo'd carrier in yellow
        if (isMuted) {
            printf("\033[90m%-9s ", carriers[i].name);
        } else if (isSoloed) {
            printf("\033[33m%-9s\033[0m%s", carriers[i].name, selected ? "\033[1m " : " ");
        } else {
            printf("%-9s ", carriers[i].name);
        }

        drawLevelBar(lvl, 10);
        printf("  %.2f", lvl);

        if (isMuted) {
            printf("  \033[90m[M]\033[0m");
            if (selected) printf("\033[1m");
        }

        if (carriers[i].brightness != nullptr) {
            float brt = carriers[i].brightness->load(std::memory_order_relaxed);
            printf("  brt: %+.0fdB", brt);
        } else if (i == kCarrierDry) {
            // Dry carrier: show HPF cutoff instead of brightness
            float hpfFreq = shared.dryHPFFreq.load(std::memory_order_relaxed);
            if (hpfFreq <= 0.0f) {
                printf("  HPF: OFF");
            } else {
                printf("  HPF: %.0f Hz", hpfFreq);
            }
        }

        // Show per-carrier transpose (pitched carriers only)
        if (kCarriers[i].jsonTransposeKey != nullptr) {
            int t = shared.carrierTranspose[i].load(std::memory_order_relaxed);
            if (t != 0) {
                printf("  T:%+d", t);
            }
        }

        // Show active MIDI notes for this carrier
        if (i < kNumPitchedCarriers) {
            int noteCount = shared.displayNoteCounts[i].load(std::memory_order_relaxed);
            if (noteCount > 0) {
                printf("  \033[33m");
                for (int n = 0; n < noteCount; n++) {
                    int note = shared.displayNotes[i][n].load(std::memory_order_relaxed);
                    if (note >= 0) {
                        char nb[6];
                        midiToNoteName(note, nb, sizeof(nb));
                        if (n > 0) printf(" ");
                        printf("%s", nb);
                    }
                }
                printf("\033[39m"); // reset color only (preserve bold for selected row)
            }
        }

        if (selected) {
            printf("    <--\033[0m");
        }
        if (isMuted) printf("\033[0m");
        printf("\033[K\n");
    }

    printf("\033[K\n");

    bool autoChord = shared.autoChordEnabled.load(std::memory_order_relaxed);
    if (autoChord) {
        printf("Chord: \033[33mON\033[0m (base C3-E3-G3, per-carrier transpose above)\033[K\n");
    } else {
        printf("Chord: \033[31mOFF\033[0m\033[K\n");
    }

    // Pitch tracking summary
    bool ptOn = shared.pitchTrackEnabled.load(std::memory_order_relaxed);
    int hm = shared.harmonyMode.load(std::memory_order_relaxed);
    printf("Pitch: %s  Harmony: %s\033[K\n",
           ptOn ? "\033[32mON\033[0m" : "\033[31mOFF\033[0m",
           harmonyModeName(static_cast<HarmonyMode>(hm % static_cast<int>(HarmonyMode::Count))));

    // Recording status
    bool recording = shared.recording.load(std::memory_order_relaxed);
    if (recording) {
        int secs = static_cast<int>(recordSampleCount / static_cast<int64_t>(sampleRate));
        printf("\n\033[1;31m\xe2\x97\x8f REC %d:%02d\033[0m\033[K\n", secs / 60, secs % 60);
    } else {
        printf("\033[K\n\033[K\n");
    }

    printf("\033[K\n");
    printf("Tab:Page  \xe2\x86\x91\xe2\x86\x93:Nav  1-9,0:Sel  +/-:Level  B/V:Brt/HPF  Q/W:Semi  A/S:Oct  C:Chord  T:Pitch  Space:Solo  Enter:Mute  D:Monitor  M:Source  O:Rec  P:Save  F:Load  Esc:Quit\033[K\n");
}

static void drawPage2(SharedState& shared, int selectedItem) {
    // Noise gate
    float gateThresh = shared.micGateThreshold.load(std::memory_order_relaxed);
    bool selGate = (selectedItem == 0);
    printf("Modulator Noise Gate:\033[K\n");
    printf("%s [1] Gate Thresh ", selGate ? "\033[1m>" : " ");
    drawDbBar(gateThresh, -60.0f, 0.0f, 10);
    if (gateThresh >= 0.0f) {
        printf("  OFF");
    } else {
        printf("  %.0f dB", gateThresh);
    }
    if (selGate) printf("    <--\033[0m");
    printf("\033[K\n");

    // Show mic buffer latency
    float micLatMs = shared.micBufferLatencyMs.load(std::memory_order_relaxed);
    if (shared.useMic.load(std::memory_order_relaxed) && micLatMs > 0.0f) {
        printf("  Mic buf: %.1f ms\033[K\n", micLatMs);
    } else {
        printf("\033[K\n");
    }

    printf("Modulator Compressor:\033[K\n");

    float thresh = shared.modCompThreshold.load(std::memory_order_relaxed);
    float ratio = shared.modCompRatio.load(std::memory_order_relaxed);

    bool sel2 = (selectedItem == 1);
    printf("%s [2] Threshold  ", sel2 ? "\033[1m>" : " ");
    drawDbBar(thresh, -40.0f, 0.0f, 10);
    printf("  %.0f dB", thresh);
    if (sel2) printf("    <--\033[0m");
    printf("\033[K\n");

    bool sel3 = (selectedItem == 2);
    printf("%s [3] Ratio      ", sel3 ? "\033[1m>" : " ");
    drawDbBar(ratio, 1.0f, 20.0f, 10);
    printf("  %.1f:1", ratio);
    if (sel3) printf("    <--\033[0m");
    printf("\033[K\n");

    // Modulator compressor GR meter
    float modGR = shared.modCompGainReduction.load(std::memory_order_relaxed);
    printf("      GR:        ");
    drawGRBar(modGR, 10);
    printf(" %6.1f dB\033[K\n", modGR);

    printf("\033[K\nModulator EQ:\033[K\n");

    float lo = shared.modEqLow.load(std::memory_order_relaxed);
    float mi = shared.modEqMid.load(std::memory_order_relaxed);
    float hi = shared.modEqHigh.load(std::memory_order_relaxed);

    const char* eqLabels[] = {"Low  200Hz", "Mid 1.5kHz", "High  4kHz"};
    float eqVals[] = {lo, mi, hi};

    for (int i = 0; i < 3; i++) {
        bool sel = (selectedItem == i + 3);
        printf("%s [%d] %-10s ", sel ? "\033[1m>" : " ", i + 4, eqLabels[i]);
        drawDbBar(eqVals[i], -12.0f, 12.0f, 10);
        printf("  %+.0f dB", eqVals[i]);
        if (sel) printf("    <--\033[0m");
        printf("\033[K\n");
    }

    printf("\033[K\n\033[K\n\033[K\n\033[K\n");
    printf("Tab:Page  1-6:Sel  +/-:Adjust  M:Source  T:Pitch  P:Save  F:Load  Esc:Quit\033[K\n");
}

static void drawPage3(SharedState& shared, int selectedItem) {
    printf("Output Compressor:\033[K\n");

    float thresh = shared.outCompThreshold.load(std::memory_order_relaxed);
    float ratio = shared.outCompRatio.load(std::memory_order_relaxed);

    bool sel1 = (selectedItem == 0);
    printf("%s [1] Threshold  ", sel1 ? "\033[1m>" : " ");
    drawDbBar(thresh, -40.0f, 0.0f, 10);
    printf("  %.0f dB", thresh);
    if (sel1) printf("    <--\033[0m");
    printf("\033[K\n");

    bool sel2 = (selectedItem == 1);
    printf("%s [2] Ratio      ", sel2 ? "\033[1m>" : " ");
    drawDbBar(ratio, 1.0f, 20.0f, 10);
    printf("  %.1f:1", ratio);
    if (sel2) printf("    <--\033[0m");
    printf("\033[K\n");

    // Output compressor GR meter
    float outGR = shared.outCompGainReduction.load(std::memory_order_relaxed);
    printf("      GR:        ");
    drawGRBar(outGR, 10);
    printf(" %6.1f dB\033[K\n", outGR);

    printf("\033[K\nOutput EQ:\033[K\n");

    float lo = shared.outEqLow.load(std::memory_order_relaxed);
    float mi = shared.outEqMid.load(std::memory_order_relaxed);
    float hi = shared.outEqHigh.load(std::memory_order_relaxed);

    const char* eqLabels[] = {"Low  200Hz", "Mid 2.5kHz", "High  3kHz"};
    float eqVals[] = {lo, mi, hi};

    for (int i = 0; i < 3; i++) {
        bool sel = (selectedItem == i + 2);
        printf("%s [%d] %-10s ", sel ? "\033[1m>" : " ", i + 3, eqLabels[i]);
        drawDbBar(eqVals[i], -12.0f, 12.0f, 10);
        printf("  %+.0f dB", eqVals[i]);
        if (sel) printf("    <--\033[0m");
        printf("\033[K\n");
    }

    bool eqOn = shared.eqEnabled.load(std::memory_order_relaxed);
    printf("\033[K\nEQ: %s\033[K\n", eqOn ? "\033[32mON\033[0m" : "\033[31mOFF\033[0m");

    printf("\033[K\n\033[K\n\033[K\n\033[K\n");
    printf("Tab:Page  1-5:Sel  +/-:Adjust  E:EQ on/off  M:Source  T:Pitch  P:Save  F:Load  Esc:Quit\033[K\n");
}

static const char* scaleNames[] = {"Chromatic", "Major", "Minor"};

static void drawPage4(SharedState& shared, int selectedItem) {
    bool ptOn = shared.pitchTrackEnabled.load(std::memory_order_relaxed);
    float detFreq = shared.detectedPitch.load(std::memory_order_relaxed);
    float conf = shared.pitchConfidence.load(std::memory_order_relaxed);
    int hm = shared.harmonyMode.load(std::memory_order_relaxed);
    int sc = shared.scaleType.load(std::memory_order_relaxed);
    int sk = shared.scaleKey.load(std::memory_order_relaxed);

    // Detected pitch note name
    char pitchNote[8] = "--";
    if (detFreq > 20.0f) {
        int midiNote = static_cast<int>(roundf(69.0f + 12.0f * log2f(detFreq / 440.0f)));
        midiToNoteName(midiNote, pitchNote, sizeof(pitchNote));
    }

    printf("Pitch Tracking: %s", ptOn ? "\033[32mON\033[0m" : "\033[31mOFF\033[0m");
    printf("        Detected: \033[33m%3s\033[0m (%.0f Hz)  Conf: ", pitchNote, detFreq);
    drawConfidenceBar(conf, 10);
    printf(" %.2f\033[K\n", conf);

    auto harmMode = static_cast<HarmonyMode>(hm % static_cast<int>(HarmonyMode::Count));
    printf("Scale: \033[33m%s %s\033[0m            Harmony: \033[33m%s\033[0m\033[K\n\n",
           noteNames[sk % 12], scaleNames[sc % ScaleQuantizer::Count],
           harmonyModeName(harmMode));

    // Selectable items
    float uvGain = shared.unvoicedNoiseGain.load(std::memory_order_relaxed);
    float uvThresh = shared.unvoicedConfThreshold.load(std::memory_order_relaxed);
    float strongConf = shared.strongConfThreshold.load(std::memory_order_relaxed);

    float pitchLo = shared.pitchMinFreq.load(std::memory_order_relaxed);
    float pitchHi = shared.pitchMaxFreq.load(std::memory_order_relaxed);

    int snapMode = shared.pitchSnapMode.load(std::memory_order_relaxed);
    float corrThresh = shared.pitchCorrThreshold.load(std::memory_order_relaxed);
    float corrAmt = shared.pitchCorrAmount.load(std::memory_order_relaxed);
    float noiseAttk = shared.noiseAttackMs.load(std::memory_order_relaxed);
    float noiseRel = shared.noiseReleaseMs.load(std::memory_order_relaxed);

    static const char* snapModeNames[] = {"Off", "Soft", "Hard"};

    bool autoSustOn = shared.autoSustainEnabled.load(std::memory_order_relaxed);
    float sustThreshDb = shared.autoSustainThresholdDb.load(std::memory_order_relaxed);
    float sustMinMs = shared.autoSustainMinTimeMs.load(std::memory_order_relaxed);

    const char* items[] = {"Pitch Track", "Harmony", "Scale", "Key",
                           "Unvoiced Noise Level", "UV Conf Threshold", "Strong Conf",
                           "Pitch Lo Hz", "Pitch Hi Hz",
                           "Pitch Snap", "Corr Threshold", "Corr Amount",
                           "Noise Attack", "Noise Release",
                           "Auto-Sustain", "Auto-rel mod thresh", "Auto-rel min time"};
    static_assert(sizeof(items) / sizeof(items[0]) == kPage4Items, "kPage4Items mismatch");
    char valueBufs[kPage4Items][16];

    snprintf(valueBufs[0], sizeof(valueBufs[0]), "%s", ptOn ? "ON" : "OFF");
    snprintf(valueBufs[1], sizeof(valueBufs[1]), "%s", harmonyModeName(harmMode));
    snprintf(valueBufs[2], sizeof(valueBufs[2]), "%s", scaleNames[sc % ScaleQuantizer::Count]);
    snprintf(valueBufs[3], sizeof(valueBufs[3]), "%s", noteNames[sk % 12]);
    snprintf(valueBufs[4], sizeof(valueBufs[4]), "%.2f", uvGain);
    snprintf(valueBufs[5], sizeof(valueBufs[5]), "%.2f", uvThresh);
    snprintf(valueBufs[6], sizeof(valueBufs[6]), "%.2f", strongConf);
    snprintf(valueBufs[7], sizeof(valueBufs[7]), "%.0f", pitchLo);
    snprintf(valueBufs[8], sizeof(valueBufs[8]), "%.0f", pitchHi);
    snprintf(valueBufs[9], sizeof(valueBufs[9]), "%s", snapModeNames[snapMode % 3]);
    snprintf(valueBufs[10], sizeof(valueBufs[10]), "%.0f ct", corrThresh);
    snprintf(valueBufs[11], sizeof(valueBufs[11]), "%.0f%%", corrAmt * 100.0f);
    snprintf(valueBufs[12], sizeof(valueBufs[12]), "%.0f ms", noiseAttk);
    snprintf(valueBufs[13], sizeof(valueBufs[13]), "%.0f ms", noiseRel);
    snprintf(valueBufs[14], sizeof(valueBufs[14]), "%s", autoSustOn ? "ON" : "OFF");
    if (sustThreshDb >= 0.0f)
        snprintf(valueBufs[15], sizeof(valueBufs[15]), "0 dB");
    else
        snprintf(valueBufs[15], sizeof(valueBufs[15]), "%.0f dB", sustThreshDb);
    if (sustMinMs >= kSustainTimeInfinityMs)
        snprintf(valueBufs[16], sizeof(valueBufs[16]), "INF");
    else
        snprintf(valueBufs[16], sizeof(valueBufs[16]), "%.0f ms", sustMinMs);

    float corrCents = shared.pitchCorrectionCents.load(std::memory_order_relaxed);

    for (int i = 0; i < kPage4Items; i++) {
        bool sel = (selectedItem == i);
        if (sel) printf("\033[1m");
        printf("%s [%2d] %-24s  ", sel ? ">" : " ", i + 1, items[i]);
        if (i == 4) {
            // Unvoiced noise level: show bar graph (0.0-0.50 range)
            drawLevelBar(uvGain * 2.0f, 10);
            printf("  ");
        }
        printf("%-14s", valueBufs[i]);
        // Pitch correction graph next to Corr Amount (index 11)
        if (i == 11 && snapMode > 0) {
            float absCc = fabsf(corrCents);
            float normalized = std::min(1.0f, absCc / 50.0f);
            int filled = static_cast<int>(normalized * 8 + 0.5f);
            filled = std::max(0, std::min(8, filled));
            printf(" ");
            const char* color = (absCc < 15.0f) ? "\033[32m" :  // green
                                (absCc < 30.0f) ? "\033[33m" :  // yellow
                                                   "\033[31m";  // red
            for (int b = 0; b < filled; b++) printf("%s\xe2\x96\x88\033[0m", color);
            for (int b = filled; b < 8; b++) printf("\033[90m\xe2\x96\x91\033[0m");
            printf(" %+.0f ct", corrCents);
        }
        if (sel) printf("  <--\033[0m");
        printf("\033[K\n");
    }

    printf("\033[K\n\033[K\n");
    printf("Tab:Page  1-4:Sel  +/-:Cycle  T:Pitch  H:Harmony  M:Source  P:Save  F:Load  Esc:Quit\033[K\n");
}

static void drawPage5(SharedState& /*shared*/, MIDIInput* midi, CCMapping* ccMappings) {
    // MIDI status with note names
    if (midi != nullptr && midi->client != 0) {
        int sources = midi->sourceCount.load(std::memory_order_relaxed);
        printf("MIDI Sources: \033[33m%d\033[0m\033[K\n", sources);

        // Active notes
        printf("Active Notes: ");
        bool anyNote = false;
        for (int n = 0; n < MIDIInput::kMaxNotes; n++) {
            int note = midi->activeNotes[n].load(std::memory_order_relaxed);
            if (note >= 0) {
                char nbuf[8];
                midiToNoteName(note, nbuf, sizeof(nbuf));
                if (anyNote) printf(" ");
                printf("\033[33m%s\033[0m", nbuf);
                anyNote = true;
            }
        }
        // Show sustained notes in gray parentheses
        bool anySustained = false;
        for (int n = 0; n < MIDIInput::kMaxNotes; n++) {
            int note = midi->sustainedNotes[n].load(std::memory_order_relaxed);
            if (note >= 0) {
                char nbuf[8];
                midiToNoteName(note, nbuf, sizeof(nbuf));
                if (anyNote || anySustained) printf(" ");
                printf("\033[90m(%s)\033[0m", nbuf);
                anySustained = true;
                anyNote = true;
            }
        }
        if (!anyNote) printf("--");
        printf("\033[K\n");

        // Recent CC activity from ring buffer
        printf("\033[K\nRecent CCs:\033[K\n");
        {
            int wIdx = midi->recentCCWriteIdx.load(std::memory_order_relaxed);
            bool anyShown = false;
            for (int r = 0; r < MIDIInput::kRecentCCCount; r++) {
                int idx = ((wIdx - 1 - r) % MIDIInput::kRecentCCCount + MIDIInput::kRecentCCCount)
                          % MIDIInput::kRecentCCCount;
                auto& evt = midi->recentCCs[idx];
                if (evt.cc == MIDIInput::kCCEventEmpty) break; // hit unwritten slot, stop
                printf("  CC%-3d = %-3d", evt.cc, evt.val);
                // Show a mini bar for the value
                int filled = evt.val * 8 / 127;
                printf("  ");
                for (int b = 0; b < filled; b++) printf("\033[36m\xe2\x96\x88\033[0m");
                for (int b = filled; b < 8; b++) printf("\033[90m\xe2\x96\x91\033[0m");
                printf("\033[K\n");
                anyShown = true;
            }
            if (!anyShown) printf("  --\033[K\n");
        }

        // CC mapping display — one per line
        printf("\033[K\nCC Mapping (L:learn):\033[K\n");
        for (int i = 0; i < kNumCCMappings; i++) {
            if (ccMappings[i].ccNumber >= 0) {
                float curVal = ccMappings[i].target->load(std::memory_order_relaxed);
                printf("  CC%-3d \xe2\x86\x92 %-10s  val: %6.2f  [%.0f..%.0f]\033[K\n",
                       ccMappings[i].ccNumber, ccMappings[i].name,
                       curVal, ccMappings[i].minVal, ccMappings[i].maxVal);
            } else {
                printf("  --    \xe2\x86\x92 %-10s  (unmapped)\033[K\n", ccMappings[i].name);
            }
        }
    } else {
        printf("MIDI: Not available\033[K\n");
    }

    printf("\033[K\n");
    printf("Tab:Page  L:CC Learn  M:Source  T:Pitch  P:Save  F:Load  Esc:Quit\033[K\n");
}

static void drawDisplay(SharedState& shared, int currentPage, int selectedItem,
                         const char* modulatorName, bool micAvailable,
                         MIDIInput* midi, CCMapping* ccMappings,
                         bool savingMode, const char* saveComment,
                         int64_t recordSampleCount = 0, float sampleRate = 48000.0f,
                         const char* profileName = nullptr, bool profileDirty = false) {
    drawHeader(shared, currentPage, modulatorName, micAvailable, profileName, profileDirty);

    switch (currentPage) {
        case 0: drawPage1(shared, selectedItem, recordSampleCount, sampleRate); break;
        case 1: drawPage2(shared, selectedItem); break;
        case 2: drawPage3(shared, selectedItem); break;
        case 3: drawPage4(shared, selectedItem); break;
        case 4: drawPage5(shared, midi, ccMappings); break;
    }

    if (savingMode) {
        printf("\033[K\n\033[1mProfile name (Enter to save, Esc cancel): %s_\033[0m\033[K\n", saveComment);
    }

    printf("\033[J");
    fflush(stdout);
}
