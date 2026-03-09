// Written by Andrew Benson — https://github.com/drewster99
// Copyright (C) 2026 Nuclear Cyborg Corp. MIT License — see LICENSE file.

#pragma once

#include "vocoder_constants.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <climits>
#include <atomic>

#include "vocoder_carrier_table.h"

/// Find a JSON key's value in a flat JSON line. Returns pointer to value start, or nullptr.
static const char* jsonFindValue(const char* json, const char* key) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char* p = strstr(json, pattern);
    if (p == nullptr) return nullptr;
    p += strlen(pattern);
    while (*p == ' ') p++;
    return p;
}

static float jsonFloat(const char* json, const char* key, float defaultVal) {
    const char* v = jsonFindValue(json, key);
    if (v == nullptr) return defaultVal;
    char* end = nullptr;
    float val = strtof(v, &end);
    if (end == v) return defaultVal;
    return val;
}

static int jsonInt(const char* json, const char* key, int defaultVal) {
    const char* v = jsonFindValue(json, key);
    if (v == nullptr) return defaultVal;
    char* end = nullptr;
    long val = strtol(v, &end, 10);
    if (end == v) return defaultVal;
    return static_cast<int>(val);
}

static bool jsonBool(const char* json, const char* key, bool defaultVal) {
    const char* v = jsonFindValue(json, key);
    if (v == nullptr) return defaultVal;
    if (strncmp(v, "true", 4) == 0) return true;
    if (strncmp(v, "false", 5) == 0) return false;
    return defaultVal;
}

/// Parse ccMap array: "ccMap":[1,2,-1,...] into output array.
static void jsonIntArray(const char* json, const char* key, int* out, int maxCount) {
    const char* v = jsonFindValue(json, key);
    if (v == nullptr || *v != '[') return;
    v++; // skip '['
    for (int i = 0; i < maxCount; i++) {
        while (*v == ' ') v++;
        if (*v == ']' || *v == '\0') break;
        char* end = nullptr;
        long val = strtol(v, &end, 10);
        if (end == v) break;
        out[i] = static_cast<int>(val);
        v = end;
        if (*v == ',') v++;
    }
}

