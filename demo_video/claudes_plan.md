# Video Generation Plan — Vocoder Demo Video

## Context
Create a demo video showcasing 3 vocoder audio clips, with audio-reactive visualizations, to embed in the GitHub README (via drag-drop MP4). The generation should be scripted in Python for easy iteration.

## Audio Clip Order & Names
1. `andrew-vocoder-built-in-pitch-track-chord.m4a` → "Pitch Tracked Chord" (~11.2s)
2. `andrew-vocoder-test-scratchy-robot.m4a` → "Scratchy Robot" (~7.6s)
3. `andrew-vocoder-test-manyvoice-robot.m4a` → "Many Voices Robot" (~6.4s)

**Total duration: ~25.2s**

## Video Specs
- Resolution: 1280x720 (16:9, good for GitHub embed)
- Frame rate: 30fps
- Output: MP4 (H.264 video + AAC audio)

## Visual Design

### Unmute Banner (first 6 seconds)
- Yellow (#FFD700) rectangular background, black bold text: "(Unmute your sound to listen)"
- Centered horizontally, offset 10% from top
- Font size: 28pt+ bold
- Blinks: 1s visible, 1s hidden, repeating for 6s (3 blinks)

### Per-Clip Display
- **Clip title** displayed just below where the unmute banner sits, clearly visible throughout that clip's duration
- **Audio-reactive visualization** filling most of the screen

### 3 Visualization Styles (one per clip, each with distinct color palette)

1. **Pitch Tracked Chord** — **Frequency bar equalizer** (deep blue/cyan palette, dark background)
   - FFT-based vertical bars, heights driven by frequency bands
   - Bars with slight glow/gradient effect
   - ~64 bars across the screen

2. **Scratchy Robot** — **Circular radial waveform** (red/orange palette, dark background)
   - Audio waveform drawn as a radial/polar plot around a center point
   - Spiky, aggressive look matching the "scratchy" character
   - Pulsing circle radius based on RMS energy

3. **Many Voices Robot** — **Mirrored waveform with particles** (green/purple palette, dark background)
   - Centered horizontal waveform mirrored top/bottom
   - Amplitude-scaled, with trailing fade effect
   - Color gradient along the waveform

### Background
- Dark (#111111 or similar) base for all clips, with the visualization colors providing contrast

## Implementation

### Single Python script: `make_demo_video.py`

**Dependencies:** numpy, Pillow (both already installed), ffmpeg (already installed)

**Approach:**
1. Use ffmpeg to decode each m4a → raw PCM (via subprocess)
2. Use numpy for FFT / waveform analysis per frame
3. Use Pillow (PIL.ImageDraw) to render each frame as PNG in memory
4. Pipe raw frames to ffmpeg via subprocess to encode video
5. Concatenate audio clips with ffmpeg into one combined audio track
6. Mux video + audio into final MP4

**Script structure:**
```
make_demo_video.py
├── load_audio(path) → numpy array
├── render_unmute_banner(draw, frame_num, ...)
├── render_title(draw, title, ...)
├── render_equalizer(draw, fft_data, colors, ...)
├── render_radial(draw, waveform, colors, ...)
├── render_mirrored_wave(draw, waveform, colors, ...)
├── generate_frames() → yields PIL Images
└── main() → orchestrates encoding pipeline
```

**Key details:**
- Frame-by-frame rendering: at 30fps, ~756 frames total
- FFT window: 2048 samples, gives good frequency resolution at 44.1kHz
- Audio analysis: compute per-frame FFT magnitudes and RMS for visualization
- Font: Use Pillow's built-in or a system font (macOS has plenty in `/System/Library/Fonts/`)

## Files Created
- `make_demo_video.py` — the generator script
- `vocoder_demo.mp4` — the output video (gitignored, regenerable)

## Verification
1. Run `python3 make_demo_video.py`
2. Open `vocoder_demo.mp4` in QuickTime to verify:
   - Audio plays correctly in the right order
   - Unmute banner blinks for first 6s
   - Each clip has its title and distinct visualization
   - Visualizations react to the audio
