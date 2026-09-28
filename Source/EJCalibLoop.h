#pragma once

#include <JuceHeader.h>
#include <cmath>

// EJCalibLoop.h — compressor calibration by pre-gain (21t-d, 25 Sep 2026).
//
// WHY A SHARED HEADER. The loop drives whichever ChainHost owns the per-slot tallies, and that is not one
// process: while a Link's rack is leased it is V2's borrowHost, after deselect it is the Link's own chainHost,
// and on the mix bus it is V2's own. Both binaries compile THIS file, the state rides the sidecar, and a handover
// continues the loop instead of ending it.
//
// WHAT IT DOES. After a build or edit leaves a dynamics-role slot in the rack, the slot's drive (its pre-gain) is
// nudged 1 dB at a time toward a gain-reduction band, with the post-trim mirrored so the next slot's input does
// not move. GR is measured per 3 s window as slot-in LUFS minus slot-out LUFS - the figures 21s R3 already
// publishes. It stops when two consecutive windows sit in the band, after 6 steps, or when the drive reaches the
// +/-12 dB limit - and when it stops at the limit it says the band was not reached and names the figure it DID
// measure, never one it did not.
//
// WHAT IT IS NOT. It is not a timer: every decision is taken on a WINDOW, and a window that was not measured
// (dropped ring frames) is not a measurement - it cannot advance the step count. Silence is its own state, with
// its own 30 s clock, and nothing is written while it waits.
//
// TWO MODES, AND PASSIVE IS THE DEFAULT (21t-g item 2, 26 Sep 2026 ruling). The response's calibration block says
// which, and a block that says nothing means PASSIVE:
//
//   PASSIVE - the ordinary case, because by the time a user asks for a compressor the client has usually been
//   keeping figures for minutes. Nothing is asked of the user: no card, no "Listening...", no request to play
//   anything. The host measures the slot's in/out over the same windows it already measures, judges the GR band
//   by the same 1 dB / two-in-band / six-step rules, and the ONLY thing said at the end is one line, and only if
//   something actually moved. A loop that found the compressor already in band says nothing at all - there is
//   nothing to report and a report would be noise.
//
//   LISTEN - exactly what this loop has always done, cards and prompts included, entered only when the block
//   says mode "listen" (the server sends that when it has heard less than 30 s, or when the user asks to
//   calibrate).
//
// TWO ACTUATORS. DRIVE is the slot's pre-gain with the post-trim mirrored, as before. THRESHOLD dials the
// compressor's own threshold control (the name the profile uses), in the direction the profile's sense says makes
// it work harder, and leaves the drive where the staging put it. `param` may be an ARRAY - paired L/R thresholds -
// and then every one of them is stepped together to the same value, because a pair at different values is a
// different device.
namespace echojay
{

struct CalibLoop
{
    enum class State { Idle, Listening, Waiting, Adjusted, Clamped };
    enum class Mode  { Passive, Listen };            // PASSIVE unless the block says otherwise
    // 21t-j (28 Sep 2026 ruling): THE KIND DECIDES THE DIRECTION.
    //   Threshold  a threshold: LOWER is harder.
    //   Input      a named input/drive control on the plugin (the MC 77's "Input L/R"): HIGHER is harder.
    //   Drive      EchoJay's own pre-slot gain stage: higher is harder, and it is not a plugin control.
    // The 10:24:56 block called the MC 77's Input a THRESHOLD with sense "lower_is_harder", so "ease off" moved
    // the control UP - into more drive - and Sean heard the opposite of what he asked for.
    enum class Actuator { Drive, Threshold, Input };

    static constexpr float kStepDb      = 1.0f;    // one move, ruled
    static constexpr float kDriveLimit  = 12.0f;   // +/- , ruled
    static constexpr int   kMaxSteps    = 6;       // ruled
    static constexpr int   kInBandRuns  = 2;       // two CONSECUTIVE windows in the band
    static constexpr int   kAskAfter    = 2;       // 21t-i: two JUDGED windows, then it reports and asks (ruled)
    // 21t-j: what "unbounded below" means in practice. A control whose display reads "-inf" at the bottom has no
    // numeric minimum; a write still has to land somewhere, and nothing a compressor is set to lives below this.
    static constexpr float kPracticalFloorDb = -96.0f;
    static constexpr double kNoSignalMs = 30000.0; // ruled
    static constexpr float kCeilingDbTp = -3.0f;   // the input ceiling the drive may not push past (21p item 3)

    // ---- the state that rides the sidecar ----
    juce::String plugin;               // the slot's plugin name, for the card and the log
    Mode   mode        = Mode::Passive;   // the default, per the ruling: no card, no prompt, one closing line
    Actuator actuator  = Actuator::Drive;
    juce::StringArray params;          // the threshold control(s); empty for the drive actuator
    int    senseSign   = -1;           // threshold: -1 = lower is harder (the dB case), +1 = higher is harder
    float  value       = 0.0f;         // the ACTUATOR's current value in dB (the threshold, or the drive)
    float  minDb       = -60.0f;       // the profile's range for the threshold control
    float  maxDb       = 12.0f;
    int    slot        = -1;
    float  lo          = 2.0f;         // the band, from the op's gr_target_db
    float  hi          = 3.0f;
    float  preDb       = 0.0f;         // the drive as it stands
    int    steps       = 0;            // moves MADE (a window that is not a measurement never advances this)
    int    window      = 0;            // windows SEEN, for the log
    int    inBandRun   = 0;
    int    phraseIdx   = -1;           // which closing question was used last: never the same one twice running
    float  lastGr      = std::numeric_limits<float>::quiet_NaN();
    double noSignalMs  = 0.0;
    bool   awaitFresh  = false;        // a move or a handover just happened: the next window is not judged
    State  state       = State::Idle;
    bool   closingOwed = false;        // the loop ended and nobody has posted the message yet
    bool   headroomStopped = false;    // it stopped because the INPUT ran out of headroom, not the step budget
    float  headroomLimit = kDriveLimit; // the drive at which the slot input would reach -3 dBTP, last computed
    // ---- MEASURE AND ASK (21t-i, 27 Sep 2026 ruling) ---------------------------------------------------------
    // PASSIVE NO LONGER STEPS ON ITS OWN. The compressor is set ONCE, at build, from the block; after two judged
    // windows the loop REPORTS what it measured and asks. A comparative comes back as a re-targeted block and buys
    // exactly ONE step, then two more windows and the same question again. No card, no closing line, no step budget.
    float  stepDb       = kStepDb;     // the block's "step" for a threshold; the drive always moves 1 dB
    int    judged       = 0;           // judged windows since the last report or step
    bool   asked        = false;       // the question has been posted for the current figure
    bool   noSignalSaid = false;       // "play it and I'll tell you what it's doing" has been said once
    int    pendingStep  = 0;           // +1 harder / -1 softer, set by a comparative, spent on the next window
    // WHAT THE QUESTION QUOTES: the BLOCK's heard_s, not the slot tally's. The slot's heard time is the age of
    // that slot's own measurement (it starts when the plugin is inserted), so quoting it would tell the user the
    // compressor was set from three seconds of the track when the server set it from two minutes of it.
    float  blockHeardS  = std::numeric_limits<float>::quiet_NaN();
    bool   fromWorking  = false;       // source "working_position": there is no heard time to quote
    float  slotHeardS   = 0.0f;        // the slot tally's own heard time. Logged, NEVER quoted in the sentence.
    int    stepsTaken   = 0;           // comparatives honoured, for the log
    juce::String askOwed;              // the one chat line this loop owes, handed out exactly once
    // ---- 21t-j (28 Sep 2026 rulings) -------------------------------------------------------------------------
    // freshWanted   windows with SIGNAL still owed before the next ask. Three after any write to the actuator:
    //               the 10:26:42 ask quoted a figure 40 s older than the edit it was answering.
    // lastHeardS    the slot input's heard time at the last judged window. A window whose heard time has NOT
    //               advanced is not a sample - it is the same 3 s window read twice, which is how gr sat at 5.6
    //               for 190 windows through transport stops and a 2.8 dB actuator move.
    int    freshWanted = 0;
    float  lastHeardS  = -1.0f;
    // ---- THE SETTLE IS THE TAIL OF THE BUILD (28 Sep 2026 ruling) -------------------------------------------
    // One build, one chat line. The build is not complete until the slot has heard the vocal and landed, and the
    // SAME line completes in place. No ask state, no "check it", no timer: a build waits for audio as long as it
    // takes, HEARD time counts rather than clock time, and a stop pauses the settle while play resumes it. At most
    // three steps within 15 s of heard audio; after landing nothing moves except on a comparative.
    bool   settling   = false;
    bool   landed     = false;
    int    settleSteps = 0;
    float  settleHeardS = 0.0f;
    float  settleStartHeardS = -1.0f;
    static constexpr int   kSettleMaxSteps  = 3;      // ruled
    static constexpr float kSettleMaxHeardS = 15.0f;  // ruled, in HEARD seconds
    // The output/makeup control, when the block named one, and what the loop last wrote to it.
    juce::StringArray senseParams;     // the plugin's own GR meter, when the block named one
    bool   grReadable = false;         // ...and whether the block says it reads as dB
    float  sensedGrDb = std::numeric_limits<float>::quiet_NaN();   // what that meter last read, positive dB
    // 21t-j (owed): THE GR-METER CROSS-CHECK. A block can SAY a control reads as dB; whether it does is a
    // question about the plugin, and the only honest answer is the control's own text beside its raw position.
    // Five windows of it after a build, on whichever host owns the rack, and then it stops - this is a check, not
    // a running commentary. `kSenseLogWindows` is the budget; senseLogsOwed is what is left of it.
    static constexpr int kSenseLogWindows = 5;   // ruled: five windows, then quiet
    int    senseLogsOwed = 0;
    juce::StringArray outParams;
    float  outValue = std::numeric_limits<float>::quiet_NaN();
    float  outMin = -24.0f, outMax = 0.0f;
    float  levelChangeDb = 0.0f;       // what the slot last measured itself adding (out - in)
    float  slotGainDb = 0.0f;          // EchoJay's own per-slot output gain, where the hold goes with no control
    float  levelTrimmedDb = 0.0f;      // how much this loop has taken off the output to hold the level
    bool   levelHeld = false;          // the hold has been applied for the current actuator position
    static constexpr int kFreshAfterWrite = 3;   // ruled