/// Apply a JSON settings line to SharedState. Returns false if the line is empty.
static bool applySettingsLine(const char* json, SharedState& shared,
                              int* ccNumbers = nullptr, int ccCount = 0) {
    if (json == nullptr || json[0] == '\0') return false;

    // Carrier levels, brightness, and transpose — table-driven from kCarriers[]
    for (int i = 0; i < kNumCarriers; i++) {
        shared.carrierLevels[i].store(
            jsonFloat(json, kCarriers[i].jsonLevelKey, kCarriers[i].defaultLevel),
            std::memory_order_relaxed);
        if (kCarriers[i].jsonBrightKey)
            shared.carrierBrightness[i].store(
                jsonFloat(json, kCarriers[i].jsonBrightKey, 0.0f),
                std::memory_order_relaxed);
        if (kCarriers[i].jsonTransposeKey)
            shared.carrierTranspose[i].store(
                jsonInt(json, kCarriers[i].jsonTransposeKey, 0),
                std::memory_order_relaxed);
    }
    // Legacy fallback: old "transpose" key applied to organ/cello/brass when per-carrier keys absent
    {
        int organT = jsonInt(json, "organTranspose", INT_MIN);
        int celloT = jsonInt(json, "celloTranspose", INT_MIN);
        int brassT = jsonInt(json, "brassTranspose", INT_MIN);
        if (organT == INT_MIN && celloT == INT_MIN && brassT == INT_MIN) {
            int oldTranspose = jsonInt(json, "transpose", 0);
            shared.carrierTranspose[kCarrierFirstDLS + 0].store(oldTranspose, std::memory_order_relaxed);
            shared.carrierTranspose[kCarrierFirstDLS + 1].store(oldTranspose, std::memory_order_relaxed);
            shared.carrierTranspose[kCarrierFirstDLS + 2].store(oldTranspose, std::memory_order_relaxed);
        }
    }
    shared.instrumentIndex.store(jsonInt(json, "instrument", 0), std::memory_order_relaxed);
    shared.autoChordEnabled.store(jsonBool(json, "autoChord", true), std::memory_order_relaxed);

    // Modulator processing
    shared.modCompThreshold.store(jsonFloat(json, "modCompThresh", 0.0f), std::memory_order_relaxed);
    shared.modCompRatio.store(jsonFloat(json, "modCompRatio", 4.0f), std::memory_order_relaxed);
    shared.modEqLow.store(jsonFloat(json, "modEqLo", 0.0f), std::memory_order_relaxed);
    shared.modEqMid.store(jsonFloat(json, "modEqMid", 0.0f), std::memory_order_relaxed);
    shared.modEqHigh.store(jsonFloat(json, "modEqHi", 0.0f), std::memory_order_relaxed);

    // Output processing
    shared.outCompThreshold.store(jsonFloat(json, "outCompThresh", 0.0f), std::memory_order_relaxed);
    shared.outCompRatio.store(jsonFloat(json, "outCompRatio", 4.0f), std::memory_order_relaxed);
    shared.outEqLow.store(jsonFloat(json, "outEqLo", kDefaultOutEqLow), std::memory_order_relaxed);
    shared.outEqMid.store(jsonFloat(json, "outEqMid", kDefaultOutEqMid), std::memory_order_relaxed);
    shared.outEqHigh.store(jsonFloat(json, "outEqHi", kDefaultOutEqHigh), std::memory_order_relaxed);

    // Pitch tracking
    shared.pitchTrackEnabled.store(jsonBool(json, "pitchTrack", false), std::memory_order_relaxed);
    shared.harmonyMode.store(jsonInt(json, "harmony", 0), std::memory_order_relaxed);
    shared.scaleType.store(jsonInt(json, "scale", 0), std::memory_order_relaxed);
    shared.scaleKey.store(jsonInt(json, "key", 0), std::memory_order_relaxed);
    shared.unvoicedNoiseGain.store(jsonFloat(json, "uvNoiseGain", 0.15f), std::memory_order_relaxed);
    shared.strongConfThreshold.store(jsonFloat(json, "strongConf", 0.50f), std::memory_order_relaxed);
    {
        // Ensure unvoiced threshold stays below strong threshold after loading
        float strongConf = shared.strongConfThreshold.load(std::memory_order_relaxed);
        float uvConf = jsonFloat(json, "uvConfThresh", 0.30f);
        if (uvConf >= strongConf) uvConf = strongConf - 0.05f;
        if (uvConf < 0.10f) uvConf = 0.10f;
        shared.unvoicedConfThreshold.store(uvConf, std::memory_order_relaxed);
    }
    float pitchLo = std::max(20.0f, jsonFloat(json, "pitchMinHz", 50.0f));
    float pitchHi = std::min(5000.0f, jsonFloat(json, "pitchMaxHz", 2000.0f));
    if (pitchHi - pitchLo < 5.0f) pitchHi = pitchLo + 5.0f;
    shared.pitchMinFreq.store(pitchLo, std::memory_order_relaxed);
    shared.pitchMaxFreq.store(pitchHi, std::memory_order_relaxed);
    shared.pitchSnapMode.store(jsonInt(json, "pitchSnap", 2), std::memory_order_relaxed);
    shared.pitchCorrThreshold.store(jsonFloat(json, "pitchCorrThresh", 25.0f), std::memory_order_relaxed);
    shared.pitchCorrAmount.store(jsonFloat(json, "pitchCorrAmt", 0.50f), std::memory_order_relaxed);
    shared.noiseAttackMs.store(jsonFloat(json, "noiseAttackMs", 5.0f), std::memory_order_relaxed);
    shared.noiseReleaseMs.store(jsonFloat(json, "noiseReleaseMs", 5.0f), std::memory_order_relaxed);

    // Auto-sustain
    shared.autoSustainEnabled.store(jsonBool(json, "autoSustain", false), std::memory_order_relaxed);
    shared.autoSustainThresholdDb.store(jsonFloat(json, "autoSustainThreshDb", -30.0f), std::memory_order_relaxed);
    shared.autoSustainMinTimeMs.store(jsonFloat(json, "autoSustainMinMs", 10.0f), std::memory_order_relaxed);

    // Misc
    shared.eqEnabled.store(jsonBool(json, "eq", true), std::memory_order_relaxed);
    // useMic is not restored from profiles — modulator source is controlled only by 'M' key
    shared.micGateThreshold.store(jsonFloat(json, "micGate", -40.0f), std::memory_order_relaxed);

    // Dry HPF
    shared.dryHPFFreq.store(jsonFloat(json, "dryHPF", kDefaultDryHPFFreq), std::memory_order_relaxed);

    // Carrier muted states
    for (int i = 0; i < kNumCarriers; i++) {
        char key[32];
        snprintf(key, sizeof(key), "mute%d", i);
        shared.carrierMuted[i].store(jsonBool(json, key, false), std::memory_order_relaxed);
    }

    // Dirty flags so EQ/brightness/HPF filters get redesigned on first callback
    shared.modEqNeedsRedesign.store(true, std::memory_order_relaxed);
    shared.outEqNeedsRedesign.store(true, std::memory_order_relaxed);
    shared.carrierBrightnessNeedsRedesign.store(true, std::memory_order_relaxed);
    shared.dryHPFNeedsRedesign.store(true, std::memory_order_relaxed);
    shared.synthNeedsReconfigure.store(true, std::memory_order_relaxed);

    // CC mappings
    if (ccNumbers != nullptr && ccCount > 0) {
        jsonIntArray(json, "ccMap", ccNumbers, ccCount);
    }

    return true;
}

