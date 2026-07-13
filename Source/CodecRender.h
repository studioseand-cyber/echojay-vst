#pragma once
#include <JuceHeader.h>
#include <vector>

// =============================================================================
//  Codec Player (phase 1) — offline render of a source WAV through a lossy
//  codec and back: source -> encoded -> decoded temp wav. AAC via the OS
//  (AudioToolbox ExtAudioFile, macOS ONLY — non-Mac builds hide the AAC
//  presets; Media Foundation is deferred). Ogg Vorbis via JUCE's BUNDLED
//  libvorbis (Xiph BSD, see THIRD_PARTY_LICENSES.md) — no new dependency.
//
//  Honest labelling: "-style" presets, never claiming the platform's exact
//  encoder.
//
//  Normalise: ONE gain, computed from the SOURCE's integrated LUFS to hit
//  -14 LUFS, applied identically to the original copy AND the decoded
//  render. The codec is encoded from the UNGAINED master (so a hot master
//  hits the codec exactly as it would in distribution — inter-sample overs
//  and all), then both sides are gain-matched so the A/B compares the
//  codec, not a level drop.
//
//  Cache: ~/Library/EchoJay/codec_cache keyed on source path+mtime+preset+
//  normalise; keeps the most recent 16 renders (LRU by file mtime).
// =============================================================================
namespace CodecRender
{
    struct Preset
    {
        const char* id;      // cache key component
        const char* name;    // card title
        const char* sub;     // one-line description
        int         kbps;
        bool        isAAC;   // AAC = mac only
    };
    const std::vector<Preset>& presets();   // AAC entries absent off-mac

    struct Result
    {
        bool ok = false;
        bool cacheHit = false;
        juce::String error;
        juce::String originalWav;   // gain-matched original copy
        juce::String renderWav;     // decoded (and gain-matched) codec render
        std::vector<float> origThumb, renderThumb;   // ~300-pt abs-peak
    };

    // BLOCKING — call on a background thread only.
    Result render(const juce::String& sourcePath, const Preset& preset, bool normalise);
}
