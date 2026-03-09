# Drew's Vocoder Toy

A real-time channel vocoder that runs in your Mac's Terminal. Talk or sing into your mic (or feed it an audio file) and it turns your voice into a robot, an organ, a choir, a sci-fi alien, or all of them at once. Wear headphones when talking or singing into the mic. AirPods are fine, but they add a noticeable delay to vocoded output sound.

## Hear It First - Demo Video



https://github.com/user-attachments/assets/6f52b2ba-d79b-417a-835f-af2889619cbc



The video is built on these 3 audio files, which can be found in the [demo_video](demo_video) folder:

| File | What it sounds like |
|------|-------------------|
| `andrew-vocoder-built-in-pitch-track-chord.m4a` | Voice tracking its own pitch to generate chords |
| `andrew-vocoder-test-manyvoice-robot.m4a` | Multiple carrier voices layered together |
| `andrew-vocoder-test-scratchy-robot.m4a` | Gritty robot voice |

### How Claude Made The Demo Video

If you're at all curious at how Claude Code was used to make the demo video from the audio files above, you can [read more here](demo_video/README.md).

## Build & Run

### Prerequisites

You need Apple's Command Line Tools installed. If you've never done this before, open Terminal (it's in Applications > Utilities) and run:

```bash
xcode-select --install
```

A dialog will pop up — click Install and wait a minute or two. You only have to do this once.

### Build it

```bash
make
```

That's it. You'll get an executable called `drews_vocoder_toy`.

### Run it

```bash
./drews_vocoder_toy
```

macOS will ask for microphone permission the first time. Say yes (you can always use audio files instead if you prefer).

## How to Use It

The whole thing runs in your Terminal window. No mouse — just your keyboard.

### Switching pages

Press **Tab** to cycle through the five pages:

1. **Carriers** — the sounds your voice gets blended with (sawtooth, organ, cello, brass, and more)
2. **Modulator** — noise gate, compressor, and EQ for the input signal
3. **Output** — master compressor and EQ
4. **Pitch** — pitch tracking, harmonies, and scale settings
5. **MIDI** — connect a MIDI keyboard or controller

### The basics

| Key | What it does |
|-----|-------------|
| **1–9, 0** | Select a carrier (on the Carriers page) |
| **+/-** | Turn the selected carrier's level up or down |
| **Arrow keys** | Navigate items on any page |
| **Enter** | Mute/unmute a carrier |
| **Space** | Solo a carrier (hear just that one) |
| **M** | Switch between mic input and audio files |
| **P** | Save your current settings as a profile |
| **F** | Load a saved profile |
| **Esc** | Quit |

‘M’ will switch between files in the test_audio folder and then switch to the microphone.
You can record the output too. If you’ve made a sound you like, hit ‘P’ to save it as a new Profile.

### Quick start

1. Run `./drews_vocoder_toy`
2. Press **M** until you see an audio file selected as the modulator (top of screen)
3. You'll hear the vocoder processing that file through the default carriers
4. Press **1** to select Sawtooth, then **+** a few times to crank it up
5. Press **4** to select Organ, **+** to bring it in
6. Try **T** to toggle pitch tracking, **C** to toggle auto-chord
7. Press **Tab** to explore the other pages

### Using your microphone

If you want to use your own voice as the input, press **M** to switch to mic mode. **You need headphones.** Without them, the speaker output feeds back into the mic and you'll get a howling feedback loop — not the fun kind.

Wired headphones or AirPods both work. Just make sure your Mac's input is set to the mic you want (System Settings > Sound > Input).

### Using a MIDI keyboard

Got a MIDI keyboard or controller? Plug it into your Mac (USB or via a Bluetooth MIDI adapter) **before** you launch the app. The vocoder scans for MIDI devices at startup and won't detect ones connected later.

Once it's connected, just play notes. The carriers will follow whatever you play — up to 8 notes at once for full chords. MIDI input automatically takes priority over pitch tracking, so you can switch between playing keys and using your voice without changing any settings.

If your controller has knobs or faders, press **Tab** until you're on the **MIDI** page, then press **L** to enter learn mode. Turn a knob, then select a parameter, and they're linked.

## How it Works

A channel vocoder works by analyzing the *shape* of one sound (your voice — the **modulator**) and imposing that shape onto another sound (a synthesizer — the **carrier**). The result sounds like the carrier is “talking” (or singing!).

Here's the signal flow:

```
Your voice (mic or audio file)
  -> Noise Gate -> EQ -> Compressor
  -> 28-band analysis (splits your voice into frequency bands)
      x Carrier mix (saw, organ, cello, noise, brass, etc.)
  -> Output Compressor -> Output EQ
  -> Soft Clipper -> Your speakers
```

The 28 bandpass filters chop your voice into narrow frequency slices. For each slice, an envelope follower tracks how loud that frequency band is, moment by moment. Those loudness envelopes are multiplied onto the corresponding bands of the carrier signal. When all 28 bands are summed back together, the carrier has inherited the spectral shape — the vowels, consonants, and inflections — of your voice.

### Carriers

The vocoder mixes up to 13 carriers simultaneously:

- **Sawtooth, Noise, Buzz** — simple built-in oscillators
- **Organ, Cello, Syn Brass, Str Ens 2, Pad NewAg, FX Sci-Fi, Bagpipe, Ld Fifths, Pad Poly** — Apple's built-in DLS synthesizer instruments, driven by MIDI
- **Dry** — your unprocessed voice mixed back in

Each carrier has its own level, brightness (filter cutoff), and transpose controls.

### Pitch tracking

When pitch tracking is on (**T**), the vocoder detects the pitch of your voice in real time using the YIN algorithm and plays the carriers at that pitch. You can also enable harmony mode (**H**) to generate chords from your voice, or constrain notes to a musical scale.

### MIDI

Connect a MIDI keyboard before launching and the carriers play whatever notes you hold down — up to 8-voice polyphony. MIDI takes priority over pitch tracking whenever notes are active. MIDI CC knobs can be mapped to any parameter via learn mode (**L** on the MIDI page). Note: MIDI devices must be connected before the app starts — hot-plugging isn't supported.

### Technical details

- Single-threaded DSP running in a CoreAudio callback — no allocations, no locks, no blocking
- Lock-free atomic communication between the audio thread and UI thread
- All filters are biquad IIR with manual denormal flushing
- Runs at your system's native sample rate (usually 44.1 or 48 kHz)
- Built as a single C++ compilation unit — `drews_vocoder_toy.cpp` includes 14 header-only modules

## Author

Andrew Benson
[GitHub](https://github.com/drewster99) · [X/Twitter](https://x.com/TheDrewBenson) · [LinkedIn](https://www.linkedin.com/in/thedrewbenson/)

## License

Copyright (C) 2026 Nuclear Cyborg Corp. Licensed under the [MIT License](LICENSE).
