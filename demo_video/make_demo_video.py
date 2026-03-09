#!/usr/bin/env python3
"""Generate a demo video showcasing vocoder audio clips with audio-reactive visualizations."""

import subprocess
import math
import sys
import os
import random
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

# === Config ===
WIDTH, HEIGHT = 1280, 720
FPS = 30
SAMPLE_RATE = 44100
FFT_SIZE = 2048
BG_COLOR = (17, 17, 17)
OUTPUT_FILE = "vocoder_demo.mp4"
FADE_SAMPLES = int(0.008 * SAMPLE_RATE)  # 8ms fade-in/out to eliminate pops

CLIPS = [
    ("andrew-vocoder-built-in-pitch-track-chord.m4a", "Pitch Tracked Chord"),
    ("andrew-vocoder-test-scratchy-robot.m4a", "Scratchy Robot"),
    ("andrew-vocoder-test-manyvoice-robot.m4a", "Many Voices Robot"),
]

GITHUB_URL = "github.com/drewster99/drews-vocoder-toy"
APP_TITLE = "Drew's Vocoder Toy"
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
    audio = np.frombuffer(result.stdout, dtype=np.float32).copy()
    # Apply short fade-in and fade-out to eliminate pops at clip boundaries
    fade_len = min(FADE_SAMPLES, len(audio) // 2)
    fade_in = np.linspace(0.0, 1.0, fade_len, dtype=np.float32)
    fade_out = np.linspace(1.0, 0.0, fade_len, dtype=np.float32)
    audio[:fade_len] *= fade_in
    audio[-fade_len:] *= fade_out
    return audio


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
    """Load Futura fonts for a clean, techy look."""
    futura_path = "/System/Library/Fonts/Supplemental/Futura.ttc"
    try:
        title_font = ImageFont.truetype(futura_path, 36, index=2)      # Bold
        banner_font = ImageFont.truetype(futura_path, 30, index=2)     # Bold
        app_title_font = ImageFont.truetype(futura_path, 22, index=0)  # Medium
        url_font = ImageFont.truetype(futura_path, 18, index=0)        # Medium
        return title_font, banner_font, app_title_font, url_font
    except OSError:
        print("Warning: Could not load Futura, falling back to default font")
        d = ImageFont.load_default()
        return d, d, d, d


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

    text = "Unmute your sound to listen"
    bbox = draw.textbbox((0, 0), text, font=font)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]

    pad_x, pad_y = 28, 14
    x = (WIDTH - tw) // 2
    y = int(HEIGHT * 0.10)

    # Yellow background rectangle
    draw.rounded_rectangle(
        [x - pad_x, y - pad_y, x + tw + pad_x, y + th + pad_y],
        radius=10,
        fill=(255, 215, 0),
    )
    draw.text((x, y), text, fill=(20, 20, 20), font=font)


def render_title(draw: ImageDraw.Draw, title: str, font: ImageFont.FreeTypeFont):
    """Render clip title below the banner area."""
    bbox = draw.textbbox((0, 0), title, font=font)
    tw = bbox[2] - bbox[0]
    x = (WIDTH - tw) // 2
    y = int(HEIGHT * 0.10) + 60
    # White text with slight shadow
    draw.text((x + 2, y + 2), title, fill=(40, 40, 40), font=font)
    draw.text((x, y), title, fill=(255, 255, 255), font=font)


def render_persistent_text(draw: ImageDraw.Draw, app_font: ImageFont.FreeTypeFont,
                           url_font: ImageFont.FreeTypeFont):
    """Render app title (upper left) and GitHub URL (lower right)."""
    margin = 20
    # App title — upper left
    draw.text((margin, margin), APP_TITLE, fill=(160, 160, 160), font=app_font)
    # GitHub URL — lower right
    bbox = draw.textbbox((0, 0), GITHUB_URL, font=url_font)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    draw.text((WIDTH - tw - margin, HEIGHT - th - margin), GITHUB_URL,
              fill=(120, 120, 120), font=url_font)


# === Equalizer State (smoothing across frames) ===

