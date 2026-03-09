// Written by Andrew Benson — https://github.com/drewster99
// Copyright (C) 2026 Nuclear Cyborg Corp. MIT License — see LICENSE file.

#pragma once

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>
#include <AudioToolbox/AudioToolbox.h>

// ============================================================================
// WAV File I/O (from vocoder_test.cpp)
// ============================================================================

#pragma pack(push, 1)
struct WAVHeader {
    char riff[4];
    uint32_t fileSize;
    char wave[4];
    char fmt[4];
    uint32_t fmtSize;
    uint16_t audioFormat;
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
};

struct WAVDataChunk {
    char data[4];
    uint32_t dataSize;
};
#pragma pack(pop)

struct AudioFileBuffer {
    std::vector<float> samples;
    uint32_t sampleRate = 0;
    uint16_t numChannels = 0;
    size_t numFrames = 0;
};

static bool readWAV(const char* path, AudioFileBuffer& buf) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "ERROR: Cannot open '%s'\n", path);
        return false;
    }

    WAVHeader header;
    if (fread(&header, sizeof(WAVHeader), 1, f) != 1) {
        fprintf(stderr, "ERROR: Cannot read WAV header from '%s'\n", path);
        fclose(f);
        return false;
    }

    if (memcmp(header.riff, "RIFF", 4) != 0 || memcmp(header.wave, "WAVE", 4) != 0) {
        fprintf(stderr, "ERROR: Not a valid WAV file: '%s'\n", path);
        fclose(f);
        return false;
    }

    if (header.audioFormat != 1) {
        fprintf(stderr, "ERROR: Only PCM WAV supported (got format %d)\n", header.audioFormat);
        fclose(f);
        return false;
    }

    if (header.bitsPerSample != 16) {
        fprintf(stderr, "ERROR: Only 16-bit WAV supported (got %d bits)\n", header.bitsPerSample);
        fclose(f);
        return false;
    }

    if (header.numChannels == 0 || header.sampleRate == 0) {
        fprintf(stderr, "ERROR: Invalid WAV: %d channels, %u Hz sample rate\n",
                header.numChannels, header.sampleRate);
        fclose(f);
        return false;
    }

    if (header.fmtSize > 16) {
        fseek(f, header.fmtSize - 16, SEEK_CUR);
    }

    WAVDataChunk dataChunk;
    while (true) {
        if (fread(&dataChunk, sizeof(WAVDataChunk), 1, f) != 1) {
            fprintf(stderr, "ERROR: Cannot find data chunk in '%s'\n", path);
            fclose(f);
            return false;
        }
        if (memcmp(dataChunk.data, "data", 4) == 0) {
            break;
        }
        fseek(f, dataChunk.dataSize, SEEK_CUR);
    }

    size_t totalSamples = dataChunk.dataSize / sizeof(int16_t);

    // Sanity check: reject absurdly large data chunks (cap at ~250M samples ≈ 87 min stereo 48kHz)
    constexpr size_t kMaxWAVSamples = 250 * 1024 * 1024;
    if (totalSamples > kMaxWAVSamples) {
        fprintf(stderr, "ERROR: WAV data chunk too large (%zu samples) in '%s'\n", totalSamples, path);
        fclose(f);
        return false;
    }

    size_t numFrames = totalSamples / header.numChannels;

    std::vector<int16_t> rawSamples(totalSamples);
    size_t readCount = fread(rawSamples.data(), sizeof(int16_t), totalSamples, f);
    fclose(f);

    if (readCount != totalSamples) {
        numFrames = readCount / header.numChannels;
    }

    buf.samples.resize(numFrames);
    buf.sampleRate = header.sampleRate;
    buf.numChannels = 1;
    buf.numFrames = numFrames;

    for (size_t i = 0; i < numFrames; i++) {
        float sum = 0.0f;
        for (int ch = 0; ch < header.numChannels; ch++) {
            sum += rawSamples[i * header.numChannels + ch] / 32768.0f;
        }
        buf.samples[i] = sum / header.numChannels;
    }

    return true;
}

