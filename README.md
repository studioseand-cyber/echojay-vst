# EchoJay VST Plugin — Setup Guide

## What This Is

A JUCE-based VST3/AU plugin that sits on any channel in your DAW and gives you real-time metering with AI mix feedback. Fully native UI — no web browser, no internet required for metering. Uses the same EchoJay account as the web app with the same usage limits (3 free messages/day, 100 for Pro).

## Login & Usage Limits

When you first open the plugin, you'll see a login screen. Use the same email and password as your echojay.ai account. The plugin stores your auth token locally so you only need to log in once — it persists across DAW sessions.

**Free:** 3 AI messages per day (metering is always unlimited)
**Pro (£9.99/mo):** 100 AI messages per day

The usage counter shows in the top-right of the plugin UI. Usage is shared across the web app and plugin — if you've used 2 messages on the web app, you have 1 left in the plugin. Limits reset at midnight.

If your session expires, the plugin drops back to the login screen. Your capture history and meter data are not lost — they're stored locally in the DAW session.

## How It Works

### Live Metering (always on)
Drop EchoJay on any channel — master bus, vocal bus, bass, drums, whatever. All 12 meters run in real-time the moment audio flows through: LUFS (momentary, short-term, integrated), LRA, true peak (4x oversampled), RMS, crest factor, DC offset, stereo width, correlation, and 8-band spectrum. The plugin is 100% pass-through — it never modifies your audio.

### Multi-Instance + Channel Detection
Run multiple instances across your mixer. Each instance can be set to a channel type — Full Mix, Vocals, Bass, Drums, Instruments, Synths, or FX — or left on **Auto-Detect**. In auto mode, EchoJay analyses the frequency spectrum to classify the signal:
- **Bass**: dominant sub/low energy, narrow stereo, minimal highs
- **Drums**: high crest factor (transients), wide spectrum
- **Vocals**: strong mid/presence peak, weak sub content
- **Synths**: strong mid, wide stereo, low crest
- **Instruments**: moderate spectrum without drum transients or vocal presence

The AI adjusts its feedback based on channel type — sibilance for vocals, mono compatibility for bass, transient punch for drums, etc.

### Capture Mode
Hit **Capture**, play your section, then **Stop** (or just press spacebar in your DAW). When the audio stops, EchoJay:
1. Finalises the averaged meter data for that pass
2. Stores it as a snapshot (Pass 1, Pass 2, etc.)
3. **Records the audio** and saves a 32-bit float WAV to `~/Documents/EchoJay/Captures/`
4. **Shows a live waveform** above the meters during capture
5. **Freezes the waveform and meters** on stop — so you can see exactly what was captured
6. **Automatically sends the data to the AI** for instant feedback

No extra button press — play, stop, read the feedback.

### Waveform Display & Playback
During capture, a live waveform panel appears above the loudness meters showing the audio being recorded in real time with a blue-to-purple gradient. When capture completes:
- The waveform freezes and dims slightly to indicate it's a snapshot
- The meter panels also freeze, showing the averaged capture data instead of live audio
- A **Play** button appears in the status bar to audition the captured audio
- The WAV filename is shown next to the play button
- A green playback cursor sweeps across the waveform during playback
- Hit **Reset** to clear the waveform and return to live metering

### Revision Comparison
Capture Pass 1, make changes in your DAW, capture Pass 2. The AI automatically compares both passes and tells you exactly what changed — "low end came up 2dB, crest factor dropped, the compressor might be working too hard."

### Reference Tracks
Three ways to load a reference:
- **Drag and drop** — drag a WAV, MP3, FLAC, AIFF, OGG, or M4A from Finder or your DAW's browser onto the plugin window
- **Load Reference button** — opens a file browser to pick a track
- Both analyse the file offline on a background thread using the same meter engine (LUFS, true peak, stereo, spectrum, everything)

Reference results are stored in memory for the session. They appear in the Compare view with a purple border to distinguish them from your captures.

### Compare Mixes
Hit the purple **Compare** button to switch from meters to comparison mode:
- Two dropdowns let you pick any combination of captures and references
- Side-by-side cards show all meter data for both selections
- Hit **AI Compare** — the AI gives a detailed breakdown of how your mix measures up against the reference, with specific advice on what to fix
- Works for capture-vs-reference (compare against a commercial release) and capture-vs-capture (compare Pass 1 vs Pass 2)

### Plugin Scanner
Hit **Scan Plugins** and EchoJay finds every VST3, AU, and VST plugin on your system. Results are cached so it's instant on relaunch. When the AI gives feedback, it knows your exact plugin list and builds chains using only tools you have — with specific frequencies, ratios, attack/release times.

**Mac paths scanned:**
- /Library/Audio/Plug-Ins/VST3
- /Library/Audio/Plug-Ins/Components (AU)
- /Library/Audio/Plug-Ins/VST
- Plus ~/Library equivalents

**Windows paths scanned:**
- C:\Program Files\Common Files\VST3
- C:\Program Files\VSTPlugins
- C:\Program Files\Steinberg\VSTPlugins
- Plus x86 equivalents

## UI Layout

