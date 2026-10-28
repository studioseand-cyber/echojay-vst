#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <cmath>
#include <vector>

/** ---- THE CLIP STAGE'S FIGURE (A1c, 28 Oct 2026) -------------------------------------------

    B's `CONTRACT_clip_drive_2026-10-23.md`, from Sean's testing on 23 Oct: *"the clip stage before
    the limiter did nothing on most builds … the plan's 'a light clip, about 1.5 dB off the peaks'
    wasn't true"*. His ruling is that the clip stage is dialled **on playback, on its slot card**:
    raise the slot's IN until the peaks clip by about 1-1.5 dB, lower the slot's OUT by the same
    amount so the level into the limiter is unchanged, and **check the peak reduction across the
    slot**.

    EVERYTHING HERE IS ARITHMETIC OVER TWO ARRAYS, which is why it is a header: the arrays are the
    ones `ChainHost::SlotLevels` already publishes - each side's `hopTruePeak[]`, the true peak of
    each 100 ms block - so the dial needs no new measurement, only this reduction and a window long
    enough to trust it. The window is the one thing the contract bounds and the one thing a snapshot
    cannot give: 30 hops is 3 s and it asks for 8, so the caller accumulates PAIRS across its own
    ticks and hands them here. A figure that travels without its window cannot be checked (decision
    4 of COMPARE_REFERENCE_PLAN), so every answer carries how many blocks made it.               */
