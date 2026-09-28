/*
  EjmapFixtureUnit.h

  THE UNIT RULE the compressor-profile fixtures were built with, written down
  because it was in no committed code. The 25 Sep sampling pass that produced
  the 74 fixtures (echojay-saas scripts/fixtures/compressor-profiles/) derived
  each control's `unit` itself, and the driver was never committed. Step 1 of
  the certification work found it the hard way: the probe matched XLA-3's
  fixture on 152 of 155 fields, and the three that differed were units the
  probe does not print.

  It is a DERIVATION (decision D2, 28 Sep): it needs re-computing, never
  re-measuring, so it lives here in EJ Map and never in the signed probe.

  THE RULE:
    1. The plugin's parameter label, when it has one.
    2. Otherwise the text after a leading number at the 0.0 point.
    3. If the 0.0 point does not start with a number, the same at the 1.0
       point.
    4. Otherwise no unit.
  The 0.5 point is never consulted. That is why this function does not take
  it.

  HOW IT WAS ESTABLISHED (28 Sep 2026, measured, not inferred):
    - Real labels read through the signed probe (--list-params) for 13 of the
      74 products, 435 controls: EVERY ONE has an empty label, yet 88 carry a
      unit. For those products the unit is display text, all of it. The rule
      reproduces all 435 exactly.
    - Across all 1,783 controls it gives a unit to none of the 1,096 that have
      none. That is the strong test, because a label can supply a unit but
      never remove one. 356 units come from the text, and the other 331 must
      come from labels on products not measured here.
    - It beat four simpler rules that each failed on a real case: majority
      over all three points (Neve "400mS / 1.5S / AUTO" is mS, not S); "the
      last point" (AMEK "20 Hz / 200 Hz / 2.0 kHz" is Hz); "skip the points
      that are words" (Mike-E "Bypass / 4:1 / NUKE" has no unit); and "the
      suffix all three points share" (Purple "Off / 200 Hz / 2.0 kHz" is kHz).

  A NUMBER is an optional sign (-, + or U+2212), one or more ASCII digits,
  then optionally '.' and one or more digits. A BARE LEADING DOT IS NOT A
  NUMBER. That one clause decides the four exceptions, and the pins in
  RoundTripTest.cpp name them:
    - SSL G3 MultiBusComp Low / Mid / High Release, ".1 s / .6 s / AUTO": no
      unit, because neither end starts with a number.
    - Empirical Labs Mike-E Comp Ratio, "Bypass / 4:1 / NUKE": no unit,
      because the 4:1 is at the middle point.
  THE RULE IS BUG-COMPATIBLE ON LEADING-DOT NUMBERS. This is a choice, not a
  finding. ".1 s" is plainly seconds, and the rule returns no unit for it
  because that is what the sampling pass did and three shipped fixtures (SSL
  G3 Low / Mid / High Release) depend on it. That is right for REPRODUCING
  those fixtures, and wrong for everything else: every NEW fixture this rule
  writes will also record no unit for a ".1 s" control, so the defect is
  forward-looking wrong data, not only a quirk kept for the past. If the three
  SSL G3 fixtures are ever re-sampled, fix the rule and those fixtures
  together in ONE change, never the rule alone. The pin in RoundTripTest.cpp
  fails if someone "fixes" the rule silently, and that is what it is for.

  U+2212 IS NOT A MINUS SIGN - CORRECTED 28 Sep, and bug-compatible like the
  leading dot. The first version of this header counted U+2212 as a sign and
  called it a choice the data did not decide. The unit data alone does not
  decide it. The RANGE data does (EjmapFixtureRange.h): kHs Compressor's
  Threshold "−40.00 dB / −17.00 dB / +6.00 dB" is recorded as named positions,
  because only the ASCII "+6.00" parsed. One number parser made both fields,
  so this one must match it. No fixture unit changes: kHs's Threshold still
  reads dB, from its 1.0 point. The same rule as the leading dot applies: fix
  it only together with re-sampling the affected fixtures.

  THE ONE CHOICE THE DATA DOES NOT DECIDE (every variant scores the same on all
  1,783 controls): an empty remainder at the 0.0 point ("0.00") means no unit.
  It does NOT fall back to the 1.0 point. Pinned, so any change is deliberate.
*/

#pragma once

#include <juce_core/juce_core.h>
#include <optional>

namespace ejmap::fixtureunit
{

/** The text after a leading number, trimmed; nullopt when the text does not
    start with one. U+202F and U+00A0 (kHs prints "40.00 dB") are read as
    ordinary spaces first.
*/
struct LeadingNumber { double value = 0.0; juce::String remainder; };

/** THE NUMBER PARSER the fixtures were built with, shared by the unit and range
    rules so the two can never disagree about what a number is: an optional ASCII
    sign (- or +), one or more ASCII digits, then optionally '.' and one or more
    digits. NOT a number: a bare leading dot (".1 s") or a U+2212 minus
    ("−40.00") - both bug-compatible, see above.
*/
inline std::optional<LeadingNumber> leadingNumber (const juce::String& text)
{
    const auto t = text.replaceCharacter ((juce::juce_wchar) 0x202F, ' ')
                       .replaceCharacter ((juce::juce_wchar) 0x00A0, ' ')
                       .trimStart();
    auto isAsciiDigit = [] (juce::juce_wchar c) { return c >= '0' && c <= '9'; };
    const int n = t.length();
    int i = 0;
    if (i < n && (t[i] == '-' || t[i] == '+'))
        ++i;
    const int digitsFrom = i;
    while (i < n && isAsciiDigit (t[i])) ++i;
    if (i == digitsFrom)
        return std::nullopt;                 // ".1 s" and "−40" land here, deliberately
    if (i + 1 < n && t[i] == '.' && isAsciiDigit (t[i + 1]))
    {
        ++i;
        while (i < n && isAsciiDigit (t[i])) ++i;
    }
    return LeadingNumber { t.substring (0, i).getDoubleValue(), t.substring (i).trim() };
}

inline std::optional<juce::String> remainderAfterLeadingNumber (const juce::String& text)
{
    if (auto n = leadingNumber (text)) return n->remainder;
    return std::nullopt;
}

/** The fixture `unit` for one control. Empty means no unit. textAt0 and
    textAt1 are the plugin's display text at normalised 0.0 and 1.0, exactly
    as --text-at printed them.
*/
inline juce::String unitFor (const juce::String& label,
                             const juce::String& textAt0,
                             const juce::String& textAt1)
{
    if (label.trim().isNotEmpty())
        return label.trim();
    for (const auto* t : { &textAt0, &textAt1 })
        if (auto r = remainderAfterLeadingNumber (*t))
            return *r;                       // may be empty: a bare number has no unit
    return {};
}

} // namespace ejmap::fixtureunit
