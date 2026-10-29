#pragma once
// =============================================================================================
//  EJSlotSwallow.h - A SLOT THAT IS NOT PASSING THE PROGRAMME, SEEN FROM THE SLOT METERS
//  (Sean's 11:30 report of 29 Oct 2026, item 2; B's loss guard, C's point 72)
// =============================================================================================
//  His words:
//
//      *"The plugin can see a drop like this on the slot meter: report it to the server as a fault
//      and stop the run with a plain sentence, rather than waiting."*
//
//  The case was Shadow Hills Mastering Compressor on the Mix Bus, IN -15.7 and OUT -43.6. B found
//  the cause on the server (the profile's neutral write ran only on the first-listen dial path, so
//  Optical Gain went in at 1 of a 1..24 range whose neutral is 7) and has a loss guard of its own
//  with `lossMarginDb = 4`. This is the plugin's half, and it matters because **only the plugin
//  sees the slot meters live**: the server learns the two levels when a check runs, which can be
//  a minute later, and his run sat waiting in the meantime.
//
//  ---- IT BYPASSES THE SLOT; IT DOES NOT UNDO THE STEP (C's point 72, accepted) --------------
//
//  B's contract line said "undoes the step". Undo here is LIFO, so if anything landed after that
//  compressor - a later step, or his own hand - an undo takes back the wrong thing. A bypass is
//  deterministic, reversible, addresses THAT slot and nothing else, and leaves the plugin and its
//  settings in place for him to look at. B is changing the contract line to match, so both halves
//  describe the same action.
//
//  ---- THE THRESHOLD IS THE DIAL'S OWN AIM -----------------------------------------------------
//
//  B now sends `intendedGrDb` on a compressor add/set (the profile's figure when the step was
//  dialled, else the dial's aim). When it is absent, 6 dB stands in - which is what B and I
//  settled on, written here so the fallback is one number in one place rather than a guess at two
//  call sites. Past intended + margin, held for a few seconds WITH SIGNAL, the slot is bypassed.
//
//  HELD, not sampled: a compressor pumping hard on a transient can read 25 dB down for one hop and
//  be doing exactly what it was asked to. The three-second hold is the difference between "it is
//  working" and "nothing is coming out", and the decision below takes the hold as an argument so
//  the caller's clock is the one on record (the "clock-stated timings" rule).
// =============================================================================================

#include <JuceHeader.h>

namespace echojay
{

/** What B's guard calls lossMarginDb: how far past the intended gain reduction counts as loss
    rather than compression. */
inline constexpr float kSwallowMarginDb = 4.0f;
/** The stand-in when a step carries no `intendedGrDb`. B's contract and this agree on 6. */
inline constexpr float kSwallowAssumedGrDb = 6.0f;
/** How long the loss must stand, with signal, before the slot is bypassed. */
inline constexpr double kSwallowHoldMs = 3000.0;
/** Below this at the slot's INPUT there is no programme to lose, so nothing is judged. */
inline constexpr float kSwallowInputFloorDb = -40.0f;

/** One slot's two readings, as the slot meter frame carries them (EJSlotMeters.h). */
struct SlotLossReading
{
    int          slot1 = 0;        ///< 1-based, as every sentence to him counts slots
    juce::String plugin;
    bool         inKnown = false,  outKnown = false;
    float        inDb = -200.0f,   outDb = -200.0f;
    bool         bypassed = false; ///< a slot out of circuit is not measured and not judged
};

enum class SlotLossAct
{
    Nothing,    ///< within what was asked, or there is nothing to judge
    Watch,      ///< past the margin, but not for long enough yet
    Bypass      ///< past the margin and held: take the slot out and say so
};

struct SlotLossVerdict
{
    SlotLossAct  act = SlotLossAct::Nothing;
    float        lossDb = 0.0f;      ///< in minus out, when both are known
    float        allowedDb = 0.0f;   ///< intended + margin, for the sentence and the log
    juce::String say;                ///< what he reads (empty unless act == Bypass)
    const char*  why = "nothing to judge on this slot";
};

/** The whole decision, pure.

    `heldMs` is how long this slot has read past the margin WITH signal at its input, measured by
    the caller's wall clock. `intendedGrDb` is B's figure when the step carried one, or NaN.

    A BYPASSED SLOT IS NEVER JUDGED: it is out of circuit, so its meters are not a measurement -
    the same rule slotMeterEntry keeps ("bypassed wins over everything"). */
inline SlotLossVerdict slotLossVerdict (const SlotLossReading& r,
                                        float intendedGrDb,
                                        double heldMs,
                                        float marginDb = kSwallowMarginDb,
                                        double holdMs  = kSwallowHoldMs)
{
    SlotLossVerdict v;
    if (r.bypassed)        { v.why = "this slot is bypassed, so its meters are not a measurement"; return v; }
    if (! (r.inKnown && r.outKnown))
                           { v.why = "this slot does not publish both legs, so no loss can be measured"; return v; }
    if (! (std::isfinite (r.inDb) && std::isfinite (r.outDb)))
                           { v.why = "one of the two readings is not a number"; return v; }
    if (r.inDb <= kSwallowInputFloorDb)
                           { v.why = "nothing is reaching this slot, so there is nothing to lose"; return v; }

    const float intended = std::isfinite (intendedGrDb) ? std::abs (intendedGrDb)
                                                        : kSwallowAssumedGrDb;
    v.lossDb    = r.inDb - r.outDb;
    v.allowedDb = intended + marginDb;
    if (v.lossDb <= v.allowedDb)
    {
        v.why = "the drop is inside the gain reduction this step asked for, plus the margin";
        return v;
    }
    if (heldMs < holdMs)
    {
        v.act = SlotLossAct::Watch;
        v.why = "past the margin, but not for long enough to call it loss - a compressor can read "
                "far down for a moment and be doing what it was asked";
        return v;
    }
    v.act = SlotLossAct::Bypass;
    v.why = "past the asked-for reduction plus the margin, held with signal - this slot is not "
            "passing the programme";
    const auto name = r.plugin.isNotEmpty() ? r.plugin : juce::String ("that plugin");
    v.say = name + " was taking " + juce::String (v.lossDb, 1) + " dB out of the signal ("
          + juce::String (r.inDb, 1) + " dB in, " + juce::String (r.outDb, 1) + " dB out) when "
          + juce::String (intended, 0) + " dB was asked for, so I've bypassed it and stopped. "
            "Its settings are untouched: turn its output up, or take it out, then ask me again.";
    return v;
}

/** Which slot to act on when more than one is losing: the WORST, and the first of equals.

    Deliberately one at a time. Bypassing two plugins on one tick, from one sentence, is a bigger
    change than he asked for and the second may only look bad because the first is starving it. */
inline int worstLosingSlot (const std::vector<SlotLossVerdict>& v)
{
    int best = -1;
    float worst = 0.0f;
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i].act == SlotLossAct::Bypass && v[i].lossDb > worst)
        { worst = v[i].lossDb; best = (int) i; }
    return best;
}

} // namespace echojay