/// Read any audio file format supported by Apple AudioToolbox (WAV, MP3, M4A, AAC, AIF, etc.)
/// Returns mono float samples at the file's native sample rate.
static bool readAudioFileAT(const char* path, AudioFileBuffer& buf) {
    CFURLRef fileURL = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(path),
        static_cast<CFIndex>(strlen(path)),
        false);
    if (!fileURL) {
        fprintf(stderr, "ERROR: Cannot create URL for '%s'\n", path);
        return false;
    }

    ExtAudioFileRef extFile = nullptr;
    OSStatus status = ExtAudioFileOpenURL(fileURL, &extFile);
    CFRelease(fileURL);
    if (status != noErr) {
        fprintf(stderr, "ERROR: Cannot open audio file '%s' (OSStatus %d)\n", path, static_cast<int>(status));
        return false;
    }

    // Get the file's native format to determine sample rate and frame count
    AudioStreamBasicDescription fileFormat = {};
    UInt32 propSize = sizeof(fileFormat);
    status = ExtAudioFileGetProperty(extFile, kExtAudioFileProperty_FileDataFormat, &propSize, &fileFormat);
    if (status != noErr) {
        fprintf(stderr, "ERROR: Cannot get format for '%s' (OSStatus %d)\n", path, static_cast<int>(status));
        ExtAudioFileDispose(extFile);
        return false;
    }

    // Get total frame count
    SInt64 totalFrames = 0;
    propSize = sizeof(totalFrames);
    status = ExtAudioFileGetProperty(extFile, kExtAudioFileProperty_FileLengthFrames, &propSize, &totalFrames);
    if (status != noErr || totalFrames <= 0) {
        fprintf(stderr, "ERROR: Cannot get frame count for '%s' (OSStatus %d, frames %lld)\n",
                path, static_cast<int>(status), totalFrames);
        ExtAudioFileDispose(extFile);
        return false;
    }

    // Cap at ~87 min mono 48kHz to match WAV reader safety limit
    constexpr SInt64 kMaxFrames = 250 * 1024 * 1024;
    if (totalFrames > kMaxFrames) {
        fprintf(stderr, "ERROR: Audio file too large (%lld frames) in '%s'\n", totalFrames, path);
        ExtAudioFileDispose(extFile);
        return false;
    }

    // Set client format: mono Float32 at file's native sample rate
    AudioStreamBasicDescription clientFormat = {};
    clientFormat.mSampleRate = fileFormat.mSampleRate;
    clientFormat.mFormatID = kAudioFormatLinearPCM;
    clientFormat.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
    clientFormat.mBitsPerChannel = 32;
    clientFormat.mChannelsPerFrame = 1;
    clientFormat.mFramesPerPacket = 1;
    clientFormat.mBytesPerFrame = sizeof(float);
    clientFormat.mBytesPerPacket = sizeof(float);

    status = ExtAudioFileSetProperty(extFile, kExtAudioFileProperty_ClientDataFormat,
                                     sizeof(clientFormat), &clientFormat);
    if (status != noErr) {
        fprintf(stderr, "ERROR: Cannot set client format for '%s' (OSStatus %d)\n", path, static_cast<int>(status));
        ExtAudioFileDispose(extFile);
        return false;
    }

    // Read all frames
    auto numFrames = static_cast<size_t>(totalFrames);
    buf.samples.resize(numFrames);
    buf.sampleRate = static_cast<uint32_t>(fileFormat.mSampleRate);
    buf.numChannels = 1;
    buf.numFrames = numFrames;

    constexpr UInt32 kReadChunkFrames = 8192;
    size_t framesRead = 0;
    while (framesRead < numFrames) {
        UInt32 framesToRead = static_cast<UInt32>(std::min(
            static_cast<size_t>(kReadChunkFrames), numFrames - framesRead));

        AudioBufferList abl;
        abl.mNumberBuffers = 1;
        abl.mBuffers[0].mNumberChannels = 1;
        abl.mBuffers[0].mDataByteSize = framesToRead * sizeof(float);
        abl.mBuffers[0].mData = buf.samples.data() + framesRead;

        UInt32 ioFrames = framesToRead;
        status = ExtAudioFileRead(extFile, &ioFrames, &abl);
        if (status != noErr) {
            fprintf(stderr, "ERROR: Read failed for '%s' at frame %zu (OSStatus %d)\n",
                    path, framesRead, static_cast<int>(status));
            ExtAudioFileDispose(extFile);
            return false;
        }
        if (ioFrames == 0) break; // EOF
        framesRead += ioFrames;
    }

    ExtAudioFileDispose(extFile);

    // Adjust if we read fewer frames than expected
    if (framesRead < numFrames) {
        buf.numFrames = framesRead;
        buf.samples.resize(framesRead);
    }

    return true;
}

