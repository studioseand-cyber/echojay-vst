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
namespace echojay
{

struct CalibLoop
{
    enum class State { Idle, Listening, Waiting, Adjusted, Clamped };

    static constexpr float kStepDb      = 1.0f;    // one move, ruled
    static constexpr float kDriveLimit  = 12.0f;   // +/- , ruled
    static constexpr int   kMaxSteps    = 6;       // ruled
    static constexpr int   kInBandRuns  = 2;       // two CONSECUTIVE windows in the band
    static constexpr double kNoSignalMs = 30000.0; // ruled
    static constexpr float kCeilingDbTp = -3.0f;   // the input ceiling the drive may not push past (21p item 3)

    // ---- the state that rides the sidecar ----
    juce::String plugin;               // the slot's plugin name, for the card and the log
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
        juce::String card;          // what the card says now
        juce::String logLine;       // EJCalib: ... , one per window
        bool  finished   = false;   // the loop ended on this window
        juce::String closing;       // the closing message, when it ended
    };

    void begin (const juce::String& pluginName, int slotIndex, float bandLo, float bandHi, float openingDrive)
    {
        plugin = pluginName; slot = slotIndex;
        lo = juce::jmin (bandLo, bandHi); hi = juce::jmax (bandLo, bandHi);
        preDb = openingDrive;
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

        // (4) OUT OF THE BAND: one step toward it, unless the budget, the drive limit or the HEADROOM says stop.
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
        s.writeDrive = true; s.newPre = preDb; s.newPost = -preDb;
        s.card = card(); s.logLine = log ("listening");
        return s;
    }

    juce::String card() const
    {
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
        return c;
    }
    bool active() const { return state != State::Idle; }

    juce::String log (const char* stateWord) const
    {
        return "EJCalib: \"" + plugin + "\" window " + juce::String (window)
             + " gr=" + grText() + " pre=" + signed1 (preDb) + " post=" + signed1 (-preDb)
             + " state=" + stateWord;
    }

private:
    static juce::String signed1 (float v)
    { return (v >= 0.0f ? "+" : "") + juce::String (v, 1); }
    juce::String grText() const
    { return (lastGr == lastGr) ? juce::String (lastGr, 1) : juce::String ("--"); }
    juce::String driveText() const { return signed1 (preDb); }
};

} // namespace echojay