/// Extract the profile name (comment) from a JSON line into buf. Returns length.
static int jsonProfileName(const char* json, char* buf, int bufSize) {
    buf[0] = '\0';
    const char* v = jsonFindValue(json, "comment");
    if (v == nullptr || *v != '"') return 0;
    v++;
    int len = 0;
    while (*v != '\0' && *v != '"' && len < bufSize - 1) {
        if (*v == '\\' && *(v + 1) != '\0') v++; // skip escape
        buf[len++] = *v++;
    }
    buf[len] = '\0';
    return len;
}

/// Extract the timestamp from a JSON line into buf.
static void jsonTimestamp(const char* json, char* buf, int bufSize) {
    buf[0] = '\0';
    const char* v = jsonFindValue(json, "timestamp");
    if (v == nullptr || *v != '"') return;
    v++;
    int len = 0;
    while (*v != '\0' && *v != '"' && len < bufSize - 1) {
        buf[len++] = *v++;
    }
    buf[len] = '\0';
}

/// Read all lines from vocoder_settings.jsonl into an array. Returns count.
static constexpr int kMaxProfiles = 50;

static int readAllProfiles(char profiles[][4096], int maxProfiles) {
    FILE* f = fopen("vocoder_settings.jsonl", "r");
    if (f == nullptr) return 0;
    int count = 0;
    while (count < maxProfiles && fgets(profiles[count], 4096, f) != nullptr) {
        if (strlen(profiles[count]) > 1) {
            count++;
        }
    }
    fclose(f);
    return count;
}

