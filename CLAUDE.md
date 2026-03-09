# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**drews_vocoder_toy** is a standalone C++17 command-line tool for real-time channel vocoder audio processing on macOS.

## Building and Running

```bash
# Build (produces ./drews_vocoder_toy executable)
make

# Run (interactive terminal UI, requires mic + speaker access)
./drews_vocoder_toy

# Clean
make clean
```

Single compilation unit: `drews_vocoder_toy.cpp` includes all 14 header-only modules. Linked against Apple frameworks: AudioToolbox, CoreAudio, CoreFoundation, CoreMIDI, AudioUnit. No external dependencies.

## Architecture

### Threading Model

Two threads with lock-free communication:
- **Audio thread** (CoreAudio callback): reads `SharedState` atomics with `memory_order_relaxed`, performs all DSP. No allocations, no locks, no blocking in this path.
- **Main thread**: terminal UI rendering + keyboard input loop. Writes `SharedState` atomics to signal parameter changes. Dirty flags trigger DSP reconfiguration (filter coefficient redesign).

### Signal Flow

```
Modulator (mic or looped WAV file)
  → Noise Gate → Mod EQ (3-band) → Mod Compressor
  → Channel Vocoder (28 bands, heterodyne mixing)
      × Carrier mix (saw/noise/buzz + DLS synths + additive synths)
  → Dry HPF blend → Output Compressor → Output EQ (3-band)
  → Soft Clipper → Speaker + optional WAV recording
```

### Key Structures

- **`SharedState`** (`drews_vocoder_toy.cpp`): All UI→audio atomic parameters. Carrier levels/brightness/transpose, pitch tracking state, compression/EQ params, metering outputs, MIDI auto-sustain.
- **`RenderContext`** (`drews_vocoder_toy.cpp`): Audio-thread-only DSP state. Contains the vocoder, carriers, DLS synth, pitch detector, filters, compressors, recording buffer. Not touched by UI after initialization.

### Module Map

| Header | Responsibility |
|--------|---------------|
| `vocoder_core.h` | Channel vocoder DSP: bandpass filters, envelope followers, heterodyne demodulation |
| `vocoder_render.h` | Audio callback glue: mode selection (MIDI > PitchTrack > Chord > Silent), carrier mixing, DSP chain orchestration |
| `vocoder_carriers.h` | Simple oscillators (saw/noise/buzz) for non-DLS carrier generation |
| `vocoder_dls_synth.h` | Apple DLS synth via AUGraph — 9 GM instruments on separate MIDI channels, pull-rendered |
| `vocoder_pitch.h` | YIN pitch detector, scale quantizer, harmony generator |
| `vocoder_midi_input.h` | CoreMIDI polling, polyphonic note tracking (8 voices), CC mapping with learn mode |
| `vocoder_dynamics.h` | Noise gate (with hysteresis/hold) and soft-knee compressor |
| `vocoder_biquad.h` | Biquad filter (bandpass, high-shelf, low-shelf, peaking EQ, highpass) |
| `vocoder_settings.h` | JSONL settings persistence — keys auto-derived from `vocoder_carrier_table.h` |
| `vocoder_ui.h` | 5-page terminal UI (Carriers, Modulator, Output, Pitch, MIDI) with ANSI color rendering |
| `vocoder_carrier_table.h` | Central carrier descriptor table — adding a carrier is a one-line change here |
| `vocoder_constants.h` | All DSP tuning parameters and magic numbers in one place |
| `vocoder_ring_buffer.h` | Lock-free SPSC ring buffers for recording and metering |
| `vocoder_wav_io.h` | WAV file loading (with resampling) and streaming WAV recording |
| `vocoder_terminal.h` | Terminal raw mode, ANSI escape helpers |

### Carrier Priority System

Mode is determined per audio callback: **MIDI > PitchTrack > Chord > Silent**. When MIDI notes are active, they override pitch tracking. When pitch tracking detects voice, it overrides the default auto-chord. This is implemented in `renderUpdateDLSMode` in `vocoder_render.h`.

### Settings Persistence

JSONL format — one JSON object per line, each representing a complete profile snapshot. JSON keys are auto-generated from the `CarrierDescriptor` table in `vocoder_carrier_table.h`, so adding a new carrier automatically gets settings I/O.

## Critical Design Constraints

- **Real-time safety**: No malloc/free, no mutex, no blocking I/O in the audio callback. Pre-allocated buffers only.
- **Denormal handling**: Manual flush-to-zero in biquad state and ring buffers to avoid denormal performance penalty on x86.
- **Atomics**: `memory_order_relaxed` for most audio-thread reads (acceptable staleness). `release`/`acquire` only where ordering matters.
- **DLS synth**: Pull-rendered (not callback-driven) — the audio thread calls into AUGraph to render exactly the samples it needs per callback.

## Testing

No automated test suite. Testing is manual:
- Test audio files in `test_audio/` (speech and singing samples)
- WAV recordings captured via the `R` key in the UI
- Settings profiles in `vocoder_settings.jsonl` for regression testing specific configurations