    // ---- one 3 s window, as the host measured it ----
    struct Window
    {
        bool  measured = false;   // a full window with no dropped frames
        bool  silent   = false;   // below the gate: nothing was playing
        // 21t-j (28 Sep 2026, the general compressor rule): TWO SENSORS, BOTH FROM THE SLOT'S OWN TAPS, and no
        // product knowledge in either.
        //   grDb           GAIN REDUCTION as a CREST DIFFERENCE:
        //                    (in SHORTMAX - in SHORT90) - (out SHORTMAX - out SHORT90)
        //                  A compressor pulls the loud phrases down toward where the programme sits, so the crest
        //                  shrinks; makeup gain moves both output terms together and cancels. It needs no meter
        //                  parameter and works on any plugin.
        //   levelChangeDb  THE LEVEL CHANGE: out minus in on SHORT90 (INT when SHORT90 has not closed). It drives
        //                  the hold, and it is NOT gain reduction - reading it as one is what reported a vocal
        //                  getting 7.6 dB LOUDER as "-7.6 dB of gain reduction".
        float grDb     = 0.0f;
        float levelChangeDb = 0.0f;
        // 21t-d: the slot's INPUT true peak, as measured at the drive this window ran at. The drive limit is not
        // a fixed +12: it is whatever drive brings this figure to -3 dBTP, because past that the loop would be
        // buying gain reduction with a clipped input. -200 = the host could not read it (then +12 alone applies).
        float inTruePeakDb = -200.0f;
        // 21t-i: what the host has heard on this slot so far, in seconds - the figure the question quotes.
        float heardSeconds = 0.0f;
        // 21t-j: the plugin's OWN gain-reduction meter, in dB and POSITIVE, when the block named one and the host
        // could read it. NaN = the plugin published nothing, and then the reply carries no GR figure at all.
        float sensedGrDb = std::numeric_limits<float>::quiet_NaN();
    };

    // ---- what the host should do about it ----
    struct Step
    {
        bool  writeDrive = false;   // set the slot's pre-gain to newPre and its post-trim to -newPre
        float newPre     = 0.0f;
        float newPost    = 0.0f;
        // 21t-g: the THRESHOLD actuator. Every name in paramNames goes to paramValue together - a paired L/R
        // threshold at two different values is a different device, so they move as one.
        bool  writeParams = false;
        juce::StringArray paramNames;
        float paramValue  = 0.0f;
        juce::String card;          // what the card says now
        juce::String logLine;       // EJThreshold: ... , one per window
        bool  finished   = false;   // the loop ended on this window
        juce::String closing;       // the closing message, when it ended (LISTEN mode only, since 21t-i)
        // 21t-i: the measure-and-ask line. ONE chat line, posted as it stands - the report of what was measured and
        // the question that invites a comparative. Empty on every window that is not a reporting one.
        juce::String ask;
        // 21t-j (a): TRUE when this line REPLACES the opening one in place, rather than being a new message. The
        // transcript carries the line's current text only - the opening line while pending, the completed line
        // once landed, never both.
        bool  askReplacesOpening = false;
        // 21t-j: the level hold. A second write, to a DIFFERENT control from the actuator, and reported in words.
        bool  writeOutput = false;
        juce::StringArray outputNames;
        float outputValue = 0.0f;
        // 21t-j: the hold through EchoJay's own per-slot output gain, for a plugin that publishes no output control.
        bool  writeSlotGain = false;
        float slotGainValue = 0.0f;
    };

    /** Everything the response's calibration block can say. Defaults are the ruled defaults: PASSIVE, the drive
        actuator, and a threshold range wide enough to be no constraint until a profile narrows it. */
    struct Config
    {
        juce::String plugin;
        int    slot = -1;
        float  lo = 2.0f, hi = 3.0f;      // gr_target_db
        Mode   mode = Mode::Passive;
        Actuator actuator = Actuator::Drive;
        juce::StringArray params;         // param, or every entry of a param array
        int    senseSign = -1;            // "lower_is_harder" (the dB threshold case) unless told otherwise
        float  startDb = 0.0f;            // the actuator's opening value: the drive, or the threshold
        float  minDb = -60.0f, maxDb = 12.0f;
        float  stepDb = kStepDb;          // the block's "step"; 1 dB unless it says otherwise
        // 21t-i re-cut (27 Sep 2026 ruling): WHAT THE SENTENCE QUOTES COMES FROM THE BLOCK.
        //   heardS  the block's own "heard_s" - how much of THIS TRACK the server's figures describe. NaN = absent.
        //   working true when source is "working_position": there is no heard time to quote, because nothing was
        //           measured; the compressor was set from where it already sat.
        float  heardS = std::numeric_limits<float>::quiet_NaN();
        bool   working = false;
        // A block may carry a NUDGE instead of (or beside) a band: the user said "ease off" or "more" and the
        // server passes that through as a direction. 0 = absent.
        int    nudge = 0;
        // ...and it may carry no band at all, in which case a re-target keeps the one the loop already has. A
        // default band silently replacing a ruled one is a different target, not a missing field.
        bool   haveBand = false;
        // ---- 21t-j (28 Sep 2026 ruling): A COMPRESSOR BUILD IS LEVEL-NEUTRAL --------------------------------
        // The MC 77 build left the vocal a lot louder with Output untouched, because nothing was holding the
        // level: the slot was adding 7.6 dB and the loop called that "gain reduction". To hold it the client has
        // to write the plugin's OWN output/makeup, and it cannot guess which control that is - the block names it
        // (`output_param`, a string or an array, with `output_min_db` / `output_max_db`). Without it the loop
        // MEASURES the level change and says it cannot hold it, which is the honest half.
        // 21t-j (28 Sep 2026, B's contract note 1): THE GR SENSOR. The block names the plugin's own gain-reduction
        // meter (`sense_param`, with `gr_readable` true) and THAT is the gain reduction. The slot's in-vs-out pair
        // is a LEVEL CHANGE and is kept for the level hold, which is what it is right for.
        juce::StringArray senseParams;
        bool   grReadable = false;
        juce::StringArray outputParams;
        float  outputStartDb = std::numeric_limits<float>::quiet_NaN();
        float  outputMinDb = -24.0f, outputMaxDb = 0.0f;
    };