namespace echojay::clipdrive
{

/** The contract's own numbers, named rather than spelled into the arithmetic. */
inline constexpr double kLoudWithinDb   = 2.0;   ///< "within 2 dB of the loudest peak in"
inline constexpr int    kMinLoudBlocks  = 10;    ///< "at least 10 loud-peak blocks"
inline constexpr double kMinSeconds     = 8.0;   ///< "at least 8 s of playback"
inline constexpr double kHopSeconds     = 0.1;   ///< the blocks these hops are
inline constexpr double kStepDb         = 0.5;   ///< "trims move in 0.5 dB steps"
inline constexpr int    kMaxCorrections = 4;     ///< "at most 4 corrections"

/** One 100 ms block as THIS PLUGIN measures it, which is not quite where the contract puts it.

    B's contract says *"Peak in: the sample peak of the signal arriving at the slot, BEFORE the IN
    trim"*, and reasons from there that *"the two trims cancel, so anything the clipper doesn't touch
    reads 0"*. In this plugin the slot's `in` tally is pushed by the Slot Pre-Trim node AFTER it has
    applied the trim - its own comment says so: *"the in tally, from what the plugin is about to
    hear"*. So with the IN trim at +I and the clipper taking R off the peaks:

        in  (after IN)  = L + I
        out (after OUT) = L + I - R - I = L - R
        in - out        = I + R          <- NOT R

    which at his worked example (+5.5 in, 1.3 dB of clipping) would read **6.8 dB** and stop the dial
    on its first window. So the IN TRIM IS ADDED BACK here, by the dial that set it, and the field is
    named for what it actually is rather than for what the contract assumed. The alternative - a
    second tap before the trim - would be a new measurement for an arithmetic difference the dial
    already knows, and this file's own rule is one definition per quantity.

    `inTrimDb` therefore travels with the pairs: the figure cannot be computed without it, so it
    cannot be forgotten. */
struct Pair { double peakInDb = -200.0, peakOutDb = -200.0; };

struct Figure
{
    bool   enough = false;          ///< the window and the block count both satisfied
    double peakReductionDb = 0.0;   ///< the MEDIAN over the loud blocks of (in - out)
    double loudestInDb = -200.0;    ///< the loudest peak in over the stretch
    double loudThresholdDb = -200.0;///< loudestInDb - kLoudWithinDb
    int    loudBlocks = 0;          ///< how many blocks were inside that 2 dB
    int    blocks = 0;              ///< how many pairs were offered
    double seconds = 0.0;           ///< blocks * kHopSeconds, the window this figure carries
    double inTrimDb = 0.0;          ///< the trim that was taken back out (see Pair)
    juce::String why;               ///< when ! enough, what was missing, in words
};

/** THE MEDIAN, NOT THE MEAN, and the contract says median: one block where the clipper caught a
    stray transient would drag a mean, and the figure is meant to describe what the clip stage is
    doing to the loud part of the programme rather than to its worst instant. */
inline double medianOf (std::vector<double> xs)
{
    if (xs.empty()) return 0.0;
    std::sort (xs.begin(), xs.end());
    const auto n = xs.size();
    return (n % 2 == 1) ? xs[n / 2] : 0.5 * (xs[n / 2 - 1] + xs[n / 2]);
}

inline Figure figureFrom (const std::vector<Pair>& pairs, double inTrimDb)
{
    Figure f;
    f.inTrimDb = inTrimDb;
    f.blocks  = (int) pairs.size();
    f.seconds = (double) f.blocks * kHopSeconds;
    if (pairs.empty()) { f.why = "no blocks were measured across the slot"; return f; }

    for (const auto& p : pairs) f.loudestInDb = std::max (f.loudestInDb, p.peakInDb);
    f.loudThresholdDb = f.loudestInDb - kLoudWithinDb;

    std::vector<double> diffs;
    for (const auto& p : pairs)
        if (p.peakInDb >= f.loudThresholdDb)
        {
            ++f.loudBlocks;
            // THE TRIM COMES BACK OUT. See Pair: this plugin's `in` is measured after the IN
            // trim, so the raw difference carries it and the clipper's own work is what is left.
            diffs.push_back ((p.peakInDb - p.peakOutDb) - inTrimDb);
        }
    f.peakReductionDb = medianOf (diffs);

    // BOTH BOUNDS, AND THE REASON NAMES THE ONE THAT FAILED. "not enough yet" and "not enough loud
    // parts" are different facts: the first is answered by playing longer, the second by playing the
    // chorus rather than the intro, and a dial that says only "no figure" cannot tell him which.
    const bool longEnough = f.seconds >= kMinSeconds;
    const bool loudEnough = f.loudBlocks >= kMinLoudBlocks;
    f.enough = longEnough && loudEnough;
    if (! longEnough && ! loudEnough)
        f.why = "only " + juce::String (f.seconds, 1) + " s heard and only "
              + juce::String (f.loudBlocks) + " loud block(s)";
    else if (! longEnough)
        f.why = "only " + juce::String (f.seconds, 1) + " s heard, and a figure needs "
              + juce::String (kMinSeconds, 0) + " s";
    else if (! loudEnough)
        f.why = "only " + juce::String (f.loudBlocks) + " of " + juce::String (f.blocks)
              + " block(s) were within " + juce::String (kLoudWithinDb, 0)
              + " dB of the loudest, and a figure needs " + juce::String (kMinLoudBlocks);
    return f;
}

/** Rounded to the step and held inside the range, which is what the contract asks of every move of
    the IN trim. Written once because the first guess and every correction both need it, and a step
    that two sites round differently is two dials. */
inline double toStep (double db, double minDb, double maxDb)
{
    const double snapped = std::round (db / kStepDb) * kStepDb;
    return juce::jlimit (minDb, maxDb, snapped);
}

/** THE FIRST GUESS, verbatim from the contract: *"IN = ceiling + 1.25 - that level"*, where "that
    level" is the loud peaks' level arriving at the slot. His worked example: peaks arriving at -4.3
    with a ceiling of -0.1 give IN = -0.1 + 1.25 + 4.3 = +5.45, so +5.5. */
inline double firstGuessInDb (double loudestInDb, double ceilingDb, double targetReductionDb,
                              double inMinDb, double inMaxDb)
{
    return toStep (ceilingDb + targetReductionDb - loudestInDb, inMinDb, inMaxDb);
}

/** The correction: *"If it's outside target_range_db, move IN by the shortfall, OUT with it."*
    Returns the new IN. The shortfall is signed, so doing too little raises IN and doing too much
    lowers it, and the step and the range are applied by the one function above. */
inline double correctedInDb (double currentInDb, double measuredReductionDb, double targetReductionDb,
                             double inMinDb, double inMaxDb)
{
    return toStep (currentInDb + (targetReductionDb - measuredReductionDb), inMinDb, inMaxDb);
}

enum class Outcome { Working, Reached, Short, Over };

/** B's three outcomes, and the two ENDS are what make `short` and `over` different from "not there
    yet": *"short: IN is at +12 and the peak reduction is still under 1 dB … over: IN is at 0 and the
    peak reduction is still over 1.5 dB"*. So an end is part of the test, not a detail of the
    wording - a dial that reported `short` while it still had travel would be giving up early. */
inline Outcome outcomeFor (double reductionDb, double rangeLoDb, double rangeHiDb,
                           double inDb, double inMinDb, double inMaxDb, int correctionsUsed)
{
    if (reductionDb >= rangeLoDb && reductionDb <= rangeHiDb) return Outcome::Reached;
    const bool atTop    = inDb >= inMaxDb - 1.0e-6;
    const bool atBottom = inDb <= inMinDb + 1.0e-6;
    if (reductionDb < rangeLoDb && (atTop || correctionsUsed >= kMaxCorrections))    return Outcome::Short;
    if (reductionDb > rangeHiDb && (atBottom || correctionsUsed >= kMaxCorrections)) return Outcome::Over;
    return Outcome::Working;
}

inline const char* outcomeWord (Outcome o)
{
    switch (o)
    {
        case Outcome::Reached: return "reached";
        case Outcome::Short:   return "short";
        case Outcome::Over:    return "over";
        case Outcome::Working: break;
    }
    return "";
}

/** What A reports back beside the dial (the contract's own shape):
    `{ slot, name, in_db, out_db, peak_reduction_db, level_change_db, outcome }`.
    OUT IS ALWAYS MINUS IN - *"out_follows_in: true … Never one without the other"* - so it is not a
    separate input here: a caller that could pass them independently could report a pair the slot
    never held. */
inline juce::var reportVar (int slot1, const juce::String& name, double inDb,
                            double peakReductionDb, double levelChangeDb, Outcome o)
{
    auto* r = new juce::DynamicObject();
    r->setProperty ("slot", slot1);
    r->setProperty ("name", name);
    r->setProperty ("in_db",  juce::String (inDb, 1).getDoubleValue());
    r->setProperty ("out_db", juce::String (-inDb, 1).getDoubleValue());
    r->setProperty ("peak_reduction_db", juce::String (peakReductionDb, 1).getDoubleValue());
    r->setProperty ("level_change_db",   juce::String (levelChangeDb, 1).getDoubleValue());
    r->setProperty ("outcome", outcomeWord (o));
    return juce::var (r);
}

} // namespace echojay::clipdrive
