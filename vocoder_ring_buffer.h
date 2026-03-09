// Written by Andrew Benson — https://github.com/drewster99
// Copyright (C) 2026 Nuclear Cyborg Corp. MIT License — see LICENSE file.

#pragma once

#include <atomic>
#include <cstdint>

// ============================================================================
// Lock-Free Ring Buffer (for mic input)
// ============================================================================

static constexpr int kRingBufferSize = 16384;
static constexpr int kRecordRingSize = 65536; // ~1.4s at 48kHz — plenty for 50ms drain interval

static_assert((kRingBufferSize & (kRingBufferSize - 1)) == 0, "kRingBufferSize must be power of 2");
static_assert((kRecordRingSize & (kRecordRingSize - 1)) == 0, "kRecordRingSize must be power of 2");

struct RingBuffer {
    float buffer[kRingBufferSize] = {};
    std::atomic<uint32_t> writePos{0};
    std::atomic<uint32_t> readPos{0};

    void write(const float* data, int count) {
        uint32_t wp = writePos.load(std::memory_order_relaxed);
        for (int i = 0; i < count; i++) {
            buffer[wp & (kRingBufferSize - 1)] = data[i];
            wp++;
        }
        writePos.store(wp, std::memory_order_release);
    }

    float read() {
        uint32_t rp = readPos.load(std::memory_order_relaxed);
        uint32_t wp = writePos.load(std::memory_order_acquire);
        if (rp == wp) return 0.0f;
        float val = buffer[rp & (kRingBufferSize - 1)];
        readPos.store(rp + 1, std::memory_order_release);
        return val;
    }

    uint32_t available() {
        return writePos.load(std::memory_order_acquire) - readPos.load(std::memory_order_relaxed);
    }

    bool writeSingle(float sample) {
        uint32_t wp = writePos.load(std::memory_order_relaxed);
        uint32_t rp = readPos.load(std::memory_order_acquire);
        if (wp - rp >= kRingBufferSize - 1) return false; // full
        buffer[wp & (kRingBufferSize - 1)] = sample;
        writePos.store(wp + 1, std::memory_order_release);
        return true;
    }

    void skip(uint32_t count) {
        readPos.fetch_add(count, std::memory_order_release);
    }
};

/// Lock-free SPSC ring buffer for streaming recorded audio from audio thread to main thread.
struct RecordRingBuffer {
    float buffer[kRecordRingSize] = {};
    std::atomic<uint32_t> writePos{0};
    std::atomic<uint32_t> readPos{0};

    bool writeSingle(float sample) {
        uint32_t wp = writePos.load(std::memory_order_relaxed);
        uint32_t rp = readPos.load(std::memory_order_acquire);
        if (wp - rp >= kRecordRingSize - 1) return false;
        buffer[wp & (kRecordRingSize - 1)] = sample;
        writePos.store(wp + 1, std::memory_order_release);
        return true;
    }

    float read() {
        uint32_t rp = readPos.load(std::memory_order_relaxed);
        uint32_t wp = writePos.load(std::memory_order_acquire);
        if (rp == wp) return 0.0f;
        float val = buffer[rp & (kRecordRingSize - 1)];
        readPos.store(rp + 1, std::memory_order_release);
        return val;
    }

    uint32_t available() {
        return writePos.load(std::memory_order_acquire) - readPos.load(std::memory_order_relaxed);
    }

    void reset() {
        readPos.store(0, std::memory_order_relaxed);
        writePos.store(0, std::memory_order_release);
    }
};
