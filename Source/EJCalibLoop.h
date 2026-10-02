#pragma once

#include <JuceHeader.h>
#include <cmath>
#include <functional>   // (i): configsFromBlock takes the caller's slot-name lookup
#include "EJCompCheck.h"   // COMP_PROFILE_SPEC_v1 section 7: the one check
#include <vector>

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
    /** 21t-m item 2 (29 Sep 2026 ruling): WHAT THIS RUN IS FOR.
        A BUILD does not hunt. It applies the working position, matches the level ONCE through OUT, says so in
        one line and stops - no windows, no rungs, no "landing it as it plays". Sean's 21:53 compressor walked
        three rungs and 18 windows for a build he never asked to have dialled.
        AN ASK ("harder", "more", "ease off", "softer", "land it") moves ONE rung of the plan's actuator and
        reports what it moved and what the meters read. */
    enum class Purpose { buildHold, askRung };

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
    // 21t-m item 6c, REPLACED 30 Sep 2026 by letter (n): THE SLOT'S OWN IDENTITY, not the rack revision.
    // chainRevision was the wrong witness. It moves on add, remove AND move - but Sean's 19:03:33 log shows a
    // deferred settle and two in-flight fallback serves bumping it 9 -> 10 with no slot added, removed or moved,
    // and the Tube-Tech loop cancelled at .499 saying "the rack was rebuilt under this loop". A parameter or map
    // write must never cancel a loop. What actually matters is whether THIS slot still holds THIS plugin, so the
    // witness is ChainHost::slotIdentityKey(slot) - index plus the plugin's uid/fingerprint. Empty is "not
    // captured", which never cancels.
    juce::String slotIdent;
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
    // (g) 30 Sep 2026, with a correction of its own: the holding tail is deleted, so a loop that has finished goes
    // Idle - and with it went the one record of HOW it finished. Adjusted (it reached the band) and Clamped (it ran
    // out of range or headroom) are different facts, and the closing sentence is prose. endHere() copies the state
    // here before clearing it, so the ending stays readable to a caller and to a guard.
    State  endedAs     = State::Idle;
    bool   closingOwed = false;        // the loop ended and nobody has posted the message yet
    // 30 Sep 2026 ruling: THE MESSAGE SURVIVES THE ENDING. closingMessage() is derived from `state`, and the chat
    // takes it LATER (calibTakeClosing reads the stored loop), so a loop that ends at its close - which is now
    // every loop - would have nothing left to say. The text is captured at the close and this is what is posted.
    juce::String closingOwedText;
    // Letter (l), 30 Sep 2026 ruling: IS THIS A COMPRESSOR SLOT? A compressor's input is MEANT to go over its
    // threshold, so the -3 dBTP no-stage-above ceiling (21p item 3) does not belong on it - that ceiling belongs on
    // the OUT, which the hold owns. On a compressor slot the loop's IN is bounded by the slot range (+/-12) only.
    // Sean's 18:25:43-58: a track peaking at -5.0 dBTP allowed the drive exactly 2 dB and then said "band not
    // reached - drive limited by headroom at +2.0 dB", on a compressor that had not begun to work.
    bool   dynamicsSlot = false;
    // COMP_PROFILE_SPEC_v1 items 3 and 4: what the one check found, for the closing line and the log. All
    // false/NaN means there was no profile, and the line then says "set as dialled, no profile yet".
    bool   hasProfile = false;
    // THE FEATURE FLAG, CARRIED ON THE LOOP (2 Oct 2026). hasProfile answers "did THIS slot get a profile";
    // this answers "is the measured-profile feature on at all", and the closing line needs BOTH. With the flag
    // off the line must be letter (q)'s, word for word, because "default OFF means byte-for-byte what it is
    // today" - and "no profile yet" is actively misleading to someone for whom profiles do not exist. Set by
    // stampCompProfileOnLoop when ChainHost::compProfilesEnabled(), whether or not a profile was found, because
    // section 7's wording covers the unprofiled compressor too once the feature is on. Default false, so the
    // Link - which has no profile path yet - keeps (q)'s wording.
    bool   profilesFeatureOn = false;
    // The server's own word for it: `from_profile` on the calibration block, true when IT set this compressor
    // open-loop from a profile. The plugin having a profile in its map payload is not the same statement.
    bool   blockFromProfile = false;
    bool   profileChecked = false;
    bool   profileCorrected = false;
    bool   profileNotEngaging = false;
    float  profileCorrectionDb = 0.0f;
    float  profileObservedDropDb = std::numeric_limits<float>::quiet_NaN();
    // The profile's own numbers, copied onto the loop by the host at begin() so the loop needs no ChainHost.
    float  profileExpectedGrDb    = std::numeric_limits<float>::quiet_NaN();
    float  profileExpectedLevelDb = std::numeric_limits<float>::quiet_NaN();
    float  profileStaticGainDb    = 0.0f;
    juce::String profileAmountControl;
    float  profileAmountNormAfter = 0.0f;   // where the amount goes if the check eases it
    float  profileCurrentNorm = 0.0f;       // where it is NOW, read off the plugin when the loop started
    juce::var profileVar;                   // the profile itself, for the curve the ease reads
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
    // 21t-k item 3 (28 Sep 2026 ruling, superseding the 15 s cap): THE BUDGET IS THREE STEPS, each judged on
    // TWO fresh windows, with a 45 s heard CEILING as the backstop. The 15 s cap ended Sean's Zip settle after
    // ONE step, because every write waits for fresh windows before the next judgement and three of those spend
    // the whole budget: 3 steps x 2 fresh windows x 3 s is 18 s of heard audio before the third step is judged.
    // A cap shorter than the machine's own cadence is not a budget, it is a stop.
    static constexpr int   kSettleMaxSteps  = 3;      // ruled
    // Letter (m), 30 Sep 2026 ruling: THE BUILD SEEKS THE BAND. "On a build the loop moves EchoJay's IN, passive,
    // window by window, until the gain reduction is inside the band or the slot range is exhausted; only then does
    // the hold set OUT once." This is the cap on that seek, in WINDOWS. (e)'s one-shot is superseded for the seek;
    // what survives of it is that the HOLD still runs once and the loop still ends at its close.
    static constexpr int   kBuildMaxWindows = 12;     // ruled
    static constexpr float kSettleMaxHeardS = 45.0f;  // ruled, in HEARD seconds - the backstop, not the budget
    // The output/makeup control the block named, IF it named one. 21t-m (29 Sep 2026 ruling): the HOLD never
    // writes it - these are kept because the block's parse carries them and the card/log report what the build
    // set, not because anything here moves them.
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
    float  levelTrimmedDb = 0.0f;      // what the hold ACTUALLY wrote (21t-k: not what it wanted to write)
    bool   levelHoldClamped = false;   // ...and true when the control ran out before the level was held
    float  levelHoldLimitDb = 0.0f;    // where it stopped
    bool   levelHeld = false;          // the hold has written at least once at the current actuator position
    // ---- 21t-m item 1 (29 Sep 2026 ruling): THE HOLD CONVERGES ----------------------------------------------
    // Sean's 12:55-12:57 demo: -6.00, -12.00, -18.00, -24.00 on four consecutive holds, every one of them
    // reporting "the slot was 6.0 dB louder out than in", and the closing sentence then said "Output trimmed
    // 6.0 dB" with 24 written. Two faults met: the sensor's out tap sits BEFORE this gain (21t-k item 3, and it
    // stays there - it is the PLUGIN's change the card and the block quote), while the write was
    // previous + delta against a measurement that could never see the previous write. The delta was 6.0 every
    // time because it was the SAME untouched excess re-read, not a per-pass clamp.
    //   residualDb  = the plugin's change PLUS what this hold has already written = what is still wrong at the
    //                 slot's true output. It is what the hold acts on, so the loop closes.
    //   holdOpen / holdWrites / holdBaseGainDb / holdFirstExcessDb enforce the ruled shape (29 Sep 2026): the
    //                 hold runs ONCE, on the LANDED drive - one absolute correction, one re-measurement, at most
    //                 one refinement, and the total never beyond the first measured excess plus 1 dB. Nothing is
    //                 held between drive steps; that is the fault behind -6 / -12 / -18 / -24.
    float  levelResidualDb = 0.0f;
    bool   holdOpen = false;                                              // the settle has landed and the hold owes a window
    bool   holdDone = false;                                              // ...and it has finished: nothing more is written
    int    holdWrites = 0;                                                // writes this settle - never more than two
    bool   landedInBand = false;
    Purpose purpose = Purpose::askRung;   // 21t-m item 2
    float  holdBaseGainDb = 0.0f;                                         // the slot gain the hold opened from
    float  holdFirstExcessDb = std::numeric_limits<float>::quiet_NaN();   // the excess the first window measured
    static constexpr int   kHoldMaxWrites   = 2;     // ruled: one correction, at most one refinement
    static constexpr float kHoldOpenDb      = 1.0f;  // ...the first write is owed above this
    static constexpr float kHoldRefineDb    = 0.5f;  // ...and the refinement above this
    // 21t-m (29 Sep 2026 ruling): the slot output gain's own range, and the ONLY thing that limits the hold.
    static constexpr float kSlotGainMinDb = -24.0f, kSlotGainMaxDb = 12.0f;

    /** MAKE-UP ABOVE THIS MUCH MEANS THE COMPRESSOR IS TAKING TOO MUCH OFF (2 Oct 2026 ruling).
        Sean's 11:35 session on the Tube-Tech CL 1B: the hold wrote OUT +12.0 dB - the ceiling above - and the
        closing line still said "level matched". It had matched the level, which is why the old wording was not
        false; but a compressor that needs 12 dB of make-up is pulling 12 dB down, and reporting that as a match
        tells the user everything is fine when the threshold is far too low. 6 dB is the ruled line. */
    static constexpr float kOverCompressionDb = 6.0f;
    static constexpr int kFreshAfterWrite = 2;   // 21t-k item 3 (ruled 28 Sep 2026): two fresh windows per step
    // 21t-m item 5: how much of a window's worth of NEW heard audio makes a window judgeable. Two thirds - a
    // window is 3 s and a judged one must be most of that; Sean's 0.7 s sliver from a stopping transport is not.
    static constexpr float kFreshWindowFraction = 0.67f;

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
        // 21t-m item 1: EchoJay's OWN per-slot output gain as it reads at this window, from the control itself -
        // not from what this loop believes it last wrote. levelChangeDb is measured BEFORE it (21t-k item 3), so
        // the hold's residual is levelChangeDb + this, and a hold that has already corrected the slot sees it.
        // NaN = the host did not supply it, and then the loop falls back to its own slotGainDb.
        float slotOutGainDb = std::numeric_limits<float>::quiet_NaN();
        // 21t-m item 1: and EchoJay's own PRE-TRIM on this slot - the drive. levelChangeDb is measured AFTER it
        // (21t-k item 3: the sensor measures the plugin, not the staging), and the drive's compensating post-trim
        // goes to setSlotTrimDb, which is the COMPARE trim and is only in circuit while an A/B runs (ruling R2,
        // 24 Sep 2026). So a drive of +7.6 dB raises the chain by 7.6 dB and NOTHING takes it back. The hold's
        // job is that the slot comes out where it went in, so its residual counts every gain EchoJay itself put
        // in the slot - the pre-trim in front and the output gain behind. NaN = the host did not supply it.
        float slotPreTrimDb = std::numeric_limits<float>::quiet_NaN();
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
        // 21t-j's "a second write, to a DIFFERENT control from the actuator" is GONE (21t-m, 29 Sep 2026 ruling):
        // the hold writes EchoJay's own OUT and nothing else, so there is no second write target and no Step
        // field for one. A plugin's output control is left exactly where the build put it.
        // 21t-j: the hold through EchoJay's own per-slot output gain, for a plugin that publishes no output control.
        bool  writeSlotGain = false;
        // COMP_PROFILE_SPEC_v1 item 3: the ONE correction the check may make - the profile's own amount control,
        // moved once toward less gain reduction. The host writes it exactly as it writes a named-param step.
        bool writeAmount = false;
        juce::String amountControl;
        float amountNorm = 0.0f;
        float slotGainValue = 0.0f;
    };

    /** Everything the response's calibration block can say. Defaults are the ruled defaults: PASSIVE, the drive
        actuator, and a threshold range wide enough to be no constraint until a profile narrows it. */
    struct Config
    {
        Purpose purpose = Purpose::askRung;
        juce::String plugin;
        int    slot = -1;
        float  lo = 2.0f, hi = 3.0f;      // gr_target_db
        Mode   mode = Mode::Passive;
        Actuator actuator = Actuator::Drive;
        juce::StringArray params;         // param, or every entry of a param array
        int    senseSign = -1;            // "lower_is_harder" (the dB threshold case) unless told otherwise
        float  startDb = 0.0f;            // the actuator's opening value: the drive, or the threshold
        // 21t-j (28 Sep 2026): DID THE BLOCK ITSELF CARRY start_db? The caller fills startDb from the slot's
        // staging when a drive block sends null, which is right - opening at 0 would undo the staging - but it
        // makes a substituted value indistinguishable from one the server sent. The "a block carrying start_db
        // IS the step" rule then read every nudge-only block as already-moved and bought nothing: "ease off" did
        // nothing at all. This flag is the wire's own answer and the only thing that rule may consult.
        bool   startFromBlock = false;
        bool   dynamicsSlot = false;      // (l): set by the caller from ChainHost::slotIsDynamics - see the member
        // COMP_PROFILE_SPEC_v1 section 6.6, AND WHERE THEY ACTUALLY LIVE (1 Oct 2026, against B's real output):
        // expected_gr_db, expected_level_db and from_profile are fields of the CALIBRATION BLOCK for that
        // compressor - `calibrations[]`, keyed by its own 1-based `slot` - not of the chain's slot object. The
        // first cut read them off the chain entry and would have found nothing on a real reply.
        float  expectedGrDb    = std::numeric_limits<float>::quiet_NaN();
        float  expectedLevelDb = std::numeric_limits<float>::quiet_NaN();
        bool   fromProfile = false;
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
    /** (i) 30 Sep 2026, B's contract (CONTRACT_GROUPS, "Every compressor in a build gets its own hold"):
        EVERY COMPRESSOR'S BLOCK, from the chain object. `calibrations` is an array with one block per compressor
        in chain order, each with the same fields as `calibration`; `calibration` is byte-for-byte
        `calibrations[0]` and stays the primary, so a reader that knows only it keeps working. A chain with no
        compressor carries NEITHER field - never an empty array - and this returns nothing for it.

        In the shared header because the single-block parser is (21t-g item 6b): V2 and the Link must not read the
        same wire differently. `nameForSlot` is the caller's local knowledge - which host, and what that slot is
        called - because the parser has no rack.

        A block that is not usable is SKIPPED with its reason, and the others still run: one compressor's bad
        block is not a reason to leave the rest of a build unheld. */
    static int configsFromBlock (const juce::var& chain, int numSlots, bool busBand,
                                 const std::function<juce::String(int)>& nameForSlot,
                                 std::vector<Config>& out, juce::String& whyOut)
    {
        out.clear();
        const auto arr = chain.getProperty ("calibrations", juce::var());
        const auto single = chain.getProperty ("calibration", juce::var());
        auto readOne = [&] (const juce::var& blk) -> bool
        {
            auto* o = blk.getDynamicObject();
            if (o == nullptr) return false;
            const int wireSlot = o->hasProperty ("slot") ? ((int) o->getProperty ("slot")) - 1 : -1;
            const juce::String nm = (nameForSlot && wireSlot >= 0 && wireSlot < numSlots) ? nameForSlot (wireSlot)
                                                                                         : juce::String();
            Config c; juce::String why;
            if (! configFromBlock (blk, numSlots, busBand, nm, c, why))
            {
                if (why.isNotEmpty())
                    whyOut << (whyOut.isEmpty() ? "" : "; ") << "slot " << juce::String (wireSlot + 1) << ": "
                           << why.trim();
                return false;
            }
            if (why.isNotEmpty())
                whyOut << (whyOut.isEmpty() ? "" : "; ") << "slot " << juce::String (wireSlot + 1) << ": "
                       << why.trim();
            out.push_back (c);
            return true;
        };
        if (auto* a = arr.getArray())
        {
            for (const auto& blk : *a) readOne (blk);
            // THE PRIMARY IS calibrations[0] AND IS THE FIRST COMPRESSOR IN CHAIN ORDER. The contract says the two
            // fields are byte-for-byte equal, so nothing is read from `calibration` when the array is there - a
            // disagreement between them is the server's to fix, and silently preferring one would hide it.
            if (! out.empty()) return (int) out.size();
            // An array that yielded nothing usable still must not fall through to a block it duplicates.
            if (! a->isEmpty()) return 0;
        }
        // FALLBACK: the single block, for a server that has not shipped the array yet.
        readOne (single);
        return (int) out.size();
    }

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
        out.startFromBlock = haveStart;
        // 21t-k item 3 (28 Sep 2026 ruling): A DRIVE BLOCK THAT CARRIES A PARAM AND A START IS CONTRADICTORY,
        // AND IS REFUSED. Sean's Zip build arrived as actuator "drive" with param "Threshold" and start_db
        // -15.00 taken from the working position; the kind won, so the -15 went onto EchoJay's own staging
        // pre-gain instead of the plugin's Threshold, a -35 dB vocal reached the detector at -50, and the
        // compressor did nothing for eighteen windows. The client cannot tell which half the server meant, so
        // it follows the kind and IGNORES THE START: the drive opens at the staging already on the slot.
        if (out.actuator == Actuator::Drive && ! out.params.isEmpty() && haveStart)
        {
            out.startDb = std::numeric_limits<float>::quiet_NaN();
            out.startFromBlock = false;
            whyOut << "a drive block carries a param and a start - contradictory: the param is \"" << out.params[0]
                   << "\" and start_db " << juce::String ((float) (double) startV, 2)
                   << " is IGNORED; the drive opens at the staging already on the slot. ";
        }
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
            // 30 Sep 2026: SAID ONLY WHERE IT GOVERNS SOMETHING. min_db/max_db bound the THRESHOLD actuator's
            // control; the loop now moves EchoJay's own IN and nothing else, and the server sends every block as
            // drive with param null - so this note fired on every block while describing a range nothing reads.
            if (mn.isVoid() && o->hasProperty ("min_db") && out.actuator == Actuator::Threshold)
                whyOut << "min_db null (the low end prints \"-inf\") - taking " << juce::String (kPracticalFloorDb, 0)
                       << " dB as the practical floor. ";
        }

        // ---- expected_gr_db / expected_level_db / from_profile (COMP_PROFILE_SPEC_v1) ----------------------
        // A number or nothing. NaN means the block said nothing, which is today's behaviour for that slot - and is
        // a different statement from 0 dB of expected gain reduction. expected_level_db is explicitly null on a
        // threshold unit in the server's own output, so null must read as absent and not as zero.
        {
            auto num = [o] (const char* key) -> float
            {
                if (! o->hasProperty (key)) return std::numeric_limits<float>::quiet_NaN();
                const auto v = o->getProperty (key);
                if (v.isDouble() || v.isInt() || v.isInt64()) return (float) (double) v;
                return std::numeric_limits<float>::quiet_NaN();
            };
            out.expectedGrDb    = num ("expected_gr_db");
            out.expectedLevelDb = num ("expected_level_db");
            out.fromProfile     = (bool) o->getProperty ("from_profile");
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


    /** THE ONE PLACE THE PURPOSE DECIDES HOW A LOOP OPENS (2 Oct 2026).

        This rule existed TWICE - once in begin(Config) and once in the 6-arg begin - and on 30 Sep letter (m)
        changed it in one of them. begin(Config) got (m)'s rule, "a build opens with its SEEK AHEAD of it"
        (settleSteps 0); the 6-arg overload kept (e)'s withdrawn rule, "already spent" (kSettleMaxSteps), while
        its comment claimed it derived "the SAME three things begin(Config) derives". PluginProcessor.cpp reaches
        that overload with a caller-supplied purpose - it is the road the "Build this chain" pill takes - so the
        same build opened with opposite settle budgets depending on which entry point it came through, and which
        one you got was invisible at the call site. calib_link_guard's (f) leg is what caught it.
        Two copies of a rule is one rule too many: they drift, and when they disagree nobody knows which one
        decided. So the derivation lives here, once, and both entry points call it.
        THE RULE IS (m)'s, which is the current one:
          buildHold  the seek is ahead of it - settleSteps 0, bounded by the 12-window cap and the slot range,
                     and it promises NOTHING on the way in, because its only line is the closing one;
          askRung    one rung - kSettleMaxSteps - 1 - and it says on the way in what it is doing. */
    void openFromPurpose (Purpose p)
    {
        purpose = p;
        settling = true; landed = false; settleHeardS = 0.0f; settleStartHeardS = -1.0f;
        settleSteps = (p == Purpose::buildHold) ? 0 : kSettleMaxSteps - 1;
        askOwed     = (p == Purpose::buildHold) ? juce::String() : openingLine();
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
        resetHoldBudget();
        levelHoldClamped = false; levelHoldLimitDb = 0.0f;
        blockHeardS = c.heardS; fromWorking = c.working;
        purpose = c.purpose;
        dynamicsSlot = c.dynamicsSlot;   // (l)
        openFromPurpose (c.purpose);   // settling/landed/settleSteps/askOwed - ONE derivation, see below
        senseLogsOwed = senseParams.isEmpty() ? 0 : kSenseLogWindows;   // 21t-j: the cross-check, five windows
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

    /** The pre-21t-g entry point: a LISTEN pass on the drive, which is what every existing caller meant.
        21t-m (30 Sep 2026): IT TAKES A PURPOSE, like begin(Config). It never set one, so every loop started
        through it kept the member default askRung and closed with the ASK line - "Doing about 0.0 dB of gain
        reduction ... Say 'ease off' or 'more'" - on a build. That is the road the "Build this chain" pill takes:
        the reply's calibration block carries `ops`, so startCalibrationFromChain returns 0 and
        startCalibrationForEdit falls through to startCalibrationFromOps, which reaches this overload. */
    void begin (const juce::String& pluginName, int slotIndex, float bandLo, float bandHi, float openingDrive,
                Purpose p)
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
        openFromPurpose (p);           // the SAME derivation begin(Config) uses - not a second copy of it
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
        resetHoldBudget();          // ...and its own budget: the first excess it measures is the one it may spend
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
        // 21t-m item 3 (29 Sep 2026): ...AND NEITHER CAN THE HEARD CLOCK. lastHeardS is a reading of the tally
        // that measured the last judged window, and the staleness gate compares the NEXT window's heard time
        // against it. Sean's NEOLD U17 handed over at 12:58:28 carrying the first host's 126.4 s; the second
        // host's own tally started at zero, never passed it, and every window for the next NINE MINUTES logged
        // state=stale-window with heard frozen at 24.1 s. The gate is right; the number it compared was another
        // host's. The new host's first window is its own baseline.
        lastHeardS = -1.0f;
        // The settle's heard budget re-anchors on the new tally on the next window, keeping what was already
        // spent (see onWindow): the work done is not undone by who is doing it, and neither is it charged twice.
        settleStartHeardS = -1.0f;
        return log ("handover");
    }

    bool running() const { return state == State::Listening || state == State::Waiting; }
    /** 30 Sep 2026 ruling: THE HOLDING TAIL IS DELETED FOR EVERY PURPOSE. A loop that has said its one line is
        over - no settling, no holding, no stale windows, no re-post, ever. The pending text is kept (the chat
        takes it on a later call, out of the stored loop) and the state goes Idle so nothing judges another
        window: active() is what calibTick gates on, and Adjusted/Clamped were active. */
    void endHere()
    {
        if (closingOwed && closingOwedText.isEmpty()) closingOwedText = closingMessage();
        endedAs = state;
        state = State::Idle;
    }
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
        // 21t-m item 5 (29 Sep 2026 ruling): ...AND "STALE" IS NOT ONLY "DID NOT ADVANCE AT ALL". Sean's MV2
        // pass, windows 50-55:
        //     w50-54  slot 21.4s  state=stale-window     <- correctly rejected, the transport had stopped
        //     w55     slot 22.1s  state=settled          <- 0.7 s of new audio, JUDGED AS A WHOLE WINDOW
        // The gate compared against +1 ms, so the tail of a stopping transport counted as a fresh 3 s window and
        // spent one of the settle's three. A judged window has to be a WINDOW's worth of new audio, so the bar
        // is a fraction of the window the host actually measured rather than a constant that could drift from it.
        const float minFreshS = juce::jmax (0.25f, (float) (windowMs * 0.001) * kFreshWindowFraction);
        if (w.heardSeconds > 0.0f && lastHeardS >= 0.0f && w.heardSeconds < lastHeardS + minFreshS)
        {
            s.card = card();
            s.logLine = log (w.heardSeconds > lastHeardS + 1.0e-3f ? "stale-window (part)" : "stale-window");
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
                endHere();                          // 30 Sep 2026 ruling: Listen reports, and then it is over
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
                endHere();
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
        // (l) 30 Sep 2026 ruling: ON A COMPRESSOR SLOT THE INPUT HEADROOM DOES NOT BIND THE DRIVE. The ceiling
        // is the OUT's business and the hold owns it; the IN's only bound is the slot range. On any other slot the
        // 21p item 3 ceiling stands unchanged, which is why this reads the flag rather than deleting the arithmetic.
        headroomLimit = (! dynamicsSlot && w.inTruePeakDb > -190.0f)
                            ? preDb + (kCeilingDbTp - w.inTruePeakDb) : kDriveLimit;
        const bool up = want > preDb;
        const float upperLimit = up ? juce::jmin (kDriveLimit, headroomLimit) : kDriveLimit;
        if (up && want > upperLimit + 1.0e-4f && headroomLimit < kDriveLimit - 1.0e-4f)
        {
            // Stopped by HEADROOM, not by the budget: a different sentence, because it is a different fact and
            // the user can act on it (turn the source down, or accept less compression).
            state = State::Clamped; closingOwed = true; headroomStopped = true;
            s.finished = true; s.closing = closingMessage();
            s.card = card(); s.logLine = log ("clamped");
            endHere();
            return s;
        }
        if (steps >= kMaxSteps || std::abs (want) > kDriveLimit + 1.0e-4f || (up && want > upperLimit + 1.0e-4f))
        {
            state = State::Clamped; closingOwed = true;
            s.finished = true; s.closing = closingMessage();
            s.card = card(); s.logLine = log ("clamped");
            endHere();
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
            // 21t-m item 1: a comparative reopens the settle, and with it the hold - the drive is about to move,
            // so the level it was holding is about to change. Nothing is written until it lands again.
            if (s.writeDrive || s.writeParams) { freshWanted = kFreshAfterWrite; levelHeld = false; levelTrimmedDb = 0.0f; resetHoldBudget(); }
            s.card = card();
            s.logLine = log ((s.writeDrive || s.writeParams) ? "stepped" : "at-the-limit");
            return s;
        }

        // 21t-m item 1 (29 Sep 2026 ruling, SUPERSEDING 21t-j's "once per actuator position"): THE HOLD RUNS
        // ONCE, AFTER THE SETTLE HAS LANDED. Sean's demo logged level-hold-slot at windows 2, 9, 16 and 25 - one
        // per settle stage - and each of them re-read the same untouched +6.0 excess and added another -6, so the
        // slot gain walked -6, -12, -18, -24. Nothing is held BETWEEN drive steps any more: the drive lands
        // first, then one fresh window is measured at a point after EchoJay's own slot output gain, one absolute
        // correction is written, one re-measurement is taken, and at most one refinement follows. After that the
        // output is not touched again until the user asks. The hold block now lives below, past the settle.
        levelChangeDb = w.levelChangeDb;   // out minus in on SHORT90: the slot's own contribution, as ruled
        // THE RESIDUAL. levelChangeDb is taken BEFORE this gain (21t-k item 3, and it stays there - it is the
        // PLUGIN's change, which is what the card, the block and the reply quote). The residual is that change
        // plus what the hold has already written: what is still wrong at the slot's true output. The gain comes
        // from the control itself when the host supplies it; the loop's own belief is only the fallback, and
        // before the hold has written anything the reading is adopted, so a slot that already carried a gain is
        // not double-counted.
        const float stagingIn = (w.slotPreTrimDb == w.slotPreTrimDb) ? w.slotPreTrimDb : 0.0f;
        if (w.slotOutGainDb == w.slotOutGainDb)
        {
            if (holdWrites == 0 && std::abs (w.slotOutGainDb - slotGainDb) > 0.05f) slotGainDb = w.slotOutGainDb;
            levelResidualDb = levelChangeDb + w.slotOutGainDb + stagingIn;
        }
        else
            levelResidualDb = levelChangeDb + slotGainDb + stagingIn;

        // ---- THE SETTLE: the tail of the build (28 Sep 2026 ruling) ----------------------------------------
        // HEARD time counts, not clock time: a stop mid-settle simply stops adding to it, and playing resumes it.
        // 21t-m item 3: re-anchoring after a handover keeps the budget already spent - subtracting it here
        // means the new host's heard time continues the settle rather than restarting or inheriting it.
        if (settleStartHeardS < 0.0f) settleStartHeardS = slotHeardS - settleHeardS;
        settleHeardS = juce::jmax (0.0f, slotHeardS - settleStartHeardS);

        if (settling && ! landed)
        {
            const float gr = measuredGrDb();
            const bool inBand = (gr == gr) && gr >= lo - 0.05f && gr <= hi + 0.05f;
            // (q) 30 Sep 2026 ruling, NARROWING (m): ON A BUILD A COMPRESSOR SLOT GETS NO DRIVE SEEK AT ALL.
            // Compressor calibration is being redesigned around measured profiles, so the loop stops guessing at
            // a drive: IN stays where the dial left it, the hold matches the level on OUT once, and the line says
            // so. The seek code stays for a NON-compressor slot, which is what (m) built it for.
            const bool noSeek = (purpose == Purpose::buildHold && dynamicsSlot);
            const bool budget = noSeek
                                    ? false
                                    : (purpose == Purpose::buildHold
                                           ? (window <= kBuildMaxWindows)
                                           : (settleSteps < kSettleMaxSteps && settleHeardS <= kSettleMaxHeardS));
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
                // (m) 30 Sep 2026: A BUILD MOVES WINDOW BY WINDOW, as ruled. The three-fresh-window wait after a
                // write belongs to an ASK, whose one figure has to describe the setting the question is about; a
                // build is seeking, and waiting three windows per dB spent the whole 12-window cap on three dB.
                // ONE fresh window still separates the move from the reading, so the GR quoted is never measured
                // across the change itself.
                if (s.writeParams || s.writeDrive)
                    freshWanted = (purpose == Purpose::buildHold) ? 0 : kFreshAfterWrite;
                awaitFresh = true;
                s.card = card();
                s.logLine = log (s.writeParams || s.writeDrive ? "settling" : "settle-at-the-limit");
                if (s.writeParams || s.writeDrive) return s;
            }
            // LANDED: in the band, or the step budget or the heard-audio budget is spent.
            landed = true; settling = false;
            landedInBand = inBand;
            // 21t-m item 1 (ruled): ...AND NOW THE HOLD, ONCE, ON THE FINAL DRIVE. The closing line is not handed
            // out yet: it has to state what the hold wrote, and the hold has not measured a fresh window at this
            // drive position. The settle's landing opens the hold and waits; holdStep() below closes the line.
            {   // 21t-m (ruled): the hold always has a target - EchoJay's own OUT - so it always opens.
                holdOpen = true; holdDone = false; holdWrites = 0;
                holdBaseGainDb = slotGainDb;
                holdFirstExcessDb = std::numeric_limits<float>::quiet_NaN();
                judged = 0;
                // ONE FRESH WINDOW AT THE LANDED DRIVE, before anything is written - but only if the drive
                // actually MOVED. A compressor that was already in band took no step, so nothing has been
                // written since this window was measured and this window IS at the landed drive: making it wait
                // two more would be nine seconds of silence bought for nothing, on the ORDINARY case. Caught by
                // loudness_loop_guard 21t-g (6b), which asks for the line after three in-band windows.
                // 30 Sep 2026 ruling: A BUILD IS A ONE-SHOT - one measurement window, one write, one line. It
                // never waits for a fresh window here: Sean's 10:45 build spent FIVE windows getting to the hold
                // (landed at window 1, held at window 6) for a level match it could have made on the first.
                freshWanted = (purpose == Purpose::buildHold) ? 0 : (settleSteps > 0 ? kFreshAfterWrite : 0);
                if (freshWanted == 0)
                    return holdStep (s);   // nothing moved: judge this window and close the line now
                s.card = card();
                s.logLine = log (inBand ? "landed-holding"
                                        : (purpose == Purpose::buildHold
                                               ? (window > kBuildMaxWindows ? "landed-window-cap-holding"
                                                                            : "landed-slot-range-holding")
                                               : (settleSteps >= kSettleMaxSteps ? "landed-step-budget-holding"
                                                                                : "landed-heard-budget-holding")));
                return s;
            }
            asked = true;
            askOwed = completedLine();
            s.ask = askOwed;
            s.askReplacesOpening = true;
            s.card = card();
            s.logLine = log (inBand ? "landed" : (settleSteps >= kSettleMaxSteps ? "landed-step-budget"
                                                                                 : "landed-heard-budget"));
            return s;
        }

        // ---- 21t-m item 1 (29 Sep 2026 ruling): THE HOLD, ONCE, AFTER LANDING ------------------------------
        // At most TWO writes for the whole settle: one absolute correction, then one refinement if the
        // re-measurement is still more than kHoldRefineDb out. After that the output is not touched again until
        // the user asks. Nothing here runs between drive steps - that is the design fault this replaces.
        // COMP_PROFILE_SPEC_v1 section 7: THE ONE CHECK, BEFORE THE HOLD. It runs once per build on a compressor
        // slot that has a profile and an expected figure; whatever it decides, the hold then matches the level on
        // OUT once, as now. The decision itself is echojay::CompCheck, so it is the same on both hosts and a guard
        // drives all three outcomes with figures of its own.
        if (landed && holdOpen && ! holdDone && hasProfile && ! profileChecked)
        {
            CompCheck::Reading cr;
            cr.dialSettled = true;                       // landed: the dial settled before the settle landed
            cr.loudHeardSeconds = slotHeardS;
            cr.levelChangeDb = levelChangeDb;
            cr.expectedGrDb = profileExpectedGrDb;
            cr.expectedLevelDb = profileExpectedLevelDb;
            cr.staticGainDb = profileStaticGainDb;
            const auto cd = CompCheck::decide (cr);
            if (cd.outcome == CompCheck::Outcome::notYet)
            {   // not yet: say so once per window and let the next one try. The hold waits with it.
                s.card = card();
                s.logLine = log ("profile-check-waiting");
                return s;
            }
            profileChecked = true;
            profileObservedDropDb = cd.observedDropDb;
            if (cd.outcome == CompCheck::Outcome::tooMuch)
            {
                profileCorrected = true;
                profileCorrectionDb = cd.correctionDb;
                // THE TARGET POSITION, off the profile's own curve, from where the control actually is.
                profileAmountNormAfter = CompCheck::amountNormForLessGr (profileVar, profileCurrentNorm,
                                                                        cd.correctionDb);
                s.writeAmount = true;
                s.amountControl = profileAmountControl;
                s.amountNorm = profileAmountNormAfter;
                s.card = card();
                s.logLine = log ("profile-eased");
                return s;                                 // one move, then the next window holds
            }
            if (cd.outcome == CompCheck::Outcome::notEngaging)
                profileNotEngaging = true;
            // inRange, notEngaging and noCheck all fall through to the hold, which is section 7's last line.
        }
        if (landed && holdOpen && ! holdDone)
            return holdStep (s);

        // AFTER LANDING nothing moves except on a comparative, and nothing is said.
        s.card = card();
        s.logLine = log (landed ? "holding" : "measuring");
        return s;
    }

    /** 21t-m item 1: the hold, on the LANDED drive. One absolute write, one re-measurement, at most one
        refinement; the closing line is handed out when it is finished, so the sentence can state the total. */
    Step& holdStep (Step& s)
    {
        // 21t-m (29 Sep 2026): THE HOLD WRITES ECHOJAY'S OWN OUT, ALWAYS, AND IT WRITES THE WHOLE RESIDUAL.
        // Never a plugin's output control - a plugin's own output is left exactly where the build put it. The
        // slot has two EchoJay gains, IN and OUT, and the hold sets OUT.
        //
        // THE CAP IS GONE (ruled, tonight). It read "the total may never exceed the first measured excess plus
        // 1 dB", and on Sean's 21:54:16 compressor that turned a measured residual of 3.00 dB into a written
        // -1.00 and told him "Output -1.0 dB, its limit - the slot is still 2.0 dB out". The cap was protecting
        // against the wind-up that the once-after-landing rule had already made impossible, and all it did was
        // stop the hold finishing its job. THE ONLY LIMIT IS THE CONTROL'S OWN RANGE.
        const float excess = levelResidualDb;

        const float threshold = holdWrites == 0 ? kHoldOpenDb : kHoldRefineDb;
        if (std::abs (excess) > threshold && holdWrites < kHoldMaxWrites)
        {
            const float wasGain  = slotGainDb;
            const float wantGain = juce::jlimit (kSlotGainMinDb, kSlotGainMaxDb, slotGainDb - excess);
            if (std::abs (wantGain - wasGain) > 0.05f)
            {
                slotGainDb = wantGain;
                ++holdWrites;
                levelTrimmedDb = slotGainDb - holdBaseGainDb;        // the WRITTEN TOTAL, absolute
                levelHeld = true;
                // CLAMPED means the CONTROL ran out - it is at an end of its range and the residual is not
                // closed. Nothing else clamps any more, so this cannot be reported for any other reason.
                levelHoldClamped = std::abs ((wasGain - excess) - wantGain) > 0.05f;
                levelHoldLimitDb = slotGainDb;
                s.writeSlotGain = true; s.slotGainValue = slotGainDb;
                // 30 Sep 2026 ruling: a BUILD does not come back to refine. It sets OUT once so the level
                // matches, and closes on the same step - so the sentence states the write it just made.
                if (purpose != Purpose::buildHold)
                {
                    freshWanted = kFreshAfterWrite;
                    judged = 0;
                    s.card = card();
                    s.logLine = log (holdWrites == 1 ? "level-hold-slot" : "level-hold-slot-refine");
                    return s;
                }
                s.logLine = log ("level-hold-slot");
            }
            levelHoldClamped = true; levelHoldLimitDb = slotGainDb;  // the control is already at its end
        }
        // FINISHED: within the threshold, out of writes, or out of control. The closing line goes now, and it
        // states the written total.
        holdDone = true; holdOpen = false; asked = true;
        askOwed = completedLine();
        s.ask = askOwed;
        // A build posted no opening line (begin() left askOwed empty for it), so there is nothing of its own to
        // rewrite; an ask did, and its completion still replaces it.
        s.askReplacesOpening = purpose != Purpose::buildHold;
        s.card = card();
        const auto word = log (holdWrites == 0 ? "landed-level-already-held"
                                              : (holdWrites == 1 ? "landed-held-one-write"
                                                                 : "landed-held-two-writes"));
        s.logLine = word;
        // 30 Sep 2026 ruling: THE HOLDING TAIL IS DELETED FOR A BUILD. It has applied the working position,
        // measured once, set OUT once and said so. It is over: no settling, no holding windows, no re-post,
        // ever. Sean's 10:45 build logged 88 more windows at state=holding after its close. The band result is
        // in the line above and goes no further - a build reports what it did, not what it is still watching.
        // askOwed stays set: calibTakeAsk reads the STORED loop, so the line is still delivered after this.
        // ...and the same for an ASK (30 Sep 2026): the rung has said what it moved and what it measured, and
        // that is the whole of it. Before this, state stayed Listening and every later window logged
        // state=holding - for as long as the transport ran.
        endHere();
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
    /** "taking about 12 dB off: too much, check the threshold", or empty when the make-up is reasonable.
        ONE derivation, called by both closing-line branches - the flag-off (letter (q)) one and section 7's -
        because the same rule written twice is how (e)'s withdrawn settle budget survived in one of two begin()s
        until calib_link_guard caught it. The figure quoted is EchoJay's own OUT, which is what the hold wrote and
        therefore what the compressor took off. */
    juce::String overCompressionPhrase() const
    {
        if (! dynamicsSlot || ! (slotGainDb > kOverCompressionDb)) return {};
        juce::String p;
        p << "taking about " << juce::String (juce::roundToInt (slotGainDb))
          << " dB off: too much, check the threshold";
        // At the ceiling the number is not even the whole story - the trim ran out before the level was held.
        if (slotGainDb >= kSlotGainMaxDb - 0.05f)
            p << " (my output trim is at its +" << juce::String (juce::roundToInt (kSlotGainMaxDb))
              << " dB ceiling, so it may be taking off more than this)";
        return p;
    }

    juce::String completedLine() const
    {
        // 21t-m item 2 (29 Sep 2026 ruling): TWO CLOSING LINES, one per purpose.
        // A BUILD: "Built. Level held, Output -N dB." - what it did, in one line, with no promise to keep.
        if (purpose == Purpose::buildHold)
        {
            // (m) 30 Sep 2026 ruling: the build's one line carries THE THREE NUMBERS - the IN it moved, the gain
            // reduction it read, the OUT it set - and says so in the same line when the band was not reached.
            // Before this it said only what the hold wrote, so a build that never moved the drive read "Level
            // already matched, nothing to hold." and told Sean nothing about the compressor at all.
            juce::String b = "Built.";
            // (q): A COMPRESSOR IS SET AS DIALLED. It was not driven, so there is no drive figure to report and no
            // band to have reached or missed - only what the hold did about the level.
            if (dynamicsSlot && ! profilesFeatureOn)
            {
                // THE FLAG IS OFF, SO THIS IS LETTER (q)'s LINE, WORD FOR WORD (restored 2 Oct 2026).
                // Item 4 rewrote this whole branch to section 7's wording and did it OUTSIDE the flag, so with
                // the feature off a compressor build stopped saying what (q) ruled it should say and started
                // saying "no profile yet" to users who have no profiles and no way to get one. Part 2's rule is
                // "behind a flag, default OFF", and a closing line is behaviour. Found by level_loop_guard (16),
                // which asserts (q)'s wording and was the only thing standing between this and a shipped build.
                // OVER-COMPRESSION OUTRANKS "matched" (2 Oct 2026). The level IS matched in this case - that is
                // what the hold just did - so the old line was not false, only useless: it reported success while
                // the compressor pulled 12 dB down. What the user needs is the cause, not the symptom.
                if (const auto over = overCompressionPhrase(); over.isNotEmpty())
                    b << " Set as dialled, " << over << ". Output " << signed1 (slotGainDb) << " dB.";
                else
                {
                    b << " Set as dialled, level ";
                    if (std::abs (levelTrimmedDb) > 0.05f)
                        b << "matched, Output " << signed1 (slotGainDb) << " dB.";
                    else if (std::abs (levelResidualDb) > 1.0f)
                        b << "NOT matched - the slot is " << juce::String (std::abs (levelResidualDb), 1) << " dB "
                          << (levelResidualDb > 0.0f ? "louder" : "quieter")
                          << " out than in and my output trim has no more to give.";
                    else
                        b << "already matched.";
                }
                return b;
            }
            if (dynamicsSlot)
            {
                // COMP_PROFILE_SPEC_v1 item 4 / section 7: THE LINE SAYS WHERE THE SETTING CAME FROM, and names
                // the plugin, because on a two-compressor build the user needs to know which one it is about.
                //   with a profile: "EMO-D5: about 2 dB on the loud phrases, from its profile. Output -1.5 dB."
                //   without one:    "NEOLD U2A: set as dialled, no profile yet."
                if (hasProfile)
                {
                    juce::String p = plugin + ":";
                    const float gr = measuredGrDb();
                    if (gr == gr) p << " about " << juce::String (juce::roundToInt (gr)) << " dB on the loud phrases,";
                    else          p << " no gain-reduction reading yet,";
                    p << " from its profile.";
                    if (profileNotEngaging)
                        p << " It is not compressing at all - the profile looks wrong and I have reported it.";
                    else if (profileCorrected)
                        p << " I eased it back " << juce::String (profileCorrectionDb, 1) << " dB.";
                    if (std::abs (levelTrimmedDb) > 0.05f) p << " Output " << signed1 (slotGainDb) << " dB.";
                    // Section 7 says what the profile asked for; this says what it actually cost. A profile that
                    // needs 12 dB of make-up is wrong for this material whatever its own numbers claim.
                    if (const auto over = overCompressionPhrase(); over.isNotEmpty())
                        p << " It is " << over << ".";
                    return p;
                }
                juce::String p = plugin + ": set as dialled, no profile yet.";
                if (const auto over = overCompressionPhrase(); over.isNotEmpty()) p << " It is " << over << ".";
                if (std::abs (levelTrimmedDb) > 0.05f) p << " Output " << signed1 (slotGainDb) << " dB.";
                else if (std::abs (levelResidualDb) > 1.0f)
                    p << " The slot is " << juce::String (std::abs (levelResidualDb), 1) << " dB "
                      << (levelResidualDb > 0.0f ? "louder" : "quieter")
                      << " out than in and my output trim has no more to give.";
                return p;
            }
            const float gr = measuredGrDb();
            if (std::abs (preDb) > 0.05f)
                b << " Drive " << signed1 (preDb) << " dB,";
            if (gr == gr)
                b << " " << juce::String (gr, 1) << " dB of gain reduction";
            else
                b << " no gain-reduction reading";
            if (! landedInBand)
                b << " - the " << juce::String (lo, 0) << "-" << juce::String (hi, 0) << " dB band was not reached"
                  << (std::abs (std::abs (preDb) - kDriveLimit) < 0.05f
                          ? juce::String (" and the drive is at its limit")
                          : juce::String());
            if (std::abs (levelTrimmedDb) > 0.05f)
                b << ". Level held, Output " << signed1 (slotGainDb) << " dB.";
            else if (std::abs (levelResidualDb) > 1.0f)
                b << ". The slot is " << juce::String (std::abs (levelResidualDb), 1) << " dB "
                  << (levelResidualDb > 0.0f ? "louder" : "quieter")
                  << " out than in and my output trim has no more to give.";
            else
                b << ". Level already matched, nothing to hold.";
            return b;
        }
        // AN ASK: what it moved, and what the meters read for it.
        if (purpose == Purpose::askRung && steps > 0)
        {
            juce::String a = knobText() + " " + signed1 (value - (writesNamedParam() ? stepDb * (float) senseSign : kStepDb))
                           + juce::String::fromUTF8 (" \xe2\x86\x92 ") + signed1 (value);
            const float gr = measuredGrDb();
            if (gr == gr) a << ", " << juce::String (gr, 1) << " dB on the loud phrases";
            a << (std::abs (levelResidualDb) <= 1.0f ? ", level held" : ", level NOT held");
            return a + ".";
        }
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
            out += levelHoldClamped
                     ? " Output " + signed1 (levelHoldLimitDb) + " dB, its limit - the slot is still "
                       + juce::String (std::abs (levelResidualDb), 1)
                       + " dB out and I cannot hold the rest."
                     // 21t-m item 1 (ruled): the WRITTEN TOTAL. Sean's demo wrote -24.00 over four passes and
                     // this sentence said "Output trimmed 6.0 dB", which was the last move.
                     : " Output trimmed " + juce::String (std::abs (levelTrimmedDb), 1) + " dB in total to hold the level.";
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
            level = levelHoldClamped
                      ? " Output " + signed1 (levelHoldLimitDb) + " dB, its limit - the slot is still "
                        + juce::String (std::abs (levelResidualDb), 1)
                        + " dB out and I cannot hold the rest."
                      // 21t-m item 1 (ruled): the WRITTEN TOTAL, not the last move.
                      : " Output trimmed " + juce::String (std::abs (levelTrimmedDb), 1) + " dB in total to hold the level.";
        else if (std::abs (levelResidualDb) > 1.0f)
            // 21t-m (ruled): the hold always writes EchoJay's own OUT, so "I have no output control for it here"
            // is no longer a thing that can be true. What CAN be true is that OUT ran out of range.
            level = " The slot is still " + juce::String (std::abs (levelResidualDb), 1) + " dB "
                  + (levelResidualDb > 0.0f ? "louder" : "quieter")
                  + " out than in and my output trim has no more to give.";
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
        o->setProperty ("slotIdent", slotIdent);
        o->setProperty ("endedAs", (int) endedAs);
        // 30 Sep 2026: THE PURPOSE RIDES THE SIDECAR. It was in neither direction, and for an OWN rack that never
        // showed - calibStore holds a live CalibLoop there. On a LINK rack every tick round-trips through this,
        // so a build was a build for one tick and an askRung from the second, which is what put the measure-and-ask
        // line on Sean's 10:45 build ("... Doing about 0.1 dB of gain reduction").
        o->setProperty ("purpose", (int) purpose);
        o->setProperty ("dynamicsSlot", dynamicsSlot);   // (l): a handover must not put the ceiling back
        o->setProperty ("lo", (double) lo);     o->setProperty ("hi", (double) hi);
        o->setProperty ("preDb", (double) preDb);
        o->setProperty ("steps", steps);        o->setProperty ("window", window);
        o->setProperty ("inBandRun", inBandRun); o->setProperty ("phraseIdx", phraseIdx);
        o->setProperty ("lastGr", (lastGr == lastGr) ? juce::var ((double) lastGr) : juce::var());
        o->setProperty ("noSignalMs", noSignalMs);
        o->setProperty ("awaitFresh", awaitFresh);
        o->setProperty ("state", (int) state);
        o->setProperty ("closingOwed", closingOwed);
        o->setProperty ("closingOwedText", closingOwedText);
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
        // 21t-m item 1: the hold's budget rides the handover too, or the second host would open a fresh budget
        // mid-settle and be entitled to spend the whole excess again - the wind-up by another road.
        o->setProperty ("holdOpen", holdOpen);
        o->setProperty ("holdDone", holdDone);
        o->setProperty ("holdWrites", holdWrites);
        o->setProperty ("holdBaseGainDb", (double) holdBaseGainDb);
        o->setProperty ("holdFirstExcessDb", (double) holdFirstExcessDb);
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
        c.slotIdent = o->getProperty ("slotIdent").toString();
        if (o->hasProperty ("endedAs")) c.endedAs = (State) (int) o->getProperty ("endedAs");
        c.dynamicsSlot = (bool) o->getProperty ("dynamicsSlot");
        if (o->hasProperty ("purpose"))
            c.purpose = ((int) o->getProperty ("purpose") == (int) Purpose::buildHold) ? Purpose::buildHold
                                                                                      : Purpose::askRung;
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
        c.closingOwedText = o->getProperty ("closingOwedText").toString();
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
          c.holdOpen   = o->hasProperty ("holdOpen") ? (bool) o->getProperty ("holdOpen") : false;
          c.holdDone   = o->hasProperty ("holdDone") ? (bool) o->getProperty ("holdDone") : c.levelHeld;
          c.holdWrites = o->hasProperty ("holdWrites") ? (int) o->getProperty ("holdWrites") : (c.levelHeld ? 1 : 0);
          c.holdBaseGainDb = o->hasProperty ("holdBaseGainDb") ? (float) (double) o->getProperty ("holdBaseGainDb") : c.slotGainDb;
          c.holdFirstExcessDb = o->hasProperty ("holdFirstExcessDb")
                                  ? (float) (double) o->getProperty ("holdFirstExcessDb")
                                  : std::numeric_limits<float>::quiet_NaN();
          const auto sp = o->getProperty ("senseParams").toString();
          if (sp.isNotEmpty()) c.senseParams.addLines (sp);
          c.grReadable = (bool) o->getProperty ("grReadable");
          const auto sg = o->getProperty ("sensedGrDb");
          c.sensedGrDb = sg.isVoid() ? std::numeric_limits<float>::quiet_NaN() : (float) (double) sg; }
        c.lastHeardS = o->hasProperty ("lastHeardS") ? (float) (double) o->getProperty ("lastHeardS") : -1.0f;
        return c;
    }
    bool active() const { return state != State::Idle; }

    /** 21t-m item 1: a new actuator position (or a fresh build) owes a new hold, with its own budget: the first
        excess IT measures is the one it may spend, and no correction from the position before it carries over. */
    void resetHoldBudget() noexcept
    {
        holdOpen = false; holdDone = false; holdWrites = 0;
        holdBaseGainDb = slotGainDb;
        holdFirstExcessDb = std::numeric_limits<float>::quiet_NaN();
        levelResidualDb = 0.0f;
    }

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
                      // 21t-k item 3: BOTH figures, because one of them jumped. `heard` is the settle's own budget
                      // (slot heard MINUS where the settle opened) and the slot's is the tally's gated total;
                      // Sean's Zip log read 0.0 s for ten windows and then 15.1 s, which is the slot's counter
                      // reporting nothing until its in leg bound and then reporting all of it at once. With both
                      // printed, that is readable in the line instead of inferred from it.
                      + " heard=" + juce::String (settleHeardS, 1) + "s (slot " + juce::String (slotHeardS, 1) + "s)"
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
