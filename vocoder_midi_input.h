// Written by Andrew Benson — https://github.com/drewster99
// Copyright (C) 2026 Nuclear Cyborg Corp. MIT License — see LICENSE file.

#pragma once

#include <cstdint>
#include <atomic>

#include <CoreMIDI/CoreMIDI.h>

// ============================================================================
// MIDI Input (CoreMIDI)
// ============================================================================

struct MIDIInput {
    MIDIClientRef client = 0;
    MIDIPortRef port = 0;

    static constexpr int kMaxNotes = 8;
    std::atomic<int> activeNotes[kMaxNotes]; // MIDI note numbers, -1 = inactive
    std::atomic<int> noteCount{0};
    std::atomic<bool> hasActiveNotes{false};
    std::atomic<int> sourceCount{0};

    // Auto-sustain: holds notes after note-off until new note-on or voice silence
    std::atomic<int> sustainedNotes[kMaxNotes]; // MIDI note numbers, -1 = empty
    std::atomic<bool> hasSustainedNotes{false};
    std::atomic<bool> autoSustainEnabled{false}; // written by audio thread per-block

    // CC storage
    std::atomic<uint8_t> ccValues[128];
    std::atomic<int> lastCCNumber{-1};
    std::atomic<int> lastCCValue{-1};

    // Direct CC-to-parameter application (from MIDI thread)
    static constexpr int kMaxCCSlots = 16;
    struct CCSlotConfig {
        std::atomic<float>* target;
        float minVal, maxVal;
        int autoSwitchInstrument; // -1 = none, 0-8 = instrument index
    };
    CCSlotConfig ccSlots[kMaxCCSlots] = {};
    std::atomic<int8_t> ccToSlot[128];           // CC number -> slot index (-1 = unmapped)
    std::atomic<int>* instrumentIndex = nullptr;  // for instrument auto-switch
    std::atomic<bool>* synthNeedsReconfigure = nullptr;
    std::atomic<bool> ccReady{false};             // gate: set after setup complete

    // Recent CC ring buffer (for display)
    static constexpr uint8_t kCCEventEmpty = 0xFF; // sentinel: invalid CC number
    struct CCEvent { uint8_t cc; uint8_t val; };
    static constexpr int kRecentCCCount = 8;
    CCEvent recentCCs[kRecentCCCount];
    std::atomic<int> recentCCWriteIdx{0};

    MIDIInput() {
        for (int i = 0; i < kMaxNotes; i++) {
            activeNotes[i].store(-1, std::memory_order_relaxed);
            sustainedNotes[i].store(-1, std::memory_order_relaxed);
        }
        for (int i = 0; i < 128; i++) {
            ccValues[i].store(0xFF, std::memory_order_relaxed); // 0xFF = not yet received
            ccToSlot[i].store(-1, std::memory_order_relaxed);
        }
        for (int i = 0; i < kRecentCCCount; i++) {
            recentCCs[i] = {kCCEventEmpty, 0};
        }
    }