    /** THE WIRE SHAPE, PARSED ONCE (21t-g item 6b, 26 Sep 2026). Both binaries compile this file, so the block is
        read HERE and nowhere else - a shape parsed in two places is two shapes, and the two would drift the first
        time the server added a field.

        THE LITERALS, verbatim from the server (CONTRACT_GROUPS):
          mode      "passive" | "listen"                       - never null; anything else is a violation
          actuator  "threshold" | "drive"                      - never null; anything else is a violation
          sense     "lower_is_harder" | "higher_is_harder" | null
          source    "tally" | "listen"                         - carried for the log, not acted on
          measure   "short90" | "shortmax" | "int" | null      - carried for the log, not acted on
          param     a string for one knob, an ARRAY for a pair; start_db applies to every entry
          start_db  a number, or null (the server heard under 30 s and set nothing)
          min_db / max_db / gr_target_db: numbers

        A THRESHOLD WITH A NULL SENSE IS A CONTRACT VIOLATION and is NOT dialled: which way a knob makes a
        compressor work harder is not something to assume from its name. It runs the DRIVE instead and says so.
        Every rejection is written into `whyOut` so the caller can log it: a block quietly reinterpreted is a
        block nobody can debug.
        Returns false when there is no usable block at all (no object, or a slot this rack does not have). */
    static bool configFromBlock (const juce::var& block, int numSlots, bool busBand,
                                 const juce::String& pluginName, Config& out, juce::String& whyOut)
    {
        auto* o = block.getDynamicObject();
        if (o == nullptr) return false;                       // no compressor in this chain: no block
        // THE WIRE IS 1-BASED and this converts; the REJECTION PRINTS THE WIRE VALUE VERBATIM (21t-h, 27 Sep
        // 2026). The first cut printed the converted number, so a 0-based sender - which is what the server was
        // doing on 27 Sep - produced "slot 0 is not in this rack", a line that reads like an empty rack and hides
        // the base mismatch completely. Printing both numbers makes it a glance: "wire slot 0, rack has 1 slot(s)".
        const bool haveSlot = o->hasProperty ("slot");
        const int wireSlot = haveSlot ? (int) o->getProperty ("slot") : -1;
        const int slot = haveSlot ? wireSlot - 1 : -1;                        // 1-based on the wire
        if (slot < 0 || slot >= numSlots)
        {
            whyOut << "wire slot " << (haveSlot ? juce::String (wireSlot) : juce::String ("(absent)"))
                   << " (1-based, so slot index " << juce::String (slot) << "), rack has "
                   << juce::String (numSlots) << " slot(s); nothing started. ";
            return false;
        }
        out = Config{};
        out.slot = slot;
        out.plugin = pluginName;
        out.lo = busBand ? 1.0f : 2.0f; out.hi = busBand ? 2.0f : 3.0f;
        if (auto* band = o->getProperty ("gr_target_db").getArray())
            if (band->size() == 2)
            {
                out.lo = (float) (double) band->getUnchecked (0); out.hi = (float) (double) band->getUnchecked (1);
                out.haveBand = true;   // 21t-i re-cut: a re-target with no band keeps the loop's current one
            }

        // ---- mode: exactly two strings --------------------------------------------------------------------
        const auto modeS = o->getProperty ("mode").toString().trim();
        if (modeS == "listen")        out.mode = Mode::Listen;
        else if (modeS == "passive")  out.mode = Mode::Passive;
        else
        {
            out.mode = Mode::Passive;      // the quiet default, per the ruling: a block that says nothing is passive
            whyOut << "mode \"" << modeS << "\" is not \"passive\" or \"listen\" - running PASSIVE. ";
        }

        // ---- actuator: the KIND, and the kind decides the direction (21t-j, 28 Sep 2026 ruling) -----------
        const auto actS = o->getProperty ("actuator").toString().trim();
        if (actS == "threshold")   out.actuator = Actuator::Threshold;
        else if (actS == "input")  out.actuator = Actuator::Input;     // a named drive control on the plugin
        else if (actS == "drive")  out.actuator = Actuator::Drive;     // EchoJay's own pre-slot gain stage
        else
        {
            out.actuator = Actuator::Drive;
            whyOut << "actuator \"" << actS << "\" is not \"threshold\", \"input\" or \"drive\" - running the DRIVE. ";
        }

        // ---- param: one name, or an array of them (a pair moves together) ---------------------------------
        {
            const auto pv = o->getProperty ("param");
            if (auto* pa = pv.getArray())
                for (const auto& e : *pa) { const auto n = e.toString().trim(); if (n.isNotEmpty()) out.params.add (n); }
            else if (pv.toString().trim().isNotEmpty()) out.params.add (pv.toString().trim());
        }

        // ---- sense: FROM THE KIND (21t-j, 28 Sep 2026 ruling), never from the wire's opinion --------------
        // The 10:24:56 block declared the MC 77's "Input L/R" as actuator "threshold" with sense
        // "lower_is_harder", so "ease off" moved the control UP - into MORE input, harder compression - and the
        // user heard the opposite of what he asked for. A threshold is lower-is-harder; an input or a drive is
        // higher-is-harder; that follows from what the control IS, and a sense field that disagrees with the kind
        // is a contradiction the client names and does not follow.
        const auto senseS = o->getProperty ("sense").toString().trim();
        const bool senseKnown = (senseS == "lower_is_harder" || senseS == "higher_is_harder");
        out.senseSign = senseForActuator (out.actuator);
        if (senseKnown)
        {
            const int wireSense = (senseS == "higher_is_harder") ? 1 : -1;
            if (wireSense != out.senseSign)
                whyOut << "sense \"" << senseS << "\" contradicts actuator \"" << actS << "\" ("
                       << (out.senseSign < 0 ? "lower" : "higher") << " is harder for that kind) - following the "
                       << "KIND, as ruled. ";
        }
        if (out.actuator == Actuator::Threshold && ! senseKnown)
            whyOut << "actuator \"threshold\" carries no sense - taking lower-is-harder from the kind. ";
        if (out.actuator != Actuator::Drive && out.params.isEmpty())
        {
            out.actuator = Actuator::Drive;
            whyOut << "actuator \"" << actS << "\" names no param - running the DRIVE. ";
        }

        // ---- start_db: a number, or null (nothing is written) ---------------------------------------------
        // start_db: a number, or null. NULL IS "UNSET", NEVER ZERO (21t-g, 26 Sep 2026 ruling) - for either
        // actuator. A drive block with start_db null is an unprofiled compressor whose staging pre-gain the server
        // has already written on the slot: the loop must OPEN FROM THAT and step from there, because opening from 0
        // would undo the staging in one move and then spend its six steps climbing back. NaN out means "ask the
        // host what is on the slot", and the caller does exactly that.
        const auto startV = o->getProperty ("start_db");
        const bool haveStart = ! startV.isVoid() && (startV.isDouble() || startV.isInt() || startV.isInt64());
        out.startDb = haveStart ? (float) (double) startV : std::numeric_limits<float>::quiet_NaN();
        // ---- min_db / max_db, and what NULL means (21t-j, 28 Sep 2026, B's contract note) -------------------
        // B now sends min_db null for a control whose low end prints "-inf" (the MC 77's Input reads "-inf dB" at
        // normalised 0). TODAY'S PARSER WOULD READ THAT AS 0.0: juce::var(null) converts to 0.0, so the range
        // became [0, max] and every target below zero clamped to zero - on a control whose useful travel is all
        // below zero. Null now means UNBOUNDED BELOW, and the practical floor is stated rather than infinite: a
        // write has to land somewhere, and -96 dB is below anything a compressor control is set to.
        {
            const auto mn = o->getProperty ("min_db");
            if (o->hasProperty ("min_db"))
                out.minDb = mn.isVoid() ? kPracticalFloorDb : (float) (double) mn;
            const auto mx = o->getProperty ("max_db");
            if (o->hasProperty ("max_db"))
                out.maxDb = mx.isVoid() ? 0.0f : (float) (double) mx;
            if (mn.isVoid() && o->hasProperty ("min_db"))
                whyOut << "min_db null (the low end prints \"-inf\") - taking " << juce::String (kPracticalFloorDb, 0)
                       << " dB as the practical floor. ";
        }

        // ---- nudge: the user's comparative, passed through (21t-i re-cut, 27 Sep 2026 ruling) --------------
        // "harder" and "softer" are the two literals. On a re-target this decides the direction on its own, with
        // or without a band, because it IS what the user said - the band moving is only the server's way of saying
        // the same thing. Absent means no nudge; anything else is logged and ignored, never guessed at.
        {
            const auto nudgeS = o->getProperty ("nudge").toString().trim();
            if (nudgeS == "harder")      out.nudge = 1;
            else if (nudgeS == "softer") out.nudge = -1;
            else if (nudgeS.isNotEmpty())
                whyOut << "nudge \"" << nudgeS << "\" is not \"harder\" or \"softer\" - ignored. ";
        }

        // ---- heard_s and source: WHAT THE QUESTION QUOTES --------------------------------------------------
        // heard_s is how much of the track the server's figures describe, and it is the number the measure-and-ask
        // sentence says out loud. The slot tally's own heard time is a different quantity (the age of that slot's
        // measurement) and quoting it would understate the setting's basis by two orders of magnitude on a fresh
        // insert. source "working_position" means nothing was measured at all, and the sentence says so instead.
        {
            const auto hv = o->getProperty ("heard_s");
            if (! hv.isVoid() && (hv.isDouble() || hv.isInt() || hv.isInt64()))
            {
                const float h = (float) (double) hv;
                if (h >= 0.0f) out.heardS = h;
                else whyOut << "heard_s " << juce::String (h, 1) << " is negative - ignored. ";
            }
            out.working = o->getProperty ("source").toString().trim() == "working_position";
        }

        // ---- sense_param: the plugin's OWN gain-reduction meter (21t-j, B's note 1) ------------------------
        {
            const auto sv = o->getProperty ("sense_param");
            if (auto* sa = sv.getArray())
                for (const auto& e : *sa) { const auto n = e.toString().trim(); if (n.isNotEmpty()) out.senseParams.add (n); }
            else if (sv.toString().trim().isNotEmpty()) out.senseParams.add (sv.toString().trim());
            out.grReadable = (bool) o->getProperty ("gr_readable");
            if (! out.senseParams.isEmpty() && ! out.grReadable)
                whyOut << "sense_param named without gr_readable - the meter will be read but no GR figure is "
                          "reported, because the plugin has not published one in dB. ";
        }

        // ---- output_param: the control that holds the level (21t-j, 28 Sep 2026 ruling) --------------------
        {
            const auto ov = o->getProperty ("output_param");
            if (auto* oa = ov.getArray())
                for (const auto& e : *oa) { const auto n = e.toString().trim(); if (n.isNotEmpty()) out.outputParams.add (n); }
            else if (ov.toString().trim().isNotEmpty()) out.outputParams.add (ov.toString().trim());
            const auto osv = o->getProperty ("output_start_db");
            if (! osv.isVoid() && (osv.isDouble() || osv.isInt() || osv.isInt64()))
                out.outputStartDb = (float) (double) osv;
            if (o->hasProperty ("output_min_db"))
            {
                const auto omn = o->getProperty ("output_min_db");
                out.outputMinDb = omn.isVoid() ? kPracticalFloorDb : (float) (double) omn;
            }
            if (o->hasProperty ("output_max_db"))
            {
                const auto omx = o->getProperty ("output_max_db");
                out.outputMaxDb = omx.isVoid() ? 0.0f : (float) (double) omx;
            }
            // NO NOTE when the block names no output control: since 21t-j the hold falls back to EchoJay's own
            // per-slot output gain, so this is a choice of actuator, not a deficiency. Which one was used is
            // logged at the moment the hold is written, where it can be checked against the audio.
        }

        // ---- step: how far ONE comparative moves the knob (21t-i, 27 Sep 2026 ruling) ----------------------
        // The block says how big a step this control takes; a threshold in 3 dB detents cannot be nudged by 1 dB.
        // Absent, null or nonsensical means 1 dB, which is what every block before this round meant. The DRIVE
        // always moves 1 dB - it is our own gain stage, not the plugin's control, and the ruling fixes it there.
        {
            const auto stepV = o->getProperty ("step");
            const bool haveStep = ! stepV.isVoid() && (stepV.isDouble() || stepV.isInt() || stepV.isInt64());
            const float st = haveStep ? std::abs ((float) (double) stepV) : 0.0f;
            if (haveStep && st > 0.0f) out.stepDb = st;
            else if (haveStep) whyOut << "step \"" << stepV.toString() << "\" is not a usable size - stepping 1 dB. ";
        }

        // ---- source and measure: logged, not acted on, and their literals are named so a new one shows ----
        {
            const auto src = o->getProperty ("source").toString().trim();
            const auto mea = o->getProperty ("measure").toString().trim();
            // THE FOUR `source` VALUES, and what each one means about where start_db came from (27 Sep 2026):
            //   "tally"             the channel's kept figures - the ordinary case, and why the mode is passive
            //   "measured"          the turn's own meter reading (added 27 Sep, after 21t-h was packaged)
            //   "working_position"  the slot's working position, written rather than measured
            //   "listen"            an ASKED calibration, and only that
            // The field is LOGGED, not acted on: the loop's behaviour comes from `mode` and `actuator`. It is
            // accepted by name anyway, because a legitimate value flagged as "NOT AS CONTRACTED" trains the reader
            // to ignore that line - and that line is how a real contract break gets noticed.
            if (src.isNotEmpty() && src != "tally" && src != "measured"
                && src != "working_position" && src != "listen")
                whyOut << "source \"" << src << "\" is not \"tally\" or \"listen\". ";
            if (mea.isNotEmpty() && mea != "short90" && mea != "shortmax" && mea != "int")
                whyOut << "measure \"" << mea << "\" is not short90/shortmax/int. ";
        }
        return true;
    }

