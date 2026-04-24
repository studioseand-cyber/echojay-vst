# EchoJay — AI-Powered Mix Feedback Plugin

## What Is EchoJay?

EchoJay is a VST3/AU/AAX plugin that gives you real-time metering and AI-powered mix feedback directly inside your DAW. Drop it on any channel, capture your audio, and get instant, actionable advice from an AI assistant that knows your DAW, your plugins, and your experience level.

Works with Logic Pro, Pro Tools, Ableton Live, FL Studio, Cubase, Reaper, Studio One, and any DAW that supports VST3, AU, or AAX.

## Features

### AI Mix Assistant
The core of EchoJay. A built-in chat interface powered by AI that analyses your audio and delivers genre-contextual, actionable mix advice. It knows your channel type, your genre, and your installed plugins — so when it says "add a bell at 3.2kHz with Pro-Q 3", it's using tools you actually own. Ask follow-up questions, request alternatives, or get a second opinion on any mix decision.

### AI Compare
Load a reference track or capture multiple passes of your mix, then let the AI compare them side by side. It breaks down exactly what changed — loudness, stereo width, frequency balance, dynamics — and tells you whether your changes improved the mix or pulled it in the wrong direction.

### Precision Metering
12 meters running in real time: LUFS (momentary, short-term, integrated), LRA, true peak (4x oversampled), RMS, crest factor, DC offset, stereo width, correlation, and 8-band spectrum. 100% pass-through — EchoJay never modifies your audio.

### Capture & Automatic Analysis
Hit Capture, play your section, stop. EchoJay finalises the meter data, stores it as a snapshot, records a 32-bit float WAV, and automatically sends everything to the AI for instant feedback. No extra steps.

### Reference Tracks
Drag and drop a WAV, MP3, FLAC, AIFF, OGG, or M4A onto the plugin to load a reference. EchoJay analyses it offline using the same meter engine and stores the results for comparison. Compare your mix against any commercial release.

### Plugin Scanner
Scans your system for every VST3, AU, and AAX plugin you own. The AI uses this to build chains and make suggestions with specific settings — frequencies, ratios, attack/release times — using only the tools in your arsenal.

### Multi-Instance & Channel Detection
Run multiple instances across your mixer. Set each to a channel type (Full Mix, Vocals, Bass, Drums, Instruments, Synths, FX) or use Auto-Detect. The AI adjusts its feedback accordingly — sibilance checks for vocals, mono compatibility for bass, transient punch for drums.

### Compact Mode
Double-click the EchoJay logo to toggle compact mode — a smaller window with just the chat and capture controls. Perfect for when you want quick AI feedback without the full meter display.

## Pricing

Metering is always free and unlimited. AI features use a credit system:

| Plan | Price | AI Credits/Day |
|------|-------|---------------|
| Free | £0 | 2 |
| Pro | £9.99/mo | 50 |
| Studio | £19.99/mo | 150 |
| ITS Platinum | £14.99/mo | 15 (via Inside the Sound) |

Credits are shared across the plugin and web app. Limits reset daily.

## Installation

### macOS (Recommended)
Download the DMG, open it, and double-click the installer package. It automatically installs:
- **VST3** to /Library/Audio/Plug-Ins/VST3/
- **AU** to /Library/Audio/Plug-Ins/Components/
- **AAX** to /Library/Application Support/Avid/Audio/Plug-Ins/

Restart your DAW and rescan plugins if needed.

**Requirements:** macOS 12 (Monterey) or later. Universal binary — runs natively on Apple Silicon and Intel Macs.

### Windows
Download the VST3 from the latest release and copy `EchoJay.vst3` to:
- `C:\Program Files\Common Files\VST3\`

Restart your DAW and rescan plugins.

## Supported DAWs

| DAW | Format |
|-----|--------|
| Pro Tools | AAX |
| Logic Pro | AU |
| Ableton Live | VST3 / AU |
| FL Studio | VST3 |
| Cubase / Nuendo | VST3 |
| Reaper | VST3 / AU |
| Studio One | VST3 / AU |
| GarageBand | AU |

## Getting Started

1. Install EchoJay and open it in your DAW
2. Log in with your echojay.ai account (or create one at [echojay.ai](https://echojay.ai))
3. Select your channel type and genre
4. Hit **Capture**, play your section, then stop
5. Read the AI feedback and ask follow-up questions in the chat

## Build From Source

### Prerequisites
- **JUCE 8.0.12+** — https://juce.com/download/
- **CMake 3.22+**
- **Xcode** (macOS) or **Visual Studio 2022** (Windows)
- **AAX SDK** (optional, for Pro Tools support) — https://developer.avid.com

### macOS
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0
cmake --build build --config Release
```

### Windows
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target EchoJay_VST3
```

## Links

- **Website:** [echojay.ai](https://echojay.ai)
- **Support:** [echojay.ai/support](https://echojay.ai/support)
- **Inside the Sound:** [insidethesound.com](https://insidethesound.com)

© 2026 EchoJay