class EqualizerState:
    """Maintains smoothed bar heights and peak hold for the equalizer visualization."""

    def __init__(self, num_bars: int = 64):
        self.num_bars = num_bars
        self.smoothed = np.zeros(num_bars)
        self.peaks = np.zeros(num_bars)
        self.peak_vel = np.zeros(num_bars)
        self.update_counter = 0

    def reset(self):
        self.smoothed[:] = 0
        self.peaks[:] = 0
        self.peak_vel[:] = 0
        self.update_counter = 0

    def update(self, raw_heights: np.ndarray):
        """Update smoothed heights and peak hold. Call every frame."""
        # Smooth: fast attack, slower decay
        attack = 0.5
        decay = 0.08
        for i in range(self.num_bars):
            if raw_heights[i] > self.smoothed[i]:
                self.smoothed[i] += (raw_heights[i] - self.smoothed[i]) * attack
            else:
                self.smoothed[i] += (raw_heights[i] - self.smoothed[i]) * decay

        # Peak hold with gravity
        for i in range(self.num_bars):
            if self.smoothed[i] > self.peaks[i]:
                self.peaks[i] = self.smoothed[i]
                self.peak_vel[i] = 0
            else:
                self.peak_vel[i] += 0.002  # gravity
                self.peaks[i] -= self.peak_vel[i]
                if self.peaks[i] < 0:
                    self.peaks[i] = 0