    void begin (const Config& c)
    {
        plugin = c.plugin; slot = c.slot;
        lo = juce::jmin (c.lo, c.hi); hi = juce::jmax (c.lo, c.hi);
        mode = c.mode; actuator = c.actuator; params = c.params; senseSign = c.senseSign < 0 ? -1 : 1;
        minDb = juce::jmin (c.minDb, c.maxDb); maxDb = juce::jmax (c.minDb, c.maxDb);
        stepDb = (c.stepDb > 0.0f) ? c.stepDb : kStepDb;
        judged = 0; asked = false; noSignalSaid = false; pendingStep = 0; slotHeardS = 0.0f; stepsTaken = 0;
        askOwed.clear();
        freshWanted = 0; lastHeardS = -1.0f;
        senseParams = c.senseParams; grReadable = c.grReadable;
        sensedGrDb = std::numeric_limits<float>::quiet_NaN();
        outParams = c.outputParams; outValue = c.outputStartDb;
        outMin = juce::jmin (c.outputMinDb, c.outputMaxDb); outMax = juce::jmax (c.outputMinDb, c.outputMaxDb);
        levelChangeDb = 0.0f; levelTrimmedDb = 0.0f; levelHeld = false; slotGainDb = 0.0f;
        blockHeardS = c.heardS; fromWorking = c.working;
        // THE BUILD OPENS THE SETTLE (28 Sep 2026 ruling) and says so in ONE line, which the completion edits in
        // place. No ask state, no timer: the build is not complete until the slot has heard the vocal and landed.
        settling = true; landed = false; settleSteps = 0; settleHeardS = 0.0f; settleStartHeardS = -1.0f;
        senseLogsOwed = senseParams.isEmpty() ? 0 : kSenseLogWindows;   // 21t-j: the cross-check, five windows
        askOwed = openingLine();
        // ONE current value, whichever knob is being dialled: the drive keeps preDb (the mirror needs it), the
        // threshold keeps value. Both are set so a log line and a closing sentence can be written either way.
        value = c.startDb;
        preDb = (actuator == Actuator::Drive) ? c.startDb : 0.0f;   // a named control leaves the staging alone
        steps = 0; window = 0; inBandRun = 0; noSignalMs = 0.0;
        lastGr = std::numeric_limits<float>::quiet_NaN();
        awaitFresh = false; closingOwed = false; headroomStopped = false;
        headroomLimit = kDriveLimit;
        state = State::Listening;
    }

    /** The pre-21t-g entry point: a LISTEN pass on the drive, which is what every existing caller meant. */
    void begin (const juce::String& pluginName, int slotIndex, float bandLo, float bandHi, float openingDrive)
    {
        plugin = pluginName; slot = slotIndex;
        lo = juce::jmin (bandLo, bandHi); hi = juce::jmax (bandLo, bandHi);
        mode = Mode::Listen; actuator = Actuator::Drive; params.clear(); senseSign = -1;
        minDb = -60.0f; maxDb = 12.0f;
        preDb = openingDrive; value = openingDrive;
        steps = 0; window = 0; inBandRun = 0; noSignalMs = 0.0;
        lastGr = std::numeric_limits<float>::quiet_NaN();
        awaitFresh = false; closingOwed = false; headroomStopped = false;
        headroomLimit = kDriveLimit;
        stepDb = kStepDb; judged = 0; asked = false; noSignalSaid = false;
        pendingStep = 0; slotHeardS = 0.0f; stepsTaken = 0; askOwed.clear();
        freshWanted = 0; lastHeardS = -1.0f;
        settling = false; landed = false; settleSteps = 0; settleHeardS = 0.0f; settleStartHeardS = -1.0f;
        senseLogsOwed = 0;
        blockHeardS = std::numeric_limits<float>::quiet_NaN(); fromWorking = false;
        state = State::Listening;
    }

    /** 21t-j (28 Sep 2026 ruling): AN ACCEPTED WRITE TO THE ACTUATOR IS THE LOOP'S POSITION. At 10:26:32.931 a
        chain_edit landed Input L/R = -24.0 dB and 0.67 s later this loop wrote -21.20, because it stepped from the
        -22.2 IT was holding. Whatever moves the actuator - an edit, a build, a user - tells the loop, and the loop
        steps from there or not at all.

        Returns true when the position actually changed, so a caller can log a move it did not make itself. */
    bool actuatorWrittenTo (float newValue)
    {
        const float was = writesNamedParam() ? value : preDb;
        if (std::abs (was - newValue) < 1.0e-4f) return false;
        if (writesNamedParam()) value = newValue;
        else                  { preDb = newValue; value = newValue; }
        // A window that straddles the move is a window about two settings, and the next ask waits for three that
        // do not.
        awaitFresh = true;
        freshWanted = kFreshAfterWrite;
        judged = 0; asked = false;
        levelHeld = false;          // a new actuator position owes a new level hold (ruled)
        return true;
    }

    /** A NEW target while the loop is running restarts it FROM THE CURRENT DRIVE (ruled): the band changed, the
        drive it has already found has not.

        21t-i: in MEASURE AND ASK this is the ONLY thing that moves the knob. The user said "ease off" or "more",
        the server moved the band, and that re-targeted block buys EXACTLY ONE step in the direction the band
        moved - threshold by the block's "step", drive by 1 dB. A block whose band did not move buys no step: a
        write with no comparative behind it is the automatic stepping this round retired. */
    void retarget (float bandLo, float bandHi, float newStepDb = 0.0f, int nudge = 0, bool haveBand = true,
                   bool blockCarriedStart = false)
    {
        const float wasMid = 0.5f * (lo + hi);
        // A BLOCK WITH NO BAND KEEPS THE ONE THE LOOP HAS (21t-i re-cut). The server may send only a nudge; a
        // default band substituted here would be a different target arriving as a missing field.
        if (haveBand) { lo = juce::jmin (bandLo, bandHi); hi = juce::jmax (bandLo, bandHi); }
        if (newStepDb > 0.0f) stepDb = newStepDb;
        steps = 0; inBandRun = 0; awaitFresh = true; closingOwed = false;
        state = State::Listening;
        if (mode != Mode::Passive) return;
        const float nowMid = 0.5f * (lo + hi);
        // THE NUDGE WINS WHERE THERE IS ONE (ruled): "ease off" and "more" are the user's words passed straight
        // through, and they mean the same thing whether or not the server also moved the band. Without one, the
        // band's movement is the comparative, as it has been.
        pendingStep = (nudge > 0) ? 1
                    : (nudge < 0) ? -1
                    : (nowMid > wasMid + 1.0e-3f) ? 1
                    : (nowMid < wasMid - 1.0e-3f) ? -1 : 0;
        // 21t-j (ruled): A BLOCK CARRYING start_db IS THE STEP. The server did the move; the loop owes nothing
        // further, or the user gets two moves for one word - which is exactly what -24.0 then -21.2 was.
        if (blockCarriedStart) pendingStep = 0;
        judged = 0; asked = false;
        // A COMPARATIVE REOPENS THE LINE (ruled): the user asked for a change, so the same shape runs again - the
        // step, then the completed line in place. A chain_edit carrying start_db is the user's own move and starts
        // no settle: pendingStep is already 0 there, and landing is immediate on the next judged window.
        // THE COMPARATIVE'S OWN STEP IS THE SETTLE'S STEP. The budget opens SPENT, so the next judged window
        // after the move lands and completes the line in place; leaving a step in the budget would give the user
        // two moves for one word, which is the -24.0-then-21.2 fault this round closed.
        if (pendingStep != 0) { landed = false; settling = true; settleSteps = kSettleMaxSteps; settleHeardS = 0.0f; settleStartHeardS = -1.0f; }
        // AND THE NEXT WINDOW IS JUDGED. awaitFresh exists because a window that straddles a KNOB MOVE is a window
        // about two settings; a re-target on its own has moved nothing yet, so skipping a window here would only
        // make the user wait another three seconds for the answer to "more".
        if (pendingStep != 0) awaitFresh = false;
    }