/// Load the last line from vocoder_settings.jsonl and apply to SharedState.
/// If profileNameOut is provided, the profile name is copied into it.
static bool loadLastSettings(SharedState& shared, int* ccNumbers = nullptr,
                             int ccCount = 0,
                             char* profileNameOut = nullptr,
                             int profileNameOutSize = 0) {
    FILE* f = fopen("vocoder_settings.jsonl", "r");
    if (f == nullptr) return false;

    char lastLine[4096] = {};
    char line[4096];
    while (fgets(line, sizeof(line), f) != nullptr) {
        if (strlen(line) > 1) {
            memcpy(lastLine, line, strlen(line) + 1);
        }
    }
    fclose(f);

    if (!applySettingsLine(lastLine, shared, ccNumbers, ccCount)) return false;

    // Print loaded profile info
    char name[256];
    jsonProfileName(lastLine, name, sizeof(name));
    char ts[64];
    jsonTimestamp(lastLine, ts, sizeof(ts));

    if (name[0] != '\0') {
        printf("Loaded profile: \"%s\"", name);
    } else {
        printf("Loaded last saved settings");
    }
    if (ts[0] != '\0') {
        printf(" (saved %s)", ts);
    }
    printf("\n");

    if (profileNameOut != nullptr && profileNameOutSize > 0) {
        strncpy(profileNameOut, name, profileNameOutSize - 1);
        profileNameOut[profileNameOutSize - 1] = '\0';
    }

    return true;
}

