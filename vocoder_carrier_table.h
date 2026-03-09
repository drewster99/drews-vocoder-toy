// Written by Andrew Benson — https://github.com/drewster99
// Copyright (C) 2026 Nuclear Cyborg Corp. MIT License — see LICENSE file.

#pragma once

// ============================================================================
// Carrier Descriptor Table
// ============================================================================
//
// Central definition of all carrier types. Adding a new carrier is a one-line
// change here — settings I/O, display, and CC mapping all derive from this table.

static constexpr int kNumCarriers = 13;         // 12 pitched/noise + dry
static constexpr int kNumPitchedCarriers = 12;   // excludes dry
static constexpr int kCarrierSaw = 0;
static constexpr int kCarrierNoise = 1;
static constexpr int kCarrierBuzz = 2;
static constexpr int kCarrierFirstDLS = 3;
static constexpr int kCarrierLastDLS = 11;
static constexpr int kCarrierDry = 12;
static constexpr int kNumDLSCarriers = 9;

struct CarrierDescriptor {
    const char* displayName;      // for Page 1 UI: "Sawtooth", "Noise", etc.
    const char* ccName;           // for CC mapping display: "Saw", "Noise", etc.
    const char* jsonLevelKey;     // JSON key for level: "saw", "noise", etc.
    const char* jsonBrightKey;    // JSON key for brightness: "sawBrt", nullptr for Dry
    const char* jsonTransposeKey; // JSON key for transpose: "sawTranspose", nullptr for Dry
    float defaultLevel;           // initial level (0.0 - 1.0)
    int dlsChannelIndex;          // -1 for non-DLS, 0-8 for DLS MIDI channel
};

static const CarrierDescriptor kCarriers[kNumCarriers] = {
    {"Sawtooth",  "Saw",      "saw",        "sawBrt",        "sawTranspose",        0.50f, -1},
    {"Noise",     "Noise",    "noise",      "noiseBrt",      nullptr,               0.00f, -1},
    {"Buzz",      "Buzz",     "buzz",       "buzzBrt",       "buzzTranspose",       0.00f, -1},
    {"Organ",     "Organ",    "organ",      "organBrt",      "organTranspose",      0.50f,  0},
    {"Cello",     "Cello",    "cello",      "celloBrt",      "celloTranspose",      0.00f,  1},
    {"Syn Brass", "SynBrass", "brass",      "brassBrt",      "brassTranspose",      0.00f,  2},
    {"Str Ens 2", "StrEns2",  "strEns2",    "strEns2Brt",    "strEns2Transpose",    0.00f,  3},
    {"Pad NewAg", "PadNewAg", "padNewAge",  "padNewAgeBrt",  "padNewAgeTranspose",  0.00f,  4},
    {"FX Sci-Fi", "FXSciFi",  "fxSciFi",    "fxSciFiBrt",    "fxSciFiTranspose",    0.00f,  5},
    {"Bagpipe",   "Bagpipe",  "bagpipe",    "bagpipeBrt",    "bagpipeTranspose",    0.00f,  6},
    {"Ld Fifths", "LdFifths", "leadFifths", "leadFifthsBrt", "leadFifthsTranspose", 0.00f,  7},
    {"Pad Poly",  "PadPoly",  "padPoly",    "padPolyBrt",    "padPolyTranspose",    0.00f,  8},
    {"Dry",       "Dry",      "dry",        nullptr,         nullptr,               0.00f, -1},
};

// ============================================================================
// CC Mapping (shared between UI, settings, and main)
// ============================================================================

#include <atomic>

struct CCMapping {
    int ccNumber;               // -1 = unmapped
    std::atomic<float>* target;
    float minVal, maxVal;
    const char* name;
};

static constexpr int kNumCCMappings = kNumCarriers + 1; // carriers + gate