    /** The rack moved to the other host. The step count is UNCHANGED - the work already done is not undone by
        who is doing it - and the first window on the new host is not judged, because it is that host's first. */
    juce::String onHandover()
    {
        awaitFresh = true;
        inBandRun  = 0;   // a run of in-band windows cannot span two different tallies
        return log ("handover");
    }

    bool running() const { return state == State::Listening || state == State::Waiting; }
    /** 21t-j (28 Sep 2026 ruling): THE PENDING BUILD IS CANCELLED by any user or chat edit to that slot, or by the
        plugin being removed or replaced. The line closes with the setting AS IT STANDS - it does not vanish, and it
        does not keep promising to land something that nobody is landing any more. */
    juce::String cancelSettle (const juce::String& why)
    {
        if (! settling || landed) return {};
        settling = false; landed = true; asked = true;
        askOwed = completedLine();
        return log (("cancelled: " + why).toRawUTF8());
    }

    /** True when this loop moves a NAMED control on the plugin rather than EchoJay's own gain stage. */
    bool writesNamedParam() const { return actuator == Actuator::Threshold || actuator == Actuator::Input; }
    /** The sense, from the KIND and nothing else (ruled 28 Sep 2026): -1 = lower is harder, +1 = higher. */
    static int senseForActuator (Actuator a) { return a == Actuator::Threshold ? -1 : 1; }

    /** THE CROSS-CHECK LINE (21t-j, owed to the 28 Sep ruling). One line per window, five of them, carrying the
        control's RAW position, the text the plugin prints for it, and whether that text parsed as dB at all.
        Composed here so both hosts write the SAME line and a guard can assert its shape without a plugin.

        Why the raw position is on it: "GR Meter L" at 0.41 printing "4.2 dB" is a meter reading in dB; the same
        control printing "0.41" is a normalised position that a dB parser will happily read as 0.41 dB, and the
        reply would then quote four tenths of a dB of gain reduction for a compressor doing four. The two figures
        side by side are what tells them apart, and nothing here guesses: it prints what it found. */
    static juce::String senseCrossCheckLine (const juce::String& plugin, const juce::String& control,
                                             float raw, const juce::String& text, bool parsedOk, float parsedDb,
                                             int windowNumber)
    {
        juce::String s;
        s << "EJGrMeter: " << plugin << " \"" << control << "\" window " << juce::String (windowNumber)
          << "/" << juce::String (kSenseLogWindows)
          << " raw=" << juce::String (raw, 4)
          << " text=\"" << text.trim() << "\""
          << " parsed=" << (parsedOk ? juce::String (parsedDb, 2) + " dB" : juce::String ("NO"))
          << " prints_db=" << (parsedOk ? "yes" : "no");
        // THE COINCIDENCE IS WORTH WRITING DOWN, and it is an OBSERVATION, not a guess: the dB parser will read
        // a bare normalised position ("0.41") as 0.41 dB perfectly happily, and then a compressor doing four dB
        // would be reported as doing four tenths. When the parsed figure IS the raw position, the line says so
        // and leaves the conclusion to whoever reads it.
        if (parsedOk && raw >= 0.0f && raw <= 1.0f && std::abs ((float) parsedDb - raw) < 0.01f)
            s << " NOTE: the parsed figure equals the raw position - this control may not be printing dB";
        return s;
    }

    /** Spends one of the five, and says whether the caller should write the line. */
    bool takeSenseLog (int& windowNumberOut)
    {
        if (senseLogsOwed <= 0) return false;
        windowNumberOut = kSenseLogWindows - senseLogsOwed + 1;
        --senseLogsOwed;
        return true;
    }

    Step onWindow (const Window& w, double windowMs)
    {
        Step s;
        ++window;
        if (! running()) { s.card = card(); return s; }
        if (w.heardSeconds > 0.0f) slotHeardS = w.heardSeconds;
        if (w.sensedGrDb == w.sensedGrDb) sensedGrDb = std::abs (w.sensedGrDb);   // positive, always (ruled)

        // (1) NOT A MEASUREMENT. Dropped frames mean the host did not see a whole window; that is not evidence of
        // anything and cannot move the drive or the step count. It counts toward the no-signal clock ONLY if it
        // was also silent - "nothing arrived" and "nothing was playing" are different facts.
        if (! w.measured)
        {
            if (w.silent) noSignalMs += windowMs;
            if (noSignalMs >= kNoSignalMs) { state = State::Waiting; askNoSignal (s); }
            s.card = card(); s.logLine = log (w.silent ? "waiting" : "listening");
            return s;
        }
        if (w.silent)
        {
            noSignalMs += windowMs;
            if (noSignalMs >= kNoSignalMs) { state = State::Waiting; askNoSignal (s); }
            s.card = card(); s.logLine = log ("waiting");
            return s;
        }

        // 21t-j (28 Sep 2026 ruling): A WINDOW WITH NO SIGNAL IS NOT A SAMPLE - and neither is the SAME window
        // read twice. The slot tallies' `known` flag means "has heard 3 s at some point" and never goes false, so
        // once audio stopped the pair kept answering with its last closed window: gr read 5.6 for 190 windows,
        // through transport stops, silence and a 2.8 dB actuator change, and the ask quoted it 40 s later. The
        // heard time is the clock: if it has not advanced since the last judged window, nothing new was heard.
        if (w.heardSeconds > 0.0f && lastHeardS >= 0.0f && w.heardSeconds <= lastHeardS + 1.0e-3f)
        {
            s.card = card(); s.logLine = log ("stale-window");
            return s;
        }
        if (w.heardSeconds > 0.0f) lastHeardS = w.heardSeconds;
        // Signal is back: the wait ends where it started, with nothing changed while it waited.
        noSignalMs = 0.0;
        if (state == State::Waiting) state = State::Listening;
        lastGr = w.grDb;

        // (2) THE FRESH WINDOW after a move or a handover is seen, logged and NOT judged: it may straddle the
        // change, and a decision taken on it would be a decision about two different racks.
        if (awaitFresh) { awaitFresh = false; s.card = card(); s.logLine = log ("listening"); return s; }
        // THREE FRESH WINDOWS AFTER ANY WRITE (ruled): the figure the ask quotes must describe the setting the ask
        // is about. One window is not enough on a compressor whose release spans seconds.
        if (freshWanted > 0)
        {
            --freshWanted;
            s.card = card(); s.logLine = log (freshWanted > 0 ? "settling" : "settled");
            return s;
        }

        // (3) MEASURE AND ASK (passive, 27 Sep 2026 ruling). The knob was set ONCE, at build; from here the loop
        // only MEASURES and REPORTS. Two judged windows and it posts one line saying what it measured and asks;
        // a comparative comes back as a re-targeted block and buys exactly one step, then it asks again. THE
        // AUTOMATIC STEPPING BELOW IS THE LISTEN PATH ONLY - passive never reaches it.
        if (mode == Mode::Passive) return measureAndAsk (w, s);

        // (4) IN THE BAND, twice running, ends it.
        if (w.grDb >= lo && w.grDb <= hi)
        {
            if (++inBandRun >= kInBandRuns)
            {
                state = State::Adjusted; closingOwed = true;
                s.finished = true; s.closing = closingMessage();
                s.card = card(); s.logLine = log ("adjusted");
                return s;
            }
            s.card = card(); s.logLine = log ("listening");
            return s;
        }
        inBandRun = 0;

        // (5) OUT OF THE BAND: one step toward it, unless the budget, the range or the HEADROOM says stop.
        //
        // THE THRESHOLD ACTUATOR takes the simpler road: a threshold costs the slot's input no headroom, so the
        // only limits are the step budget and the profile's own range. Which way is "harder" comes from the
        // profile's sense, never from a guess - a dB threshold compresses harder as it falls, and a control whose
        // sense was not sampled is not this actuator at all (the server sends "drive" for those).
        if (writesNamedParam())
        {
            const bool harder = w.grDb < lo;                      // too little reduction -> work it harder
            const float wantP = value + (harder ? (float) senseSign : -(float) senseSign) * kStepDb;
            if (steps >= kMaxSteps || wantP < minDb - 1.0e-4f || wantP > maxDb + 1.0e-4f)
            {
                state = State::Clamped; closingOwed = true;
                s.finished = true; s.closing = closingMessage();
                s.card = card(); s.logLine = log ("clamped");
                return s;
            }
            value = wantP; ++steps; awaitFresh = true;
            s.writeParams = true; s.paramNames = params; s.paramValue = value;
            s.card = card(); s.logLine = log ("listening");
            return s;
        }
        const float want = w.grDb < lo ? preDb + kStepDb : preDb - kStepDb;
        // THE HEADROOM LIMIT (21t-d): the drive at which the slot's input true peak would reach -3 dBTP. The
        // input TP was measured AT THE CURRENT DRIVE, so the headroom left is (-3 - inTP) dB and the limit is
        // this drive plus that. It binds only upward - driving DOWN never costs headroom.
        headroomLimit = (w.inTruePeakDb > -190.0f) ? preDb + (kCeilingDbTp - w.inTruePeakDb) : kDriveLimit;
        const bool up = want > preDb;
        const float upperLimit = up ? juce::jmin (kDriveLimit, headroomLimit) : kDriveLimit;
        if (up && want > upperLimit + 1.0e-4f && headroomLimit < kDriveLimit - 1.0e-4f)
        {
            // Stopped by HEADROOM, not by the budget: a different sentence, because it is a different fact and
            // the user can act on it (turn the source down, or accept less compression).
            state = State::Clamped; closingOwed = true; headroomStopped = true;
            s.finished = true; s.closing = closingMessage();
            s.card = card(); s.logLine = log ("clamped");
            return s;
        }
        if (steps >= kMaxSteps || std::abs (want) > kDriveLimit + 1.0e-4f || (up && want > upperLimit + 1.0e-4f))
        {
            state = State::Clamped; closingOwed = true;
            s.finished = true; s.closing = closingMessage();
            s.card = card(); s.logLine = log ("clamped");
            return s;
        }
        preDb = want; ++steps; awaitFresh = true;
        value = preDb;                      // one "current value", whichever knob it is
        s.writeDrive = true; s.newPre = preDb; s.newPost = -preDb;
        s.card = card(); s.logLine = log ("listening");
        return s;
    }