    static void readProc(const MIDIPacketList* pktList, void* readProcRefCon,
                         void* /*srcConnRefCon*/) {
        auto* self = static_cast<MIDIInput*>(readProcRefCon);
        const MIDIPacket* pkt = &pktList->packet[0];

        for (UInt32 i = 0; i < pktList->numPackets; i++) {
            for (UInt16 j = 0; j < pkt->length; ) {
                uint8_t status = pkt->data[j];
                if ((status & 0xF0) == 0x90 && j + 2 < pkt->length) {
                    // Note On
                    uint8_t note = pkt->data[j + 1];
                    uint8_t vel = pkt->data[j + 2];
                    if (vel > 0) {
                        self->noteOn(note);
                    } else {
                        self->noteOff(note);
                    }
                    j += 3;
                } else if ((status & 0xF0) == 0x80 && j + 2 < pkt->length) {
                    // Note Off
                    uint8_t note = pkt->data[j + 1];
                    self->noteOff(note);
                    j += 3;
                } else if ((status & 0xF0) == 0xB0 && j + 2 < pkt->length) {
                    // Control Change
                    uint8_t ccNum = pkt->data[j + 1] & 0x7F;
                    uint8_t ccVal = pkt->data[j + 2] & 0x7F;
                    self->ccValues[ccNum].store(ccVal, std::memory_order_relaxed);
                    self->lastCCNumber.store(ccNum, std::memory_order_relaxed);
                    self->lastCCValue.store(ccVal, std::memory_order_relaxed);

                    // Push to recent CC ring buffer
                    int ridx = self->recentCCWriteIdx.load(std::memory_order_relaxed);
                    self->recentCCs[ridx] = {ccNum, ccVal};
                    self->recentCCWriteIdx.store((ridx + 1) % kRecentCCCount, std::memory_order_relaxed);

                    // Apply CC mapping directly (no polling needed)
                    if (self->ccReady.load(std::memory_order_acquire)) {
                        int8_t slot = self->ccToSlot[ccNum].load(std::memory_order_relaxed);
                        if (slot >= 0 && slot < kMaxCCSlots && self->ccSlots[slot].target) {
                            auto& cfg = self->ccSlots[slot];
                            float val = cfg.minVal + (ccVal / 127.0f) * (cfg.maxVal - cfg.minVal);
                            cfg.target->store(val, std::memory_order_relaxed);

                            // Auto-switch UI-selected instrument when CC adjusts an instrument level
                            // No synthNeedsReconfigure — DLS channels have fixed programs, volumes via CC7
                            int want = cfg.autoSwitchInstrument;
                            if (want >= 0 && self->instrumentIndex) {
                                self->instrumentIndex->store(want, std::memory_order_relaxed);
                            }
                        }
                    }

                    j += 3;
                } else if (status >= 0xF8) {
                    // System real-time (Clock, Start, Continue, Stop, Active Sensing, Reset): 1 byte
                    j += 1;
                } else if (status >= 0x80) {
                    // Skip other channel/system common messages
                    if ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0) {
                        j += 2;  // Program Change, Channel Pressure
                    } else if (status == 0xF0) {
                        // SysEx — skip to end
                        while (j < pkt->length && pkt->data[j] != 0xF7) j++;
                        j++;
                    } else if (status == 0xF1 || status == 0xF3) {
                        j += 2;  // MTC Quarter Frame, Song Select
                    } else if (status >= 0xF4 && status <= 0xF7) {
                        j += 1;  // Undefined (F4, F5), Tune Request (F6), EOX (F7)
                    } else {
                        j += 3;  // Remaining channel voice: Pitch Bend (0xE0), etc.
                    }
                } else {
                    j++; // running status or data byte
                }
            }
            pkt = MIDIPacketNext(pkt);
        }
    }

    /// Release all sustained notes. Idempotent — safe for concurrent calls from
    /// MIDI thread (noteOn) and audio thread (threshold/disable).
    void releaseSustainedNotes() {
        for (int i = 0; i < kMaxNotes; i++)
            sustainedNotes[i].store(-1, std::memory_order_relaxed);
        hasSustainedNotes.store(false, std::memory_order_relaxed);
    }

    void noteOn(uint8_t note) {
        // When auto-sustain is on, release all sustained notes before adding new ones
        if (autoSustainEnabled.load(std::memory_order_relaxed))
            releaseSustainedNotes();

        // Add note to first empty slot
        for (int i = 0; i < kMaxNotes; i++) {
            int expected = -1;
            if (activeNotes[i].compare_exchange_strong(expected, static_cast<int>(note),
                    std::memory_order_relaxed)) {
                noteCount.fetch_add(1, std::memory_order_relaxed);
                hasActiveNotes.store(true, std::memory_order_relaxed);
                return;
            }
            // Already have this note? ignore
            if (activeNotes[i].load(std::memory_order_relaxed) == static_cast<int>(note)) {
                return;
            }
        }
    }

    void noteOff(uint8_t note) {
        // CAS-remove from activeNotes
        for (int i = 0; i < kMaxNotes; i++) {
            int expected = static_cast<int>(note);
            if (activeNotes[i].compare_exchange_strong(expected, -1,
                    std::memory_order_relaxed)) {
                int count = noteCount.fetch_sub(1, std::memory_order_relaxed) - 1;
                hasActiveNotes.store(count > 0, std::memory_order_relaxed);

                // When auto-sustain is on, move the note to sustainedNotes
                if (autoSustainEnabled.load(std::memory_order_relaxed)) {
                    for (int s = 0; s < kMaxNotes; s++) {
                        int exp = -1;
                        if (sustainedNotes[s].compare_exchange_strong(exp, static_cast<int>(note),
                                std::memory_order_relaxed)) {
                            hasSustainedNotes.store(true, std::memory_order_relaxed);
                            break;
                        }
                        // Already sustained? skip
                        if (sustainedNotes[s].load(std::memory_order_relaxed) == static_cast<int>(note))
                            break;
                    }
                }
                return;
            }
        }
    }

    static void notifyProc(const MIDINotification* /*message*/, void* /*refCon*/) {
        // Could handle hotplug here; for now, no-op
    }

    bool init() {
        OSStatus status = MIDIClientCreate(CFSTR("VocoderMixer"), notifyProc, this, &client);
        if (status != noErr) return false;

        status = MIDIInputPortCreate(client, CFSTR("VocoderInput"), readProc, this, &port);
        if (status != noErr) {
            MIDIClientDispose(client);
            client = 0;
            return false;
        }

        // Connect to all available MIDI sources
        ItemCount numSources = MIDIGetNumberOfSources();
        int connected = 0;
        for (ItemCount i = 0; i < numSources; i++) {
            MIDIEndpointRef src = MIDIGetSource(i);
            if (src != 0) {
                status = MIDIPortConnectSource(port, src, nullptr);
                if (status == noErr) connected++;
            }
        }
        sourceCount.store(connected, std::memory_order_relaxed);
        return true;
    }

    void shutdown() {
        if (port) {
            MIDIPortDispose(port);
            port = 0;
        }
        if (client) {
            MIDIClientDispose(client);
            client = 0;
        }
    }
};
