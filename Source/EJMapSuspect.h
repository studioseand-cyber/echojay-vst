#pragma once
// =============================================================================================
//  EJMapSuspect.h - A REDUCTION CONTROL THAT CHANGES NO GAIN REDUCTION
//  (Sean's approval of 25 Oct 2026, built as his item 6 of the 26 Oct overnight list)
// =============================================================================================
//  Sean, after the Vac Attack: *"a set on a control the map calls reduction or threshold that
//  doesn't change the slot's IN/OUT difference means the map or the request is wrong, and the card
//  says so instead of a confident success."*
//
//  WHY IT IS WORTH HAVING. On 25 Oct the agent set "Left Reduction" and "Right Reduction" to 3 on
//  the Mixland Vac Attack. Both writes landed, both read back "3.0", and the card said so with
//  confidence - while the knobs that moved were the two GAIN knobs. Every layer was telling the
//  truth about what it had done; nobody was checking whether what it did was what was ASKED. We
//  already measure every slot's input and output, so the one fact that would have caught it was in
//  the building: a compressor whose reduction control has just been raised by 3 of its own units
//  and whose in-to-out difference did not move is either wrongly mapped or wrongly asked.
//
//  WHAT IT MUST NOT DO is accuse a plugin on no evidence. Three things make the check unavailable
//  rather than negative, and each is a separate state: nothing was playing (no difference to
//  measure), the slot does not report a reading, or the control is not one this rule is about.
//  "Unavailable" says nothing on the card - the 9 Aug silence rule - and only a measured
//  no-change speaks.
// =============================================================================================

namespace echojay
{

enum class MapVerdict
{
    notChecked,   // this rule does not apply, or there was nothing to measure
    consistent,   // the difference moved: the control does what its name says
    suspect       // it did not move, and it should have
};

/** Is this the kind of control the rule is about? The map's own words for a compressor's amount:
    anything that claims to set compression. Deliberately short and literal - a rule that guessed
    from a name would be the very fault it is here to catch. */
inline bool controlClaimsReduction (const juce::String& name)
{
    const auto n = name.trim().toLowerCase();
    return n.contains ("reduction") || n.contains ("threshold") || n.contains ("thresh")
        || n.contains ("compression");
}

/** `heardSignal`   - the slot measured audio over the window; without it there is no difference.
    `haveBefore/After` - a reading exists on each side of the write.
    `diffBeforeDb/AfterDb` - the slot's IN minus OUT, in dB, before and after.
    `minChangeDb`   - how much the difference must move to count. 0.5 dB is the read-back ladder's
                      own smallest meaningful step, and well above the metering's noise. */
inline MapVerdict mapVerdictForSet (bool controlIsReductionLike, bool heardSignal,
                                    bool haveBefore, bool haveAfter,
                                    float diffBeforeDb, float diffAfterDb,
                                    float minChangeDb = 0.5f)
{
    if (! controlIsReductionLike)            return MapVerdict::notChecked;
    if (! heardSignal)                       return MapVerdict::notChecked;
    if (! haveBefore || ! haveAfter)         return MapVerdict::notChecked;
    if (std::abs (diffAfterDb - diffBeforeDb) >= minChangeDb) return MapVerdict::consistent;
    return MapVerdict::suspect;
}

/** The sentence the card carries when the check is SUSPECT, and nothing otherwise. It names the
    control, both figures and both possibilities, because the build cannot tell which of the two is
    wrong and must not pick one. */
inline juce::String mapSuspectLine (const juce::String& pluginName, const juce::String& control,
                                    float diffBeforeDb, float diffAfterDb)
{
    return "I set " + control + " on " + pluginName + " and it read back, but the slot is doing the "
           "same work as before (" + juce::String (diffBeforeDb, 1) + " dB in-to-out before, "
         + juce::String (diffAfterDb, 1) + " dB after). A control called \"" + control + "\" that "
           "changes no gain reduction means either this plugin's map names the wrong knob, or that "
           "is not the control for what was asked. Worth checking on the plugin before trusting it.";
}

} // namespace echojay