    /** ONE judged window in measure-and-ask. Nothing here decides to move the knob: either a comparative already
        bought a step and this window spends it, or the window is measured, reported and left alone. */
    Step& measureAndAsk (const Window& w, Step& s)
    {
        ++judged;

        // THE ONE STEP A COMPARATIVE BUYS. Spent on the first judged window after the re-target, so the move is
        // made against a reading this host actually took, and counted so the log can say how many there have been.
        if (pendingStep != 0)
        {
            const bool harder = pendingStep > 0;
            pendingStep = 0;
            if (writesNamedParam())
            {
                const float want = juce::jlimit (minDb, maxDb,
                                        value + (harder ? (float) senseSign : -(float) senseSign) * stepDb);
                if (std::abs (want - value) > 1.0e-4f)
                {
                    value = want; ++steps; ++stepsTaken;
                    s.writeParams = true; s.paramNames = params; s.paramValue = value;
                }
            }
            else
            {
                const float want = juce::jlimit (-kDriveLimit, kDriveLimit, preDb + (harder ? kStepDb : -kStepDb));
                if (std::abs (want - preDb) > 1.0e-4f)
                {
                    preDb = want; value = preDb; ++steps; ++stepsTaken;
                    s.writeDrive = true; s.newPre = preDb; s.newPost = -preDb;
                }
            }
            judged = 0; asked = false; awaitFresh = true;
            if (s.writeDrive || s.writeParams) { freshWanted = kFreshAfterWrite; levelHeld = false; }   // 21t-j: settle, then hold the level again, then report
            s.card = card();
            s.logLine = log ((s.writeDrive || s.writeParams) ? "stepped" : "at-the-limit");
            return s;
        }

        // 21t-j (ruled): LEVEL-NEUTRAL, BEFORE IT SPEAKS. The slot's own out-minus-in IS what the chain is adding
        // at this point; holding it within 1 dB is what makes a compressor build level-neutral. Written ONCE per
        // actuator position, to the control the block named, and said out loud in the reply.
        levelChangeDb = w.levelChangeDb;   // out minus in on SHORT90: the slot's own contribution, as ruled
        // 21t-j (ruled): WITH NO OUTPUT CONTROL NAMED, the hold goes to EchoJay's own per-slot output gain, which
        // every slot has after this cut. "The slot's output equals its input within 1 dB" is then a promise the
        // product keeps on any plugin, not one that depends on what the plugin publishes.
        // 21t-j (ruled): the hold belongs to the SETTLE. After landing nothing moves at all until a comparative
        // reopens it - a slot that drifts out of level later is not something the product quietly corrects.
        if (! landed && ! levelHeld && std::abs (levelChangeDb) > 1.0f && outParams.isEmpty())
        {
            slotGainDb = juce::jlimit (-24.0f, 12.0f, slotGainDb - levelChangeDb);
            levelTrimmedDb = -levelChangeDb;
            levelHeld = true;
            freshWanted = kFreshAfterWrite;
            judged = 0;
            s.writeSlotGain = true; s.slotGainValue = slotGainDb;
            s.card = card(); s.logLine = log ("level-hold-slot");
            return s;
        }
        if (! landed && ! levelHeld && std::abs (levelChangeDb) > 1.0f && ! outParams.isEmpty() && outValue == outValue)
        {
            const float want = juce::jlimit (outMin, outMax, outValue - levelChangeDb);
            const float moved = want - outValue;
            if (std::abs (moved) > 0.05f)
            {
                outValue = want;
                levelTrimmedDb = moved;
                levelHeld = true;
                freshWanted = kFreshAfterWrite;      // the hold is a write: settle before quoting anything
                judged = 0;
                s.writeOutput = true; s.outputNames = outParams; s.outputValue = outValue;
                s.card = card(); s.logLine = log ("level-hold");
                return s;
            }
            levelHeld = true;   // at the control's limit: nothing more to give, and the reply will say so
        }

        // ---- THE SETTLE: the tail of the build (28 Sep 2026 ruling) ----------------------------------------
        // HEARD time counts, not clock time: a stop mid-settle simply stops adding to it, and playing resumes it.
        if (settleStartHeardS < 0.0f) settleStartHeardS = slotHeardS;
        settleHeardS = juce::jmax (0.0f, slotHeardS - settleStartHeardS);

        if (settling && ! landed)
        {
            const float gr = measuredGrDb();
            const bool inBand = (gr == gr) && gr >= lo - 0.05f && gr <= hi + 0.05f;
            const bool budget = settleSteps < kSettleMaxSteps && settleHeardS <= kSettleMaxHeardS;
            if (! inBand && budget && gr == gr)
            {
                // ONE STEP TOWARD THE BAND. This is the only automatic movement the product makes, it belongs to
                // the build, and it is bounded by both a step count and HEARD audio.
                const bool harder = gr < lo;
                ++settleSteps;
                if (writesNamedParam())
                {
                    const float want = juce::jlimit (minDb, maxDb,
                                            value + (harder ? (float) senseSign : -(float) senseSign) * stepDb);
                    if (std::abs (want - value) > 1.0e-4f)
                    {
                        value = want; ++steps;
                        s.writeParams = true; s.paramNames = params; s.paramValue = value;
                    }
                }
                else
                {
                    const float want = juce::jlimit (-kDriveLimit, kDriveLimit, preDb + (harder ? kStepDb : -kStepDb));
                    if (std::abs (want - preDb) > 1.0e-4f)
                    {
                        preDb = want; value = preDb; ++steps;
                        s.writeDrive = true; s.newPre = preDb; s.newPost = -preDb;
                    }
                }
                if (s.writeParams || s.writeDrive) { freshWanted = kFreshAfterWrite; levelHeld = false; }
                awaitFresh = true;
                s.card = card();
                s.logLine = log (s.writeParams || s.writeDrive ? "settling" : "settle-at-the-limit");
                if (s.writeParams || s.writeDrive) return s;
            }
            // LANDED: in the band, or the step budget or the heard-audio budget is spent. The SAME line completes
            // in place - not a second message (ruled (a)).
            landed = true; settling = false; asked = true;
            askOwed = completedLine();
            s.ask = askOwed;
            s.askReplacesOpening = true;
            s.card = card();
            s.logLine = log (inBand ? "landed" : (settleSteps >= kSettleMaxSteps ? "landed-step-budget"
                                                                                 : "landed-heard-budget"));
            return s;
        }

        // AFTER LANDING nothing moves except on a comparative, and nothing is said.
        s.card = card();
        s.logLine = log (landed ? "holding" : "measuring");
        return s;
    }

    /** The measure-and-ask line, verbatim to the ruling: what is on, what it was set from, what it is doing, and
        the two words that move it. The gain-reduction figure is the same one decimal the log line prints, so the
        sentence the user reads and the line in the log are traceable to each other. */
    /** THE FIGURE THE REPLY QUOTES (21t-j, 28 Sep 2026 ruling): gain reduction as a POSITIVE number, always. The
        sense reads the slot's input minus its output, so a plugin whose makeup is up reads NEGATIVE - the
        10:25:05 ask said "about -7.6 dB of gain reduction", which is not a thing. The magnitude is the reduction;
        a negative reading means the slot is LOUDER out than in, which is a level fault, not a reduction, and
        levelNote() is where that gets said. */
    juce::String grPositiveText() const
    { return (lastGr == lastGr) ? juce::String (std::abs (lastGr), 1) : juce::String ("--"); }
    /** THE FIGURE THE REPLY QUOTES (28 Sep 2026 general rule): the MEASURED crest difference, always positive. A
        plugin's own GR meter, when the block names one and it prints dB, is logged beside this as a cross-check
        and is never the reply's number. */
    float measuredGrDb() const
    { return (lastGr == lastGr) ? std::abs (lastGr) : std::numeric_limits<float>::quiet_NaN(); }