static void saveSettings(SharedState& shared, const char* comment, MIDIInput* /*midi*/,
                          CCMapping* ccMappings = nullptr) {
    FILE* f = fopen("vocoder_settings.jsonl", "a");
    if (f == nullptr) {
        fprintf(stderr, "WARNING: Cannot save settings (failed to open vocoder_settings.jsonl)\n");
        return;
    }

    // Get timestamp
    time_t now = time(nullptr);
    struct tm tm_buf;
    struct tm* tm = localtime_r(&now, &tm_buf);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm);

    // Escape comment for JSON (handle " and \ characters)
    char escapedComment[512];
    int out = 0;
    for (int in = 0; comment[in] != '\0' && out < 510; in++) {
        if (comment[in] == '"' || comment[in] == '\\') {
            escapedComment[out++] = '\\';
        }
        escapedComment[out++] = comment[in];
    }
    escapedComment[out] = '\0';

    fprintf(f, "{\"timestamp\":\"%s\",\"comment\":\"%s\"", timestamp, escapedComment);

    // Carrier levels, brightness, and transpose — table-driven from kCarriers[]
    for (int i = 0; i < kNumCarriers; i++) {
        fprintf(f, ",\"%s\":%.2f", kCarriers[i].jsonLevelKey,
                shared.carrierLevels[i].load(std::memory_order_relaxed));
        if (kCarriers[i].jsonBrightKey)
            fprintf(f, ",\"%s\":%.0f", kCarriers[i].jsonBrightKey,
                    shared.carrierBrightness[i].load(std::memory_order_relaxed));
        if (kCarriers[i].jsonTransposeKey)
            fprintf(f, ",\"%s\":%d", kCarriers[i].jsonTransposeKey,
                    shared.carrierTranspose[i].load(std::memory_order_relaxed));
    }
    fprintf(f, ",\"instrument\":%d",
            shared.instrumentIndex.load(std::memory_order_relaxed));
    fprintf(f, ",\"autoChord\":%s",
            shared.autoChordEnabled.load(std::memory_order_relaxed) ? "true" : "false");

    // Modulator processing
    fprintf(f, ",\"modCompThresh\":%.0f,\"modCompRatio\":%.1f",
            shared.modCompThreshold.load(std::memory_order_relaxed),
            shared.modCompRatio.load(std::memory_order_relaxed));
    fprintf(f, ",\"modEqLo\":%.0f,\"modEqMid\":%.0f,\"modEqHi\":%.0f",
            shared.modEqLow.load(std::memory_order_relaxed),
            shared.modEqMid.load(std::memory_order_relaxed),
            shared.modEqHigh.load(std::memory_order_relaxed));

    // Output processing
    fprintf(f, ",\"outCompThresh\":%.0f,\"outCompRatio\":%.1f",
            shared.outCompThreshold.load(std::memory_order_relaxed),
            shared.outCompRatio.load(std::memory_order_relaxed));
    fprintf(f, ",\"outEqLo\":%.0f,\"outEqMid\":%.0f,\"outEqHi\":%.0f",
            shared.outEqLow.load(std::memory_order_relaxed),
            shared.outEqMid.load(std::memory_order_relaxed),
            shared.outEqHigh.load(std::memory_order_relaxed));

    // Pitch tracking
    fprintf(f, ",\"pitchTrack\":%s,\"harmony\":%d,\"scale\":%d,\"key\":%d",
            shared.pitchTrackEnabled.load(std::memory_order_relaxed) ? "true" : "false",
            shared.harmonyMode.load(std::memory_order_relaxed),
            shared.scaleType.load(std::memory_order_relaxed),
            shared.scaleKey.load(std::memory_order_relaxed));
    fprintf(f, ",\"uvNoiseGain\":%.2f,\"uvConfThresh\":%.2f,\"strongConf\":%.2f",
            shared.unvoicedNoiseGain.load(std::memory_order_relaxed),
            shared.unvoicedConfThreshold.load(std::memory_order_relaxed),
            shared.strongConfThreshold.load(std::memory_order_relaxed));
    fprintf(f, ",\"pitchMinHz\":%.0f,\"pitchMaxHz\":%.0f",
            shared.pitchMinFreq.load(std::memory_order_relaxed),
            shared.pitchMaxFreq.load(std::memory_order_relaxed));
    fprintf(f, ",\"pitchSnap\":%d,\"pitchCorrThresh\":%.0f,\"pitchCorrAmt\":%.2f",
            shared.pitchSnapMode.load(std::memory_order_relaxed),
            shared.pitchCorrThreshold.load(std::memory_order_relaxed),
            shared.pitchCorrAmount.load(std::memory_order_relaxed));
    fprintf(f, ",\"noiseAttackMs\":%.0f,\"noiseReleaseMs\":%.0f",
            shared.noiseAttackMs.load(std::memory_order_relaxed),
            shared.noiseReleaseMs.load(std::memory_order_relaxed));

    fprintf(f, ",\"eq\":%s",
            shared.eqEnabled.load(std::memory_order_relaxed) ? "true" : "false");

    // Mic settings
    // useMic is not saved — modulator source is controlled only by 'M' key

    // Noise gate
    fprintf(f, ",\"micGate\":%.0f",
            shared.micGateThreshold.load(std::memory_order_relaxed));

    // Dry HPF
    fprintf(f, ",\"dryHPF\":%.0f",
            shared.dryHPFFreq.load(std::memory_order_relaxed));

    // Carrier muted states
    for (int i = 0; i < kNumCarriers; i++) {
        bool muted = shared.carrierMuted[i].load(std::memory_order_relaxed);
        if (muted) {
            fprintf(f, ",\"mute%d\":true", i);
        }
    }

    // Auto-sustain
    fprintf(f, ",\"autoSustain\":%s",
            shared.autoSustainEnabled.load(std::memory_order_relaxed) ? "true" : "false");
    fprintf(f, ",\"autoSustainThreshDb\":%.0f",
            shared.autoSustainThresholdDb.load(std::memory_order_relaxed));
    fprintf(f, ",\"autoSustainMinMs\":%.0f",
            shared.autoSustainMinTimeMs.load(std::memory_order_relaxed));

    // CC mappings
    if (ccMappings != nullptr) {
        fprintf(f, ",\"ccMap\":[");
        for (int i = 0; i < kNumCCMappings; i++) {
            if (i > 0) fprintf(f, ",");
            fprintf(f, "%d", ccMappings[i].ccNumber);
        }
        fprintf(f, "]");
    }

    fprintf(f, "}\n");
    fclose(f);
}
