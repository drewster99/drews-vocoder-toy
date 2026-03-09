#!/usr/bin/env python3
"""Generate a demo video showcasing vocoder audio clips with audio-reactive visualizations."""

import subprocess
import struct
import math
import io
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

# === Config ===
WIDTH, HEIGHT = 1280, 720
FPS = 30
SAMPLE_RATE = 44100
FFT_SIZE = 2048
BG_COLOR = (17, 17, 17)
OUTPUT_FILE = "vocoder_demo.mp4"

CLIPS = [
    ("andrew-vocoder-built-in-pitch-track-chord.m4a", "Pitch Tracked Chord"),
    ("andrew-vocoder-test-scratchy-robot.m4a", "Scratchy Robot"),
    ("andrew-vocoder-test-manyvoice-robot.m4a", "Many Voices Robot"),
]

UNMUTE_DURATION = 6.0  # seconds

# === Audio Loading ===

def load_audio(path: str) -> np.ndarray:
    """Decode audio file to mono float32 numpy array at SAMPLE_RATE using ffmpeg."""
    cmd = [
        "ffmpeg", "-i", path,
        "-f", "f32le", "-acodec", "pcm_f32le",
        "-ar", str(SAMPLE_RATE), "-ac", "1",
        "-v", "quiet", "-"
    ]
    result = subprocess.run(cmd, capture_output=True)
    if result.returncode != 0:
        raise RuntimeError(f"ffmpeg failed to decode {path}: {result.stderr.decode()}")
    return np.frombuffer(result.stdout, dtype=np.float32)


def get_frame_audio(audio: np.ndarray, frame_idx: int, window_size: int = FFT_SIZE) -> np.ndarray:
    """Extract audio samples centered around the given frame's time position."""
    center = int(frame_idx * SAMPLE_RATE / FPS)
    start = max(0, center - window_size // 2)
    end = start + window_size
    if end > len(audio):
        end = len(audio)
        start = max(0, end - window_size)
    chunk = audio[start:end]
    if len(chunk) < window_size:
        chunk = np.pad(chunk, (0, window_size - len(chunk)))
    return chunk


# === Font Loading ===

def load_fonts():
    """Load fonts for title and banner text."""
    bold_path = "/System/Library/Fonts/Supplemental/Arial Bold.ttf"
    regular_path = "/System/Library/Fonts/Supplemental/Arial.ttf"
    try:
        title_font = ImageFont.truetype(bold_path, 36)
        banner_font = ImageFont.truetype(bold_path, 30)
        return title_font, banner_font
    except OSError:
        print("Warning: Could not load Arial Bold, falling back to default font")
        return ImageFont.load_default(), ImageFont.load_default()


# === Rendering Functions ===

def render_unmute_banner(draw: ImageDraw.Draw, global_frame: int, font: ImageFont.FreeTypeFont):
    """Render blinking unmute banner for the first 6 seconds."""
    t = global_frame / FPS
    if t >= UNMUTE_DURATION:
        return
    # Blink: 1s on, 1s off
    cycle = t % 2.0
    if cycle >= 1.0:
        return

    text = "(Unmute your sound to listen)"
    bbox = draw.textbbox((0, 0), text, font=font)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]

    pad_x, pad_y = 24, 12
    x = (WIDTH - tw) // 2
    y = int(HEIGHT * 0.10)

    # Yellow background rectangle
    draw.rounded_rectangle(
        [x - pad_x, y - pad_y, x + tw + pad_x, y + th + pad_y],
        radius=8,
        fill=(255, 215, 0),
    )
    # Black text
    draw.text((x, y), text, fill=(0, 0, 0), font=font)


def render_title(draw: ImageDraw.Draw, title: str, font: ImageFont.FreeTypeFont):
    """Render clip title below the banner area."""
    bbox = draw.textbbox((0, 0), title, font=font)
    tw = bbox[2] - bbox[0]
    x = (WIDTH - tw) // 2
    y = int(HEIGHT * 0.10) + 60
    # White text with slight shadow
    draw.text((x + 2, y + 2), title, fill=(40, 40, 40), font=font)
    draw.text((x, y), title, fill=(255, 255, 255), font=font)