    /** ...and on a track, a figure outside the band gets a clause of its own, in the user's terms. */
    juce::String bandNote() const
    {
        if (! (lastGr == lastGr)) return {};
        const float mag = std::abs (lastGr);
        if (mag > hi + 0.05f)
            return " - more than the " + juce::String (lo, 0) + "-" + juce::String (hi, 0) + " I'm after";
        if (mag < lo - 0.05f)
            return " - less than the " + juce::String (lo, 0) + "-" + juce::String (hi, 0) + " I'm after";
        return {};
    }

    /** THE OPENING LINE (ruled): posted by the build itself and edited in place when the settle lands. It promises
        exactly what the product then does - land it as it plays - and asks for nothing. */
    juce::String openingLine() const
    {
        const juce::String from = fromWorking
            ? juce::String (", set from the working position")
            : (blockHeardS == blockHeardS
                   ? ", set from " + juce::String (juce::roundToInt (blockHeardS)) + " s of this track"
                   : juce::String());
        return plugin + " on" + from + ", landing it as it plays...";
    }

    /** THE COMPLETED LINE: the same line, finished. Every figure in it is a meter sample. */
    juce::String completedLine() const
    {
        const juce::String from = fromWorking
            ? juce::String (", set from the working position")
            : (blockHeardS == blockHeardS
                   ? ", set from " + juce::String (juce::roundToInt (blockHeardS)) + " s of this track"
                   : juce::String());
        juce::String out = plugin + " on" + from + ".";
        const float gr = measuredGrDb();
        if (gr == gr)
            out += " Doing about " + juce::String (gr, 1) + " dB of gain reduction" + bandNote() + ".";
        if (std::abs (levelTrimmedDb) > 0.05f)
            out += " Output trimmed " + juce::String (std::abs (levelTrimmedDb), 1) + " dB to hold the level.";
        else if (std::abs (levelChangeDb) > 1.0f)
            out += " It is " + juce::String (std::abs (levelChangeDb), 1) + " dB "
                 + (levelChangeDb > 0.0f ? "louder" : "quieter") + " through the plugin and I could not hold it.";
        return out + " Say 'ease off' or 'more'.";
    }

    juce::String askMessage() const
    {
        // WHERE THE SETTING CAME FROM, in the block's own terms (27 Sep 2026 ruling): the server's heard_s for a
        // tally or a measured start, and "the working position" when nothing was measured at all. A block with a
        // tally source and no heard_s says neither rather than inventing a number.
        // THE CLAUSE IS OMITTED, NOT STUBBED, when there is nothing to say. The first cut substituted the word
        // "on" and the sentence read "Pro-C 2 is on, on, doing about 2.5 dB" - the guard caught it.
        const juce::String from = fromWorking
            ? juce::String (", set from the working position")
            : (blockHeardS == blockHeardS
                   ? ", set from " + juce::String (juce::roundToInt (blockHeardS)) + " s of this track"
                   : juce::String());
        // ...AND THE REPLY SAYS WHAT WAS DONE ABOUT THE LEVEL (ruled): the trim that holds it, or plainly that
        // the level moved and nothing here can hold it, because no output control was named.
        juce::String level;
        if (std::abs (levelTrimmedDb) > 0.05f)
            level = " Output trimmed " + juce::String (std::abs (levelTrimmedDb), 1) + " dB to hold the level.";
        else if (std::abs (levelChangeDb) > 1.0f && outParams.isEmpty())
            level = " It is " + juce::String (std::abs (levelChangeDb), 1) + " dB "
                  + (levelChangeDb > 0.0f ? "louder" : "quieter")
                  + " through the plugin and I have no output control for it here, so the level is not held.";
        // 21t-j (ruled): NO FIGURE THE PLUGIN DID NOT PUBLISH. A GR figure is quoted only from the plugin's own
        // meter (sense_param, gr_readable); the slot's in-vs-out pair is a level change and is reported as one.
        // EVERY FIGURE IN A REPLY IS A METER SAMPLE (ruled). The crest difference is measured on this slot's own
        // taps, so there is one to quote as soon as a window has been judged.
        const float gr = measuredGrDb();
        if (gr == gr)
            return plugin + " is on" + from + ", doing about " + juce::String (gr, 1)
                 + " dB of gain reduction on the loud phrases" + bandNote() + "." + level
                 + " How's that sounding? Say 'ease off' or 'more'.";
        return plugin + " is on" + from + "." + level + " How's that sounding? Say 'ease off' or 'more'.";
    }

    /** Nothing heard in 30 s: it cannot report a figure it does not have, so it asks for the one thing that would
        give it one. Said once - repeating it every 30 s of silence would be nagging about a chat nobody opened. */
    void askNoSignal (Step& s)
    {
        if (mode != Mode::Passive || noSignalSaid) return;
        noSignalSaid = true;
        askOwed = plugin + " is on - play it and I'll tell you what it's doing.";
        s.ask = askOwed;
    }

    juce::String card() const
    {
        // PASSIVE SAYS NOTHING WHILE IT RUNS. No card, so no "Listening... play the loudest part" and no
        // "Waiting for playback" - nothing is waiting for the user, and telling them to play something for a
        // measurement already in hand is a wrong instruction.
        if (mode == Mode::Passive) return {};
        switch (state)
        {
            case State::Waiting:   return "Waiting for playback - play the loudest part of this channel";
            case State::Adjusted:
            case State::Clamped:   return plugin + " working " + grText() + " dB";
            case State::Listening: return (lastGr == lastGr) ? plugin + " working " + grText() + " dB"
                                                             : juce::String ("Listening... play the loudest part");
            case State::Idle:
            default:               return {};
        }
    }

    /** The closing message: ONE clause naming what was adjusted, then a general question about the CHAIN, drawn
        from a small set and never the same one twice running. No pills - the user answers in words and the
        server's follow-up rule routes it. */
    juce::String closingMessage()
    {
        // PASSIVE HAS NO CLOSING LINE (27 Sep 2026 ruling). It does not close: it measures, reports and waits for
        // the user. The only lines it ever posts are the measure-and-ask ones, and those go out through askOwed.
        if (mode == Mode::Passive) return {};
        static const char* kQuestions[] = {
            "How does the chain sound now?",
            "Does that sit better with the rest of the mix?",
            "Anything else on this chain you want changed?",
            "How is it sounding to you?"
        };
        const int n = (int) (sizeof (kQuestions) / sizeof (kQuestions[0]));
        int pick = (phraseIdx + 1) % n;
        if (pick == phraseIdx) pick = (pick + 1) % n;   // never the same one twice running
        phraseIdx = pick;

        const juce::String what =
            (state == State::Clamped && headroomStopped)
                ? plugin + ": band not reached - drive limited by headroom at " + driveText()
                  + " dB, working " + grText() + " dB."
          : state == State::Clamped
                ? "I could not get " + plugin + " into the " + juce::String (lo, 1) + "-" + juce::String (hi, 1)
                  + " dB band with drive alone - it is working " + grText() + " dB at "
                  + driveText() + " dB of drive."
                : "Adjusted the " + plugin + " to " + driveText() + " dB - it is working " + grText() + " dB.";
        return what + " " + kQuestions[pick];
    }