```
┌──────────────────────────────────────────────────────────────────────┐
│ EchoJay  [Auto-Detect ▾] [Hip-Hop ▾] [Capture] [Reset] [Compare]  │
│                                              sean  PRO  48/100 msgs │
├──────────────────────────────────┬───────────────────────────────────┤
│  MOMENTARY  SHORT TERM  INTEG  LRA  │  ✦ EchoJay AI                │
│  -9.2 LUFS  -8.1 LUFS  -11.5  7.0  │                               │
│                                      │  Pass 1 captured — Vocals     │
│  RMS L      RMS R    TRUE PEAK PEAK  │  channel | 32.4s | -11.5     │
│  -12.0 dB   -12.0 dB  -0.8    -2.1  │  LUFS integrated...           │
│                                      │                               │
│  CREST      DC OFFSET  WIDTH   CORR  │  Your presence range is       │
│  9.9 dB     2.41 mV    32%    0.81   │  sitting about 3dB below      │
│                                      │  where it needs to be.        │
│  SPECTRUM                            │  Open your Pro-Q 3, add a     │
│  ▓▓ ▓▓▓ ▓▓▓▓ ▓▓▓ ▓▓ ▓▓ ▓ ▓        │  bell at 3.2kHz, +2.5dB,     │
│  SUB LOW LO-M MID UP-M PRES HI AIR  │  Q 1.8...                     │
│                                      │                               │
│  CAPTURES                            │  ┌──────────────────────────┐ │
│  Pass 1 | Vocals | -11.5 LUFS | 32s │  │ Ask about your mix...    │ │
│  Pass 2 | Vocals | -10.8 LUFS | 30s │  └──────────────────────────┘ │
└──────────────────────────────────┴───────────────────────────────────┘

COMPARE VIEW (when Compare is active):
┌──────────────────────────────────────────────────────────────────────┐
│ EchoJay  [...] [Back] [Load Ref] [Pass 1 ▾] [⬥ Drake ▾] [AI Comp] │
├──────────────────────────────────┬───────────────────────────────────┤
│  ┌─────────────┐ ┌─────────────┐│  ✦ EchoJay AI                    │
│  │ Pass 1      │ │⬥ Drake Ref  ││                                   │
│  │ Int -11.5   │ │ Int -9.0    ││  Your mix sits 2.5dB quieter     │
│  │ TP  -0.8    │ │ TP  -0.3    ││  than the reference. The ref has │
│  │ Width 32%   │ │ Width 55%   ││  more energy in the upper-mids   │
│  │ Crest 9.9   │ │ Crest 7.2   ││  (2-4kHz)...                     │
│  │ Corr  0.81  │ │ Corr  0.74  ││                                   │
│  │ LRA   7.0   │ │ LRA   5.1   ││                                   │
│  └─────────────┘ └─────────────┘│                                   │
│                                  │  ┌──────────────────────────────┐ │
│  Drop reference track here       │  │ Ask about your mix...        │ │
└──────────────────────────────────┴───────────────────────────────────┘
```

## Build

### Prerequisites
1. **JUCE** — Download from https://juce.com/download/ and extract (e.g. to ~/JUCE)
2. **CMake 3.22+** — `brew install cmake` on Mac
3. **Xcode** (Mac) or **Visual Studio 2022** (Windows)

### Mac
```bash
cd ~/Downloads/echojay-vst
mkdir build && cd build
cmake .. -DJUCE_PATH=~/JUCE
cmake --build . --config Release
```

Plugins install to:
- AU: ~/Library/Audio/Plug-Ins/Components/EchoJay.component
- VST3: ~/Library/Audio/Plug-Ins/VST3/EchoJay.vst3

### Windows
```bash
mkdir build && cd build
cmake .. -DJUCE_PATH=C:/JUCE -G "Visual Studio 17 2022"
cmake --build . --config Release
```

VST3 installs to: C:\Program Files\Common Files\VST3\EchoJay.vst3

## API Integration

The AI chat is now fully wired. It works in two modes:

### Option A: EchoJay Backend (default)
Uses your existing echojay.ai deployment. The plugin calls `/api/chat` with the same message format as the web app — no extra setup if you're already logged in.

### Option B: Direct OpenAI
If you want the plugin to call OpenAI directly (no echojay.ai backend needed):

1. The plugin stores settings at:
   - **Mac:** `~/Library/Application Support/EchoJay/api_settings.json`
   - **Windows:** `%APPDATA%/EchoJay/api_settings.json`

2. Create/edit that file:
```json
{
  "endpoint": "https://api.openai.com/v1/chat/completions",
  "apiKey": "sk-your-openai-key-here",
  "authToken": ""
}
```

3. Restart the plugin. It'll call OpenAI directly with GPT-4o-mini.

The system prompt is built automatically with the channel type, genre, and your installed plugins. Same AI behaviour as the web app.

## Key Files

```
echojay-vst/
├── CMakeLists.txt                  ← Build config
├── Source/
│   ├── PluginProcessor.h/cpp       ← Audio, capture, channel detection, compare context
│   ├── PluginEditor.h/cpp          ← Native UI (meters, chat, login, compare, drag & drop)
│   ├── MeterEngine.h/cpp           ← DSP (LUFS, true peak, stereo, spectrum)
│   ├── PluginScanner.h/cpp         ← System plugin discovery + caching
│   ├── EchoJayAPI.h/cpp            ← HTTP client (login, auth, chat, usage limits)
│   └── ReferenceAnalyser.h/cpp     ← Offline audio file analysis for references
└── Resources/
```

## Supported Formats
- **VST3** — Windows, Mac (Ableton, FL Studio, Cubase, Reaper, etc.)
- **AU** — Mac only (Logic Pro, GarageBand)
- **Standalone** — Runs without a DAW for testing
