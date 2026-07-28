/*
    EqMove.h  —  the exact-apply bridge between an AI "eq_bands" move and the
    EqEngine band model. Deliberately JUCE-free so the allocation/merge logic
    is unit-testable in isolation; the juce::var parsing is a thin layer on top
    (see SurgicalEqProcessor).

    Semantics
    ---------
    A move set is applied as a *per-band merge*, not a wholesale replace, so an
    AI suggestion never silently wipes the user's other bands:

      * band >= 1  → target that exact 1-based band index.
      * band <= 0  → auto-allocate the lowest currently-disabled band.
      * disable    → turn the target band off (leaving its other params intact).

    This is what makes "−3 dB, 203 Hz, Q 4.5, band 3" land exactly where asked.
*/

#pragma once

#include "EqEngine.h"

#include <cstring>
#include <cctype>

namespace echojay
{

struct EqMove
{
    int      band    = -1;      // 1-based target; <=0 == auto-allocate
    bool     disable = false;   // true => disable the target band
    BandSpec spec;              // applied when disable == false (enabled forced true)
};

// case-insensitive, tolerant of separators: "low_shelf","low-shelf","lowshelf"
inline bool parseBandType (const char* s, BandType& out) noexcept
{
    if (s == nullptr) return false;
    char norm[24]; int j = 0;
    for (int i = 0; s[i] != '\0' && j < 23; ++i)
    {
        const char c = s[i];
        if (c == '_' || c == '-' || c == ' ') continue;       // drop separators
        norm[j++] = (char) std::tolower ((unsigned char) c);
    }
    norm[j] = '\0';

    auto eq = [&] (const char* k) { return std::strcmp (norm, k) == 0; };

    if (eq ("bell") || eq ("peak") || eq ("peaking") || eq ("pk"))      { out = BandType::Bell;      return true; }
    if (eq ("lowshelf")  || eq ("lshelf") || eq ("ls") || eq ("lsh"))   { out = BandType::LowShelf;  return true; }
    if (eq ("highshelf") || eq ("hshelf") || eq ("hs") || eq ("hsh"))   { out = BandType::HighShelf; return true; }
    if (eq ("notch") || eq ("band") || eq ("bandreject") || eq ("br"))  { out = BandType::Notch;     return true; }
    if (eq ("highpass") || eq ("hipass") || eq ("hp") || eq ("hpf"))    { out = BandType::HighPass;  return true; }
    if (eq ("lowpass")  || eq ("lopass") || eq ("lp") || eq ("lpf"))    { out = BandType::LowPass;   return true; }
    return false;
}

inline const char* bandTypeToString (BandType t) noexcept
{
    switch (t)
    {
        case BandType::Bell:      return "bell";
        case BandType::LowShelf:  return "lowshelf";
        case BandType::HighShelf: return "highshelf";
        case BandType::Notch:     return "notch";
        case BandType::HighPass:  return "highpass";
        case BandType::LowPass:   return "lowpass";
        case BandType::NumTypes:
        default:                  return "bell";
    }
}

// Apply moves onto an existing band array. Returns the number applied.
// `bands` has `maxBands` entries. Unresolvable auto-allocations (no free band)
// are skipped and counted in `outSkipped` if provided.
inline int applyEqMoves (BandSpec* bands, int maxBands,
                         const EqMove* moves, int count, int* outSkipped = nullptr) noexcept
{
    int applied = 0, skipped = 0;

    for (int m = 0; m < count; ++m)
    {
        const EqMove& mv = moves[m];
        int idx = -1;

        if (mv.band >= 1 && mv.band <= maxBands)
        {
            idx = mv.band - 1;                       // explicit target
        }
        else
        {
            for (int i = 0; i < maxBands; ++i)       // lowest free (disabled) band
                if (! bands[i].enabled) { idx = i; break; }
        }

        if (idx < 0) { ++skipped; continue; }        // nowhere to put it

        if (mv.disable)
        {
            bands[idx].enabled = false;
        }
        else
        {
            bands[idx] = mv.spec;
            bands[idx].enabled = true;               // a set always enables
        }
        ++applied;
    }

    if (outSkipped != nullptr) *outSkipped = skipped;
    return applied;
}

} // namespace echojay