    // ---- the state as it rides the sidecar -------------------------------------------------------------------
    // One object, written by whichever host owns the tallies and read by the other and by V2's editor. Every
    // field the loop needs to continue is here: a handover that lost the step count would restart the work.
    juce::var toVar() const
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("plugin", plugin);      o->setProperty ("slot", slot);
        o->setProperty ("lo", (double) lo);     o->setProperty ("hi", (double) hi);
        o->setProperty ("preDb", (double) preDb);
        o->setProperty ("steps", steps);        o->setProperty ("window", window);
        o->setProperty ("inBandRun", inBandRun); o->setProperty ("phraseIdx", phraseIdx);
        o->setProperty ("lastGr", (lastGr == lastGr) ? juce::var ((double) lastGr) : juce::var());
        o->setProperty ("noSignalMs", noSignalMs);
        o->setProperty ("awaitFresh", awaitFresh);
        o->setProperty ("state", (int) state);
        o->setProperty ("closingOwed", closingOwed);
        o->setProperty ("headroomStopped", headroomStopped);
        // 21t-g: the mode and the actuator ride too, or a handover would turn a passive threshold pass into a
        // listen drive pass halfway through - which is a different loop, on a different knob, talking to the user.
        o->setProperty ("mode", (int) mode);
        o->setProperty ("actuator", (int) actuator);
        o->setProperty ("params", params.joinIntoString ("\n"));
        o->setProperty ("senseSign", senseSign);
        o->setProperty ("value", (double) value);
        o->setProperty ("minDb", (double) minDb);
        o->setProperty ("maxDb", (double) maxDb);
        // 21t-i: the measure-and-ask state. A handover that lost pendingStep would drop the user's comparative on
        // the floor; one that lost askOwed would swallow the only line the loop ever says.
        o->setProperty ("stepDb", (double) stepDb);
        o->setProperty ("judged", judged);
        o->setProperty ("asked", asked);
        o->setProperty ("noSignalSaid", noSignalSaid);
        o->setProperty ("pendingStep", pendingStep);
        o->setProperty ("slotHeardS", (double) slotHeardS);
        o->setProperty ("blockHeardS", (blockHeardS == blockHeardS) ? juce::var ((double) blockHeardS) : juce::var());
        o->setProperty ("fromWorking", fromWorking);
        o->setProperty ("stepsTaken", stepsTaken);
        o->setProperty ("askOwed", askOwed);
        o->setProperty ("freshWanted", freshWanted);
        o->setProperty ("outParams", outParams.joinIntoString ("\n"));
        o->setProperty ("outValue", (outValue == outValue) ? juce::var ((double) outValue) : juce::var());
        o->setProperty ("outMin", (double) outMin); o->setProperty ("outMax", (double) outMax);
        o->setProperty ("levelTrimmedDb", (double) levelTrimmedDb);
        o->setProperty ("levelChangeDb", (double) levelChangeDb);
        o->setProperty ("slotGainDb", (double) slotGainDb);
        o->setProperty ("settling", settling);
        o->setProperty ("landed", landed);
        o->setProperty ("settleSteps", settleSteps);
        o->setProperty ("settleHeardS", (double) settleHeardS);
        o->setProperty ("settleStartHeardS", (double) settleStartHeardS);
        o->setProperty ("levelHeld", levelHeld);
        o->setProperty ("senseParams", senseParams.joinIntoString ("\n"));
        o->setProperty ("grReadable", grReadable);
        o->setProperty ("sensedGrDb", (sensedGrDb == sensedGrDb) ? juce::var ((double) sensedGrDb) : juce::var());
        o->setProperty ("lastHeardS", (double) lastHeardS);
        return juce::var (o);
    }
    static CalibLoop fromVar (const juce::var& v)
    {
        CalibLoop c;
        auto* o = v.getDynamicObject();
        if (o == nullptr) return c;
        c.plugin = o->getProperty ("plugin").toString();
        c.slot   = (int) o->getProperty ("slot");
        c.lo = (float) (double) o->getProperty ("lo");  c.hi = (float) (double) o->getProperty ("hi");
        c.preDb = (float) (double) o->getProperty ("preDb");
        c.steps = (int) o->getProperty ("steps");       c.window = (int) o->getProperty ("window");
        c.inBandRun = (int) o->getProperty ("inBandRun"); c.phraseIdx = (int) o->getProperty ("phraseIdx");
        const auto g = o->getProperty ("lastGr");
        c.lastGr = g.isVoid() ? std::numeric_limits<float>::quiet_NaN() : (float) (double) g;
        c.noSignalMs = (double) o->getProperty ("noSignalMs");
        c.awaitFresh = (bool) o->getProperty ("awaitFresh");
        c.state = (State) (int) o->getProperty ("state");
        c.closingOwed = (bool) o->getProperty ("closingOwed");
        c.headroomStopped = (bool) o->getProperty ("headroomStopped");
        // 21t-g. An OLDER sidecar has none of these: mode falls back to LISTEN, not passive, because that is what
        // a loop written by an older binary was - inferring "passive" from a missing field would silence a pass
        // that had been showing a card.
        c.mode = o->hasProperty ("mode") ? (Mode) (int) o->getProperty ("mode") : Mode::Listen;
        c.actuator = o->hasProperty ("actuator") ? (Actuator) (int) o->getProperty ("actuator") : Actuator::Drive;
        c.params.clear();
        if (o->hasProperty ("params"))
        {
            const auto joined = o->getProperty ("params").toString();
            if (joined.isNotEmpty()) c.params.addLines (joined);
        }
        c.senseSign = o->hasProperty ("senseSign") ? ((int) o->getProperty ("senseSign") < 0 ? -1 : 1) : -1;
        c.value = o->hasProperty ("value") ? (float) (double) o->getProperty ("value") : c.preDb;
        c.minDb = o->hasProperty ("minDb") ? (float) (double) o->getProperty ("minDb") : -60.0f;
        c.maxDb = o->hasProperty ("maxDb") ? (float) (double) o->getProperty ("maxDb") : 12.0f;
        // 21t-i. An older sidecar has none of these; the defaults are a loop that has measured nothing and owes
        // nothing, which is what a loop written before this round was.
        c.stepDb = o->hasProperty ("stepDb") ? (float) (double) o->getProperty ("stepDb") : kStepDb;
        if (! (c.stepDb > 0.0f)) c.stepDb = kStepDb;
        c.judged = (int) o->getProperty ("judged");
        c.asked = (bool) o->getProperty ("asked");
        c.noSignalSaid = (bool) o->getProperty ("noSignalSaid");
        c.pendingStep = (int) o->getProperty ("pendingStep");
        c.slotHeardS = (float) (double) o->getProperty ("slotHeardS");
        {   // A handover must not lose what the sentence quotes. An older sidecar has neither key: no heard time to
            // quote (NaN) and not a working-position start, which is what a pre-re-cut loop was.
            const auto bh = o->getProperty ("blockHeardS");
            c.blockHeardS = bh.isVoid() ? std::numeric_limits<float>::quiet_NaN() : (float) (double) bh;
            c.fromWorking = (bool) o->getProperty ("fromWorking");
        }
        c.stepsTaken = (int) o->getProperty ("stepsTaken");
        c.askOwed = o->getProperty ("askOwed").toString();
        c.freshWanted = (int) o->getProperty ("freshWanted");
        { const auto joined = o->getProperty ("outParams").toString();
          if (joined.isNotEmpty()) c.outParams.addLines (joined);
          const auto ov = o->getProperty ("outValue");
          c.outValue = ov.isVoid() ? std::numeric_limits<float>::quiet_NaN() : (float) (double) ov;
          if (o->hasProperty ("outMin")) c.outMin = (float) (double) o->getProperty ("outMin");
          if (o->hasProperty ("outMax")) c.outMax = (float) (double) o->getProperty ("outMax");
          c.levelTrimmedDb = (float) (double) o->getProperty ("levelTrimmedDb");
          c.levelChangeDb = (float) (double) o->getProperty ("levelChangeDb");
          c.slotGainDb = (float) (double) o->getProperty ("slotGainDb");
          c.settling = (bool) o->getProperty ("settling");
          c.landed = (bool) o->getProperty ("landed");
          c.settleSteps = (int) o->getProperty ("settleSteps");
          c.settleHeardS = (float) (double) o->getProperty ("settleHeardS");
          c.settleStartHeardS = o->hasProperty ("settleStartHeardS")
                                  ? (float) (double) o->getProperty ("settleStartHeardS") : -1.0f;
          c.levelHeld = (bool) o->getProperty ("levelHeld");
          const auto sp = o->getProperty ("senseParams").toString();
          if (sp.isNotEmpty()) c.senseParams.addLines (sp);
          c.grReadable = (bool) o->getProperty ("grReadable");
          const auto sg = o->getProperty ("sensedGrDb");
          c.sensedGrDb = sg.isVoid() ? std::numeric_limits<float>::quiet_NaN() : (float) (double) sg; }
        c.lastHeardS = o->hasProperty ("lastHeardS") ? (float) (double) o->getProperty ("lastHeardS") : -1.0f;
        return c;
    }
    bool active() const { return state != State::Idle; }

    juce::String log (const char* stateWord) const
    {
        // The log names the MODE and the KNOB, because "the loop moved something" is not readable a week later
        // without them - a passive threshold pass and a listen drive pass look identical otherwise.
        return "EJThreshold: \"" + plugin + "\" window " + juce::String (window)
             + " gr=" + grText()
             + (writesNamedParam()
                    ? " " + knobText() + "=" + signed1 (value)
                    : " pre=" + signed1 (preDb) + " post=" + signed1 (-preDb))
             + " mode=" + juce::String (mode == Mode::Passive ? "passive" : "listen")
             + (settling || landed
                    ? " settle=" + juce::String (settleSteps) + "/" + juce::String (kSettleMaxSteps)
                      + " heard=" + juce::String (settleHeardS, 1) + "s"
                    : juce::String())
             + " state=" + stateWord;
    }

    /** What is being dialled, in the user's words: the profile's own control name, or "drive". */
    juce::String knobText() const
    {
        if (writesNamedParam() && ! params.isEmpty())
            return params.size() == 1 ? params[0] : params.joinIntoString (" + ");
        return "drive";
    }

private:
    static juce::String signed1 (float v)
    {
        // NEGATIVE ZERO reads as a defect in the number: the mirrored post-trim of a drive at 0.0 printed
        // "post=+-0.0" in the window line. -0.0f is >= 0.0f, so the sign prefix was right and the value was not.
        const float w = (v == 0.0f) ? 0.0f : v;
        return (w >= 0.0f ? "+" : "") + juce::String (w, 1);
    }
    juce::String grText() const
    { return (lastGr == lastGr) ? juce::String (lastGr, 1) : juce::String ("--"); }
    // The number the closing line quotes: whichever knob this loop is dialling.
    juce::String driveText() const { return signed1 (writesNamedParam() ? value : preDb); }
};

} // namespace echojay
