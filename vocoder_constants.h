// Written by Andrew Benson — https://github.com/drewster99
// Copyright (C) 2026 Nuclear Cyborg Corp. MIT License — see LICENSE file.

#pragma once

// ============================================================================
// Named Constants
// ============================================================================
//
// Central definitions for DSP tuning parameters, EQ frequencies, vocoder
// defaults, and UI timing.  Gathered here so magic numbers don't hide
// in multiple files and so tuning is a one-place change.

// --- DSP ---
static constexpr float kPortamentoAlpha         = 0.003f;   // log-space smoothing (~15ms glide)
static constexpr float kPitchHysteresisSemitones = 0.6f;    // prevents pitch jitter
static constexpr float kBrightnessShelfFreq     = 2000.0f;  // high-shelf cutoff for carrier brightness
static constexpr float kMicTargetLatencySeconds  = 0.004f;  // 4ms target mic buffer latency
static constexpr float kPeakDecayCoeff          = 0.1f;     // peak meter exponential decay
static constexpr float kUnvoicedNoiseSmoothCoeff = 0.005f;  // unvoiced noise level smoothing

// --- Vocoder ---
static constexpr int   kDefaultNumBands  = 28;
static constexpr float kDefaultFilterQ   = 6.0f;
static constexpr float kDefaultStartFreq = 100.0f;
static constexpr float kDefaultEndFreq   = 8000.0f;

// --- Modulator EQ frequencies ---
static constexpr float kModEqLowFreq  = 200.0f;
static constexpr float kModEqMidFreq  = 1500.0f;
static constexpr float kModEqHighFreq = 4000.0f;

// --- Output EQ frequencies ---
static constexpr float kOutEqLowFreq  = 200.0f;
static constexpr float kOutEqMidFreq  = 2500.0f;
static constexpr float kOutEqHighFreq = 3000.0f;

// --- Output EQ default gains (dB) ---
static constexpr float kDefaultOutEqLow  = -3.0f;
static constexpr float kDefaultOutEqMid  =  4.0f;
static constexpr float kDefaultOutEqHigh =  8.0f;

// --- UI ---
static constexpr int kDisplayRefreshUs       = 50000;  // 50ms = 20Hz refresh
static constexpr int kMaxTransposeSemitones  = 36;

// --- Auto-sustain ---
static constexpr float kSustainTimeInfinityMs = 999999.0f; // sentinel: never release on silence

// --- Dry HPF ---
static constexpr float kDefaultDryHPFFreq = 2000.0f;  // Hz, default cutoff for dry signal high-pass

// --- Audio buffer ---
static constexpr int kDesiredAudioBufferFrames = 32;
static constexpr int kMaxCallbackFrames = 8192;
