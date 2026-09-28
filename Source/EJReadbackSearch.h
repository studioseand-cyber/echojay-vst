#pragma once
// ===========================================================================
// EJReadbackSearch: landing a dB target on a control by READING IT BACK
// (21t-j, 28 Sep 2026, B's contract note 3).
//
// WHY. A control is placed from its param map's sampled anchors. The MC 77's
// "Input L" profiles as THREE points - "-inf dB" at 0.0, "-24.0 dB" at 0.5,
// "0.0 dB" at 1.0 - so a start around -30 has no curve to sit on: the map
// clamps it to the lowest anchor and the control lands at -24, which is the
// middle of its travel. Until the profile is re-sampled with a real curve
// below -24, the only honest way to land -30 is to ask the plugin where -30
// is: walk the normalised value, read the plugin's OWN display text at each
// step, and stop when the text says what was asked for.
//
// THE RULES THIS KEEPS:
//   read-only until it lands   every probe is getText(norm), which asks what
//                              the display WOULD read; the parameter is not
//                              moved until the search has finished and then
//                              exactly once, to the best position found.
//   never invents a number     "-inf" is not a reading, and a control whose
//                              text carries no number at its ends is refused
//                              rather than searched.
//   needs an order             a control whose two ends read the same is not
//                              monotonic in dB, so bisection would be
//                              meaningless; it is refused and says so.
//
// House rules: no em-dashes anywhere in this file.
// ===========================================================================
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>

namespace echojay {

/** "-inf dB" is an absence, not a number: it comes back as a huge magnitude of the right sign so a caller can
    reject it, and everything else is the leading numeric token of the plugin's own text. */
inline bool parseDisplayDb (const juce::String& text, double& outDb)
{
    const auto t = text.trim();
    if (t.containsIgnoreCase ("inf")) { outDb = t.startsWithChar ('-') ? -1.0e9 : 1.0e9; return true; }
    juce::String num;
    for (auto c : t)
    {
        if (juce::CharacterFunctions::isDigit (c) || c == '.' || c == '-' || c == '+') num << c;
        else if (num.isNotEmpty()) break;
    }
    // 21t-j (28 Sep 2026, found by the GR-meter cross-check leg): A STRING WITH NO DIGITS IS NOT A NUMBER.
    // The scan above collects sign and point characters as happily as digits, so "--" - which is what a meter
    // prints when it has nothing to show, and what this product's own strips draw - came through as the string
    // "--" and getDoubleValue() turned it into 0.0. A GR meter reading "--" was therefore a reading of 0.0 dB,
    // and a readback search would have believed it. The old guard listed three exact strings; this one asks the
    // question that matters.
    if (num.isEmpty() || ! num.containsAnyOf ("0123456789")) return false;
    outDb = num.getDoubleValue();
    return true;
}

struct ReadbackSearch
{
    bool  landed = false;      // within tolerance
    float position = 0.0f;     // the normalised value chosen
    float landedDb = 0.0f;     // what the display read there
    juce::String refusal;      // why nothing was searched, when it was not
};

/** Bisect the control for a dB target. Reads only; the caller writes the position. */
inline ReadbackSearch searchForDb (const juce::AudioProcessorParameter& q, float targetDb,
                                   float toleranceDb = 0.5f, int maxSteps = 12)
{
    ReadbackSearch r;
    auto readAt = [&q] (float norm, double& db)
    { return parseDisplayDb (q.getText (juce::jlimit (0.0f, 1.0f, norm), 256), db); };

    double at0 = 0.0, at1 = 0.0;
    if (! readAt (0.0f, at0) || ! readAt (1.0f, at1))
    { r.refusal = "the control does not print a number at its ends"; return r; }
    if (std::abs (at1 - at0) < 1.0e-6)
    { r.refusal = "the control reads the same at both ends - not monotonic in dB"; return r; }
    const bool ascending = at1 > at0;

    float lo = 0.0f, hi = 1.0f;
    bool haveBest = false;
    double bestDb = 0.0;
    for (int i = 0; i < maxSteps; ++i)
    {
        const float mid = 0.5f * (lo + hi);
        double db = 0.0;
        if (! readAt (mid, db)) { r.refusal = "the control stopped printing a number mid-search"; return r; }
        // An "-inf" reading is not a distance from the target; it is only ever BELOW it, so the search moves up.
        const bool isInf = db <= -1.0e8 || db >= 1.0e8;
        if (! isInf && (! haveBest || std::abs (db - (double) targetDb) < std::abs (bestDb - (double) targetDb)))
        { r.position = mid; bestDb = db; haveBest = true; }
        if (! isInf && std::abs (db - (double) targetDb) <= (double) toleranceDb) break;
        const bool tooLow = isInf ? true : (ascending ? (db < (double) targetDb) : (db > (double) targetDb));
        if (tooLow) lo = mid; else hi = mid;
    }
    if (! haveBest) { r.refusal = "every probe read as -inf"; return r; }
    r.landedDb = (float) bestDb;
    r.landed = std::abs (bestDb - (double) targetDb) <= (double) toleranceDb;
    return r;
}

} // namespace echojay
