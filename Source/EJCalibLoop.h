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
    enum class Actuator { Drive, Threshold };

    static constexpr float kStepDb      = 1.0f;    // one move, ruled
    static constexpr float kDriveLimit  = 12.0f;   // +/- , ruled
    static constexpr int   kMaxSteps    = 6;       // ruled
    static constexpr int   kInBandRuns  = 2;       // two CONSECUTIVE windows in the band
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

    // ---- one 3 s window, as the host measured it ----
    struct Window
    {
        bool  measured = false;   // a full window with no dropped frames
        bool  silent   = false;   // below the gate: nothing was playing
        float grDb     = 0.0f;    // slot-in LUFS minus slot-out LUFS
        // 21t-d: the slot's INPUT true peak, as measured at the drive this window ran at. The drive limit is not
        // a fixed +12: it is whatever drive brings this figure to -3 dBTP, because past that the loop would be
        // buying gain reduction with a clipped input. -200 = the host could not read it (then +12 alone applies).
        float inTruePeakDb = -200.0f;
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
        juce::String closing;       // the closing message, when it ended
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
            { out.lo = (float) (double) band->getUnchecked (0); out.hi = (float) (double) band->getUnchecked (1); }

        // ---- mode: exactly two strings --------------------------------------------------------------------
        const auto modeS = o->getProperty ("mode").toString().trim();
        if (modeS == "listen")        out.mode = Mode::Listen;
        else if (modeS == "passive")  out.mode = Mode::Passive;
        else
        {
            out.mode = Mode::Passive;      // the quiet default, per the ruling: a block that says nothing is passive
            whyOut << "mode \"" << modeS << "\" is not \"passive\" or \"listen\" - running PASSIVE. ";
        }

        // ---- actuator: exactly two strings ----------------------------------------------------------------
        const auto actS = o->getProperty ("actuator").toString().trim();
        if (actS == "threshold")   out.actuator = Actuator::Threshold;
        else if (actS == "drive")  out.actuator = Actuator::Drive;
        else
        {
            out.actuator = Actuator::Drive;
            whyOut << "actuator \"" << actS << "\" is not \"threshold\" or \"drive\" - running the DRIVE. ";
        }

        // ---- param: one name, or an array of them (a pair moves together) ---------------------------------
        {
            const auto pv = o->getProperty ("param");
            if (auto* pa = pv.getArray())
                for (const auto& e : *pa) { const auto n = e.toString().trim(); if (n.isNotEmpty()) out.params.add (n); }
            else if (pv.toString().trim().isNotEmpty()) out.params.add (pv.toString().trim());
        }

        // ---- sense: two strings or null, and null with a threshold is a VIOLATION -------------------------
        const auto senseS = o->getProperty ("sense").toString().trim();
        const bool senseKnown = (senseS == "lower_is_harder" || senseS == "higher_is_harder");
        out.senseSign = (senseS == "higher_is_harder") ? 1 : -1;
        if (out.actuator == Actuator::Threshold && ! senseKnown)
        {
            out.actuator = Actuator::Drive;
            out.params.clear();
            whyOut << "actuator \"threshold\" with sense \"" << (senseS.isEmpty() ? juce::String ("null") : senseS)
                   << "\" - which way that knob compresses harder is not ours to guess, so the DRIVE runs instead. ";
        }
        if (out.actuator == Actuator::Threshold && out.params.isEmpty())
        {
            out.actuator = Actuator::Drive;
            whyOut << "actuator \"threshold\" names no param - running the DRIVE. ";
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
        if (o->hasProperty ("min_db")) out.minDb = (float) (double) o->getProperty ("min_db");
        if (o->hasProperty ("max_db")) out.maxDb = (float) (double) o->getProperty ("max_db");

        // ---- source and measure: logged, not acted on, and their literals are named so a new one shows ----
        {
            const auto src = o->getProperty ("source").toString().trim();
            const auto mea = o->getProperty ("measure").toString().trim();
            if (src.isNotEmpty() && src != "tally" && src != "listen")
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
        // ONE current value, whichever knob is being dialled: the drive keeps preDb (the mirror needs it), the
        // threshold keeps value. Both are set so a log line and a closing sentence can be written either way.
        value = c.startDb;
        preDb = (actuator == Actuator::Drive) ? c.startDb : 0.0f;
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
        state = State::Listening;
    }

    /** A NEW target while the loop is running restarts it FROM THE CURRENT DRIVE (ruled): the band changed, the
        drive it has already found has not. */
    void retarget (float bandLo, float bandHi)
    {
        lo = juce::jmin (bandLo, bandHi); hi = juce::jmax (bandLo, bandHi);
        steps = 0; inBandRun = 0; awaitFresh = true; closingOwed = false;
        state = State::Listening;
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

    Step onWindow (const Window& w, double windowMs)
    {
        Step s;
        ++window;
        if (! running()) { s.card = card(); return s; }

        // (1) NOT A MEASUREMENT. Dropped frames mean the host did not see a whole window; that is not evidence of
        // anything and cannot move the drive or the step count. It counts toward the no-signal clock ONLY if it
        // was also silent - "nothing arrived" and "nothing was playing" are different facts.
        if (! w.measured)
        {
            if (w.silent) noSignalMs += windowMs;
            if (noSignalMs >= kNoSignalMs) state = State::Waiting;
            s.card = card(); s.logLine = log (w.silent ? "waiting" : "listening");
            return s;
        }
        if (w.silent)
        {
            noSignalMs += windowMs;
            if (noSignalMs >= kNoSignalMs) state = State::Waiting;
            s.card = card(); s.logLine = log ("waiting");
            return s;
        }

        // Signal is back: the wait ends where it started, with nothing changed while it waited.
        noSignalMs = 0.0;
        if (state == State::Waiting) state = State::Listening;
        lastGr = w.grDb;

        // (2) THE FRESH WINDOW after a move or a handover is seen, logged and NOT judged: it may straddle the
        // change, and a decision taken on it would be a decision about two different racks.
        if (awaitFresh) { awaitFresh = false; s.card = card(); s.logLine = log ("listening"); return s; }

        // (3) IN THE BAND, twice running, ends it.
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

        // (4) OUT OF THE BAND: one step toward it, unless the budget, the range or the HEADROOM says stop.
        //
        // THE THRESHOLD ACTUATOR takes the simpler road: a threshold costs the slot's input no headroom, so the
        // only limits are the step budget and the profile's own range. Which way is "harder" comes from the
        // profile's sense, never from a guess - a dB threshold compresses harder as it falls, and a control whose
        // sense was not sampled is not this actuator at all (the server sends "drive" for those).
        if (actuator == Actuator::Threshold)
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
        // PASSIVE: one line, and only if something actually moved. A compressor that was already in band needed
        // nothing, and a message saying so is noise in a chat the user did not ask a question in.
        if (mode == Mode::Passive)
        {
            if (steps == 0) return {};
            return "Adjusted the " + plugin + " " + knobText() + " to " + driveText() + " dB.";
        }
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
        return c;
    }
    bool active() const { return state != State::Idle; }

    juce::String log (const char* stateWord) const
    {
        // The log names the MODE and the KNOB, because "the loop moved something" is not readable a week later
        // without them - a passive threshold pass and a listen drive pass look identical otherwise.
        return "EJThreshold: \"" + plugin + "\" window " + juce::String (window)
             + " gr=" + grText()
             + (actuator == Actuator::Threshold
                    ? " " + knobText() + "=" + signed1 (value)
                    : " pre=" + signed1 (preDb) + " post=" + signed1 (-preDb))
             + " mode=" + juce::String (mode == Mode::Passive ? "passive" : "listen")
             + " state=" + stateWord;
    }

    /** What is being dialled, in the user's words: the profile's own control name, or "drive". */
    juce::String knobText() const
    {
        if (actuator == Actuator::Threshold && ! params.isEmpty())
            return params.size() == 1 ? params[0] : params.joinIntoString (" + ");
        return "drive";
    }

private:
    static juce::String signed1 (float v)
    { return (v >= 0.0f ? "+" : "") + juce::String (v, 1); }
    juce::String grText() const
    { return (lastGr == lastGr) ? juce::String (lastGr, 1) : juce::String ("--"); }
    // The number the closing line quotes: whichever knob this loop is dialling.
    juce::String driveText() const { return signed1 (actuator == Actuator::Threshold ? value : preDb); }
};

} // namespace echojay