def render_equalizer(draw: ImageDraw.Draw, audio_chunk: np.ndarray):
    """Frequency bar equalizer — deep blue/cyan palette."""
    # FFT
    window = np.hanning(len(audio_chunk))
    spectrum = np.abs(np.fft.rfft(audio_chunk * window))
    # Use log-spaced frequency bins, take first half
    num_bars = 64
    freq_bins = np.logspace(np.log10(1), np.log10(len(spectrum) - 1), num_bars + 1).astype(int)

    bar_heights = np.zeros(num_bars)
    for i in range(num_bars):
        lo, hi = freq_bins[i], freq_bins[i + 1]
        if hi <= lo:
            hi = lo + 1
        bar_heights[i] = np.mean(spectrum[lo:hi])

    # Normalize
    max_val = np.max(bar_heights) if np.max(bar_heights) > 0 else 1.0
    bar_heights = bar_heights / max_val

    # Draw bars
    bar_area_top = int(HEIGHT * 0.25)
    bar_area_bottom = int(HEIGHT * 0.92)
    bar_area_height = bar_area_bottom - bar_area_top
    total_width = int(WIDTH * 0.85)
    bar_width = total_width // num_bars - 2
    x_offset = (WIDTH - total_width) // 2

    for i in range(num_bars):
        h = int(bar_heights[i] ** 0.7 * bar_area_height)  # gamma for visual punch
        if h < 2:
            h = 2
        x = x_offset + i * (bar_width + 2)
        y_top = bar_area_bottom - h
        # Color gradient: deep blue at bottom to cyan at top
        frac = i / num_bars
        r = int(0 + frac * 30)
        g = int(80 + frac * 175)
        b = int(200 + frac * 55)
        # Glow: draw a slightly wider, dimmer bar behind
        glow_color = (r // 3, g // 3, b // 3)
        draw.rectangle([x - 1, y_top - 2, x + bar_width + 1, bar_area_bottom], fill=glow_color)
        draw.rectangle([x, y_top, x + bar_width, bar_area_bottom], fill=(r, g, b))


def render_radial(draw: ImageDraw.Draw, audio_chunk: np.ndarray):
    """Circular radial waveform — red/orange palette."""
    cx, cy = WIDTH // 2, int(HEIGHT * 0.58)
    base_radius = 120
    rms = np.sqrt(np.mean(audio_chunk ** 2))
    pulse_radius = base_radius + int(rms * 400)

    # Downsample waveform for drawing
    num_points = 360
    step = max(1, len(audio_chunk) // num_points)
    samples = audio_chunk[::step][:num_points]

    max_amplitude = 200

    points = []
    for i, s in enumerate(samples):
        angle = 2 * math.pi * i / len(samples)
        r = pulse_radius + s * max_amplitude
        x = cx + r * math.cos(angle)
        y = cy + r * math.sin(angle)
        points.append((x, y))

    if len(points) < 3:
        return

    # Close the shape
    points.append(points[0])

    # Draw filled glow circle
    glow_r = int(pulse_radius * 0.4)
    draw.ellipse(
        [cx - glow_r, cy - glow_r, cx + glow_r, cy + glow_r],
        fill=(80, 20, 0),
    )

    # Draw the waveform as connected lines with varying color
    for i in range(len(points) - 1):
        frac = i / len(points)
        r = int(220 + frac * 35)
        g = int(60 + frac * 120)
        b = int(0 + frac * 30)
        draw.line([points[i], points[i + 1]], fill=(min(r, 255), min(g, 255), b), width=2)


def render_mirrored_wave(draw: ImageDraw.Draw, audio_chunk: np.ndarray):
    """Mirrored waveform — green/purple palette."""
    center_y = int(HEIGHT * 0.58)
    margin_x = int(WIDTH * 0.08)
    wave_width = WIDTH - 2 * margin_x

    # Downsample
    num_points = wave_width
    step = max(1, len(audio_chunk) // num_points)
    samples = audio_chunk[::step][:num_points]

    max_amplitude = int(HEIGHT * 0.30)

    # Draw mirrored waveform with color gradient
    for i in range(len(samples) - 1):
        x1 = margin_x + i
        x2 = margin_x + i + 1

        h1 = int(abs(samples[i]) * max_amplitude)
        h2 = int(abs(samples[i + 1]) * max_amplitude)

        frac = i / len(samples)
        # Green to purple gradient
        r = int(60 + frac * 160)
        g = int(200 - frac * 140)
        b = int(80 + frac * 175)

        # Top half
        draw.line([(x1, center_y - h1), (x2, center_y - h2)], fill=(r, g, b), width=2)
        # Bottom half (mirrored)
        draw.line([(x1, center_y + h1), (x2, center_y + h2)], fill=(r, g, b), width=2)

        # Faded trail (dimmer, slightly offset)
        trail_color = (r // 3, g // 3, b // 3)
        trail_h1 = int(h1 * 0.6)
        trail_h2 = int(h2 * 0.6)
        draw.line([(x1, center_y - trail_h1 - 4), (x2, center_y - trail_h2 - 4)], fill=trail_color, width=1)
        draw.line([(x1, center_y + trail_h1 + 4), (x2, center_y + trail_h2 + 4)], fill=trail_color, width=1)

    # Center line
    draw.line([(margin_x, center_y), (margin_x + wave_width, center_y)], fill=(100, 100, 100), width=1)


# === Frame Generator ===

def generate_frames(clips_audio: list[tuple[np.ndarray, str]], title_font, banner_font):
    """Yield PIL Image frames for the entire video."""
    render_funcs = [render_equalizer, render_radial, render_mirrored_wave]

    # Build timeline: list of (clip_index, clip_frame_index) for each global frame
    timeline = []
    for clip_idx, (audio, title) in enumerate(clips_audio):
        num_frames = int(math.ceil(len(audio) / SAMPLE_RATE * FPS))
        for f in range(num_frames):
            timeline.append((clip_idx, f))

    total_frames = len(timeline)
    print(f"Generating {total_frames} frames...")

    for global_frame, (clip_idx, clip_frame) in enumerate(timeline):
        audio, title = clips_audio[clip_idx]
        img = Image.new("RGB", (WIDTH, HEIGHT), BG_COLOR)
        draw = ImageDraw.Draw(img)

        # Audio analysis for this frame
        chunk = get_frame_audio(audio, clip_frame)

        # Visualization
        render_funcs[clip_idx](draw, chunk)

        # Title
        render_title(draw, title, title_font)

        # Unmute banner (global time)
        render_unmute_banner(draw, global_frame, banner_font)

        if global_frame % 30 == 0:
            pct = int(100 * global_frame / total_frames)
            print(f"\r  Frame {global_frame}/{total_frames} ({pct}%)", end="", flush=True)

        yield img

    print(f"\r  Frame {total_frames}/{total_frames} (100%)")


# === Main ===

def main():
    print("=== Vocoder Demo Video Generator ===\n")

    # Load fonts
    title_font, banner_font = load_fonts()

    # Load audio clips
    print("Loading audio clips...")
    clips_audio = []
    for path, title in CLIPS:
        audio = load_audio(path)
        duration = len(audio) / SAMPLE_RATE
        print(f"  {title}: {duration:.2f}s ({len(audio)} samples)")
        clips_audio.append((audio, title))

    # Concatenate audio into a single temp file using ffmpeg
    print("\nConcatenating audio...")
    concat_list = "concat_list.txt"
    with open(concat_list, "w") as f:
        for path, _ in CLIPS:
            f.write(f"file '{path}'\n")

    concat_cmd = [
        "ffmpeg", "-y", "-f", "concat", "-safe", "0",
        "-i", concat_list,
        "-c:a", "aac", "-b:a", "192k",
        "-v", "quiet",
        "temp_audio.m4a"
    ]
    subprocess.run(concat_cmd, check=True)

    # Calculate total frames
    total_samples = sum(len(a) for a, _ in clips_audio)
    total_duration = total_samples / SAMPLE_RATE
    total_frames = int(math.ceil(total_duration * FPS))
    print(f"Total duration: {total_duration:.2f}s, {total_frames} frames\n")

    # Start ffmpeg video encoder process
    print("Encoding video...")
    video_cmd = [
        "ffmpeg", "-y",
        "-f", "rawvideo", "-pix_fmt", "rgb24",
        "-s", f"{WIDTH}x{HEIGHT}", "-r", str(FPS),
        "-i", "-",  # stdin
        "-i", "temp_audio.m4a",
        "-c:v", "libx264", "-preset", "medium", "-crf", "23",
        "-c:a", "copy",
        "-pix_fmt", "yuv420p",
        "-shortest",
        "-v", "quiet",
        OUTPUT_FILE
    ]
    proc = subprocess.Popen(video_cmd, stdin=subprocess.PIPE)

    # Generate and pipe frames
    for img in generate_frames(clips_audio, title_font, banner_font):
        proc.stdin.write(img.tobytes())

    proc.stdin.close()
    proc.wait()

    if proc.returncode != 0:
        print(f"\nError: ffmpeg encoding failed with code {proc.returncode}")
        sys.exit(1)

    # Cleanup temp files
    import os
    os.remove(concat_list)
    os.remove("temp_audio.m4a")

    print(f"\nDone! Output: {OUTPUT_FILE}")
    # Show file size
    size = os.path.getsize(OUTPUT_FILE)
    print(f"File size: {size / 1024 / 1024:.1f} MB")


if __name__ == "__main__":
    main()