/// Write mono 16-bit PCM WAV file from float samples.
static bool writeWAV(const char* path, const float* samples, int numSamples, uint32_t sampleRate) {
    if (numSamples <= 0) {
        fprintf(stderr, "ERROR: writeWAV called with %d samples for '%s'\n", numSamples, path);
        return false;
    }

    // WAV data chunk size is uint32_t; clamp to avoid overflow (~2.1 billion samples)
    constexpr int64_t kMaxWAVSamples = static_cast<int64_t>(UINT32_MAX) / sizeof(int16_t);
    if (static_cast<int64_t>(numSamples) > kMaxWAVSamples) {
        fprintf(stderr, "WARNING: writeWAV clamping %d samples to WAV 4GB limit for '%s'\n",
                numSamples, path);
        numSamples = static_cast<int>(kMaxWAVSamples);
    }

    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "ERROR: Cannot create '%s'\n", path);
        return false;
    }

    uint32_t dataSize = static_cast<uint32_t>(numSamples) * sizeof(int16_t);
    uint32_t fileSize = sizeof(WAVHeader) + sizeof(WAVDataChunk) + dataSize - 8;

    WAVHeader header;
    memcpy(header.riff, "RIFF", 4);
    header.fileSize = fileSize;
    memcpy(header.wave, "WAVE", 4);
    memcpy(header.fmt, "fmt ", 4);
    header.fmtSize = 16;
    header.audioFormat = 1; // PCM
    header.numChannels = 1;
    header.sampleRate = sampleRate;
    header.byteRate = sampleRate * sizeof(int16_t);
    header.blockAlign = sizeof(int16_t);
    header.bitsPerSample = 16;

    WAVDataChunk dataChunk;
    memcpy(dataChunk.data, "data", 4);
    dataChunk.dataSize = dataSize;

    if (fwrite(&header, sizeof(WAVHeader), 1, f) != 1 ||
        fwrite(&dataChunk, sizeof(WAVDataChunk), 1, f) != 1) {
        fprintf(stderr, "ERROR: Cannot write WAV header to '%s'\n", path);
        fclose(f);
        return false;
    }

    // Convert and write in chunks to avoid huge temporary allocation
    constexpr int kChunkSize = 4096;
    int16_t chunk[kChunkSize];
    int remaining = numSamples;
    const float* src = samples;
    while (remaining > 0) {
        int count = std::min(remaining, kChunkSize);
        for (int i = 0; i < count; i++) {
            float clamped = std::max(-1.0f, std::min(1.0f, src[i]));
            chunk[i] = static_cast<int16_t>(clamped * 32767.0f);
        }
        if (fwrite(chunk, sizeof(int16_t), static_cast<size_t>(count), f) != static_cast<size_t>(count)) {
            fprintf(stderr, "ERROR: Write failed for '%s'\n", path);
            fclose(f);
            return false;
        }
        src += count;
        remaining -= count;
    }

    fclose(f);
    return true;
}
