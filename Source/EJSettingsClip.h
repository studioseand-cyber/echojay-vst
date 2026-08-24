#pragma once

#include <JuceHeader.h>

// The model-side clip for one slot's settings text in [CURRENT CHAIN].
//
// Header-inline on purpose, the EJDialMissRows.h discipline: the gate links
// the PREVIOUS build's SharedCode lib, so anything a pin exercises must live in
// a header the test TU compiles directly, or the pin measures the last build
// instead of this one.
//
// TWO FACTS ABOUT THE NUMBER, 24 Aug 2026.
//
// It is sized from the STRUCTURAL bound, not from the sample. The server
// exposes at most K_PER_PLUGIN = 12 controls per plugin
// (echojay-saas/lib/controls-note.js), so an apply can name about twelve, and a
// twelve-control tiered line runs 420 to 500 characters. The observed sample
// was thirteen applies with a largest of four (146 chars) — a cap sized from
// that would clear everything we have done and fail the first time somebody
// does something we have not.
//
// It is not tight against the turn. The server's own per-turn exposure budget
// for this material is 12,000 characters (raised from 1,750 on 11 Aug 2026,
// commit 11c41bd, "sized from measurement"). At 500 per slot an eight-slot rack
// spends 4,000, a third of what the server already spends naming the controls
// these values belong to.
//
// AND IT ANNOUNCES ITSELF. A cap that silently drops the tail reintroduces the
// exact defect the Landed / Asked, not verified / Refused tiering exists to
// remove: an omission the reader cannot see. The unverified group sits at the
// END of the string, so a silent clip would eat precisely the part that says
// "do not trust this value". The marker is therefore not decoration; it is the
// difference between a short report and a wrong one.
namespace echojay
{

inline constexpr int kModelSettingsClip = 500;

inline juce::String clipModelSettings (const juce::String& text)
{
    if (text.length() <= kModelSettingsClip) return text;
    return text.substring (0, kModelSettingsClip)
         + " [CLIPPED: this slot's settings ran past " + juce::String (kModelSettingsClip)
         + " characters and the rest is NOT shown; do not read the controls listed"
           " here as the complete set]";
}

} // namespace echojay