def render_equalizer(draw: ImageDraw.Draw, audio_chunk: np.ndarray, eq_state: EqualizerState):
    """Frequency bar equalizer — deep blue/cyan/magenta palette with peak hold and glow."""
    num_bars = eq_state.num_bars

    # FFT
    window = np.hanning(len(audio_chunk))
    spectrum = np.abs(np.fft.rfft(audio_chunk * window))
    freq_bins = np.logspace(np.log10(1), np.log10(len(spectrum) - 1), num_bars + 1).astype(int)

    raw_heights = np.zeros(num_bars)
    for i in range(num_bars):
        lo, hi = freq_bins[i], freq_bins[i + 1]
        if hi <= lo:
            hi = lo + 1
        raw_heights[i] = np.mean(spectrum[lo:hi])

    # Use log scale to compress dynamic range (prevents one bar dominating)
    raw_heights = np.log1p(raw_heights * 20)  # log(1 + x*20) compresses peaks

    # Normalize using 95th percentile so outliers don't squash everything
    p95 = np.percentile(raw_heights, 95) if np.max(raw_heights) > 0 else 1.0
    if p95 < 0.001:
        p95 = 1.0
    raw_heights = np.clip(raw_heights / p95, 0, 1.3)  # allow slight overshoot
    raw_heights = raw_heights / 1.3  # re-normalize to 0-1

    eq_state.update(raw_heights)

    # Draw bars
    bar_area_top = int(HEIGHT * 0.25)
    bar_area_bottom = int(HEIGHT * 0.90)
    bar_area_height = bar_area_bottom - bar_area_top
    total_width = int(WIDTH * 0.88)
    bar_width = total_width // num_bars - 2
    x_offset = (WIDTH - total_width) // 2

    for i in range(num_bars):
        h = int(eq_state.smoothed[i] ** 0.65 * bar_area_height)
        if h < 2:
            h = 2
        x = x_offset + i * (bar_width + 2)
        y_top = bar_area_bottom - h

        frac = i / num_bars
        # Blue → cyan → magenta gradient across bars
        if frac < 0.5:
            t = frac * 2
            r = int(20 + t * 0)
            g = int(40 + t * 200)
            b = int(220 + t * 35)
        else:
            t = (frac - 0.5) * 2
            r = int(20 + t * 180)
            g = int(240 - t * 140)
            b = int(255 - t * 55)

        # Outer glow (wider, dimmer)
        glow_color = (r // 4, g // 4, b // 4)
        draw.rectangle([x - 2, y_top - 3, x + bar_width + 2, bar_area_bottom], fill=glow_color)

        # Inner glow (slightly wider)
        mid_color = (r // 2, g // 2, b // 2)
        draw.rectangle([x - 1, y_top - 1, x + bar_width + 1, bar_area_bottom], fill=mid_color)

        # Main bar with vertical gradient (brighter at top)
        bright_r = min(255, int(r * 1.3))
        bright_g = min(255, int(g * 1.3))
        bright_b = min(255, int(b * 1.3))
        # Top portion brighter
        split = y_top + h // 3
        draw.rectangle([x, y_top, x + bar_width, split], fill=(bright_r, bright_g, bright_b))
        draw.rectangle([x, split, x + bar_width, bar_area_bottom], fill=(r, g, b))

        # Peak indicator (bright white/colored line)
        peak_h = int(eq_state.peaks[i] ** 0.65 * bar_area_height)
        peak_y = bar_area_bottom - peak_h
        if peak_y < bar_area_bottom - 4:
            peak_color = (min(255, r + 100), min(255, g + 100), min(255, b + 100))
            draw.rectangle([x, peak_y, x + bar_width, peak_y + 3], fill=peak_color)

    # Subtle reflection below
    reflect_top = bar_area_bottom + 4
    reflect_height = 30
    for i in range(num_bars):
        h = int(eq_state.smoothed[i] ** 0.65 * reflect_height)
        if h < 1:
            continue
        x = x_offset + i * (bar_width + 2)
        frac = i / num_bars
        if frac < 0.5:
            t = frac * 2
            r, g, b = int(5 + t * 0), int(10 + t * 50), int(55 + t * 9)
        else:
            t = (frac - 0.5) * 2
            r, g, b = int(5 + t * 45), int(60 - t * 35), int(64 - t * 14)
        draw.rectangle([x, reflect_top, x + bar_width, reflect_top + h], fill=(r, g, b))


# === Radial Visualization State ===

class RadialState:
    """Maintains particle list and ring pulses for the radial visualization."""

    def __init__(self):
        self.particles: list[list] = []  # [x, y, vx, vy, life, max_life, r, g, b]
        self.rings: list[list] = []      # [radius, max_radius, alpha_frac]
        self.prev_rms = 0.0
        self.rng = random.Random(42)

    def reset(self):
        self.particles.clear()
        self.rings.clear()
        self.prev_rms = 0.0


def render_radial(draw: ImageDraw.Draw, audio_chunk: np.ndarray, state: RadialState,
                  img: Image.Image):
    """Circular radial waveform — red/orange/yellow palette with particles and ring pulses."""
    cx, cy = WIDTH // 2, int(HEIGHT * 0.58)
    base_radius = 100
    rms = np.sqrt(np.mean(audio_chunk ** 2))
    pulse_radius = base_radius + int(rms * 500)

    # Spawn ring pulse on transients
    if rms > state.prev_rms * 1.5 + 0.01 and len(state.rings) < 5:
        state.rings.append([float(pulse_radius), float(pulse_radius + 200 + rms * 300), 1.0])
    state.prev_rms = rms

    # Update and draw rings
    new_rings = []
    for ring in state.rings:
        radius, max_radius, alpha = ring
        ring[0] += (max_radius - radius) * 0.08 + 3
        ring[2] -= 0.04
        if ring[2] > 0:
            new_rings.append(ring)
            a = ring[2]
            r_col = int(255 * a)
            g_col = int(120 * a)
            b_col = int(20 * a)
            r_int = int(ring[0])
            for thickness in range(3):
                rr = r_int + thickness
                draw.ellipse([cx - rr, cy - rr, cx + rr, cy + rr],
                             outline=(r_col, g_col, b_col))
    state.rings = new_rings

    # Inner glow circle — multi-layered for bloom effect
    for layer in range(5):
        scale = 0.25 + layer * 0.08
        glow_r = int(pulse_radius * scale)
        intensity = 1.0 - layer * 0.18
        color = (int(100 * intensity), int(25 * intensity), int(5 * intensity))
        draw.ellipse([cx - glow_r, cy - glow_r, cx + glow_r, cy + glow_r], fill=color)

    # Downsample waveform for drawing
    num_points = 360
    step = max(1, len(audio_chunk) // num_points)
    samples = audio_chunk[::step][:num_points]
    max_amplitude = 220

    points = []
    for i, s in enumerate(samples):
        angle = 2 * math.pi * i / len(samples)
        r = pulse_radius + s * max_amplitude
        x = cx + r * math.cos(angle)
        y = cy + r * math.sin(angle)
        points.append((x, y))

    if len(points) < 3:
        return

    points.append(points[0])

    # Draw the waveform with vibrant color cycle
    for i in range(len(points) - 1):
        frac = i / len(points)
        # Red → orange → yellow → back to red
        phase = frac * 3
        if phase < 1:
            r, g, b = 255, int(60 + phase * 160), 0
        elif phase < 2:
            t = phase - 1
            r, g, b = 255, int(220 - t * 40), int(t * 60)
        else:
            t = phase - 2
            r, g, b = 255, int(180 - t * 120), int(60 - t * 60)

        # Intensity modulation by amplitude
        amp = min(1.0, abs(samples[min(i, len(samples) - 1)]) * 4 + 0.5)
        r, g, b = int(r * amp), int(g * amp), int(b * amp)

        draw.line([points[i], points[i + 1]], fill=(min(255, r), min(255, g), min(255, b)), width=3)

        # Spawn particles at spiky points
        if abs(samples[min(i, len(samples) - 1)]) > 0.15 and state.rng.random() < 0.06:
            angle = 2 * math.pi * i / len(points)
            speed = 1.5 + state.rng.random() * 3
            vx = math.cos(angle) * speed
            vy = math.sin(angle) * speed
            life = 10 + state.rng.randint(0, 15)
            pr = min(255, r + 50)
            pg = min(255, g + 80)
            pb = min(255, b + 30)
            state.particles.append([points[i][0], points[i][1], vx, vy,
                                    float(life), float(life), pr, pg, pb])

    # Update and draw particles
    new_particles = []
    for p in state.particles:
        p[0] += p[2]  # x += vx
        p[1] += p[3]  # y += vy
        p[4] -= 1     # life -= 1
        if p[4] > 0:
            new_particles.append(p)
            alpha = p[4] / p[5]
            pr = int(p[6] * alpha)
            pg = int(p[7] * alpha)
            pb = int(p[8] * alpha)
            px, py = int(p[0]), int(p[1])
            size = max(1, int(3 * alpha))
            draw.ellipse([px - size, py - size, px + size, py + size], fill=(pr, pg, pb))
    state.particles = new_particles[:300]  # cap particle count


# === Mirrored Wave State ===

class MirroredWaveState:
    """Maintains pulse rings and energy memory for the mirrored wave visualization."""

    def __init__(self):
        self.pulses: list[list] = []  # [y_offset, spread, alpha, color_idx]
        self.prev_rms = 0.0
        self.smoothed_energy = 0.0    # decaying energy for ambient glow during silence
        self.peak_memory = 0.01       # running peak for auto-normalization
        self.rng = random.Random(99)

    def reset(self):
        self.pulses.clear()
        self.prev_rms = 0.0
        self.smoothed_energy = 0.0
        self.peak_memory = 0.01


def render_mirrored_wave(draw: ImageDraw.Draw, full_audio: np.ndarray,
                         clip_frame: int, state: MirroredWaveState):
    """Mirrored waveform — green/cyan/purple palette with pulse waves and filled regions.
    Uses a large audio window and auto-normalization for maximum visual impact."""
    center_y = int(HEIGHT * 0.58)
    margin_x = int(WIDTH * 0.06)
    wave_width = WIDTH - 2 * margin_x

    # Use a much larger window (8192 samples ≈ 186ms) for more waveform context
    big_window = 8192
    chunk = get_frame_audio(full_audio, clip_frame, window_size=big_window)

    rms = np.sqrt(np.mean(chunk ** 2))

    # Update smoothed energy (slow decay so viz stays alive during silence)
    if rms > state.smoothed_energy:
        state.smoothed_energy = rms
    else:
        state.smoothed_energy *= 0.95  # slow decay

    # Auto-normalize: track running peak and normalize against it
    peak = np.max(np.abs(chunk))
    if peak > state.peak_memory:
        state.peak_memory = peak
    else:
        state.peak_memory = max(0.002, state.peak_memory * 0.93)  # aggressive decay so quiet audio fills screen

    # Normalize samples to [-1, 1] range based on running peak
    # Aggressive normalization: even very quiet audio gets amplified to fill the screen
    norm_factor = 1.0 / max(state.peak_memory, 0.001)
    normalized = np.clip(chunk * norm_factor, -1.0, 1.0)

    # Spawn horizontal pulse waves on transients
    if rms > state.prev_rms * 1.3 + 0.005 and len(state.pulses) < 8:
        color_idx = state.rng.randint(0, 2)
        state.pulses.append([0.0, 0.0, 1.0, color_idx])
    state.prev_rms = rms

    # Downsample normalized waveform to screen width
    num_points = wave_width
    step = max(1, len(normalized) // num_points)
    samples = normalized[::step][:num_points]

    # Large amplitude scaling to fill screen vertically
    max_amplitude = int(HEIGHT * 0.38)

    # Energy-based ambient brightness (keeps viz alive during silence)
    ambient = min(1.0, state.smoothed_energy * 15 + 0.15)

    def get_color(frac: float, brightness: float = 1.0):
        """Get green→cyan→purple gradient color at position frac."""
        if frac < 0.33:
            t = frac * 3
            r = int((30 + t * 0) * brightness)
            g = int((220 - t * 40) * brightness)
            b = int((60 + t * 140) * brightness)
        elif frac < 0.66:
            t = (frac - 0.33) * 3
            r = int((30 + t * 100) * brightness)
            g = int((180 - t * 80) * brightness)
            b = int((200 + t * 55) * brightness)
        else:
            t = (frac - 0.66) * 3
            r = int((130 + t * 90) * brightness)
            g = int((100 - t * 50) * brightness)
            b = int((255 - t * 30) * brightness)
        return (min(255, r), min(255, g), min(255, b))

    # Draw filled gradient region behind waveform
    for i in range(0, len(samples) - 1, 2):
        x1 = margin_x + i
        x2 = margin_x + i + 2

        h1 = abs(samples[i]) * max_amplitude
        h2_idx = min(i + 2, len(samples) - 1)
        h2 = abs(samples[h2_idx]) * max_amplitude
        h = max(h1, h2, 3 * ambient)  # minimum fill based on energy

        frac = i / len(samples)
        fr, fg, fb = get_color(frac, 0.25 * ambient)
        if h > 1:
            draw.rectangle([x1, int(center_y - h), x2, int(center_y + h)],
                           fill=(fr, fg, fb))

    # Draw main waveform lines (bright, on top of fill)
    for i in range(len(samples) - 1):
        x1 = margin_x + i
        x2 = margin_x + i + 1

        h1 = abs(samples[i]) * max_amplitude
        h2 = abs(samples[i + 1]) * max_amplitude

        frac = i / len(samples)
        r, g, b = get_color(frac, max(ambient, 0.5))

        # Top waveform
        draw.line([(x1, center_y - h1), (x2, center_y - h2)],
                  fill=(r, g, b), width=2)
        # Bottom waveform (mirrored)
        draw.line([(x1, center_y + h1), (x2, center_y + h2)],
                  fill=(r, g, b), width=2)

        # Secondary glow lines (offset outward, wider spread)
        glow_r, glow_g, glow_b = r // 2, g // 2, b // 2
        offset = 6 + int(h1 * 0.12)
        draw.line([(x1, center_y - h1 - offset), (x2, center_y - h2 - offset)],
                  fill=(glow_r, glow_g, glow_b), width=1)
        draw.line([(x1, center_y + h1 + offset), (x2, center_y + h2 + offset)],
                  fill=(glow_r, glow_g, glow_b), width=1)

        # Tertiary outer glow (even dimmer, wider)
        if h1 > 10:
            outer_offset = offset + 8 + int(h1 * 0.06)
            dim_r, dim_g, dim_b = r // 5, g // 5, b // 5
            draw.line([(x1, center_y - h1 - outer_offset), (x2, center_y - h2 - outer_offset)],
                      fill=(dim_r, dim_g, dim_b), width=1)
            draw.line([(x1, center_y + h1 + outer_offset), (x2, center_y + h2 + outer_offset)],
                      fill=(dim_r, dim_g, dim_b), width=1)

    # Draw pulse waves expanding outward from center
    pulse_colors = [
        (100, 255, 150),  # green
        (100, 220, 255),  # cyan
        (200, 120, 255),  # purple
    ]
    new_pulses = []
    for pulse in state.pulses:
        y_off, spread, alpha, cidx = pulse
        pulse[1] += 5.0   # spread outward
        pulse[2] -= 0.025  # fade
        if pulse[2] > 0:
            new_pulses.append(pulse)
            base_r, base_g, base_b = pulse_colors[int(cidx) % 3]
            a = pulse[2]
            pr = int(base_r * a * 0.6)
            pg = int(base_g * a * 0.6)
            pb = int(base_b * a * 0.6)
            s = int(pulse[1])
            # Horizontal lines expanding from center
            for thickness in range(3):
                draw.line([(margin_x, center_y - s - thickness),
                           (margin_x + wave_width, center_y - s - thickness)],
                          fill=(pr, pg, pb), width=1)
                draw.line([(margin_x, center_y + s + thickness),
                           (margin_x + wave_width, center_y + s + thickness)],
                          fill=(pr, pg, pb), width=1)
    state.pulses = new_pulses

    # Thin center line
    draw.line([(margin_x, center_y), (margin_x + wave_width, center_y)],
              fill=(80, 80, 80), width=1)


# === Frame Generator ===


def generate_frames(clips_audio: list[tuple[np.ndarray, str]],
                    title_font, banner_font, app_font, url_font):
    """Yield PIL Image frames for the entire video."""
    eq_state = EqualizerState()
    radial_state = RadialState()
    wave_state = MirroredWaveState()

    states = [eq_state, radial_state, wave_state]

    # Build timeline: list of (clip_index, clip_frame_index) for each global frame
    timeline = []
    for clip_idx, (audio, title) in enumerate(clips_audio):
        num_frames = int(math.ceil(len(audio) / SAMPLE_RATE * FPS))
        for f in range(num_frames):
            timeline.append((clip_idx, f))

    total_frames = len(timeline)
    print(f"Generating {total_frames} frames...")

    prev_clip_idx = -1

    for global_frame, (clip_idx, clip_frame) in enumerate(timeline):
        # Reset visualization state on clip change
        if clip_idx != prev_clip_idx:
            for s in states:
                s.reset()
            prev_clip_idx = clip_idx

        audio, title = clips_audio[clip_idx]
        img = Image.new("RGB", (WIDTH, HEIGHT), BG_COLOR)
        draw = ImageDraw.Draw(img)

        # Audio analysis for this frame
        chunk = get_frame_audio(audio, clip_frame)

        # Visualization (pass state objects)
        if clip_idx == 0:
            render_equalizer(draw, chunk, eq_state)
        elif clip_idx == 1:
            render_radial(draw, chunk, radial_state, img)
        else:
            render_mirrored_wave(draw, audio, clip_frame, wave_state)

        # Persistent text overlays
        render_persistent_text(draw, app_font, url_font)

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
    title_font, banner_font, app_font, url_font = load_fonts()

    # Load audio clips (with fade-in/out applied to prevent pops)
    print("Loading audio clips...")
    clips_audio = []
    for path, title in CLIPS:
        audio = load_audio(path)
        duration = len(audio) / SAMPLE_RATE
        print(f"  {title}: {duration:.2f}s ({len(audio)} samples)")
        clips_audio.append((audio, title))

    # Build concatenated audio with crossfade applied, using ffmpeg
    # We write our processed (faded) audio to raw files, then concat
    print("\nPreparing audio...")
    raw_files = []
    for i, (audio, _) in enumerate(clips_audio):
        raw_path = f"temp_clip_{i}.raw"
        audio.tofile(raw_path)
        raw_files.append(raw_path)

    # Build ffmpeg filter to concat raw PCM clips into AAC
    # Use sequential raw inputs
    ffmpeg_inputs = []
    for raw_path in raw_files:
        ffmpeg_inputs.extend(["-f", "f32le", "-ar", str(SAMPLE_RATE), "-ac", "1", "-i", raw_path])

    num_clips = len(raw_files)
    filter_str = "".join(f"[{i}:a]" for i in range(num_clips)) + f"concat=n={num_clips}:v=0:a=1[outa]"

    concat_cmd = [
        "ffmpeg", "-y",
        *ffmpeg_inputs,
        "-filter_complex", filter_str,
        "-map", "[outa]",
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
    for img in generate_frames(clips_audio, title_font, banner_font, app_font, url_font):
        proc.stdin.write(img.tobytes())

    proc.stdin.close()
    proc.wait()

    if proc.returncode != 0:
        print(f"\nError: ffmpeg encoding failed with code {proc.returncode}")
        sys.exit(1)

    # Cleanup temp files
    for raw_path in raw_files:
        os.remove(raw_path)
    os.remove("temp_audio.m4a")

    print(f"\nDone! Output: {OUTPUT_FILE}")
    size = os.path.getsize(OUTPUT_FILE)
    print(f"File size: {size / 1024 / 1024:.1f} MB")


if __name__ == "__main__":
    main()
