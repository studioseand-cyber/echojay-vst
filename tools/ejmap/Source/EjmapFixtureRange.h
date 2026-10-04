/*
  EjmapFixtureRange.h

  THE RANGE AND DIRECTION RULES the 74 compressor-profile fixtures were built
  with, written down for the same reason as EjmapFixtureUnit.h: the 25 Sep
  sampling pass derived them and was never committed. They are derivations (D2),
  so they live in EJ Map and never in the signed probe.

  REPRODUCES ALL 1,783 CONTROLS, both fields, nothing excluded (28 Sep). Each
  clause below was forced by a named real case, and the pins in RoundTripTest.cpp
  name them.

  Inputs are the display text at the three sampled points, 0.0 / 0.5 / 1.0, read
  with the SAME number parser as the unit rule (fixtureunit::leadingNumber: ASCII
  sign, digits, optional decimal; no bare leading dot, no U+2212).

    1. FEWER THAN TWO POINTS ARE NUMBERS -> {"status": "text is not numeric"},
       direction "named positions". Forced by Drawmer 1973 Release "0.08 s / F / S"
       (only 0.0 parses) and UAD 1176AE Meter "OFF / +8 / GR" (only the middle).
    2. min and max come from the two ENDS, and a missing end borrows the middle.
       Forced by Waves C1 Comp Ratio "0.50:1 / 4.58:1 / -5.00:1": its max is 0.5,
       not 4.58. XLA-3 Noise Level "-oo dB / -90.0 dB / -60.0 dB" borrows the
       middle for its missing end, so its min is -90.
    3. at0 / at0_5 / at1 are the parsed values, null where a point did not parse.
       inverted = (the end-or-borrowed low value > the high value).
    4. endsNotNumeric lists only ENDS that did not parse, with the fixed note.
       Forced by LA-2 Meter "+10dB / GR / +4dB": both ends parse, the middle is a
       word, and the fixture carries no endsNotNumeric and no note.
    5. linear is present exactly when both ends parse. It is false when the
       middle did not parse (the LA-2 Meters). Otherwise it is true when the middle
       lies within 1% of the span from the midpoint of the ends. THE 1% IS A CHOICE:
       the data brackets it (every true deviates at most 0.476%, e.g. Distressor
       0 / 5.2 / 10.5; every false at least 2.17%, e.g. Shadow Hills Discrete Gain
       1 / 13 / 24), and any tolerance in [0.5%, 2.1%] reproduces all 1,783.
    6. direction: "flat" when min == max (UAD 1176AE Ratio "None / 2:1+4:1 /
       2:1+20:1" is flat at 2), else "descending" when inverted, else "ascending".
*/

#pragma once

#include <juce_core/juce_core.h>
#include "EjmapFixtureUnit.h"

namespace ejmap::fixturerange
{

inline const char* kEndsNote =
    "one or more sampled points print a word, not a number; the range is taken from the points that parsed";

struct Derived
{
    juce::var range;          // the fixture's `range` object, exactly its shape
    juce::String direction;   // "ascending" | "descending" | "flat" | "named positions"
};

// RANGE PARTIAL (ruled 4 Oct, after CL 1B's Gain "Off / 8.5 / 31.0" with "0.0" at instantiate 0.33): the three samples can miss
// a whole half of a control. Every parsed sample now folds into min/max - the instantiate point included (`at_instantiate`
// records it) - and `range_partial` names why the range may still not be the whole control: an end prints a word, an end
// sample is missing, or the instantiate point fell outside the sampled ends. A consumer must never resolve a display to a
// norm across such a gap by interpolation; the follow-up's re-sample (21 norms) is what fills it.
inline const char* kPartialWordEnd    = "an end prints a word";
inline const char* kPartialMissingEnd = "an end sample is missing";
inline const char* kPartialInstOut    = "the instantiate point lies outside the sampled ends";

inline Derived derive (const juce::String& at0Text, const juce::String& atHalfText, const juce::String& at1Text,
                       double linearTolerance = 0.01, const juce::String& instantiateText = {}, double instantiateNorm = -1.0)
{
    using fixtureunit::leadingNumber;
    const auto a = leadingNumber (at0Text), m = leadingNumber (atHalfText), b = leadingNumber (at1Text);
    const auto inst = instantiateText.isNotEmpty() ? leadingNumber (instantiateText) : std::nullopt;
    const int parsed = (a ? 1 : 0) + (m ? 1 : 0) + (b ? 1 : 0);

    Derived d;
    auto* o = new juce::DynamicObject();
    d.range = juce::var (o);
    juce::StringArray partial;
    if (at0Text.trim().isEmpty() || at1Text.trim().isEmpty()) partial.add (kPartialMissingEnd);
    if (parsed < 2)
    {
        o->setProperty ("status", "text is not numeric");
        if (! partial.isEmpty()) o->setProperty ("range_partial", partial.joinIntoString ("; "));
        d.direction = "named positions";
        return d;
    }
    const double lo = a ? a->value : m->value;           // a missing end borrows the middle
    const double hi = b ? b->value : m->value;
    auto num = [] (const std::optional<fixtureunit::LeadingNumber>& x) { return x ? juce::var (x->value) : juce::var(); };
    double mn = juce::jmin (lo, hi), mx = juce::jmax (lo, hi);
    if (inst)
    {
        if (inst->value < mn - 1e-9 || inst->value > mx + 1e-9) partial.add (kPartialInstOut);
        mn = juce::jmin (mn, inst->value); mx = juce::jmax (mx, inst->value);
        auto* ai = new juce::DynamicObject(); ai->setProperty ("norm", instantiateNorm); ai->setProperty ("value", inst->value); o->setProperty ("at_instantiate", juce::var (ai));
    }
    if (! a || ! b) partial.add (kPartialWordEnd);
    o->setProperty ("min", mn);
    o->setProperty ("max", mx);
    o->setProperty ("at0", num (a));
    o->setProperty ("at0_5", num (m));
    o->setProperty ("at1", num (b));
    o->setProperty ("inverted", lo > hi);
    if (! a || ! b)
    {
        juce::Array<juce::var> ends;
        if (! a) ends.add ("0.000");
        if (! b) ends.add ("1.000");
        o->setProperty ("endsNotNumeric", ends);
        o->setProperty ("note", kEndsNote);
    }
    else
    {
        bool linear = false;
        if (m)
        {
            const double span = std::abs (b->value - a->value);
            const double dev  = std::abs (m->value - 0.5 * (a->value + b->value));
            linear = span > 0.0 ? dev <= linearTolerance * span : dev == 0.0;
        }
        o->setProperty ("linear", linear);
    }
    d.direction = lo == hi ? "flat" : (lo > hi ? "descending" : "ascending");
    if (! partial.isEmpty()) o->setProperty ("range_partial", partial.joinIntoString ("; "));
    return d;
}

} // namespace ejmap::fixturerange
