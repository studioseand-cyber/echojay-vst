/*
    EqPresets.h  —  the built-in preset table behind `eq_preset` (P5 of
    SURGICAL_EQ_ENHANCEMENTS.md). JUCE-free data, tested in
    test/eq_preset_test.cpp.

    A preset is a BASE: the funnel clears the band model, lays these bands down
    by explicit index, applies the settings — and only then merges any explicit
    eq_bands from the same move on top. "Start from vocal clarity, then cut 300
    by 2" therefore does exactly what it says.

    Names resolve with the same tolerance as every other name in the EQ
    (case-insensitive, separators dropped), and the table is the single source
    the advertisement, the editor's menu and the apply path all read — so a
    preset cannot exist in one place and be missing from another.
*/

#pragma once

#include "EqEngine.h"

#include <cctype>
#include <cstring>

namespace echojay
{

struct EqPresetDef
{
    const char*     name;       // canonical kebab-case, as advertised
    const char*     blurb;      // one line for the menu + the model
    const BandSpec* bands;
    int             numBands;
    float           outputDb;   // preset's device-global settings
    bool            autoGain;
};

namespace eqpresets
{
    // BandSpec field order: enabled, type, freqHz, gainDb, q, slopeDbPerOct,
    // dynamic, thresholdDb, rangeDb, attackMs, releaseMs, channel.

    inline constexpr BandSpec kVocalClarity[] = {
        { true, BandType::HighPass,  80.0f,  0.0f, 0.707f, 12 },
        { true, BandType::Bell,     300.0f, -2.5f, 1.2f,   12 },
        { true, BandType::Bell,    3200.0f,  2.0f, 1.0f,   12 },
        { true, BandType::HighShelf, 10000.0f, 1.5f, 0.707f, 12 },
    };

    inline constexpr BandSpec kWarmTape[] = {
        { true, BandType::LowShelf,  120.0f,  1.5f, 0.707f, 12 },
        { true, BandType::Bell,      400.0f,  0.8f, 1.0f,   12 },
        { true, BandType::HighShelf, 8000.0f, -1.5f, 0.707f, 12 },
    };

    inline constexpr BandSpec kDeHarsh[] = {
        { true, BandType::Bell, 3500.0f, 0.0f, 2.5f, 12,
          true, -30.0f, -4.0f, 3.0f, 120.0f },
        { true, BandType::Bell, 7000.0f, 0.0f, 3.0f, 12,
          true, -32.0f, -3.0f, 2.0f, 100.0f },
        { true, BandType::HighShelf, 9000.0f, -1.0f, 0.707f, 12 },
    };

    inline constexpr BandSpec kSubCleanup[] = {
        { true, BandType::HighPass, 30.0f,  0.0f, 0.707f, 24 },
        { true, BandType::Bell,     60.0f, -2.0f, 2.0f,   12 },
    };

    // The air rides the SIDES (P2 routing): width up top, mono sum untouched.
    inline constexpr BandSpec kAirLift[] = {
        { true, BandType::HighShelf, 10000.0f, 3.0f, 0.707f, 12,
          false, 0.0f, 0.0f, 10.0f, 100.0f, BandChannel::Side },
        { true, BandType::HighShelf, 12000.0f, 1.0f, 0.707f, 12 },
    };

    inline constexpr BandSpec kMudCut[] = {
        { true, BandType::Bell, 250.0f, -3.0f, 1.4f, 12 },
        { true, BandType::Bell, 450.0f, -1.5f, 2.0f, 12 },
    };
} // namespace eqpresets

inline constexpr EqPresetDef kEqPresets[] = {
    { "vocal-clarity", "HPF, mud dip, presence and air for a lead vocal",
      eqpresets::kVocalClarity, 4, 0.0f, false },
    { "warm-tape",     "gentle tape-style tilt: low warmth, soft top",
      eqpresets::kWarmTape,     3, 0.0f, false },
    { "de-harsh",      "dynamic bells riding the 3.5k/7k harshness zone",
      eqpresets::kDeHarsh,      3, 0.0f, false },
    { "sub-cleanup",   "24 dB/oct rumble filter plus a tight 60 Hz dip",
      eqpresets::kSubCleanup,   2, 0.0f, false },
    { "air-lift",      "side-channel air shelf: wider, brighter, mono-safe",
      eqpresets::kAirLift,      2, 0.0f, false },
    { "mud-cut",       "clears 250-450 Hz congestion on busy sources",
      eqpresets::kMudCut,       2, 0.0f, false },
};

inline constexpr int kNumEqPresets = (int) (sizeof (kEqPresets) / sizeof (kEqPresets[0]));

// Tolerant lookup: "Vocal Clarity", "vocal_clarity" and "vocalclarity" are all
// the "vocal-clarity" preset. Nullptr when nothing matches — the caller
// reports the miss honestly rather than loading something else.
inline const EqPresetDef* findEqPreset (const char* name) noexcept
{
    if (name == nullptr) return nullptr;

    char norm[48]; int j = 0;
    for (int i = 0; name[i] != '\0' && j < 47; ++i)
    {
        const char c = name[i];
        if (c == '_' || c == '-' || c == ' ') continue;
        norm[j++] = (char) std::tolower ((unsigned char) c);
    }
    norm[j] = '\0';
    if (j == 0) return nullptr;

    for (const auto& p : kEqPresets)
    {
        char pn[48]; int k = 0;
        for (int i = 0; p.name[i] != '\0' && k < 47; ++i)
        {
            const char c = p.name[i];
            if (c == '_' || c == '-' || c == ' ') continue;
            pn[k++] = (char) std::tolower ((unsigned char) c);
        }
        pn[k] = '\0';
        if (std::strcmp (norm, pn) == 0) return &p;
    }
    return nullptr;
}

} // namespace echojay
