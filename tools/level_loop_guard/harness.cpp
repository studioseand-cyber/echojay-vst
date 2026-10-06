// level_loop_guard (21t-m, 29 Sep 2026) - THE END-TO-END LEVEL HARNESS.
//
// Sean's ruling that made this file exist: "The hold has now been fixed six times from Pro Tools logs, one
// symptom a round. That stops here: the RED for items 1 and 2 lives in an END-TO-END LEVEL HARNESS, not in a
// unit leg." So: a real EchoJayProcessor, its real ChainHost, the real LevelTally taps, the real EJCalibLoop
// driven through calibStart/calibTick, and generated audio through processBlock. Nothing is mocked except the
// plugin in the slot, which is an EchoJay Gain standing in for a compressor whose gain is a fixed number.
//
//   (1) CHANNEL, +6 dB slot. The chain out - measured AFTER the hold's slot output gain - equals the chain in
//       within 0.5 dB once the hold has written, and STAYS there for three more windows. At most TWO writes for
//       the whole settle (29 Sep ruling: the hold runs ONCE, on the landed drive, never between drive steps),
//       the second smaller than 0.5 dB, and none afterwards. The written total never exceeds the first measured
//       excess plus 1 dB, and the closing sentence STATES THAT TOTAL.
//       RED on the pre-fix tree: four holds, -6 / -12 / -18 / -24, sentence "Output trimmed 6.0 dB".
//   (2) the same at -6 dB (the slot quieter than its input) and at 0 dB (nothing owed, no write at all).
//   (3) BUS role. No chain-output write and no stored-record restore lands; the chain's last stage sets the
//       level, and the chain block prints the chain's own out separately from the last slot's.
//   (4) HANDOVER mid-loop: the second host gets a FRESH window within two windows instead of sitting in
//       state=stale-window - Sean's NEOLD U17 sat there for nine minutes with heard stuck at 24.1 s.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "ChainHost.h"
#include "EJCompCheck.h"   // 4 Oct: leg (30) drives the in_at_gr_dbfs ladder directly
#include "EchoJayAPI.h"
#include "EedDeviceRegistry.h"
// EedGainProcessor.cpp holds the BuiltinDeviceRegistrar for "EchoJay Gain", and a registrar is a static object
// with nothing referencing it - the linker drops the whole object file unless a symbol from it is used. The
// first cut of this guard did not, the registry came back empty, and every slot leg failed on an empty rack.
// This reference is what pulls the device in; level_slot_guard gets it for free by using the Level processor.
#include "EedGainProcessor.h"
#include "EedLevelProcessor.h"
#include <cstdio>
#include <cmath>
#include <memory>

// 5 Oct 2026 (Sean's ruling): THIS HARNESS DRIVES ITS OWN WINDOWS, so it opts out of the fresh-window wait
// EXPLICITLY, per loop. In the product an unknown heard-clock means WAIT, because a begin site that forgot to fill
// it would silently bring back the stale reading item 3 closed. A synthetic leg has no clock to supply - it sets
// Window::heardSeconds by hand - so it is the one legitimate caller that must say so out loud. Routed through one
// helper rather than stamped on seventy Config declarations, so the opt-out is auditable in a single place.
static void beginDriven (echojay::CalibLoop& l, echojay::CalibLoop::Config c)
{ c.noFreshWait = true; l.begin (c); }

struct EchoJayBorrowHostTestAccess
{
    static juce::String loadBuiltin (ChainHost& h, const juce::PluginDescription& d) { return h.loadBuiltinNow (d); }
};

namespace {
int failures = 0;
void check (bool ok, const juce::String& w, const juce::String& d = {})
{ std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
juce::String f1 (float v) { return juce::String (v, 2); }
void pumpMs (int ms)
{ const double end = juce::Time::getMillisecondCounterHiRes() + ms;
  while (juce::Time::getMillisecondCounterHiRes() < end) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, false); } }

// Programme: pink-ish noise with a slow envelope, so SHORTMAX and SHORT90 differ and a crest figure exists.
struct Programme
{
    juce::Random rng { 20260929 };
    float b0 = 0, b1 = 0, b2 = 0;
    int   n = 0;
    float next()
    {
        const float white = rng.nextFloat() * 2.0f - 1.0f;
        b0 = 0.99765f * b0 + white * 0.0990460f;
        b1 = 0.96300f * b1 + white * 0.2965164f;
        b2 = 0.57000f * b2 + white * 1.0526913f;
        const float pink = (b0 + b1 + b2 + white * 0.1848f) * 0.12f;
        // one loud bar in four, so the crest is a real figure rather than a constant
        const float env = ((n++ / 24000) % 4 == 0) ? 1.0f : 0.45f;
        return pink * env;
    }
};

/** Feed `seconds` of programme through the processor, in 512-sample blocks. */
void feed (EchoJayProcessor& proc, Programme& prog, double seconds)
{
    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer midi;
    const int blocks = (int) std::round (seconds * 48000.0 / 512.0);
    for (int b = 0; b < blocks; ++b)
    {
        for (int i = 0; i < 512; ++i)
        {
            const float s = prog.next();
            buf.setSample (0, i, s);
            buf.setSample (1, i, s * 0.98f);
        }
        proc.processBlock (buf, midi);
    }
}

/** One slot holding an EchoJay Gain at a fixed dB: the fake compressor. */
struct Rig
{
    std::unique_ptr<EchoJayProcessor> procPtr { std::make_unique<EchoJayProcessor>() };
    EchoJayProcessor& proc { *procPtr };
    ChainHost& h;
    Programme prog;

    // 21t-m (29 Sep 2026 ruling): the harness runs on a VIRTUAL clock so five end-to-end cases cost seconds
    // rather than minutes on every fast gate. The rate limit itself is proved at REAL time, by its own case.
    double virtualMs = 1000.0;

    Rig (ChannelType role, float slotGainDb, bool realClock = false, const juce::String& trackName = {},
         int placement = 0) : h (proc.getChainHost())
    {
        proc.prepareToPlay (48000.0, 512);
        if (! realClock) proc.calibClockMsForTest = [this] { return virtualMs; };
        if (trackName.isNotEmpty()) h.setHostTrackName (trackName);
        proc.setSelfPlacement (placement);
        proc.setChannelType (role);
        static const auto pullInGainDevice = EedGainProcessor::schema().params().size();   // see the include note
        juce::ignoreUnused (pullInGainDevice);
        const auto* gn = BuiltinDeviceRegistry::instance().findByName ("EchoJay Gain");
        check (gn != nullptr, "precondition: EchoJay Gain is registered");
        if (gn != nullptr)
            EchoJayBorrowHostTestAccess::loadBuiltin (h, BuiltinDeviceRegistry::descriptionFor (*gn));
        pumpMs (150);
        setFakeCompGain (slotGainDb);
    }
    void setFakeCompGain (float db)
    {
        auto* pp = new juce::DynamicObject(); pp->setProperty ("level_db", (double) db);
        auto* w  = new juce::DynamicObject(); w->setProperty ("params", juce::var (pp));
        h.setSlotStructuredSettings (0, juce::var (w));
        pumpMs (100);
    }
    float slotOut() const { return h.getSlotOutGainDb (0); }
    // THE SHORT-TERM figure, not the integrated one. levelDb is the gated INTEGRATED level over everything the
    // chain has heard since the run began, so after two minutes of audio a 3 dB change to the last stage moved
    // it by 0.23 dB and the first cut of this guard read that as "the write never reached the audio". What the
    // hold is judged on is the level NOW, which is the short-term reading.
    float chainInDb()  const { const auto s = h.getChainInLevels();  return s.known ? s.shortTermDb : std::numeric_limits<float>::quiet_NaN(); }
    float chainOutDb() const { const auto s = h.getChainOutLevels(); return s.known ? s.shortTermDb : std::numeric_limits<float>::quiet_NaN(); }
    // ...and the INTEGRATED one where a record's value is what is being compared (the restore legs).
    float chainInIntDb() const { const auto s = h.getChainInLevels(); return s.known ? s.levelDb : std::numeric_limits<float>::quiet_NaN(); }
    float slotPreTrim() const { return h.getSlotPreTrimDb (0); }
    /** 21t-m item 2: the Level slot's own gain, by name, so a leg can prove Listen did not move it. */
    float levelGainDb() const
    {
        for (int i = 0; i < h.getNumSlots(); ++i)
            if (h.getSlotInfo (i).name == "EchoJay Level")
                if (auto* lv = dynamic_cast<EedLevelProcessor*> (h.getSlotProcessor (i)))
                    return (float) lv->gainDb();
        return 0.0f;
    }
};

echojay::CalibLoop::Config passiveDriveCfg (const char* name)
{
    echojay::CalibLoop::Config c;
    c.plugin = name; c.slot = 0; c.lo = 2.0f; c.hi = 3.0f;
    c.mode = echojay::CalibLoop::Mode::Passive;
    c.actuator = echojay::CalibLoop::Actuator::Drive;
    c.startDb = 0.0f; c.heardS = 90.0f;
    return c;
}

/** Run the loop to completion (or to `maxWindows`), 3 s of real audio per window, recording every write to
    EchoJay's own slot output gain and every closing line. */
struct RunResult
{
    std::vector<float> slotGainWrites;   // the value OUT landed on, per HOLD write (21t-m)
    std::vector<float> everyOutWrite;    // ...and every change to OUT, drive rungs included
    juce::String closing;
    juce::StringArray logLines;
    int windows = 0;
    float chainInAtEnd = 0.0f, chainOutAtEnd = 0.0f;
    std::vector<float> chainDeltaAfterHold;   // chain out - chain in, one per window after the first write
};

RunResult runLoop (Rig& r, int maxWindows, double secondsPerWindow = 3.0)
{
    RunResult out;
    float lastGain = r.slotOut();
    bool wroteOnce = false;
    // calibTick takes ONE decision per 3 s WINDOW, and the window is wall-clock ("if (sinceMs < 3000.0) return"
    // in PluginProcessor::calibTick). Feeding three seconds of audio takes a fifth of a second here, so the first
    // cut of this guard ran fourteen ticks inside two seconds and the loop judged NOT ONE window. The window is
    // advanced on the rig's virtual clock instead; the rate limit is proved at real time by case (5).
    for (int k = 0; k < maxWindows; ++k)
    {
        feed (r.proc, r.prog, secondsPerWindow);
        r.virtualMs += 3050.0;
        r.proc.calibTick ({});
        ++out.windows;
        out.logLines.add (r.proc.calibLastLogLine());
        // 21t-m: OUT is written by TWO things now - the drive's post-cut (every rung) and the hold. A change in
        // the control is therefore no longer a proxy for "the hold wrote". The HOLD's writes are the ones whose
        // window logged level-hold-slot; every other change is the drive doing its job.
        const float g = r.slotOut();
        const bool heldThisWindow = ! out.logLines.isEmpty()
                                    && out.logLines[out.logLines.size() - 1].contains ("level-hold-slot");
        if (std::abs (g - lastGain) > 0.005f)
        {
            out.everyOutWrite.push_back (g);
            if (heldThisWindow) { out.slotGainWrites.push_back (g); wroteOnce = true; }
            lastGain = g;
            // 21t-m: "it stays level" is counted from the HOLD's write, not from the drive's first rung. Between
            // the two the plugin's own change is still uncorrected by design, and counting there would be asking
            // the chain to be level before anything had corrected it.
        }
        else if (wroteOnce)
        {
            const float ci = r.chainInDb(), co = r.chainOutDb();
            if (ci == ci && co == co) out.chainDeltaAfterHold.push_back (co - ci);
        }
        bool replaces = false;
        const auto ask = r.proc.calibTakeAsk ({}, &replaces);
        if (ask.isNotEmpty()) out.closing = ask;
    }
    out.chainInAtEnd = r.chainInDb(); out.chainOutAtEnd = r.chainOutDb();
    return out;
}

void reportRun (const char* what, const RunResult& rr)
{
    juce::String w;
    for (size_t i = 0; i < rr.slotGainWrites.size(); ++i) w += (i ? ", " : "") + juce::String (rr.slotGainWrites[i], 2);
    juce::String ow;
    for (size_t i = 0; i < rr.everyOutWrite.size(); ++i) ow += (i ? ", " : "") + juce::String (rr.everyOutWrite[i], 2);
    std::printf ("    %s: %d window(s), %d HOLD write(s) [%s]; every OUT write [%s]\n",
                 what, rr.windows, (int) rr.slotGainWrites.size(), w.isEmpty() ? "none" : w.toRawUTF8(),
                 ow.isEmpty() ? "none" : ow.toRawUTF8());
    std::printf ("    %s: chain in %.2f, chain out %.2f (out-in %+.2f dB)\n",
                 what, rr.chainInAtEnd, rr.chainOutAtEnd, rr.chainOutAtEnd - rr.chainInAtEnd);
    std::printf ("    %s: closing line: %s\n", what, rr.closing.isEmpty() ? "(none)" : rr.closing.toRawUTF8());
}

// ---------------------------------------------------------------------------------------------------------
void guardMain()
{
    std::printf ("== level_loop_guard (21t-m): the hold, end to end, on real tallies ==\n");

    // ---- (1) A CHANNEL WITH A SLOT 6 dB HOT ----------------------------------------------------------------
    {
        std::printf ("\n-- (1) channel role, the slot is +6 dB hot --\n");
        Rig r (ChannelType::LeadVocal, 6.0f);
        check (! r.h.roleIsBus(), "(1) fixture: the declared role is a CHANNEL", r.h.roleName());
        feed (r.proc, r.prog, 6.0);                       // the tallies bind before the loop opens
        r.proc.calibStart ({}, passiveDriveCfg ("Fake Comp +6"));
        const auto rr = runLoop (r, 28);
        reportRun ("(1)", rr);

        check ((int) rr.slotGainWrites.size() >= 1 && (int) rr.slotGainWrites.size() <= 2,
               "(1) AT MOST TWO WRITES for the whole settle - the hold runs once on the landed drive, never "
               "between drive steps  (RED as it stood: four, at windows 2, 9, 16 and 25)",
               juce::String ((int) rr.slotGainWrites.size()) + " write(s)");
        // WHAT THE HOLD OWES, in the ruling's own terms (29 Sep): "a slot has exactly two EchoJay gains, IN (the
        // pre-trim, which is the drive) and OUT (the slot output gain); the hold sets OUT so the slot comes out
        // where it went in, counting IN and the plugin's own change". So the correction owed is not "-6 because
        // the plugin is +6" - the settle has moved IN by then, and OUT owes the whole of IN plus the plugin.
        const float excess = 6.0f + r.slotPreTrim();
        std::printf ("    (1): IN (the drive) landed at %+.2f dB, the plugin adds +6.00 -> the hold owes %+.2f\n",
                     r.slotPreTrim(), -excess);
        if (! rr.slotGainWrites.empty())
            check (std::abs (rr.slotGainWrites.back() + excess) <= 0.6f,
                   "(1) ...and the HOLD's write lands the whole correction: IN plus the plugin, within 0.6 dB",
                   "hold wrote " + f1 (rr.slotGainWrites.back()) + ", owed " + f1 (-excess) + " dB");
        if (rr.slotGainWrites.size() >= 2)
            check (std::abs (rr.slotGainWrites[1] - rr.slotGainWrites[0]) < 0.5f,
                   "(1) ...and a SECOND write, if there is one, is a refinement smaller than 0.5 dB",
                   "moved " + f1 (rr.slotGainWrites[1] - rr.slotGainWrites[0]) + " dB");
        if (! rr.slotGainWrites.empty())
            check (std::abs (rr.slotGainWrites.back()) <= std::abs (excess) + 1.0f + 0.05f,
                   "(1) THE WRITTEN TOTAL never exceeds the first measured excess plus 1 dB  (RED as it stood: "
                   "-24.00, four times the excess)",
                   f1 (rr.slotGainWrites.back()) + " dB of " + f1 (std::abs (excess) + 1.0f) + " allowed");

        // what the HOLD moved OUT by: its write, from the value the drive's last rung left
        float holdTotalDb = 0.0f;
        if (! rr.slotGainWrites.empty() && rr.everyOutWrite.size() >= 2)
        {
            float before = 0.0f;
            for (size_t i = 0; i + 1 < rr.everyOutWrite.size(); ++i)
                if (std::abs (rr.everyOutWrite[i + 1] - rr.slotGainWrites.back()) < 0.005f) before = rr.everyOutWrite[i];
            holdTotalDb = rr.slotGainWrites.back() - before;
        }
        const float d = rr.chainOutAtEnd - rr.chainInAtEnd;
        check (d == d && std::abs (d) <= 0.5f,
               "(1) the CHAIN OUT, measured after the hold's own slot output gain, equals the chain in within "
               "0.5 dB", f1 (d) + " dB");
        int held = 0; for (float x : rr.chainDeltaAfterHold) if (std::abs (x) <= 0.6f) ++held;
        check ((int) rr.chainDeltaAfterHold.size() >= 3 && held == (int) rr.chainDeltaAfterHold.size(),
               "(1) ...and it STAYS there over three more windows",
               juce::String (held) + " of " + juce::String ((int) rr.chainDeltaAfterHold.size()) + " window(s) held");
        // 21t-m item 2: this case drives the real product through calibStart/calibTick with the DEFAULT purpose,
        // which is an ASK - a build no longer steps a rung at all (case 2e covers the build). So the line it
        // closes with is the ask's report, not the build's "in total" sentence.
        check (rr.closing.contains (juce::String::fromUTF8 (" \xe2\x86\x92 ")),
               "(1) the ask closes by reporting what it MOVED, from and to", rr.closing);
        check (rr.closing.contains ("level held") || rr.closing.contains ("level NOT held"),
               "(1) ...and whether the level came out held", rr.closing);
        if (! rr.slotGainWrites.empty())
            check (std::abs (holdTotalDb) > 0.05f,
                   "(1) ...and the hold moved OUT from where the rung left it, not from zero",
                   "hold moved " + f1 (holdTotalDb) + " dB, landing OUT at " + f1 (rr.slotGainWrites.back()));
    }

    // ---- (2) THE SAME SLOT 6 dB QUIET, AND A SLOT THAT OWES NOTHING ---------------------------------------
    {
        std::printf ("\n-- (2) channel role, the slot is -6 dB quiet --\n");
        Rig r (ChannelType::LeadVocal, -6.0f);
        feed (r.proc, r.prog, 6.0);
        r.proc.calibStart ({}, passiveDriveCfg ("Fake Comp -6"));
        const auto rr = runLoop (r, 28);
        reportRun ("(2a)", rr);
        check ((int) rr.slotGainWrites.size() >= 1 && (int) rr.slotGainWrites.size() <= 2,
               "(2a) at most two writes, the other way up too",
               juce::String ((int) rr.slotGainWrites.size()) + " write(s)");
        const float excess2 = -6.0f + r.slotPreTrim();
        if (! rr.slotGainWrites.empty())
            check (std::abs (rr.slotGainWrites.back() + excess2) <= 0.6f,
                   "(2a) the hold's write is the whole correction the other way up too: IN plus the plugin",
                   "hold wrote " + f1 (rr.slotGainWrites.back()) + ", owed " + f1 (-excess2) + " dB");
        const float d = rr.chainOutAtEnd - rr.chainInAtEnd;
        check (d == d && std::abs (d) <= 0.6f, "(2a) the chain out equals the chain in", f1 (d) + " dB");
    }
    {
        std::printf ("\n-- (2b) channel role, the slot is unity: nothing is owed --\n");
        Rig r (ChannelType::LeadVocal, 0.0f);
        feed (r.proc, r.prog, 6.0);
        r.proc.calibStart ({}, passiveDriveCfg ("Fake Comp 0"));
        const auto rr = runLoop (r, 20);
        reportRun ("(2b)", rr);
        // A UNITY PLUGIN is not a level-neutral SLOT: the settle still moves IN to look for gain reduction, and
        // OUT owes whatever IN added. What "nothing is owed" means here is that the hold writes nothing the
        // DRIVE did not put there - so the assertion is the write against IN, and the chain coming out level.
        check ((int) rr.slotGainWrites.size() <= 1,
               "(2b) a unity plugin costs at most ONE HOLD write",
               juce::String ((int) rr.slotGainWrites.size()) + " hold write(s), IN at " + f1 (r.slotPreTrim()));
        if (! rr.slotGainWrites.empty())
            check (std::abs (rr.slotGainWrites.back() + r.slotPreTrim()) <= 0.6f,
                   "(2b) ...and it is exactly minus the drive, nothing else",
                   "hold wrote " + f1 (rr.slotGainWrites.back()) + ", IN " + f1 (r.slotPreTrim()) + " dB");
        const float d2b = rr.chainOutAtEnd - rr.chainInAtEnd;
        check (d2b == d2b && std::abs (d2b) <= 0.6f,
               "(2b) ...and the chain comes out where it went in", f1 (d2b) + " dB");
    }

    // ---- (1b) THE LOOP WAITS FOR THE SLOT, NOT FOR THE DIAL (21t-m item 1, 29 Sep 2026 ruling) -----------
    {
        std::printf ("\n-- (1b) an empty rack is not \"settled\": the wait is for the slot the block names --\n");
        // Sean's 18:05 log: "EJThreshold: block not usable - wire slot 1 ... rack has 0 slot(s); nothing
        // started." at 18:05:39.634, and the plugin landed at 18:05:40.527 - 893 ms later, an async load. The
        // edit path was waiting on whenDialSettled, and dialStateSettled() walks slots_ looking for a PENDING
        // one: on an empty rack the loop body never runs and it answers "settled" by vacuous truth. Same line
        // at 18:12:56.990 on the local Mix Bus build. The vocal was left at the server's opening Threshold.
        Rig r (ChannelType::LeadVocal, 6.0f);
        auto& h = r.h;
        // The rig loads one slot in its constructor; remove it so the rack is genuinely empty, as it is in the
        // instant between "apply" and the async load landing.
        while (h.getNumSlots() > 0) h.removeSlot (0);
        pumpMs (100);
        check (h.getNumSlots() == 0, "(1b) fixture: the rack is empty, as it is while an add is in flight",
               juce::String (h.getNumSlots()) + " slot(s)");
        check (h.dialStateSettled(),
               "(1b) ...and dialStateSettled() says SETTLED on it - the vacuous truth that caused this",
               "this is the trap, not the fix");
        check (! h.slotReadyForCalibration (0),
               "(1b) but slot 0 is NOT ready for calibration on an empty rack  (RED as it stood: the edit path "
               "read \"settled\" here and started nothing, 893 ms before the plugin landed)");
        check (! h.slotReadyForCalibration (1) && ! h.slotReadyForCalibration (-1),
               "(1b) ...nor is any other index, in or out of range");

        // ...and once the slot lands, it IS ready, so the wait ends rather than hanging.
        const auto* gn = BuiltinDeviceRegistry::instance().findByName ("EchoJay Gain");
        if (gn != nullptr) EchoJayBorrowHostTestAccess::loadBuiltin (h, BuiltinDeviceRegistry::descriptionFor (*gn));
        pumpMs (200);
        check (h.getNumSlots() == 1 && h.slotReadyForCalibration (0),
               "(1b) ...and the moment the slot lands it IS ready, so the wait ends rather than hanging",
               juce::String (h.getNumSlots()) + " slot(s), ready "
               + (h.slotReadyForCalibration (0) ? "yes" : "no"));
        check (! h.slotReadyForCalibration (1),
               "(1b) ...while the slot AFTER it still is not: readiness is per slot, not per rack");

        // the DialSummary headline the ruling asks for
        check (h.dynamicsSlotCount() == 0,
               "(1b) an EchoJay Gain is not a dynamics slot, so nothing is owed a loop here",
               juce::String (h.dynamicsSlotCount()) + " dynamics slot(s)");
    }

    // ---- (1c) THE DRIVE'S POST-CUT IS LIVE, AND THE HOLD WRITES THE WHOLE RESIDUAL (21t-m item 1) --------
    {
        std::printf ("\n-- (1c) the drive raises IN and lowers the LIVE OUT; the hold writes it all --\n");
        // Sean's 21:53-21:54, ~/Desktop/ejlog_2206.txt: the settle drove pre=+1/+2/+3 with post=-1/-2/-3, the
        // post landed in the COMPARE trim (not in the path), and the chain came out 3 dB louder. The hold then
        // read "the plugin is 0.0 dB quieter out than in; residual before this write 3.00 dB; written total
        // -1.00 dB" and told him "Output -1.0 dB, its limit - the slot is still 2.0 dB out".
        //   131964  21:54:16.176  window 18 gr=0.2 pre=+3.0 post=-3.0 ... state=level-hold-slot
        //   131965  21:54:16.176  slot 5 output gain set to -1.00 dB
        Rig r (ChannelType::LeadVocal, 0.0f);   // a UNITY plugin: every dB at the chain output is EchoJay's own
        auto& h = r.h;
        check (h.getNumSlots() == 1, "(1c) fixture: one unity slot", juce::String (h.getNumSlots()) + " slot(s)");

        // THE DRIVE, written the way the loop writes it: IN up a rung, OUT down by the same.
        h.setSlotPreTrimDb (0, 3.0f);
        h.setSlotOutGainDb (0, -3.0f);
        feed (r.proc, r.prog, 12.0);
        const float d = r.chainOutDb() - r.chainInDb();
        check (d == d && std::abs (d) <= 0.5f,
               "(1c) a drive of +3 with its post-cut is LEVEL at the chain output  (RED as it stood: the cut went "
               "to the compare trim, which is only in circuit during an A/B, so the chain came out 3 dB louder)",
               f1 (d) + " dB");

        // ...AND THE HOLD WRITES THE WHOLE RESIDUAL. Stage Sean's state: IN +3, OUT still 0, so 3 dB is owed.
        h.setSlotOutGainDb (0, 0.0f);
        feed (r.proc, r.prog, 12.0);
        echojay::CalibLoop l;
        auto cfg = passiveDriveCfg ("EchoJay Compressor");
        beginDriven (l, cfg);
        echojay::CalibLoop::Window w;
        w.measured = true; w.silent = false; w.grDb = 2.5f; w.levelChangeDb = 0.0f; w.inTruePeakDb = -12.0f;
        w.slotOutGainDb = 0.0f; w.slotPreTrimDb = 3.0f;      // the state at 21:54:16
        float wrote = 0.0f; int writes = 0; float heard = 30.0f;
        for (int i = 0; i < 12; ++i)
        {
            heard += 4.0f; w.heardSeconds = heard;
            const auto st = l.onWindow (w, 3000.0);
            if (st.writeSlotGain) { wrote = st.slotGainValue; ++writes; w.slotOutGainDb = st.slotGainValue; }
        }
        std::printf ("    (1c) residual 3.00 owed -> %d write(s), OUT %.2f dB\n", writes, wrote);
        check (writes >= 1 && std::abs (wrote + 3.0f) <= 0.3f,
               "(1c) the hold writes the WHOLE measured residual  (RED as it stood: residual 3.00, written -1.00, "
               "reported to Sean as \"Output -1.0 dB, its limit\")",
               juce::String (writes) + " write(s), OUT " + f1 (wrote) + " dB, owed -3.00");
        check (writes <= 2, "(1c) ...in at most two writes", juce::String (writes) + " write(s)");
        check (std::abs (l.levelTrimmedDb + 3.0f) <= 0.3f,
               "(1c) ...and the sentence's written total is that same figure", f1 (l.levelTrimmedDb) + " dB");
        check (! l.levelHoldClamped,
               "(1c) ...and nothing reports a limit, because the control has 24 dB of range left",
               l.levelHoldClamped ? "CLAMPED" : "not clamped");
    }

    // ---- (4c) A WAIT THAT RAN OUT IS A REFUSAL (21t-m item 4, 29 Sep 2026 ruling) -----------------------
    {
        std::printf ("\n-- (4c) the slot never lands: nothing starts, and it says so --\n");
        // TWO CASES FROM SEAN'S LOG, both of them mine:
        //   22:00:26.908  "block not usable - wire slot 2 ... rack has 0 slot(s); nothing started" on
        //                 loadChainFromJson - item 1 wired the edit paths and the leased build to whenSlotReady
        //                 and left the LOCAL build reading an empty rack.
        //   21:35:08.617  "leased build settled (dial bound expired, slot NOT landed) -> 1 loop(s) started" - my
        //                 own fire lambda started the loop whatever `ready` said. A wait whose answer is ignored
        //                 is not a wait.
        Rig r (ChannelType::LeadVocal, 6.0f);
        auto& h = r.h;
        while (h.getNumSlots() > 0) h.removeSlot (0);
        pumpMs (100);
        check (h.getNumSlots() == 0, "(4c) fixture: an empty rack, as it is while a build's adds are in flight");

        // the wait must REFUSE on a rack where the slot never appears, however long it is given
        bool fired = false, readyWas = true;
        h.whenSlotReady (1, 300, [&fired, &readyWas] (bool ready) { fired = true; readyWas = ready; });
        for (int i = 0; i < 40 && ! fired; ++i) pumpMs (50);
        check (fired, "(4c) the wait does come back rather than hanging for ever");
        check (! readyWas,
               "(4c) ...and it comes back NOT READY, because slot 2 never landed  (RED as it stood: the caller "
               "started a loop anyway and logged \"slot NOT landed -> 1 loop(s) started\")",
               readyWas ? "ready (wrong)" : "not ready");

        // ...and the moment the slot lands, the same wait says ready - so this is not a wait that always refuses
        const auto* gn = BuiltinDeviceRegistry::instance().findByName ("EchoJay Gain");
        if (gn != nullptr)
        {
            EchoJayBorrowHostTestAccess::loadBuiltin (h, BuiltinDeviceRegistry::descriptionFor (*gn));
            EchoJayBorrowHostTestAccess::loadBuiltin (h, BuiltinDeviceRegistry::descriptionFor (*gn));
        }
        pumpMs (250);
        bool fired2 = false, ready2 = false;
        h.whenSlotReady (1, 600, [&fired2, &ready2] (bool rr2) { fired2 = true; ready2 = rr2; });
        for (int i = 0; i < 40 && ! fired2; ++i) pumpMs (50);
        check (fired2 && ready2,
               "(4c) ...while with slot 2 in the rack it comes back READY, so the refusal is the empty rack and "
               "not a wait that never says yes",
               juce::String (h.getNumSlots()) + " slot(s), ready " + (ready2 ? "yes" : "no"));
    }

    // ---- (2e) A BUILD HAS NO LOOP; AN ASK MOVES ONE RUNG (21t-m item 2, 29 Sep 2026 ruling) -------------
    {
        std::printf ("\n-- (2e) a build holds once and closes; an ask moves one rung and reports --\n");
        // Sean's 21:53-21:54: a BUILD walked pre=+1/+2/+3 over eighteen windows, hunting for a gain-reduction
        // band he had never asked it to find, and every rung leaked a dB. A build applies the working position,
        // matches the level once through OUT, and says so. The hunting happens only when he asks for it.
        auto runPurpose = [] (echojay::CalibLoop::Purpose purpose, int windows,
                              int& rungsOut, int& holdsOut, juce::String& closingOut)
        {
            echojay::CalibLoop l;
            auto cfg = passiveDriveCfg ("Fake Comp");
            cfg.purpose = purpose;
            beginDriven (l, cfg);
            echojay::CalibLoop::Window w;
            w.measured = true; w.silent = false; w.grDb = 0.2f; w.levelChangeDb = 6.0f; w.inTruePeakDb = -12.0f;
            w.slotOutGainDb = 0.0f; w.slotPreTrimDb = 0.0f;
            rungsOut = 0; holdsOut = 0; closingOut = {};
            float heard = 30.0f;
            for (int i = 0; i < windows; ++i)
            {
                heard += 4.0f; w.heardSeconds = heard;
                const auto st = l.onWindow (w, 3000.0);
                if (st.writeDrive || st.writeParams) { ++rungsOut; w.slotPreTrimDb = st.newPre; w.slotOutGainDb = st.newPost; }
                if (st.writeSlotGain) { ++holdsOut; w.slotOutGainDb = st.slotGainValue; }
                if (st.ask.isNotEmpty()) closingOut = st.ask;
            }
            return l.landed;
        };

        {   // A BUILD
            int rungs = 0, holds = 0; juce::String closing;
            const bool landed = runPurpose (echojay::CalibLoop::Purpose::buildHold, 14, rungs, holds, closing);
            std::printf ("    (2e) build: %d rung(s), %d hold write(s), landed %s\n    (2e) build says: %s\n",
                         rungs, holds, landed ? "yes" : "no", closing.isEmpty() ? "(nothing)" : closing.toRawUTF8());
            // SUPERSEDED 30 Sep 2026 (letter (m)): the 29 Sep rule was "a build moves no rung at all", and this
            // leg asserted rungs == 0. Sean's 18:22 log showed what that cost: a passive build read gr=0.0 against
            // a 2-3 dB band and landed on it, so the compressor was never driven and the hold found nothing to
            // hold. A build now SEEKS the band, window by window, bounded by the 12-window cap and the slot range.
            // What survives of the old rule is everything below it: one hold, one "Built." line, no promise on the
            // way in, and the loop ends at its close.
            check (rungs <= echojay::CalibLoop::kBuildMaxWindows,
                   "(2e) a build's seek is BOUNDED - it never exceeds the 12-window cap  (was: \"a build moves no "
                   "rung at all\", superseded by letter (m): that rule left the compressor undriven)",
                   juce::String (rungs) + " rung(s), cap "
                       + juce::String (echojay::CalibLoop::kBuildMaxWindows));
            check (holds >= 1 && holds <= 2, "(2e) ...it matches the level once through OUT",
                   juce::String (holds) + " hold write(s)");
            check (closing.startsWith ("Built."),
                   "(2e) ...and closes with ONE line that says what it did", closing);
            check (closing.contains ("Level held") || closing.contains ("already matched")
                       || closing.contains ("no more to give"),
                   "(2e) ...naming the level it held", closing);
            check (! closing.contains ("landing it as it plays"),
                   "(2e) ...and it never promises to land anything, because it is not going to hunt", closing);
        }
        {   // AN ASK
            int rungs = 0, holds = 0; juce::String closing;
            runPurpose (echojay::CalibLoop::Purpose::askRung, 14, rungs, holds, closing);
            std::printf ("    (2e) ask: %d rung(s), %d hold write(s)\n    (2e) ask says: %s\n",
                         rungs, holds, closing.isEmpty() ? "(nothing)" : closing.toRawUTF8());
            check (rungs == 1, "(2e) AN ASK MOVES EXACTLY ONE RUNG", juce::String (rungs) + " rung(s)");
            check (closing.isNotEmpty(), "(2e) ...and reports what it moved", closing);
        }
    }

    // ---- (2c) A SLOT THAT NAMES AN OUTPUT CONTROL: the hold still writes EchoJay's OWN OUT --------------
    {
        std::printf ("\n-- (2c) the block names an output control: it is left exactly where the build put it --\n");
        // 29 Sep 2026 ruling, replacing the named-output question: "THE HOLD WRITES EchoJay's OWN OUT, ALWAYS.
        // Never a plugin's output control, whether or not the map names one ... A plugin's own output control is
        // left exactly where the build put it." The fake compressor here is an EchoJay Gain whose level_db IS an
        // output control, and the block names it - so the old code would have written level_db and left OUT at
        // zero, and the plugin's own setting would have moved under the user.
        Rig r (ChannelType::LeadVocal, 6.0f);
        auto cfg = passiveDriveCfg ("Fake Comp, output named");
        cfg.outputParams.add ("level_db");
        cfg.outputStartDb = 6.0f;          // where the BUILD put it, and where it must still be at the end
        cfg.outputMinDb = -24.0f; cfg.outputMaxDb = 24.0f;
        feed (r.proc, r.prog, 6.0);
        r.proc.calibStart ({}, cfg);
        const auto rr = runLoop (r, 28);
        reportRun ("(2c)", rr);

        auto* gain = dynamic_cast<EedGainProcessor*> (r.h.getSlotProcessor (0));
        check (gain != nullptr, "(2c) fixture: the slot is the EchoJay Gain whose level_db the block named");
        if (gain != nullptr)
            check (std::abs ((float) gain->getParamValue (EedGainProcessor::kLevelDb) - 6.0f) <= 0.05f,
                   "(2c) THE PLUGIN'S OWN OUTPUT CONTROL IS UNTOUCHED - it is still where the build put it  (RED "
                   "as it stood: the hold wrote it instead of OUT, so the user's plugin moved under them)",
                   "level_db " + f1 ((float) gain->getParamValue (EedGainProcessor::kLevelDb)) + " dB, build set 6.00");
        check ((int) rr.slotGainWrites.size() >= 1,
               "(2c) ...and OUT carried the correction instead",
               juce::String ((int) rr.slotGainWrites.size()) + " write(s) to OUT");
        const float d2c = rr.chainOutAtEnd - rr.chainInAtEnd;
        check (d2c == d2c && std::abs (d2c) <= 0.6f,
               "(2c) ...and the chain still comes out where it went in", f1 (d2c) + " dB");
    }

    // ---- (2d) LISTEN WRITES NOTHING; GO WRITES THE LEVEL SLOT EXACTLY (21t-m item 2 small) ---------------
    {
        std::printf ("\n-- (2d) Listen measures and asks; only Go moves the Level --\n");
        // Sean's 18:13:39.890, on the Mix Bus, wrote live pre-trims on four slots under a log line claiming the
        // chain output does not move:
        //     unity trim: slot 2 Newfangled Saturate: ... -> pre -4.8 dB, post 0.8 dB
        //     unity trim: slot 3 PuigTec EQP1A (s): ... -> pre -3.2 dB, post -3.8 dB
        //     unity trims changed: 6 (match trims, compare-only: the chain output does NOT move)
        // setSlotPreTrimDb is IN, applied unconditionally by SlotPreTrim. It moved, and he heard it.
        //
        // THE FIRST CUT OF THIS LEG WAS VACUOUS and its own last assertion caught it: the chain was Gain+Level
        // with no limiter, armFromChain() needs a Level AND a limiter LAST, so the loop never armed and
        // "nothing moved" was trivially true. The arm and the measurement are now asserted BEFORE the writes
        // are judged, so this leg cannot pass by not running.
        Rig r (ChannelType::FullMix, 0.0f);
        auto& h = r.h;
        while (h.getNumSlots() > 0) h.removeSlot (0);
        auto load = [&h] (const char* name)
        {
            const auto* d = BuiltinDeviceRegistry::instance().findByName (name);
            if (d != nullptr) EchoJayBorrowHostTestAccess::loadBuiltin (h, BuiltinDeviceRegistry::descriptionFor (*d));
            pumpMs (120);
        };
        load ("EchoJay Level");        // slot 0 - what the loop drives
        load ("EchoJay Gain");         // slot 1 - a slot with something to trim
        load ("EchoJay Limiter");      // slot 2 - LAST, which is what arming needs
        check (h.getNumSlots() == 3, "(2d) fixture: Level, a trimmable slot, and a limiter last",
               juce::String (h.getNumSlots()) + " slot(s)");

        auto& loop = r.proc.loudnessLoop();
        juce::StringArray logs;
        loop.logLine   = [&logs] (const juce::String& l) { logs.add (l); };
        loop.isPlaying = [] { return true; };
        { auto* pp = new juce::DynamicObject(); pp->setProperty ("gain_db", 0.0);
          pp->setProperty ("target_lufs", -9.0); pp->setProperty ("loudness_option", 0);
          auto* w = new juce::DynamicObject(); w->setProperty ("params", juce::var (pp));
          h.setSlotStructuredSettings (0, juce::var (w)); pumpMs (120); }
        const bool armed = loop.armFromChain();
        check (armed, "(2d) fixture: the loudness loop ARMED - without this the leg proves nothing",
               armed ? juce::String ("armed") : juce::String ("NOT ARMED"));

        // every IN and OUT after arming, before the window
        std::vector<float> preIn, preOut;
        for (int i = 0; i < h.getNumSlots(); ++i) { preIn.push_back (h.getSlotPreTrimDb (i)); preOut.push_back (h.getSlotOutGainDb (i)); }
        const float levelBefore = r.levelGainDb();

        for (int k = 0; k < 24; ++k) feed (r.proc, r.prog, 1.0);
        const auto joined = logs.joinIntoString (" | ");
        check (joined.contains ("measured:") || joined.contains ("bubble:"),
               "(2d) fixture: ...and it MEASURED a window - the leg is exercising Listen, not silence",
               joined.isEmpty() ? juce::String ("(no loudness log at all)")
                                : joined.substring (juce::jmax (0, joined.length() - 90)));

        int movedIn = 0, movedOut = 0;
        for (int i = 0; i < h.getNumSlots() && i < (int) preIn.size(); ++i)
        {
            if (std::abs (h.getSlotPreTrimDb (i)  - preIn[i])  > 0.05f) ++movedIn;
            if (std::abs (h.getSlotOutGainDb (i) - preOut[i]) > 0.05f) ++movedOut;
        }
        check (movedIn == 0 && movedOut == 0,
               "(2d) LISTEN WROTE NOTHING - not one slot's IN or OUT moved  (RED as it stood: four slots' pre "
               "trims written live, under \"the chain output does NOT move\")",
               juce::String (movedIn) + " IN, " + juce::String (movedOut) + " OUT moved");
        check (std::abs (r.levelGainDb() - levelBefore) < 0.05f,
               "(2d) ...and the Level slot did not move either: Listen ASKS, Go moves it",
               "Level " + f1 (levelBefore) + " -> " + f1 (r.levelGainDb()) + " dB");
        check (! joined.contains ("unity trim:") && ! joined.contains ("unity trims changed"),
               "(2d) ...and the unity-trim lines are gone from the log with the writes they described",
               joined.contains ("unity") ? juce::String ("a unity line is still logged")
                                         : juce::String ("(no unity line)"));
    }

    // ---- (3b) ON A BUS, NOTHING WRITES IN OR OUT (21t-m item 3, 29 Sep 2026 ruling) ----------------------
    {
        std::printf ("\n-- (3b) a bus dials nothing: no loop, no hold, no slot writes --\n");
        // Sean's 22:03:49 and 22:03:59 on the Mix Bus (local rack):
        //   162727  22:03:49.993  slot 2 output gain set to 10.50 dB (EchoJay's own, ...)
        //           level hold - ... (the plugin is 13.5 dB quieter out than in; residual -10.50 dB)
        // +10.5 then +9.5 dB pushed into a slot on the mix bus, and it distorted the chain. A bus's level is its
        // last stage's, and after Go the Level slot's. Nothing else writes there.
        Rig r (ChannelType::FullMix, -13.5f);   // a slot as quiet as the EMO-D5 measured
        auto& h = r.h;
        check (h.roleIsBus(), "(3b) fixture: the chain's role is a bus", h.chainRole().text());
        const float inBefore = h.getSlotPreTrimDb (0), outBefore = h.getSlotOutGainDb (0);

        feed (r.proc, r.prog, 6.0);
        r.proc.calibStart ({}, passiveDriveCfg ("EMO-D5 (s)"));
        const auto rr = runLoop (r, 20);
        reportRun ("(3b)", rr);

        check (std::abs (h.getSlotPreTrimDb (0) - inBefore) < 0.05f
                 && std::abs (h.getSlotOutGainDb (0) - outBefore) < 0.05f,
               "(3b) NOT ONE dB WAS WRITTEN to the slot's IN or OUT on a bus  (RED as it stood: +10.50 then "
               "+9.50 dB into slot 2's OUT, and it distorted the chain)",
               "IN " + f1 (inBefore) + " -> " + f1 (h.getSlotPreTrimDb (0))
               + ", OUT " + f1 (outBefore) + " -> " + f1 (h.getSlotOutGainDb (0)));
        check (rr.slotGainWrites.empty() && rr.everyOutWrite.empty(),
               "(3b) ...and the loop wrote nothing at all, because it never started",
               juce::String ((int) rr.everyOutWrite.size()) + " OUT write(s)");

        // ...AND THE OTHER DIRECTION: the same chain on a CHANNEL still dials, or this leg is only proving that
        // calibration is broken everywhere.
        {
            Rig c (ChannelType::LeadVocal, -13.5f);
            feed (c.proc, c.prog, 6.0);
            c.proc.calibStart ({}, passiveDriveCfg ("EMO-D5 (s)"));
            const auto cr = runLoop (c, 20);
            check (! cr.everyOutWrite.empty(),
                   "(3b) ...while the SAME chain on a CHANNEL still dials, so the refusal is the role and not a "
                   "loop that stopped working",
                   juce::String ((int) cr.everyOutWrite.size()) + " OUT write(s) on the channel");
        }
    }

    // ---- (3a) THE ROLE HAS THREE SOURCES (21t-m item 5, 29 Sep 2026 ruling) --------------------------------
    {
        std::printf ("\n-- (3a) the role's three sources: the prompt, the placement selector, the track name --\n");
        // "isBusRole reads only the start-prompt channelType, so a track named 'Mix Bus' with the prompt
        // unanswered is a channel - which is today's demo." The three cases are Sean's, verbatim.
        {
            Rig r (ChannelType::LeadVocal, 0.0f, false, "Mix Bus");
            const auto role = r.proc.chainRole();
            check (role.isBus() && role.from == "name",
                   "(3a) an UNANSWERED prompt (LeadVocal) on a track named \"Mix Bus\" is a BUS, said by the "
                   "NAME  (RED as it stood: channelType was the only source, so this was a channel - and it is "
                   "the demo session)", role.text());
            check (r.h.roleIsBus(), "(3a) ...and the host it publishes to agrees", r.h.chainRole().text());
        }
        {
            Rig r (ChannelType::VocalBus, 0.0f, false, "Lead Vox");
            const auto role = r.proc.chainRole();
            check (role.isBus() && role.from == "prompt",
                   "(3a) a track named \"Lead Vox\" with the prompt answered Vocal Bus is a BUS, said by the "
                   "PROMPT - the name does not have to agree", role.text());
        }
        {
            Rig r (ChannelType::LeadVocal, 0.0f, false, "Vox 2");
            const auto role = r.proc.chainRole();
            check (! role.isBus() && role.from.isEmpty(),
                   "(3a) \"Vox 2\" with LeadVocal is a CHANNEL, and nothing claims to have decided it",
                   role.text());
            check (! r.h.roleIsBus(), "(3a) ...and the restore still lands on it (it is a channel)");
        }
        {   // the third source, which V2 has no selector for and a Link does
            Rig r (ChannelType::LeadVocal, 0.0f, false, "Vox 2", /*placement*/ 1);
            const auto role = r.proc.chainRole();
            check (role.isBus() && role.from == "placement",
                   "(3a) ...and the PLACEMENT selector saying bus is enough on its own", role.text());
        }
        {   // the wire shape, ruled verbatim
            Rig r (ChannelType::LeadVocal, 0.0f, false, "Mix Bus");
            const auto v = r.proc.chainRole().toVar();
            const auto j = juce::JSON::toString (v, true);
            std::printf ("    (3a) channelRole on the wire: %s\n", j.toRawUTF8());
            // Parsed back, not string-matched: JUCE writes "key": value with a space, and a leg that asserted
            // the spacing would be asserting JUCE's formatter rather than the ruled SHAPE.
            const auto back = juce::JSON::parse (j);
            check (back.getProperty ("kind", juce::var()).toString() == "bus"
                     && back.getProperty ("from", juce::var()).toString() == "name"
                     && back.getProperty ("name", juce::var()).toString() == "Mix Bus",
                   "(3a) the wire shape is the ruled one: kind, from, name", j);
        }
        {   // ---- THE MUSIC GATE (29 Sep 2026 ruling): chainRole() NARROWED ----------------------------------
            // "The chain is THE MUSIC when chainRole() says bus AND none of these says it is a vocal or rhythm
            // bus: the prompt answered VocalBus or DrumBus, or the track name contains vocal/vox/bv/harmony/
            // drum/perc." The five cases are Sean's, verbatim.
            {
                Rig r (ChannelType::LeadVocal, 0.0f, false, "Mix Bus");
                check (r.proc.selfKeyRoleIsMusic(),
                       "(3a music) \"Mix Bus\" with the prompt UNANSWERED is the music  (RED as it stood: the "
                       "gate read channelType, which said LeadVocal - and that is today's demo)");
            }
            {
                Rig r (ChannelType::LeadVocal, 0.0f, false, "Master");
                check (r.proc.selfKeyRoleIsMusic(), "(3a music) ...and so is \"Master\"");
            }
            {
                Rig r (ChannelType::LeadVocal, 0.0f, false, "Vocal Bus");
                check (r.proc.chainRole().isBus() && ! r.proc.selfKeyRoleIsMusic(),
                       "(3a music) \"Vocal Bus\" IS a bus and is NOT the music - the name disqualifies it",
                       r.proc.chainRole().text());
            }
            {
                Rig r (ChannelType::VocalBus, 0.0f, false, "Backings");
                check (r.proc.chainRole().isBus() && ! r.proc.selfKeyRoleIsMusic(),
                       "(3a music) ...and a VocalBus PROMPT ANSWER is not the music either, whatever the track "
                       "is called", r.proc.chainRole().text());
            }
            {
                Rig r (ChannelType::LeadVocal, 0.0f, false, "Drum Bus");
                check (r.proc.chainRole().isBus() && ! r.proc.selfKeyRoleIsMusic(),
                       "(3a music) \"Drum Bus\" is a bus and is not the music", r.proc.chainRole().text());
            }
            {
                Rig r (ChannelType::LeadVocal, 0.0f, false, "Vox 2");
                check (! r.proc.chainRole().isBus() && ! r.proc.selfKeyRoleIsMusic(),
                       "(3a music) ...and a CHANNEL is never the music, however it is named");
            }
            {   // the disqualifying words, whole-word, and what must NOT match
                auto v = [] (const char* n) { return echojay::nameReadsAsVocalOrRhythm (n); };
                check (v ("Vocal Bus") && v ("Vox Bus") && v ("BV bus") && v ("BVs") && v ("Harmony stem")
                         && v ("Harmonies") && v ("Drum Bus") && v ("Drums") && v ("Perc bus")
                         && v ("Percussion sum"),
                       "(3a music) every ruled vocal/rhythm word disqualifies, case-insensitive");
                check (! v ("Voxel bus") && ! v ("Drumming master") && ! v ("Percolator stem")
                         && ! v ("Mix Bus") && ! v ("Master"),
                       "(3a music) ...WHOLE WORDS only, so Voxel, Drumming and Percolator do not");
            }
        }

        {   // the words, whole-word and case-insensitive, and what must NOT match
            auto b = [] (const char* n) { return echojay::trackNameReadsAsBus (n); };
            check (b ("Mix Bus") && b ("2-Bus") && b ("MASTER") && b ("Gtr Stem") && b ("Sum") && b ("Print A"),
                   "(3a) the ruled words all read as a bus, case-insensitive and across a hyphen");
            check (! b ("Bussing") && ! b ("Mastered vox") && ! b ("Printer") && ! b ("Summer")
                     && ! b ("Lead Vox") && ! b ("Vox 2"),
                   "(3a) ...and WHOLE WORDS only, so Bussing, Mastered, Printer and Summer are not buses");
        }
    }

    // ---- (3) A BUS ROLE ------------------------------------------------------------------------------------
    {
        std::printf ("\n-- (3) bus role: nothing is written to the chain output, and no record is restored --\n");
        Rig r (ChannelType::FullMix, 6.0f);
        check (r.h.roleIsBus() && r.h.chainRole().from == "prompt",
               "(3) fixture: the declared role is a BUS, said by the prompt (FullMix)", r.h.roleName());

        // THE STORED-RECORD RESTORE, exactly as Sean's 12:52:28.563 line ran it. The record is taken from a
        // DIFFERENT chain running 12 dB quieter, so a restore that lands is visible as a jump and one that is
        // refused is visible as no jump at all. Built by the product's own saver, so the leg cannot pass by
        // handing the restore a shape it would have rejected anyway.
        juce::var quietRecord;
        {
            Rig q (ChannelType::LeadVocal, 0.0f);
            juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
            for (int b = 0; b < 400; ++b)
            { for (int i = 0; i < 512; ++i) { const float v = q.prog.next() * 0.25f; buf.setSample (0, i, v); buf.setSample (1, i, v); }
              q.proc.processBlock (buf, midi); }
            quietRecord = q.h.getLevelsStateVar ("Mix Bus");
            check (quietRecord.getDynamicObject() != nullptr,
                   "(3) fixture: the saver wrote a chain-level record, from a chain 12 dB quieter");
        }
        feed (r.proc, r.prog, 6.0);
        const float beforeIn = r.chainInIntDb();
        r.h.setPendingLevelsState (quietRecord, "Mix Bus");
        feed (r.proc, r.prog, 1.0);   // ...and the bus is given the same hop, so "nothing landed" means nothing
        check (r.chainInIntDb() == r.chainInIntDb() && std::abs (r.chainInIntDb() - beforeIn) < 0.4f,
               "(3) a saved tally is NOT restored onto a BUS chain  (RED as it stood: \"EJLevels: pending "
               "restore, chain in=y out=y slots=0 track=Mix Bus\")",
               "chain in " + f1 (beforeIn) + " -> " + f1 (r.chainInIntDb()) + " dB");
        {   // ...AND THE OTHER DIRECTION: the same record, the same call, on a CHANNEL - it must still land, or
            // this guard is only proving that the restore is broken everywhere.
            Rig c (ChannelType::LeadVocal, 0.0f);
            feed (c.proc, c.prog, 6.0);
            const float was = c.chainInIntDb();
            c.h.setPendingLevelsState (quietRecord, "Mix Bus");
            feed (c.proc, c.prog, 1.0);   // the restore is PENDING: it lands at a hop boundary on the audio thread
            check (std::abs (c.chainInIntDb() - was) > 0.5f,
                   "(3) ...and on a CHANNEL the SAME record still lands, so the refusal is the role and not a "
                   "restore that stopped working", "chain in " + f1 (was) + " -> " + f1 (c.chainInIntDb()) + " dB");
        }

        feed (r.proc, r.prog, 6.0);
        const float lastSlotOutBefore = r.h.getSlotOutGainDb (0);
        r.proc.calibStart ({}, passiveDriveCfg ("Fake Comp on a bus"));
        const auto rr = runLoop (r, 28);
        reportRun ("(3)", rr);
        // The per-SLOT hold is the slot's business and runs on a bus too; what may never happen on a bus is a
        // write to the CHAIN output. There is no such control in this product and this leg is what keeps it so.
        const float chainDelta = rr.chainOutAtEnd - rr.chainInAtEnd;
        // THE LAST STAGE SETS THE LEVEL, stated so it is measurable: move the LAST slot's own output gain and
        // the chain output moves with it, one for one, because nothing stands between them. (The chain tallies
        // are K-weighted and the slot tallies are Plain, so their absolute figures are not comparable and a leg
        // that subtracted one from the other would be measuring the weighting - the first cut of this leg did.)
        const int last = r.h.getNumSlots() - 1;
        check (r.h.getSlotLevels (last).measured && r.h.getSlotLevels (last).out.known,
               "(3) fixture: the last stage has a reading to set the level with");
        const float chainBefore = r.chainOutDb();
        r.h.setSlotOutGainDb (last, r.h.getSlotOutGainDb (last) - 3.0f);
        feed (r.proc, r.prog, 12.0);
        const float chainAfter = r.chainOutDb();
        check (std::abs ((chainAfter - chainBefore) + 3.0f) <= 0.6f,
               "(3) THE CHAIN'S LAST STAGE SETS THE LEVEL: -3 dB on the last stage is -3 dB at the chain output, "
               "one for one - nothing stands between them, and nothing corrects it back",
               "chain out " + f1 (chainBefore) + " -> " + f1 (chainAfter) + " dB");
        r.h.setSlotOutGainDb (last, r.h.getSlotOutGainDb (last) + 3.0f);
        juce::ignoreUnused (lastSlotOutBefore, chainDelta);

        // ...and the block says the chain's own figure, separately from the last slot's
        const auto block = EchoJayAPI::buildCurrentChainInjection (r.h);
        juce::String chainLine;
        for (const auto& l : juce::StringArray::fromLines (block))
            if (l.startsWith ("Chain in/out:")) chainLine = l.trim();
        std::printf ("    (3) the chain line: %s\n", chainLine.isEmpty() ? "(absent)" : chainLine.toRawUTF8());
        check (chainLine.isNotEmpty(),
               "(3) the chain block prints the CHAIN's own out, separately from the last slot's  (RED as it "
               "stood: five slot lines and nothing about the chain)", chainLine);
        check (chainLine.contains ("this is the WHOLE chain, not the last slot"),
               "(3) ...and says so in words, so it cannot be read as another slot line", chainLine);
        check (chainLine.contains ("role bus (said by the ") && chainLine.contains ("LAST STAGE sets this level"),
               "(3) ...and on a bus it names the role, WHICH SOURCE said so, and that the last stage sets the "
               "level  (21t-m item 5: never a bare \"bus\" the reader has to take on trust)", chainLine);
    }

    // ---- (4) A HANDOVER MID-LOOP -------------------------------------------------------------------------
    {
        std::printf ("\n-- (4) a handover mid-loop: the second host judges a window, it does not go stale --\n");
        // Sean's NEOLD U17: handed over at 12:58:28 with the FIRST host's slot heard time (126.4 s) riding the
        // sidecar as lastHeardS; the second host's own tally started at zero and never passed it, so every
        // window for nine minutes logged state=stale-window with heard stuck at 24.1 s.
        echojay::CalibLoop first;
        auto cfg = passiveDriveCfg ("NEOLD U17");
        beginDriven (first, cfg);
        echojay::CalibLoop::Window w;
        w.measured = true; w.silent = false; w.grDb = 0.4f; w.levelChangeDb = 0.1f; w.inTruePeakDb = -12.0f;
        for (int i = 0; i < 6; ++i) { w.heardSeconds = 20.0f + 3.0f * (float) i; first.onWindow (w, 3000.0); }
        const auto carried = first.toVar();

        // fromVar is STATIC and RETURNS a loop (LinkProcessor: `calibLoop_ = echojay::CalibLoop::fromVar(...)`).
        // Calling it through an instance compiles and throws the result away - the first cut did, and the leg
        // then measured a default-constructed loop.
        auto second = echojay::CalibLoop::fromVar (carried);
        // THE PRODUCT'S OWN ADOPT PATH: LinkProcessor reads the sidecar, then calls onHandover() on a running
        // loop (LinkProcessor.cpp, "if (calibLoop_.running())"). Sean's log shows exactly that - window 16
        // state=handover - so this leg calls it too, or it would be testing a road nothing drives.
        const auto handoverLine = second.onHandover();
        std::printf ("    (4) %s\n", handoverLine.toRawUTF8());
        check (second.active(), "(4) fixture: the state crossed the handover");
        check (handoverLine.contains ("state=handover"), "(4) fixture: ...through the product's own adopt path");
        int stale = 0, judged = 0;
        for (int i = 0; i < 4; ++i)
        {
            echojay::CalibLoop::Window s2 = w;
            s2.heardSeconds = 2.7f + 3.0f * (float) i;      // the SECOND host's own tally, from zero
            const auto st = second.onWindow (s2, 3000.0);
            if (st.logLine.contains ("state=stale-window")) ++stale; else ++judged;
        }
        std::printf ("    (4) after the handover: %d judged, %d stale, in 4 windows\n", judged, stale);
        check (stale <= 2 && judged >= 2,
               "(4) the second host gets a FRESH window within two windows of the handover  (RED as it stood: "
               "187 consecutive stale-window lines over nine minutes, because lastHeardS rode the sidecar and it "
               "is a reading of the FIRST host's tally)",
               juce::String (stale) + " stale of 4");
    }

    // ---- (4b) A SLIVER IS NOT A WINDOW (21t-m item 5, 29 Sep 2026 ruling) --------------------------------
    {
        std::printf ("\n-- (4b) a stopping transport's tail must not advance the settle --\n");
        // Sean's MV2 pass, windows 50-55:
        //   w50-54  slot 21.4s  state=stale-window     correctly rejected
        //   w55     slot 22.1s  state=settled          0.7 s of new audio, judged as a WHOLE 3 s window
        // The gate asked "did heard advance at all" (+1 ms), so the tail of a stopping transport spent one of
        // the settle's three windows. Replayed here at Sean's own numbers.
        echojay::CalibLoop l;
        beginDriven (l, passiveDriveCfg ("MV2 (s)"));
        echojay::CalibLoop::Window w;
        w.measured = true; w.silent = false; w.grDb = 0.2f; w.levelChangeDb = 0.1f; w.inTruePeakDb = -12.0f;
        w.heardSeconds = 21.4f;
        const auto first = l.onWindow (w, 3000.0);          // a judged window at 21.4 s
        juce::ignoreUnused (first);
        int stale = 0, judged = 0;
        for (int i = 0; i < 5; ++i)                          // w50-54: heard frozen at 21.4
        { const auto st = l.onWindow (w, 3000.0); if (st.logLine.contains ("stale-window")) ++stale; else ++judged; }
        check (stale == 5 && judged == 0,
               "(4b) five windows with the heard clock frozen are all stale",
               juce::String (stale) + " stale, " + juce::String (judged) + " judged");
        w.heardSeconds = 22.1f;                              // w55: Sean's 0.7 s sliver
        const auto sliver = l.onWindow (w, 3000.0);
        check (sliver.logLine.contains ("stale-window"),
               "(4b) ...and 0.7 s of new audio is STILL stale - a judged window is a WINDOW's worth  (RED as it "
               "stood: state=settled, and it spent one of the settle's three)",
               sliver.logLine.fromLastOccurrenceOf ("state=", true, false));
        w.heardSeconds = 21.4f + 3.0f;                       // a whole window's worth
        const auto full = l.onWindow (w, 3000.0);
        check (! full.logLine.contains ("stale-window"),
               "(4b) ...while a WHOLE window's worth is judged, so the settle still runs on real audio",
               full.logLine.fromLastOccurrenceOf ("state=", true, false));
    }

    {   // (4h) 6 OCT 2026, SEAN'S RULING: AN INPUT-DRIVE AMOUNT CONTROL CANNOT REPORT GR FROM THE LEVEL METHOD.
        // His UAD 1176LN Rev E at Input -21, 10:41:45: "gr=-0.0 ... grLevel=-4.5", the card said "4.5 dB louder out
        // than in", the VU was pinned and he could hear heavy compression. On a unit whose amount control is INPUT
        // DRIVE, out-minus-in is the input gain MINUS the gain reduction, so neither can be recovered from it.
        // Order ruled: the plugin's own GR meter, then crest, otherwise say plainly that GR cannot be measured.
        auto c = passiveDriveCfg ("UAD UA 1176LN Rev E");
        c.actuator = echojay::CalibLoop::Actuator::Input;
        c.params.clear(); c.params.add ("Input");
        {
            echojay::CalibLoop probe;
            auto cIn = c; cIn.noFreshWait = true;
            probe.begin (cIn);
            check (probe.amountIsInputDrive(),
                   "(4h) a loop moving the plugin's own Input is recognised as input drive");
        }
        {   // ...and ECHOJAY'S OWN drive is NOT, because the IN tally is taken after that trim, so it is in neither
            // side of out-minus-in. Counting it would disable the level method on every ordinary drive pass.
            echojay::CalibLoop probe;
            auto ejDrive = passiveDriveCfg ("EJ drive");   // Actuator::Drive - EchoJay's own staging trim
            ejDrive.noFreshWait = true;
            probe.begin (ejDrive);
            check (! probe.amountIsInputDrive(),
                   "(4h) ...while EchoJay's own staging drive is NOT input drive - the IN tally is taken after it");
        }
        {   // A THRESHOLD unit is not input drive either.
            echojay::CalibLoop probe;
            auto t = passiveDriveCfg ("Tube-Tech CL 1B");
            t.actuator = echojay::CalibLoop::Actuator::Threshold;
            t.params.clear(); t.params.add ("Threshold");
            t.noFreshWait = true;
            probe.begin (t);
            check (! probe.amountIsInputDrive(), "(4h) ...and a Threshold unit is not");
        }
        // AND THE REPORTED FIGURE: a window whose sensor says the GR is unmeasurable must not report a number -
        // 20 dB of real reduction behind +20 dB of input gain reads as 0 dB out-minus-in, which is the trap.
        echojay::CalibLoop l2;
        c.noFreshWait = true;
        l2.begin (c);
        echojay::CalibLoop::Window w;
        w.measured = true; w.silent = false; w.heardSeconds = 40.0f; w.inTruePeakDb = -12.0f;
        w.grDb = std::numeric_limits<float>::quiet_NaN();     // the host's "unmeasurable-inputdrive" verdict
        w.grSensor = "unmeasurable-inputdrive";
        w.levelChangeDb = 0.0f;                              // +20 dB in, 20 dB GR: out-minus-in is ZERO
        l2.onWindow (w, 3000.0);
        const float reported = l2.measuredGrDb();
        check (! (reported == reported) || std::abs (reported) > 0.5f,
               "(4h) 20 dB of reduction behind 20 dB of input gain is NOT reported as 0 dB of GR - it is reported as "
               "unknown  (RED as it stood: the level method answered 0 and the card quoted it)",
               (reported == reported) ? juce::String (reported, 2) + " dB" : juce::String ("unknown"));
    }

    {   // (4g) 5 OCT 2026, SEAN'S RULING: AN UNKNOWN HEARD CLOCK MEANS WAIT, NOT "JUDGE IT ANYWAY".
        // The dangerous direction is the silent one: if a production begin site ever fails to fill heardAtBeginS,
        // the loop must lose one window rather than go back to judging a window that predates the block. So this
        // leg uses a Config with NO clock and NO opt-out - deliberately NOT beginDriven - and asserts the first
        // window is held. The opt-out exists for harnesses only, and it has to be said out loud.
        echojay::CalibLoop l;
        auto cNoClock = passiveDriveCfg ("unknown clock");   // heardAtBeginS stays NaN, noFreshWait stays false
        l.begin (cNoClock);
        echojay::CalibLoop::Window w;
        w.measured = true; w.silent = false; w.grDb = 4.3f; w.levelChangeDb = -5.0f; w.inTruePeakDb = -12.0f;
        w.heardSeconds = 40.0f;                              // a tally that has heard plenty, all of it BEFORE this
        const auto firstNoClock = l.onWindow (w, 3000.0);
        check (firstNoClock.logLine.contains ("awaiting-fresh-window"),
               "(4g) with no heard clock stated, the FIRST window is not judged - unknown is the conservative "
               "direction, because a begin site that forgot to fill it would otherwise judge pre-block audio",
               firstNoClock.logLine.fromLastOccurrenceOf ("state=", true, false));
        w.heardSeconds = 43.0f;                              // one whole window later
        const auto secondNoClock = l.onWindow (w, 3000.0);
        check (! secondNoClock.logLine.contains ("awaiting-fresh-window"),
               "(4g) ...and the next whole window IS judged, so the cost is one window and not the loop",
               secondNoClock.logLine.fromLastOccurrenceOf ("state=", true, false));
    }

    {   // (4f) 5 OCT 2026, SEAN'S ITEM 3: THE FIRST JUDGED WINDOW MUST START AFTER THE WRITE.
        // His 11:24:18 pass, from the session log:
        //   11:24:18.913  block carried no start_db - READ "Threshold" off the plugin: -13.60 dB
        //   11:24:18.913  slot 3 both legs reset ... no window from before this can enter a sample
        //   11:24:18.963  window 1 gr=4.3 ... settleHeard=0.0s          <- 50 ms later, JUDGED
        // gr=4.3 was the reading from the PREVIOUS setting (-8.8 dB, band 4.5-5.0). The Threshold had moved 4.8 dB
        // OUTSIDE the loop, so pendingStep said nothing had moved and the one-window skip was cleared; the user was
        // told "measured about 4.3 dB on the loud phrases (aimed for 6.5)" about a setting never measured at all.
        // The slot's heard clock is the bar: a judged window needs a WHOLE window of audio heard since the write.
        echojay::CalibLoop l;
        {   // THE SLOT HAD HEARD 85 s WHEN THE BLOCK OPENED - his log's slotHeard, and what the host now passes in.
            auto c3 = passiveDriveCfg ("Tube-Tech CL 1B (item 3)");
            c3.heardAtBeginS = 85.0f;
            l.begin (c3);          // NOT beginDriven: this leg supplies the clock, which is what it is testing

        }
        echojay::CalibLoop::Window w;
        w.measured = true; w.silent = false; w.grDb = 4.3f; w.levelChangeDb = -5.0f; w.inTruePeakDb = -12.0f;
        w.heardSeconds = 85.0f;                   // the slot had already heard 85 s BEFORE the write
        const auto immediate = l.onWindow (w, 3000.0);
        check (immediate.logLine.contains ("awaiting-fresh-window"),
               "(4f) the window arriving 50 ms after the write is NOT judged  (RED as it stood: it was judged, and "
               "the figure it reported described the setting before the move)",
               immediate.logLine.fromLastOccurrenceOf ("state=", true, false));
        w.heardSeconds = 86.5f;                   // 1.5 s later: still not a whole window of new audio
        const auto partial = l.onWindow (w, 3000.0);
        check (! partial.logLine.contains ("settled") && ! partial.logLine.contains ("landed"),
               "(4f) ...and neither is one 1.5 s later - a FULL window of post-write audio is the bar",
               partial.logLine.fromLastOccurrenceOf ("state=", true, false));
        w.heardSeconds = 88.0f;                   // 85.0 + one whole 3 s window
        const auto fresh = l.onWindow (w, 3000.0);
        check (! fresh.logLine.contains ("awaiting-fresh-window"),
               "(4f) ...while a whole window of post-write audio IS judged, so the answer still arrives",
               fresh.logLine.fromLastOccurrenceOf ("state=", true, false));
    }

    // ---- (5) THE RATE LIMIT ITSELF, AT REAL TIME ---------------------------------------------------------
    {
        std::printf ("\n-- (5) the 3 s window rate limit, on the real clock --\n");
        // Ruled 29 Sep: the other four cases run on a virtual clock so the gate costs seconds. This one does not,
        // because the thing it proves IS the wall-clock limit: ticking faster than 3 s must judge nothing, and
        // the harness's own pacing assumption must be a fact rather than a belief.
        Rig r (ChannelType::LeadVocal, 6.0f, /*realClock*/ true);
        feed (r.proc, r.prog, 6.0);
        r.proc.calibStart ({}, passiveDriveCfg ("Rate limit"));
        juce::String first, second;
        feed (r.proc, r.prog, 3.0); r.proc.calibTick ({}); first = r.proc.calibLastLogLine();
        // ...and again immediately, which is INSIDE the 3 s window only if this machine fed 3 s of audio in less
        // than 3 s of wall clock. It normally takes a fifth of a second - but this leg failed once in a gate run
        // with 53 guards and malloc hardening competing for the machine, and a leg whose premise the machine can
        // take away is not a test of the product. So the elapsed clock is MEASURED, and when the premise is gone
        // the leg says so with the number instead of failing: a timing is not evidence until its clock is stated.
        const double beforeSecond = juce::Time::getMillisecondCounterHiRes();
        feed (r.proc, r.prog, 3.0); r.proc.calibTick ({}); second = r.proc.calibLastLogLine();
        const double tookMs = juce::Time::getMillisecondCounterHiRes() - beforeSecond;
        if (tookMs < 3000.0)
            check (second == first,
                   "(5) a second tick INSIDE the 3 s window judges nothing - the line is unchanged",
                   second.isEmpty() ? juce::String ("(no line)")
                                    : (second.fromLastOccurrenceOf ("window", true, false).substring (0, 40)
                                       + "  after " + juce::String (tookMs, 0) + " ms"));
        else
            std::printf ("  note  (5) NOT TESTED: feeding 3 s of audio took %.0f ms of WALL CLOCK on this machine, "
                         "so the second tick was outside the 3 s window and there was no 'inside' to test. The "
                         "rate limit itself is still proved by the leg below.\n", tookMs);
        // ...and after the window has actually passed, it judges again
        const double waitUntil = juce::Time::getMillisecondCounterHiRes() + 3100.0;
        while (juce::Time::getMillisecondCounterHiRes() < waitUntil) pumpMs (50);
        feed (r.proc, r.prog, 3.0); r.proc.calibTick ({});
        const auto third = r.proc.calibLastLogLine();
        check (third != second && third.isNotEmpty(),
               "(5) ...and once 3 s of WALL CLOCK have passed it judges the next window",
               third.fromLastOccurrenceOf ("window", true, false).substring (0, 40));
    }

    // ---- (6a) THE PLAN'S CLASSIFICATION COUNTS TOO (21t-m item 6a, 29 Sep 2026) --------------------------
    {
        std::printf ("\n-- (6a) a dynamics slot the plugin's own category does not admit to --\n");
        // Sean's 21:41:53 log, six times: "EJDialSummary: loops started 1 of 0 dynamics slots". One loop running
        // against a count of zero is a sentence that cannot be true. The slot was Waves VComp (s); the count read
        // desc.category only, and the SERVER had it right all along - "EJDialable: slot 2 (\"VComp (s)\") ...
        // category=compressor".
        //
        // MEASURED while writing this leg (30 Sep): a BUILT-IN slot carries no fingerprint - getSlotIdentity(0).fp
        // is empty for EchoJay Gain - so the map side of the OR cannot be keyed on one, and this rig holds only
        // built-ins. What is provable here is the half that runs on every slot: the plugin's own category. The
        // map half is proved by the compressor built-in, whose OWN category says compressor, and by the count
        // reading BOTH: a chain of one dynamics and one not counts exactly one.
        Rig r (ChannelType::LeadVocal, 6.0f);
        check (r.h.getSlotIdentity (0).fp.isEmpty(),
               "(6a) measured: a built-in slot carries no fingerprint, so the map half of the OR cannot be keyed "
               "on one in this rig - recorded, not assumed",
               "fp=\"" + r.h.getSlotIdentity (0).fp + "\"");
        check (r.h.dynamicsSlotCount() == 0,
               "(6a) an EchoJay Gain slot is not a dynamics slot", juce::String (r.h.dynamicsSlotCount()));
        // THE MAP HALF, on a slot that HAS a fingerprint. Apple's AUDelay: on every Mac, not PACE-wrapped (the
        // same plugin preflight_guard uses for that reason). Its own category is a delay - nothing dynamics about
        // it - so the ONLY thing that can make it count is the map, which is exactly Sean's VComp.
        juce::PluginDescription dly;
        dly.name = "AUDelay"; dly.pluginFormatName = "AudioUnit";
        dly.fileOrIdentifier = "AudioUnit:Effects/aufx,dely,appl";
        dly.uniqueId = dly.deprecatedUid = (int) (juce::int64) juce::String ("64607a6d").getHexValue64();
        r.h.loadPluginAsync (dly, ChainHost::LoadOrigin::User, {});
        for (int k = 0; k < 40 && r.h.getNumSlots() < 2; ++k) pumpMs (100);
        check (r.h.getNumSlots() == 2, "(6a) precondition: Apple's AUDelay loaded as slot 2 - without a REAL "
               "plugin there is no fingerprint to key a map by, and this leg could not be written",
               juce::String (r.h.getNumSlots()) + " slot(s)");
        const auto fp2 = r.h.getSlotIdentity (1).fp;
        check (fp2.isNotEmpty(), "(6a) precondition: it carries a fingerprint", fp2.substring (0, 16));
        check (r.h.dynamicsSlotCount() == 0,
               "(6a) precondition: a delay is not a dynamics slot by its OWN category",
               juce::String (r.h.dynamicsSlotCount()));
        if (fp2.isNotEmpty())
        {
            auto* m = new juce::DynamicObject(); m->setProperty ("category", "compressor");
            auto* maps = new juce::DynamicObject(); maps->setProperty (fp2, juce::var (m));
            r.h.storeParamMaps (juce::var (maps));
            pumpMs (150);
            check (r.h.dynamicsSlotCount() == 1,
                   "(6a) a slot whose MAP says category=compressor IS a dynamics slot  (RED as it stood: the count "
                   "read desc.category alone, so \"loops started 1 of 0 dynamics slots\" printed six times while "
                   "a loop was running on Waves VComp)",
                   juce::String (r.h.dynamicsSlotCount()) + " of " + juce::String (r.h.getNumSlots()));
        }
    }

    // ---- (6b) A SWITCH IS NOT AN ACTUATOR (21t-m item 6b, 29 Sep 2026) -----------------------------------
    {
        std::printf ("\n-- (6b) a two-position switch offered as the drive control --\n");
        // Sean's 21:23:41.538: 'Compress: manual 0.000 (unknown position "-15" (this control has Off | On))'.
        // The plan named an Off|On switch as the control to dial, and the loop tried to write -15 to it. A control
        // with two discrete positions cannot take a dB, whatever it is called, so the loop must refuse it.
        //
        // MEASURED while writing this leg (30 Sep): EchoJay's own devices expose NO juce parameters -
        // EedDeviceProcessor never calls addParameter - so the getParameters() test this started as was blind to
        // every built-in and returned "" for both a switch and a knob. The product now asks the device's SCHEMA
        // first (a `boolean` spec, or one with exactly two choices), which is where a built-in's switches live.
        Rig r (ChannelType::LeadVocal, 6.0f);
        const auto* lim = BuiltinDeviceRegistry::instance().findByName ("EchoJay Limiter");
        check (lim != nullptr, "(6b) precondition: EchoJay Limiter is registered");
        if (lim != nullptr)
        {
            EchoJayBorrowHostTestAccess::loadBuiltin (r.h, BuiltinDeviceRegistry::descriptionFor (*lim));
            pumpMs (200);
            const int slot = 1;
            check (r.h.getSlotInfo (slot).name.contains ("Limiter"),
                   "(6b) precondition: the limiter landed in slot 2", r.h.getSlotInfo (slot).name);
            if (auto* proc = r.h.getSlotProcessor (slot))
                check (proc->getParameters().isEmpty(),
                       "(6b) measured: the built-in publishes NO juce parameters, which is why the schema is asked "
                       "- recorded, not assumed",
                       juce::String (proc->getParameters().size()) + " juce parameter(s)");
            // true_peak is declared boolean in EedLimiterProcessor::schema(); ceiling_db is a dB range.
            check (r.h.switchNamedAsActuator (slot, { "true_peak" }) == "true_peak",
                   "(6b) a BOOLEAN control named as the actuator is reported as a switch  (RED as it stood: "
                   "nothing looked, and the loop wrote -15 dB to an Off|On control)",
                   "\"" + r.h.switchNamedAsActuator (slot, { "true_peak" }) + "\"");
            check (r.h.switchNamedAsActuator (slot, { "mode" }).isEmpty(),
                   "(6b) ...a THREE-choice selector is not a switch - two positions is the test, not discreteness",
                   "\"" + r.h.switchNamedAsActuator (slot, { "mode" }) + "\"");
            check (r.h.switchNamedAsActuator (slot, { "ceiling_db" }).isEmpty(),
                   "(6b) ...and a CONTINUOUS dB control is not either - the refusal cannot swallow a real actuator",
                   "\"" + r.h.switchNamedAsActuator (slot, { "ceiling_db" }) + "\"");
            check (r.h.switchNamedAsActuator (slot, { "no_such_control" }).isEmpty(),
                   "(6b) ...nor a control the plugin does not have");
        }
    }

    // ---- (6c) A LOOP THE RACK MOVED UNDER (21t-m item 6c, 29 Sep 2026) -----------------------------------
    {
        std::printf ("\n-- (6c) a loop left running by a rebuild --\n");
        // Sean's 21:53:21: a new build replaced the rack ("staleness guards passed rev=46 slots=0 base=0 ops=7",
        // then "EJPanel: rebuild slots=1") while the loop for Empirical Labs Mike-E Comp was still running. Its
        // window 116 fired at .738 and it re-posted its opening line at .740 - "Empirical Labs Mike-E Comp is on -
        // play it and I'll tell you what it's doing" - for a plugin that was no longer in the rack. The old guard
        // asked only whether the slot INDEX existed, and it did: slot 1 now held EchoJay EQ.
        //
        // The identity tested is the RACK REVISION, not the plugin's NAME. A first cut compared names and killed
        // four other cases in this file, because a name is the PLAN's label for a slot and need not be the host's.
        Rig r (ChannelType::LeadVocal, 6.0f);
        feed (r.proc, r.prog, 6.0);
        const juce::String uid;
        const int revAtStart = r.h.getChainRevision();
        const auto identAtStart = r.h.slotIdentityKey (0);
        r.proc.calibStart (uid, passiveDriveCfg ("Fake Comp +6"));
        // 30 Sep (letter (n)): the witness is the SLOT'S IDENTITY, not the rack revision - a deferred settle or a
        // map serve moved the revision under a loop that had just started and cancelled it.
        check (r.proc.calibLoad (uid).slotIdent == identAtStart && identAtStart.isNotEmpty(),
               "(6c) the loop is stamped with the identity of the slot it was started on",
               "\"" + r.proc.calibLoad (uid).slotIdent + "\"");
        // Judged windows first, on runLoop's pacing, so what follows cancels a loop that was demonstrably WORKING.
        for (int k = 0; k < 4; ++k)
        { feed (r.proc, r.prog, 3.0); r.virtualMs += 3050.0; r.proc.calibTick (uid); }
        check (r.proc.calibLoad (uid).running() && r.proc.calibLoad (uid).window >= 1,
               "(6c) precondition: it judges windows normally while the rack is untouched - the cancel must not "
               "be a loop that never ran",
               "window " + juce::String (r.proc.calibLoad (uid).window));
        // 30 Sep 2026, letter (n): AN ADD AT THE END IS NOT A REASON TO CANCEL. Sean's 19:03:33 log: a deferred
        // settle and two in-flight fallback map serves bumped the rack revision 9 -> 10 with no slot added,
        // removed or moved, and the Tube-Tech loop cancelled reading "the rack was rebuilt under this loop". The
        // witness is now slot identity, so this half of (6c) asserts SURVIVAL where it used to assert a cancel.
        const auto* g2 = BuiltinDeviceRegistry::instance().findByName ("EchoJay Gain");
        if (g2 != nullptr) EchoJayBorrowHostTestAccess::loadBuiltin (r.h, BuiltinDeviceRegistry::descriptionFor (*g2));
        pumpMs (200);
        check (r.h.getChainRevision() != revAtStart,
               "(6c) precondition: the add moved the rack revision",
               juce::String (revAtStart) + " -> " + juce::String (r.h.getChainRevision()));
        check (r.h.slotIdentityKey (0) == identAtStart,
               "(6c) ...while slot 1's own identity is untouched by it",
               "\"" + r.h.slotIdentityKey (0) + "\"");
        feed (r.proc, r.prog, 3.0); r.virtualMs += 3050.0;
        const auto said = r.proc.calibTick (uid);
        const auto after = r.proc.calibLoad (uid);
        check (after.running(),
               "(6c) the loop SURVIVES it  (RED as it stood: the rack revision was the witness, so a settle or a "
               "map serve cancelled a loop that nothing had touched)",
               after.running() ? ("still running for \"" + after.plugin + "\"") : juce::String ("CANCELLED"));
        juce::ignoreUnused (said);
        check (! r.proc.calibLastLogLine().contains ("CANCELLED"),
               "(6c) ...and says nothing about being cancelled",
               r.proc.calibLastLogLine().substring (0, 80));
    }

    // ---- (13) A REORDER THAT MOVES THE LOOP'S PLUGIN OFF ITS INDEX STILL CANCELS (letter (n)) ------------
    {
        std::printf ("\n-- (13) the cancel that must still happen: a different plugin at the loop's index --\n");
        // The other half of letter (n). What 21t-m item 6c was for is still enforced - Sean's 21:53:21 rebuild
        // left a loop for Empirical Labs Mike-E Comp running while slot 1 held EchoJay EQ - but the test is now
        // "does this index still hold this plugin", which is the question that was always meant.
        Rig r (ChannelType::LeadVocal, 6.0f);
        const auto* lim = BuiltinDeviceRegistry::instance().findByName ("EchoJay Limiter");
        if (lim != nullptr) EchoJayBorrowHostTestAccess::loadBuiltin (r.h, BuiltinDeviceRegistry::descriptionFor (*lim));
        pumpMs (200);
        check (r.h.getNumSlots() == 2 && r.h.slotIdentityKey (0) != r.h.slotIdentityKey (1),
               "(13) precondition: two slots with different identities",
               "\"" + r.h.slotIdentityKey (0) + "\" vs \"" + r.h.slotIdentityKey (1) + "\"");
        feed (r.proc, r.prog, 6.0);
        const juce::String uid;
        r.proc.calibStart (uid, passiveDriveCfg ("Fake Comp +6"));
        for (int k = 0; k < 4; ++k) { feed (r.proc, r.prog, 3.0); r.virtualMs += 3050.0; r.proc.calibTick (uid); }
        check (r.proc.calibLoad (uid).running(),
               "(13) precondition: the loop is running on slot 1",
               "window " + juce::String (r.proc.calibLoad (uid).window));
        const auto identBefore = r.proc.calibLoad (uid).slotIdent;
        // THE REORDER: the loop's plugin is no longer at index 0.
        r.h.moveSlot (0, +1);   // direction, not a target index
        pumpMs (200);
        check (r.h.slotIdentityKey (0) != identBefore,
               "(13) precondition: slot 1 now holds a different plugin",
               "\"" + identBefore + "\" -> \"" + r.h.slotIdentityKey (0) + "\"");
        feed (r.proc, r.prog, 3.0); r.virtualMs += 3050.0;
        const auto saidAfter = r.proc.calibTick (uid);
        check (! r.proc.calibLoad (uid).running(),
               "(13) the loop is CANCELLED, because its index no longer holds its plugin",
               r.proc.calibLoad (uid).running() ? juce::String ("still running") : juce::String ("cancelled"));
        check (saidAfter.isEmpty(),
               "(13) ...and it asks nothing",
               saidAfter.isEmpty() ? juce::String ("(nothing said)") : saidAfter.substring (0, 50));
        check (r.proc.calibLastLogLine().contains ("CANCELLED")
                   && r.proc.calibLastLogLine().contains ("no longer holds"),
               "(13) ...and the log names the slot and the plugin rather than a revision number",
               r.proc.calibLastLogLine().substring (0, 100));
    }

    // ---- (7) A BUILD IS A ONE-SHOT (30 Sep 2026 ruling, Sean's log on pair (d)) --------------------------
    {
        std::printf ("\n-- (7) a buildHold loop: one window, one write, one line, then it is over --\n");
        // Sean's 10:45 log on (d):
        //   window 1   settle=3/3                       the settle opens already spent, correct
        //   window 6   state=landed-level-already-held  FIVE windows spent getting to the hold
        //   then       "EchoJay Saturation on, set from the working position. Doing about 0.1 dB..."  <- the ASK line
        //   then       88 more windows, state=holding
        // Ruled: a build applies the working position, takes ONE measurement window, sets EchoJay's own OUT once,
        // posts exactly "Built. ..." and ENDS. The band result goes to the log only. Never the ask line, never a
        // settle line, no windows after the close.
        Rig r (ChannelType::LeadVocal, 6.0f);
        feed (r.proc, r.prog, 6.0);
        const juce::String uid;
        auto cfg = passiveDriveCfg ("EchoJay Saturation");
        cfg.purpose = echojay::CalibLoop::Purpose::buildHold;
        cfg.working = true;                       // "set from the working position", as Sean's build was
        r.proc.calibStart (uid, cfg);
        check (r.proc.calibLoad (uid).purpose == echojay::CalibLoop::Purpose::buildHold,
               "(7) precondition: the loop is a build");
        // Ticked until it speaks, and the WINDOWS are counted rather than the ticks: the very first tick only
        // arms the 3 s window clock (calibLastWindowMs_ starts at 0), so it judges nothing by design.
        juce::StringArray windowLines;
        juce::String said; bool replaces = false; int windowsBeforeClose = 0;
        for (int k = 0; k < 24 && said.isEmpty(); ++k)     // (m): the seek runs before the hold closes
        {
            feed (r.proc, r.prog, 3.0); r.virtualMs += 3050.0;
            r.proc.calibTick (uid);
            const auto lg = r.proc.calibLastLogLine();
            if (lg.isNotEmpty() && ! windowLines.contains (lg)) windowLines.add (lg);
            said = r.proc.calibTakeAsk (uid, &replaces);
        }
        windowsBeforeClose = windowLines.size();
        check (said.startsWith ("Built."),
               "(7) it closes with \"Built. ...\"  (RED as it stood: the measure-and-ask line \"... Doing about "
               "N dB of gain reduction. Say 'ease off' or 'more'\")",
               said.isEmpty() ? juce::String ("(nothing said)") : said);
        check (! said.contains ("Doing about") && ! said.contains ("ease off"),
               "(7) ...and it is NEVER the ask line", said);
        // SUPERSEDED 30 Sep 2026 (letter (m)): this asserted ONE measurement window. A build now seeks the band
        // first, so it takes as many windows as the seek needs, bounded by the cap. What is still true - and is
        // what (7) was really for - is that the HOLD runs once and the loop ends at its close.
        check (windowsBeforeClose <= echojay::CalibLoop::kBuildMaxWindows + 2,
               "(7) ...inside the seek's own bound (was: exactly ONE window, superseded by letter (m))",
               juce::String (windowsBeforeClose) + " window(s), cap "
                   + juce::String (echojay::CalibLoop::kBuildMaxWindows));
        check (! r.proc.calibLoad (uid).running() && ! r.proc.calibLoad (uid).active(),
               "(7) ...and the loop has ENDED - the holding tail is deleted",
               r.proc.calibLoad (uid).running() ? juce::String ("still running")
                                                : (r.proc.calibLoad (uid).active() ? juce::String ("not running, still active")
                                                                                   : juce::String ("ended")));
        check (! replaces,
               "(7) ...and it does not claim to replace an opening line, because a build posted none");
        // ...and nothing happens afterwards, however long the transport runs.
        const auto lastLine = r.proc.calibLastLogLine();
        int laterWindows = 0, laterAsks = 0;
        for (int k = 0; k < 6; ++k)
        {
            feed (r.proc, r.prog, 3.0); r.virtualMs += 3050.0;
            r.proc.calibTick (uid);
            if (r.proc.calibLastLogLine() != lastLine) ++laterWindows;
            if (r.proc.calibTakeAsk (uid, &replaces).isNotEmpty()) ++laterAsks;
        }
        check (laterWindows == 0 && laterAsks == 0,
               "(7) ...no window is judged and nothing is said after the close  (RED as it stood: 88 more windows, "
               "state=holding)",
               juce::String (laterWindows) + " window(s), " + juce::String (laterAsks) + " ask(s)");
    }

    // ---- (7b) THE PURPOSE SURVIVES THE SIDECAR (30 Sep 2026) ---------------------------------------------
    {
        std::printf ("\n-- (7b) a loop on a LINK rack keeps its purpose across the store/load round trip --\n");
        // This is why Sean saw the ASK line at all. For the OWN rack calibStore holds a live CalibLoop, so the
        // purpose survives; for a LINK rack every tick round-trips it through toVar/fromVar, and `purpose` was in
        // neither. So a build on a borrowed Link rack was a build for exactly one tick and an askRung from the
        // second one onward - which is the purpose the member default carries.
        echojay::CalibLoop l;
        auto cfg = passiveDriveCfg ("VComp (s)");
        cfg.purpose = echojay::CalibLoop::Purpose::buildHold;
        beginDriven (l, cfg);
        check (l.purpose == echojay::CalibLoop::Purpose::buildHold, "(7b) precondition: begin() set it");
        const auto round = echojay::CalibLoop::fromVar (l.toVar());
        check (round.purpose == echojay::CalibLoop::Purpose::buildHold,
               "(7b) a build is STILL a build after toVar/fromVar  (RED as it stood: purpose was in neither, so "
               "every Link-rack tick reset it to the askRung default)",
               round.purpose == echojay::CalibLoop::Purpose::buildHold ? juce::String ("buildHold")
                                                                      : juce::String ("askRung"));
        echojay::CalibLoop la;
        auto ca = passiveDriveCfg ("VComp (s)");
        ca.purpose = echojay::CalibLoop::Purpose::askRung;
        beginDriven (la, ca);
        check (echojay::CalibLoop::fromVar (la.toVar()).purpose == echojay::CalibLoop::Purpose::askRung,
               "(7b) ...and an ask is still an ask, so the field is carried and not hard-coded");
    }

    // ---- (8) AN ASK IS A ONE-SHOT TOO (30 Sep 2026 ruling) ----------------------------------------------
    {
        std::printf ("\n-- (8) askRung: one rung, one line, then the loop ends --\n");
        // Ruled with (7): the holding tail is deleted for EVERY purpose. The rung closes with its one line -
        // "Drive +4 -> +5, 3.1 dB on the loud phrases, level held." - and the loop is over. No settling, no
        // holding windows, no re-post.
        Rig r (ChannelType::LeadVocal, 6.0f);
        feed (r.proc, r.prog, 6.0);
        const juce::String uid;
        auto cfg = passiveDriveCfg ("Fake Comp +6");
        cfg.purpose = echojay::CalibLoop::Purpose::askRung;
        r.proc.calibStart (uid, cfg);
        // THE COMPARATIVE: the same block again, carrying the user's nudge. That is what buys the one rung.
        auto again = cfg; again.nudge = 1; again.haveBand = true;
        r.proc.calibStart (uid, again);
        check (r.proc.calibLoad (uid).purpose == echojay::CalibLoop::Purpose::askRung
                   && r.proc.calibLoad (uid).pendingStep != 0,
               "(8) precondition: an ask with one rung owed",
               "pendingStep " + juce::String (r.proc.calibLoad (uid).pendingStep));
        juce::String said; bool replaces = false; int ticks = 0;
        for (int k = 0; k < 20 && said.isEmpty(); ++k)
        {
            feed (r.proc, r.prog, 3.0); r.virtualMs += 3050.0; r.proc.calibTick (uid); ++ticks;
            const auto a = r.proc.calibTakeAsk (uid, &replaces);
            if (a.isNotEmpty() && ! a.endsWith ("landing it as it plays...")) said = a;
        }
        check (said.isNotEmpty() && said.contains (juce::String::fromUTF8 ("\xe2\x86\x92")),
               "(8) it closes with the rung's own line, naming what it moved from and to",
               said.isEmpty() ? juce::String ("(nothing said)") : said);
        check (! r.proc.calibLoad (uid).running() && ! r.proc.calibLoad (uid).active(),
               "(8) ...and the loop has ENDED  (RED as it stood: state stayed Listening and every later window "
               "logged state=holding, for ever)",
               r.proc.calibLoad (uid).active() ? juce::String ("still active") : juce::String ("ended"));
        const auto lastLine = r.proc.calibLastLogLine();
        int laterWindows = 0, laterAsks = 0;
        for (int k = 0; k < 6; ++k)
        {
            feed (r.proc, r.prog, 3.0); r.virtualMs += 3050.0; r.proc.calibTick (uid);
            if (r.proc.calibLastLogLine() != lastLine) ++laterWindows;
            if (r.proc.calibTakeAsk (uid, &replaces).isNotEmpty()) ++laterAsks;
            if (r.proc.calibTakeClosing (uid).isNotEmpty()) ++laterAsks;
        }
        check (laterWindows == 0 && laterAsks == 0,
               "(8) ...and nothing is judged or said afterwards, and nothing is re-posted",
               juce::String (laterWindows) + " window(s), " + juce::String (laterAsks) + " message(s)");
    }

    // ---- (8b) LISTEN REPORTS AND ENDS (30 Sep 2026 ruling) ----------------------------------------------
    {
        std::printf ("\n-- (8b) Listen: it reports, and then it is over --\n");
        // Listen's closes set state to Adjusted or Clamped, which running() already excludes - but active() does
        // not, so calibTick went on judging windows and the loop went on stepping. The closing MESSAGE is taken
        // later by calibTakeClosing, out of the stored loop, so the text has to survive the ending.
        Rig r (ChannelType::LeadVocal, 6.0f);
        feed (r.proc, r.prog, 6.0);
        const juce::String uid;
        // Listen on the drive, with a band the fake compressor's fixed 6 dB can never reach, so it clamps on the
        // step budget rather than hanging on "in band twice running" - a deterministic close either way.
        r.proc.calibStart (uid, 0, "Fake Comp +6", 2.0f, 3.0f, 0.0f, echojay::CalibLoop::Purpose::askRung);
        check (r.proc.calibLoad (uid).mode == echojay::CalibLoop::Mode::Listen,
               "(8b) precondition: the loop is a LISTEN pass");
        juce::String closing; int ticks = 0;
        for (int k = 0; k < 40 && closing.isEmpty(); ++k)
        {
            feed (r.proc, r.prog, 3.0); r.virtualMs += 3050.0; r.proc.calibTick (uid); ++ticks;
            closing = r.proc.calibTakeClosing (uid);
        }
        check (closing.isNotEmpty(),
               "(8b) it posts its closing message", closing.isEmpty() ? juce::String ("(none)") : closing);
        check (! r.proc.calibLoad (uid).active(),
               "(8b) ...and the loop has ENDED  (RED as it stood: Adjusted/Clamped are not running() but they ARE "
               "active(), so the tick kept judging windows after the report)",
               r.proc.calibLoad (uid).active() ? juce::String ("still active") : juce::String ("ended"));
        const auto lastLine = r.proc.calibLastLogLine();
        int laterWindows = 0, laterMsgs = 0;
        for (int k = 0; k < 6; ++k)
        {
            feed (r.proc, r.prog, 3.0); r.virtualMs += 3050.0; r.proc.calibTick (uid);
            if (r.proc.calibLastLogLine() != lastLine) ++laterWindows;
            if (r.proc.calibTakeClosing (uid).isNotEmpty()) ++laterMsgs;
        }
        check (laterWindows == 0 && laterMsgs == 0,
               "(8b) ...and nothing is judged or re-posted afterwards",
               juce::String (laterWindows) + " window(s), " + juce::String (laterMsgs) + " message(s)");
    }

    // ---- (9) ONE HOLD PER COMPRESSOR (30 Sep 2026, B's contract) ----------------------------------------
    {
        std::printf ("\n-- (9) two compressors in one build: each holds its own OUT, one line names both --\n");
        // CONTRACT_GROUPS, "Every compressor in a build gets its own hold": a build carried ONE block and which
        // compressor it named was whichever the pass wrote last. Sean's log: the block went to the SECOND
        // compressor, which measured 0.0 dB of gain reduction, while the FIRST did the work with no hold on it.
        // `calibrations` is one block per compressor in chain order; `calibration` is byte-for-byte calibrations[0]
        // and stays the primary.
        Rig r (ChannelType::LeadVocal, 6.0f);                  // slot 1: +6 dB
        const auto* gn = BuiltinDeviceRegistry::instance().findByName ("EchoJay Gain");
        check (gn != nullptr, "(9) precondition: EchoJay Gain is registered");
        if (gn != nullptr) EchoJayBorrowHostTestAccess::loadBuiltin (r.h, BuiltinDeviceRegistry::descriptionFor (*gn));
        pumpMs (200);
        check (r.h.getNumSlots() == 2, "(9) precondition: a two-compressor rig",
               juce::String (r.h.getNumSlots()) + " slot(s)");
        {   // slot 2 at +3 dB, so the two holds owe DIFFERENT figures and a single shared write cannot pass
            auto* pp = new juce::DynamicObject(); pp->setProperty ("level_db", 3.0);
            auto* wv = new juce::DynamicObject(); wv->setProperty ("params", juce::var (pp));
            r.h.setSlotStructuredSettings (1, juce::var (wv));
            pumpMs (250);
        }
        feed (r.proc, r.prog, 6.0);
        // THE CHAIN OBJECT AS B SENDS IT: calibrations[] with one block per compressor, calibration == [0].
        auto mkBlock = [] (int wireSlot) -> juce::var
        {
            auto* b = new juce::DynamicObject();
            b->setProperty ("mode", "passive"); b->setProperty ("actuator", "drive");
            b->setProperty ("slot", wireSlot);              // 1-based on the wire
            b->setProperty ("source", "working_position");
            juce::Array<juce::var> band; band.add (2.0); band.add (3.0);
            b->setProperty ("gr_target_db", band);
            return juce::var (b);
        };
        auto* chainObj = new juce::DynamicObject();
        juce::Array<juce::var> calibs; calibs.add (mkBlock (1)); calibs.add (mkBlock (2));
        chainObj->setProperty ("calibration", calibs.getReference (0));   // byte-for-byte calibrations[0]
        chainObj->setProperty ("calibrations", calibs);
        const juce::var chainVar (chainObj);
        std::vector<echojay::CalibLoop::Config> cfgs;
        juce::String whyAll;
        const int found = echojay::CalibLoop::configsFromBlock (chainVar, r.h.getNumSlots(), false,
                                                               [&r] (int sl) { return r.h.getSlotInfo (sl).name; },
                                                               cfgs, whyAll);
        check (found == 2 && cfgs.size() == 2 && cfgs[0].slot == 0 && cfgs[1].slot == 1,
               "(9) the parser reads BOTH blocks, in chain order, from `calibrations`  (RED as it stood: there was "
               "no array parser - only `calibration`, one compressor, whichever the pass wrote last)",
               juce::String (found) + " block(s), slots " + (cfgs.size() == 2 ? juce::String (cfgs[0].slot + 1) + "+"
                                                                               + juce::String (cfgs[1].slot + 1)
                                                                             : juce::String ("-")));
        const juce::String uid;
        for (auto& c : cfgs) c.purpose = echojay::CalibLoop::Purpose::buildHold;
        const int started = r.proc.calibStartMany (uid, cfgs);
        check (started == 2, "(9) ...and starts a hold for each", juce::String (started) + " hold(s)");
        juce::String said; bool replaces = false;
        for (int k = 0; k < 40 && said.isEmpty(); ++k)     // (m): each hold seeks the band before it closes
        {
            feed (r.proc, r.prog, 3.0); r.virtualMs += 3050.0; r.proc.calibTick (uid);
            said = r.proc.calibTakeAsk (uid, &replaces);
        }
        const float out1 = r.h.getSlotOutGainDb (0), out2 = r.h.getSlotOutGainDb (1);
        const float in1  = r.h.getSlotPreTrimDb (0), in2 = r.h.getSlotPreTrimDb (1);
        // 30 Sep (letter (m)): the figures are no longer -6.0 / -3.0. A build now DRIVES each slot's IN as it
        // seeks, and OUT carries the drive back plus that slot's own residual - so what matters, and what Sean's
        // defect was about, is that each slot is held SEPARATELY to ITS OWN figure rather than one slot being held
        // and the other left alone.
        check (std::abs (out1) > 0.05f,
               "(9) compressor 1 holds its OWN out", f1 (out1) + " dB (IN " + f1 (in1) + ")");
        check (std::abs (out2) > 0.05f,
               "(9) ...and compressor 2 holds ITS own  (RED as it stood: only one slot was ever held, and Sean's "
               "log held the one doing no work)", f1 (out2) + " dB (IN " + f1 (in2) + ")");
        check (std::abs (out1 - out2) > 0.05f,
               "(9) ...to a DIFFERENT figure, because they are different compressors",
               f1 (out1) + " vs " + f1 (out2) + " dB");
        // 6 Oct 2026: both named, and NEITHER as an ordinal. "Compressor 2" is the fallback for a loop with no
        // name left, which is exactly what the wiped companion produced - so its absence is the assertion.
        check (said.startsWith ("Built.") && said.contains ("EchoJay Gain")
               && ! said.contains ("Compressor 1 set as dialled") && ! said.contains ("Compressor 2 set as dialled"),
               "(9) ...and ONE line names each of them  ((q) changed the wording to \"set as dialled, level "
               "matched\" per compressor; what this leg is for is that BOTH are named)",
               said.isEmpty() ? juce::String ("(nothing said)") : said);
        check (! r.proc.calibLoad (uid).active() && r.proc.calibExtraFor (uid).empty(),
               "(9) ...and every loop has ended - a build is a one-shot however many compressors it holds",
               r.proc.calibLoad (uid).active() ? juce::String ("primary still active")
                                              : juce::String (r.proc.calibExtraFor (uid).size()) + " companion(s) left");
        int laterAsks = 0;
        for (int k = 0; k < 4; ++k)
        {
            feed (r.proc, r.prog, 3.0); r.virtualMs += 3050.0; r.proc.calibTick (uid);
            if (r.proc.calibTakeAsk (uid, &replaces).isNotEmpty()) ++laterAsks;
        }
        check (laterAsks == 0, "(9) ...and nothing is said afterwards", juce::String (laterAsks) + " ask(s)");
    }

    // ---- (9c) THE DEFECT ITSELF, on the road that had it ------------------------------------------------
    {
        std::printf ("\n-- (9c) the single-block road holds ONE slot, which is the fault --\n");
        // The RED for (9) cannot be a compile against the pre-change tree: configsFromBlock and calibStartMany did
        // not exist, so no leg that calls them builds there. This is the next best thing and it is a real one - the
        // OLD road, calibStart with one block, on the SAME two-compressor rig. It holds the block's slot and leaves
        // the other exactly where the build put it, which is Sean's log: the block went to the second compressor,
        // which measured 0.0 dB of gain reduction, while the first did the work with no hold on it at all.
        Rig r (ChannelType::LeadVocal, 6.0f);
        const auto* gn = BuiltinDeviceRegistry::instance().findByName ("EchoJay Gain");
        if (gn != nullptr) EchoJayBorrowHostTestAccess::loadBuiltin (r.h, BuiltinDeviceRegistry::descriptionFor (*gn));
        pumpMs (200);
        {   auto* pp = new juce::DynamicObject(); pp->setProperty ("level_db", 3.0);
            auto* wv = new juce::DynamicObject(); wv->setProperty ("params", juce::var (pp));
            r.h.setSlotStructuredSettings (1, juce::var (wv)); pumpMs (250); }
        feed (r.proc, r.prog, 6.0);
        const juce::String uid;
        auto one = passiveDriveCfg ("Compressor 2");
        one.slot = 1;                                   // the block names the SECOND compressor, as Sean's did
        one.purpose = echojay::CalibLoop::Purpose::buildHold;
        r.proc.calibStart (uid, one);
        juce::String said; bool replaces = false;
        for (int k = 0; k < 40 && said.isEmpty(); ++k)     // (m): the seek runs first
        { feed (r.proc, r.prog, 3.0); r.virtualMs += 3050.0; r.proc.calibTick (uid); said = r.proc.calibTakeAsk (uid, &replaces); }
        check (std::abs (r.h.getSlotOutGainDb (1)) > 0.05f,
               "(9c) the named slot IS held", f1 (r.h.getSlotOutGainDb (1)) + " dB");
        check (std::abs (r.h.getSlotOutGainDb (0)) < 0.05f,
               "(9c) ...and the OTHER compressor is left untouched - one block, one hold, which is the defect "
               "`calibrations` exists to fix",
               f1 (r.h.getSlotOutGainDb (0)) + " dB on the unheld slot");
        check (said.startsWith ("Built.") && ! said.contains ("Compressor 2"),
               "(9c) ...and its line can only speak for the one it held", said);
    }

    // ---- (9b) THE FALLBACK: ONE BLOCK, AS BEFORE -------------------------------------------------------
    {
        std::printf ("\n-- (9b) a chain with only the single `calibration` still works --\n");
        // B's array is being added; a server that has not shipped it yet sends `calibration` alone, and the
        // contract says a chain with NO compressor carries neither field - never an empty array.
        auto* b = new juce::DynamicObject();
        b->setProperty ("mode", "passive"); b->setProperty ("actuator", "drive"); b->setProperty ("slot", 1);
        auto* only = new juce::DynamicObject(); only->setProperty ("calibration", juce::var (b));
        std::vector<echojay::CalibLoop::Config> cfgs; juce::String why;
        check (echojay::CalibLoop::configsFromBlock (juce::var (only), 2, false, nullptr, cfgs, why) == 1
                   && cfgs.size() == 1 && cfgs[0].slot == 0,
               "(9b) the single block is read when the array is absent",
               juce::String ((int) cfgs.size()) + " config(s)");
        auto* none = new juce::DynamicObject(); none->setProperty ("chain", juce::var());
        std::vector<echojay::CalibLoop::Config> c2; juce::String w2;
        check (echojay::CalibLoop::configsFromBlock (juce::var (none), 2, false, nullptr, c2, w2) == 0 && c2.empty(),
               "(9b) ...and a chain with NEITHER field starts nothing",
               juce::String ((int) c2.size()) + " config(s)");
    }

    // ---- (10) A NaN NEVER REACHES A WRITE (30 Sep 2026 ruling) -------------------------------------------
    {
        std::printf ("\n-- (10) start_db ABSENT opens from the staging, and no non-finite value is ever written --\n");
        // My own guard run at 13:52:33 printed, and PASSED anyway:
        //   "EchoJay Gain" slot 1 PASSIVE, ... dialling the drive from nan dB
        //   slot 1 output gain set to nan dB
        //   window 1 ... pre=nan post=nan
        // It passed because the hold then wrote -6.0 over the top of it. The block in that leg carried NO start_db
        // at all; configFromBlock yields NaN for absent exactly as it does for null, meaning "ask the host what is
        // on the slot" - and the V2's caller never asked. LinkProcessor does ("start_db was null on a drive block -
        // opening from the staging already on the slot"), which is why only this side showed it.
        Rig r (ChannelType::LeadVocal, 6.0f);
        // THE STAGING ALREADY ON THE SLOT, as a build leaves it.
        r.h.setSlotPreTrimDb (0, 2.5f);
        pumpMs (50);
        check (std::abs (r.h.getSlotPreTrimDb (0) - 2.5f) < 0.01f,
               "(10) precondition: the slot carries +2.5 dB of staging", f1 (r.h.getSlotPreTrimDb (0)) + " dB");
        // A DRIVE BLOCK WITH start_db ABSENT - not null, absent.
        auto* b = new juce::DynamicObject();
        b->setProperty ("mode", "passive"); b->setProperty ("actuator", "drive"); b->setProperty ("slot", 1);
        { juce::Array<juce::var> band; band.add (2.0); band.add (3.0); b->setProperty ("gr_target_db", band); }
        auto* chainObj = new juce::DynamicObject(); chainObj->setProperty ("calibration", juce::var (b));
        std::vector<echojay::CalibLoop::Config> cfgs; juce::String why;
        echojay::CalibLoop::configsFromBlock (juce::var (chainObj), r.h.getNumSlots(), false, nullptr, cfgs, why);
        check (cfgs.size() == 1 && ! (cfgs[0].startDb == cfgs[0].startDb),
               "(10) precondition: the parser yields NaN for an ABSENT start_db, as it does for null",
               cfgs.empty() ? juce::String ("no config") : juce::String (cfgs[0].startDb));
        const juce::String uid;
        for (auto& c : cfgs) c.purpose = echojay::CalibLoop::Purpose::buildHold;
        r.proc.calibStartMany (uid, cfgs);
        const float inAfter = r.h.getSlotPreTrimDb (0), outAfter = r.h.getSlotOutGainDb (0);
        check (inAfter == inAfter && outAfter == outAfter,
               "(10) neither IN nor OUT is left non-finite by the start  (RED as it stood: \"slot 1 output gain set "
               "to nan dB\", and pre=nan post=nan in the window line)",
               "IN " + f1 (inAfter) + " OUT " + f1 (outAfter));
        check (std::abs (inAfter - 2.5f) < 0.01f,
               "(10) ...and an ABSENT start_db opens from the STAGING, exactly as null does",
               f1 (inAfter) + " dB, staging was 2.5");
        const auto started = r.proc.calibLoad (uid);
        check (started.value == started.value && started.preDb == started.preDb,
               "(10) ...and the loop's own position is a number, so its first window cannot quote nan",
               "value " + f1 (started.value) + " preDb " + f1 (started.preDb));
        // THE WRITE PATH ITSELF REFUSES A NON-FINITE VALUE, whoever calls it and for whatever reason.
        const float inWas = r.h.getSlotPreTrimDb (0), outWas = r.h.getSlotOutGainDb (0);
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        r.h.setSlotPreTrimDb (0, nan);   r.h.setSlotOutGainDb (0, nan);
        r.h.setSlotPreTrimDb (0, inf);   r.h.setSlotOutGainDb (0, -inf);
        pumpMs (50);
        check (std::abs (r.h.getSlotPreTrimDb (0) - inWas) < 0.01f
                   && std::abs (r.h.getSlotOutGainDb (0) - outWas) < 0.01f,
               "(10) a NaN or an infinity handed to slot IN or OUT is REFUSED and the value is left alone  (RED as "
               "it stood: it was written, and the window line quoted it back)",
               "IN " + f1 (r.h.getSlotPreTrimDb (0)) + " (was " + f1 (inWas) + "), OUT "
                   + f1 (r.h.getSlotOutGainDb (0)) + " (was " + f1 (outWas) + ")");
        check (r.h.setSlotControlsToValue (0, juce::StringArray { "level_db" }, nan) == 0,
               "(10) ...and so is a non-finite value handed to the actuator write");
    }

    // ---- (11) A COMPRESSOR'S INPUT IS NOT CEILINGED (letter (l), 30 Sep 2026 ruling) ---------------------
    {
        std::printf ("\n-- (11) the drive on a compressor slot is bounded by the slot range, not by input headroom --\n");
        // Sean's 18:25:43-58: pre +1.0, then +2.0, then state=clamped, "band not reached - drive limited by
        // headroom at +2.0 dB, working 0.0 dB". That is the -3 dBTP no-stage-above ceiling (21p item 3) applied at
        // the COMPRESSOR'S INPUT: the track peaks at -5.0, so the loop allowed itself 2 dB and stopped on a
        // compressor that had not begun to work. A compressor's input is meant to go over its threshold; the
        // ceiling belongs on the OUT, which the hold owns.
        //
        // Driven on the loop directly, so the numbers are exact: a track peaking at -5.0 dBTP, gr far below a
        // 2-3 dB band, opening at +2.0 so the 6-step budget (kMaxSteps) can reach +8.0.
        auto runTo = [] (bool dynamics, float opening) -> echojay::CalibLoop
        {
            echojay::CalibLoop l;
            l.begin ("NEOLD V76U73", 0, 2.0f, 3.0f, opening, echojay::CalibLoop::Purpose::askRung, std::numeric_limits<float>::quiet_NaN(), true);
            l.dynamicsSlot = dynamics;
            echojay::CalibLoop::Window w;
            w.measured = true; w.silent = false;
            w.grDb = 0.0f;                 // nothing like the band, so every window wants another dB
            w.levelChangeDb = 0.0f;
            // THE INPUT TRUE PEAK IS MEASURED AT THE CURRENT DRIVE, so it RISES with it - a fixture that holds it
            // still is unphysical, and the first cut of this leg did exactly that: headroomLimit is
            // preDb + (-3 - inTP), so a frozen inTP let the limit climb with the drive and nothing ever clamped.
            // Sean's track peaks at -5.0 dBTP at the drive it was measured at, which makes the ceiling
            // -3 - (-5) = +2.0 dB of drive, wherever the drive currently sits.
            const float trackPeakAtUnityDrive = -5.0f;   // Sean's track, measured at unity drive
            float heard = 0.0f;
            for (int k = 0; k < 20; ++k)
            {
                w.heardSeconds = (heard += 4.0f);
                w.inTruePeakDb = trackPeakAtUnityDrive + l.preDb;
                l.onWindow (w, 3000.0);
                if (l.state == echojay::CalibLoop::State::Clamped
                    || l.state == echojay::CalibLoop::State::Adjusted) break;
            }
            return l;
        };
        // Opening at +2.0 so the 6-step budget can carry it to +8.0 - Sean's "gets +8, not +2".
        const auto onComp = runTo (true, 2.0f);
        check (std::abs (onComp.preDb - 8.0f) < 0.01f,
               "(11) on a COMPRESSOR slot the drive reaches +8.0 dB on a track peaking at -5 dBTP  (RED as it "
               "stood: headroomLimit = preDb + (-3 - inTP) stopped it, and Sean's log read \"drive limited by "
               "headroom at +2.0 dB\")",
               f1 (onComp.preDb) + " dB reached");
        check (! onComp.headroomStopped,
               "(11) ...and it never reports being stopped by headroom",
               onComp.headroomStopped ? juce::String ("headroomStopped") : juce::String ("not headroom"));
        check (std::abs (onComp.headroomLimit - echojay::CalibLoop::kDriveLimit) < 0.01f,
               "(11) ...because its only bound is the slot range, +12 dB",
               f1 (onComp.headroomLimit));
        // THE OTHER DIRECTION, from the same code and the same window: a slot that is NOT dynamics keeps the 21p
        // item 3 ceiling, so this is a gated change and not a deletion.
        const auto onOther = runTo (false, 2.0f);
        check (onOther.headroomStopped && onOther.preDb < 8.0f - 0.01f,
               "(11) ...while a NON-dynamics slot still stops at the -3 dBTP ceiling, so the ceiling is gated and "
               "not deleted",
               f1 (onOther.preDb) + " dB, " + (onOther.headroomStopped ? "headroom" : "budget"));
        check (std::abs (onOther.preDb - 2.0f) < 0.01f,
               "(11) ...at +2.0 dB of drive, where the INPUT reaches -3.0 dBTP on a track peaking -5.0 at unity: "
               "-3 - (-5) = +2, the 21p item 3 arithmetic untouched, and the figure Sean's 18:25:58 log printed "
               "verbatim (\"drive limited by headroom at +2.0 dB\")",
               f1 (onOther.preDb) + " dB, input now " + f1 (-5.0f + onOther.preDb) + " dBTP");
        // ...and from Sean's own opening, 0.0, the same ceiling and the same number.
        const auto seanOther = runTo (false, 0.0f);
        check (seanOther.headroomStopped && std::abs (seanOther.preDb - 2.0f) < 0.01f,
               "(11) ...and the ceiling is where the INPUT is, not where the drive started: opening at 0.0 stops at "
               "+2.0 too",
               f1 (seanOther.preDb) + " dB");
        const auto seanComp = runTo (true, 0.0f);
        check (std::abs (seanComp.preDb - 6.0f) < 0.01f && ! seanComp.headroomStopped,
               "(11) ...while the SAME opening on a compressor slot spends its whole 6-step budget instead, and "
               "stops on the budget rather than on a ceiling",
               f1 (seanComp.preDb) + " dB");
    }

    // ---- (12) A BUILD SEEKS THE BAND (letter (m), 30 Sep 2026 ruling) ------------------------------------
    {
        std::printf ("\n-- (12) a passive build with gr=0.0 moves IN, and lands when GR enters the band --\n");
        // Sean's 18:22:56-18:23:02: window 1 gr=-- waiting, window 2 gr=-- waiting, window 3 gr=0.0 pre=+0.0
        // post=+0.0 band 2.0-3.0 state=landed-level-already-held, then "Built. Level already matched, nothing to
        // hold." A reading of 0.0 against a 2-3 dB band is a compressor doing nothing, and the loop landed on it:
        // (e)'s one-shot opened the settle ALREADY SPENT, so the step condition failed on its budget term whatever
        // the reading said. Ruled: the build moves IN window by window until GR is in the band or the slot range is
        // exhausted, capped at 12 windows, and only then does the hold set OUT once.
        //
        // A COMPRESSOR MODELLED HONESTLY: gr rises with the drive (1 dB of GR for every 2 dB of drive over a
        // threshold the drive is pushing into), so the band is reachable and the seek has somewhere to go.
        auto seek = [] (float grPerDb, float* grOut, int* windowsOut) -> echojay::CalibLoop
        {
            echojay::CalibLoop::Config cfg;
            cfg.plugin = "NEOLD V76U73"; cfg.slot = 0; cfg.lo = 2.0f; cfg.hi = 3.0f;
            cfg.mode = echojay::CalibLoop::Mode::Passive;
            cfg.actuator = echojay::CalibLoop::Actuator::Drive;
            cfg.purpose = echojay::CalibLoop::Purpose::buildHold;
            // (q) 30 Sep 2026: the SEEK is the non-compressor case now. A compressor build takes no drive at all
            // and is proved by (16) below.
            cfg.startDb = 0.0f; cfg.working = true; cfg.dynamicsSlot = false;
            echojay::CalibLoop l; beginDriven (l, cfg);
            echojay::CalibLoop::Window w;
            w.measured = true; w.silent = false; w.inTruePeakDb = -20.0f;
            float heard = 0.0f; int windows = 0;
            for (int k = 0; k < 40; ++k)
            {
                w.grDb = juce::jmax (0.0f, l.preDb * grPerDb);
                w.levelChangeDb = 0.0f;
                w.slotOutGainDb = l.slotGainDb; w.slotPreTrimDb = l.preDb;
                w.heardSeconds = (heard += 4.0f);
                ++windows;
                const auto st = l.onWindow (w, 3000.0);
                if (st.ask.isNotEmpty() || ! l.active()) break;
            }
            if (grOut != nullptr) *grOut = w.grDb;
            if (windowsOut != nullptr) *windowsOut = windows;
            return l;
        };
        float grAt = 0.0f; int windows = 0;
        const auto reached = seek (0.5f, &grAt, &windows);   // 0.5 dB of GR per dB of drive -> in band by +4..+6
        check (std::abs (reached.preDb) > 0.05f,
               "(12) the build MOVED the drive instead of landing on gr=0.0  (RED as it stood: the settle opened "
               "already spent and window 3 read state=landed-level-already-held at pre=+0.0)",
               "drive " + f1 (reached.preDb) + " dB after " + juce::String (windows) + " window(s)");
        check (reached.landedInBand,
               "(12) ...and it landed when the gain reduction entered the 2-3 dB band",
               "gr " + f1 (grAt) + " dB, band 2.0-3.0, landedInBand="
                   + (reached.landedInBand ? "y" : "n"));
        check (grAt >= 2.0f - 0.05f && grAt <= 3.0f + 0.05f,
               "(12) ...with the reading actually inside it", f1 (grAt) + " dB");
        check (windows <= echojay::CalibLoop::kBuildMaxWindows + 2,
               "(12) ...inside the 12-window cap", juce::String (windows) + " window(s)");
        check (reached.askOwed.startsWith ("Built."),
               "(12) ...and it closes with one \"Built.\" line", reached.askOwed);
        check (reached.askOwed.contains ("Drive") && reached.askOwed.contains ("gain reduction")
                   && reached.askOwed.contains ("Output"),
               "(12) ...carrying the three numbers: the IN it moved, the GR it read, the OUT it set",
               reached.askOwed);
        check (! reached.active(),
               "(12) ...and the loop has still ENDED at its close, as (g) ruled",
               reached.active() ? juce::String ("still active") : juce::String ("ended"));

        // A COMPRESSOR THAT CANNOT REACH THE BAND: the drive runs out of slot range and the line SAYS SO, with
        // the numbers, in the same sentence.
        float grFlat = 0.0f; int windowsFlat = 0;
        const auto notReached = seek (0.0f, &grFlat, &windowsFlat);   // no GR however hard it is driven
        check (! notReached.landedInBand,
               "(12) a compressor that cannot reach the band does not claim to have",
               "gr " + f1 (grFlat) + " dB");
        check (std::abs (notReached.preDb - echojay::CalibLoop::kDriveLimit) < 0.05f
                   || windowsFlat >= echojay::CalibLoop::kBuildMaxWindows,
               "(12) ...it spends the slot range or the window cap first",
               "drive " + f1 (notReached.preDb) + " dB, " + juce::String (windowsFlat) + " window(s)");
        check (notReached.askOwed.contains ("band was not reached"),
               "(12) ...and says so in the same line  (RED as it stood: \"Level already matched, nothing to hold.\", "
               "which says nothing about the compressor at all)",
               notReached.askOwed);
        check (notReached.askOwed.contains ("Drive") && notReached.askOwed.contains ("0.0 dB of gain reduction"),
               "(12) ...with the numbers beside it", notReached.askOwed);
    }

    // ---- (12b) THE min_db NOTE ONLY WHERE IT GOVERNS SOMETHING -------------------------------------------
    {
        std::printf ("\n-- (12b) min_db null on a DRIVE block is not a contract violation --\n");
        // It was firing on every block: the server sends every block as drive with param null and min_db null, and
        // min_db bounds the THRESHOLD actuator's control - which the loop no longer writes at all.
        auto blk = [] (const char* actuator) {
            auto* b = new juce::DynamicObject();
            b->setProperty ("mode", "passive"); b->setProperty ("actuator", actuator);
            b->setProperty ("slot", 1); b->setProperty ("min_db", juce::var());
            if (juce::String (actuator) == "threshold")
            { b->setProperty ("param", "Threshold"); b->setProperty ("sense", "lower_is_harder"); }
            return juce::var (b);
        };
        echojay::CalibLoop::Config c1; juce::String why1;
        echojay::CalibLoop::configFromBlock (blk ("drive"), 2, false, "NEOLD V76U73", c1, why1);
        check (! why1.contains ("min_db null"),
               "(12b) a DRIVE block with min_db null is NOT reported as a contract violation  (RED as it stood: the "
               "line fired on every block)",
               why1.isEmpty() ? juce::String ("(nothing said)") : why1);
        echojay::CalibLoop::Config c2; juce::String why2;
        echojay::CalibLoop::configFromBlock (blk ("threshold"), 2, false, "NEOLD V76U73", c2, why2);
        check (why2.contains ("min_db null"),
               "(12b) ...while a THRESHOLD block still is, because there the range governs the write", why2);
    }

    // ---- (14d) 5 OCT 2026: THE PRODUCTION PATH, WITH THE HOST'S OWN CLOCK AND SEAN'S TIMING BUDGET ---------
    {
        std::printf ("\n-- (14d) a real begin through calibStartMany must judge, hold and write inside the budget --\n");
        // THIS IS THE LEG WHOSE ABSENCE LET 05a SHIP A STALLED LOOP. Every other leg here drives CalibLoop as an
        // object it holds in memory, and most now opt out of the fresh-window wait through beginDriven - so none of
        // them exercised the thing that broke: the host RELOADS the loop from its stored var on every tick, so any
        // state the var does not carry is re-initialised every window. The heard-clock anchor did not travel (by
        // design - the handover trap), so it was re-taken on every window and the wait never ended. Sean's
        // 16:19-16:23 session: windows 2-14 all state=awaiting-fresh-window, slotHeard 5.9 -> 41.2 s, no level
        // hold, no make-up. The gate was green throughout.
        //
        // So this leg goes through calibStartMany and calibTick - the real road, the real tallies, the real clock -
        // and asserts Sean's budget: a judged window within ONE window of audio after the write, and the make-up
        // written inside about 6 s of playing.
        Rig r (ChannelType::LeadVocal, 6.0f);
        feed (r.proc, r.prog, 6.0);
        const juce::String uid;
        auto c = passiveDriveCfg ("Compressor 1");
        c.slot = 0; c.purpose = echojay::CalibLoop::Purpose::buildHold;
        c.noFreshWait = false;          // THE PRODUCTION DEFAULT, stated: this leg must not opt out of anything
        std::vector<echojay::CalibLoop::Config> cfgs { c };
        check (r.proc.calibStartMany (uid, cfgs) == 1, "(14d) precondition: a real hold started");
        int windowsToJudge = -1, windowsToMakeUp = -1;
        for (int k = 0; k < 8; ++k)
        {
            feed (r.proc, r.prog, 3.0); r.virtualMs += 3050.0; r.proc.calibTick (uid);
            const auto st = r.proc.calibLoad (uid);
            if (windowsToJudge < 0 && st.window > 0 && ! st.awaitFresh) windowsToJudge = k + 1;
            if (windowsToMakeUp < 0 && std::abs (r.h.getSlotOutGainDb (0)) > 0.05f) windowsToMakeUp = k + 1;
            if (windowsToMakeUp >= 0) break;
        }
        // ONE window held at most: the first window may straddle the write, the second must be judged.
        check (windowsToJudge >= 0 && windowsToJudge <= 2,
               "(14d) a window is JUDGED within one window of audio after the write  (RED as 05a shipped: never - "
               "windows 2-14 were all awaiting-fresh-window and nothing was ever judged)",
               windowsToJudge < 0 ? juce::String ("never judged in 8 windows (24 s of audio)")
                                  : juce::String (windowsToJudge) + " window(s)");
        // 6 s of playing = two 3 s windows after the one that may be held, so three windows is the bar.
        check (windowsToMakeUp >= 0 && windowsToMakeUp <= 3,
               "(14d) ...and the level hold writes the make-up inside about 6 s of playing",
               windowsToMakeUp < 0 ? juce::String ("never written in 8 windows (24 s of audio)")
                                   : juce::String (windowsToMakeUp) + " window(s), OUT " + f1 (r.h.getSlotOutGainDb (0)));
    }

    // ---- (14c) 5 OCT 2026, SEAN'S SEQUENCE: HARDER -> A FRESH WINDOW -> THE LEVEL HOLD WRITES OUT --------
    {
        std::printf ("\n-- (14c) a hold given a whole fresh window and then a judged one writes its slot's OUT --\n");
        // Ruled by Sean after (14) went red on the item-3 change: "(14) is not a fixture question until proven".
        // (14) asserts the OUT write inside a fixture that also cancels a primary mid-flight, so it cannot tell a
        // window-alignment problem from a lost write. This leg asks the question on its own: ONE hold, a slot that
        // is genuinely 6 dB out, and enough audio after begin() for the fresh window the item-3 gate now requires
        // AND a judged window after it. If the OUT write happens here, the behaviour is intact and (14)'s budget is
        // what moved; if it does not, the item-3 gate has cost the level hold its write and that is a regression.
        Rig r (ChannelType::LeadVocal, 6.0f);
        feed (r.proc, r.prog, 6.0);
        const juce::String uid;
        auto c = passiveDriveCfg ("Compressor 1");
        c.slot = 0; c.purpose = echojay::CalibLoop::Purpose::buildHold;
        std::vector<echojay::CalibLoop::Config> cfgs { c };
        check (r.proc.calibStartMany (uid, cfgs) == 1, "(14c) precondition: one hold started",
               juce::String (r.h.getNumSlots()) + " slot(s)");
        int ticks = 0;
        for (int k = 0; k < 10; ++k)
        {
            feed (r.proc, r.prog, 3.0); r.virtualMs += 3050.0; r.proc.calibTick (uid); ++ticks;
            if (std::abs (r.h.getSlotOutGainDb (0)) > 0.05f) break;
        }
        check (std::abs (r.h.getSlotOutGainDb (0)) > 0.05f,
               "(14c) a whole fresh window, then a judged one, and the level hold writes the slot's OUT",
               "OUT " + f1 (r.h.getSlotOutGainDb (0)) + " after " + juce::String (ticks) + " window(s)");
    }

    // ---- (14) NO HOLD ENDS IN SILENCE (letter (o), 30 Sep 2026 ruling) -----------------------------------
    {
        std::printf ("\n-- (14) a companion hold outlives its primary, and every hold says how it ended --\n");
        // Sean's 19:02 session: "The Mike-E companion hold logged its start and then nothing: no window, no OUT
        // write, no end line." The cause was structural - calibTick returned at `if (! loop.active()) return {}`,
        // and the PRIMARY goes inactive at its own close, so from that tick on no companion was ever advanced
        // again. Ruled: every hold ends with exactly one line saying how it ended; if a primary cancels, its
        // companion carries on or logs its own cancel; and the watchdog counts only loops still alive.
        Rig r (ChannelType::LeadVocal, 6.0f);
        // TWO DIFFERENT plugins: a reorder of two identical ones leaves both identities unchanged, so under (n)
        // nothing cancels - correctly. The first cut of this leg used two EchoJay Gains and asserted a cancel that
        // must not happen.
        const auto* lim2 = BuiltinDeviceRegistry::instance().findByName ("EchoJay Limiter");
        if (lim2 != nullptr) EchoJayBorrowHostTestAccess::loadBuiltin (r.h, BuiltinDeviceRegistry::descriptionFor (*lim2));
        pumpMs (200);
        check (r.h.getNumSlots() == 2 && r.h.slotIdentityKey (0) != r.h.slotIdentityKey (1),
               "(14) precondition: two slots, different plugins",
               juce::String (r.h.getNumSlots()) + " slot(s)");
        feed (r.proc, r.prog, 6.0);
        const juce::String uid;
        std::vector<echojay::CalibLoop::Config> cfgs;
        for (int sl = 0; sl < 2; ++sl)
        { auto c = passiveDriveCfg (sl == 0 ? "Compressor 1" : "Compressor 2");
          c.slot = sl; c.purpose = echojay::CalibLoop::Purpose::buildHold; cfgs.push_back (c); }
        check (r.proc.calibStartMany (uid, cfgs) == 2, "(14) precondition: two holds started");
        // 5 Oct 2026: ONE MORE WINDOW BEFORE THE CANCEL. Item 3 made a loop open awaiting a fresh window, because
        // begin() resets both of the slot's legs and there is no valid measurement until a whole window has been
        // heard since. This fixture budgeted exactly enough audio for the old behaviour, so every hold here lost its
        // landing and the leg went red on the OUT write. PROVEN a budget problem and not a lost write by (14c),
        // which asks the same question on one slot with enough audio and gets OUT -1.00 in four windows. The extra
        // window is given HERE, before the reorder, so the cancel this leg is about still happens mid-flight.
        feed (r.proc, r.prog, 3.0); r.virtualMs += 3050.0; r.proc.calibTick (uid);
        // THE PRIMARY IS CANCELLED under it - its slot changes identity - while the companion is still working.
        for (int k = 0; k < 2; ++k) { feed (r.proc, r.prog, 3.0); r.virtualMs += 3050.0; r.proc.calibTick (uid); }
        r.h.moveSlot (0, +1);
        pumpMs (200);
        feed (r.proc, r.prog, 3.0); r.virtualMs += 3050.0; r.proc.calibTick (uid);
        check (! r.proc.calibLoad (uid).running(),
               "(14) the primary is cancelled by the reorder, as (n) requires",
               r.proc.calibLoad (uid).running() ? juce::String ("still running") : juce::String ("cancelled"));
        // ...and the COMPANION carries on: it is still advanced, window by window, and it finishes.
        int companionWindows = 0;
        for (int k = 0; k < 30; ++k)
        {
            feed (r.proc, r.prog, 3.0); r.virtualMs += 3050.0; r.proc.calibTick (uid);
            ++companionWindows;
            if (r.proc.calibExtraFor (uid).empty()) break;
            bool anyAlive = false;
            for (const auto& c : r.proc.calibExtraFor (uid)) if (c.active()) anyAlive = true;
            if (! anyAlive) break;
        }
        bool stillAlive = false;
        for (const auto& c : r.proc.calibExtraFor (uid)) if (c.active()) stillAlive = true;
        check (! stillAlive,
               "(14) the companion CARRIES ON after its primary is cancelled and finishes  (RED as it stood: "
               "calibTick returned early on an inactive primary, so the companion was never advanced again - no "
               "window, no OUT write, no end line)",
               stillAlive ? juce::String ("still running after " + juce::String (companionWindows) + " window(s)")
                          : juce::String ("finished in " + juce::String (companionWindows) + " window(s)"));
        bool replaces = false;
        const auto said = r.proc.calibTakeAsk (uid, &replaces);
        check (said.isNotEmpty() && said.startsWith ("Built."),
               "(14) ...and the build still posts ONE closing line even though the primary died",
               said.isEmpty() ? juce::String ("(nothing said)") : said);
        // 5 Oct 2026: THIS ASSERTION WAS ENCODING THE BUG. It demanded an OUT write unconditionally, and it passed
        // only because the loop was judging a window from BEFORE the build reset the tallies - the stale reading
        // item 3 removes. With the reading honest, this companion's level needs no match at all, and the leg's OWN
        // closing line (asserted just below, and green) says so: "Compressor 2 set as dialled, level already
        // matched." A hold that writes OUT on a slot that is already matched would be wrong.
        // What must hold is that the hold ACCOUNTED for the level - it either moved OUT or said it did not need to -
        // and never silently skipped it. The unconditional write is proved where it belongs, by (14c), on a slot
        // that genuinely is out: OUT -1.00 in four windows.
        {
            const bool wroteOut = std::abs (r.h.getSlotOutGainDb (0)) > 0.05f
                               || std::abs (r.h.getSlotOutGainDb (1)) > 0.05f;
            const bool saidMatched = said.contains ("level already matched") || said.contains ("level matched");
            check (wroteOut || saidMatched,
                   "(14) ...and it ACCOUNTED for the level - it wrote its own slot's OUT, or it said the level was "
                   "already matched  (it may no longer do the former unconditionally: that write used to come off a "
                   "pre-reset window, which is the staleness item 3 closed)",
                   "OUT1 " + f1 (r.h.getSlotOutGainDb (0)) + " OUT2 " + f1 (r.h.getSlotOutGainDb (1))
                   + (saidMatched ? " / said matched" : " / said nothing about the level"));
        }
        check (said.contains ("cancelled"),
               "(14) ...which says the primary's hold was cancelled rather than quietly omitting it", said);
    }

    // ---- (14b) THE WATCHDOG COUNTS LIVE LOOPS AND NAMES THE DEAD (letter (o)) -----------------------------
    {
        std::printf ("\n-- (14b) \"loops alive N of M\", and the dead ones by name --\n");
        Rig r (ChannelType::LeadVocal, 6.0f);
        const auto* lim = BuiltinDeviceRegistry::instance().findByName ("EchoJay Limiter");
        if (lim != nullptr) EchoJayBorrowHostTestAccess::loadBuiltin (r.h, BuiltinDeviceRegistry::descriptionFor (*lim));
        pumpMs (200);
        feed (r.proc, r.prog, 6.0);
        const juce::String uid;
        std::vector<echojay::CalibLoop::Config> cfgs;
        for (int sl = 0; sl < 2; ++sl)
        { auto c = passiveDriveCfg (sl == 0 ? "Compressor 1" : "Compressor 2");
          c.slot = sl; c.purpose = echojay::CalibLoop::Purpose::buildHold; cfgs.push_back (c); }
        r.proc.calibStartMany (uid, cfgs);
        // One window, so the companion sweep has reported at least once.
        feed (r.proc, r.prog, 3.0); r.virtualMs += 3050.0; r.proc.calibTick (uid);
        feed (r.proc, r.prog, 3.0); r.virtualMs += 3050.0; r.proc.calibTick (uid);
        r.h.setLoopsStarted (2);
        const auto before = r.h.loopsWatchdogLine();
        check (before.contains ("loops alive"),
               "(14b) the headline reports loops ALIVE, not loops started  (RED as it stood: \"loops started 1 of "
               "1\" was true of a loop that had died in silence)",
               before.fromFirstOccurrenceOf ("loops", true, false).substring (0, 60));
        // Kill them both by taking their slots away, then sweep once more.
        while (r.h.getNumSlots() > 0) r.h.removeSlot (0);
        pumpMs (200);
        feed (r.proc, r.prog, 3.0); r.virtualMs += 3050.0; r.proc.calibTick (uid);
        const auto after = r.h.loopsWatchdogLine();
        check (after.contains ("ended:"),
               "(14b) ...and NAMES the dead ones once they have ended",
               after.fromFirstOccurrenceOf ("loops", true, false).substring (0, 110));
    }

    // ---- (15) THE FOUR CORRECTIONS TO (n) AND (o) (30 Sep 2026, second ruling) ---------------------------
    {
        std::printf ("\n-- (15a) an ABSENT fingerprint is not a DIFFERENT one --\n");
        // "slotIdentityKey is fp|uid and fp can be empty. A loop stamped before the fp is known, then compared
        // after it fills in, sees \"|uid\" -> \"fp|uid\" and false-cancels, the same fault through a different door."
        // The comparison is now field by field, and only on fields BOTH sides carry.
        auto key = [] (const char* uid, const char* fp, const char* name)
        { return "uid=" + juce::String (uid) + ";fp=" + juce::String (fp) + ";name=" + juce::String (name); };
        check (ChainHost::slotIdentityStillMatches (key ("8c09913c", "", "NEOLD V76U73"),
                                                   key ("8c09913c", "75e4aa", "NEOLD V76U73")),
               "(15a) a fingerprint FILLING IN does not cancel the loop  (RED as it stood: \"|uid\" != \"fp|uid\" by "
               "string compare, so the loop cancelled itself the moment the map arrived)");
        check (ChainHost::slotIdentityStillMatches (key ("8c09913c", "75e4aa", "NEOLD V76U73"),
                                                   key ("8c09913c", "", "NEOLD V76U73")),
               "(15a) ...nor one going away again");
        check (! ChainHost::slotIdentityStillMatches (key ("8c09913c", "75e4aa", "NEOLD V76U73"),
                                                     key ("934518", "19c8bb", "Looptrotter SA2RATE2")),
               "(15a) ...while a DIFFERENT plugin still cancels: the uid decides whenever both carry one");
        check (! ChainHost::slotIdentityStillMatches (key ("8c09913c", "75e4aa", "X"),
                                                     key ("8c09913c", "OTHERFP", "X")),
               "(15a) ...and the same uid with two DIFFERENT non-empty fingerprints is a different binary");
        check (! ChainHost::slotIdentityStillMatches (key ("", "", "EchoJay Gain"),
                                                     key ("", "", "EchoJay Limiter")),
               "(15a) ...and two built-ins, which carry neither uid nor fp, are told apart by name");
        check (ChainHost::slotIdentityStillMatches ({}, key ("8c09913c", "75e4aa", "X"))
                   && ChainHost::slotIdentityStillMatches (key ("8c09913c", "75e4aa", "X"), {}),
               "(15a) ...and nothing comparable is never a cancel - a cancel must not be a guess");
    }
    {
        std::printf ("\n-- (15b) a pending SETTLE JOB holds the build-settled gate --\n");
        // "dialStateSettled waits for in-flight map fetches but not for pending settle jobs (settleJobs_), and the
        // brief asked for both. The 19:03:33 settle that landed after the gate was a deferred settle, and the
        // settle job itself bumps the rev."
        //
        // MEASURED while writing this leg: a settle job is queued ONLY when a write mismatched on the IMMEDIATE
        // read - a hosted plugin whose display is one write behind (the WaveShell case kSettleBoundMs exists for).
        // A built-in's write is exact and immediate, so this rig's own slots can never queue one. The leg therefore
        // drives a REAL plugin (Apple's AUDelay) and, if no job ever appears, says so instead of passing quietly.
        Rig r (ChannelType::LeadVocal, 6.0f);
        check (r.h.dialStateSettled() && r.h.pendingSettleJobs() == 0,
               "(15b) precondition: a clean rack is settled with no settle job queued",
               juce::String (r.h.pendingSettleJobs()) + " job(s)");
        juce::PluginDescription dly;
        dly.name = "AUDelay"; dly.pluginFormatName = "AudioUnit";
        dly.fileOrIdentifier = "AudioUnit:Effects/aufx,dely,appl";
        dly.uniqueId = dly.deprecatedUid = (int) (juce::int64) juce::String ("64607a6d").getHexValue64();
        r.h.onNeedParamMaps = [] (const juce::StringArray&) {};
        r.h.loadPluginAsync (dly, ChainHost::LoadOrigin::User, [] (const juce::String&) {});
        for (int k = 0; k < 40 && r.h.getNumSlots() < 2; ++k) pumpMs (100);
        int sawJobs = 0; bool gateHeldWhilePending = true;
        for (int attempt = 0; attempt < 6 && sawJobs == 0; ++attempt)
        {
            auto* pp = new juce::DynamicObject();
            pp->setProperty ("delay time", 0.10 + 0.05 * attempt);
            pp->setProperty ("dry/wet mix", 40.0 + attempt);
            auto* wv = new juce::DynamicObject(); wv->setProperty ("params", juce::var (pp));
            r.h.setSlotStructuredSettings (1, juce::var (wv));
            for (int k = 0; k < 10; ++k)
            {
                if (r.h.pendingSettleJobs() > 0)
                {
                    ++sawJobs;
                    if (r.h.dialStateSettled()) gateHeldWhilePending = false;   // the fault this leg is for
                    break;
                }
                pumpMs (10);
            }
            for (int k = 0; k < 40 && r.h.pendingSettleJobs() > 0; ++k) pumpMs (50);
        }
        if (sawJobs > 0)
        {
            check (gateHeldWhilePending,
                   "(15b) a slot with a PENDING SETTLE holds \"dial settled\" until it lands  (RED as it stood: the "
                   "gate read per-slot dial status and in-flight fetches only, so a deferred settle landed after it "
                   "and bumped the rack revision under a loop that had just started)",
                   gateHeldWhilePending ? juce::String ("held") : juce::String ("OPENED while a settle was pending"));
            check (r.h.pendingSettleJobs() == 0 && r.h.dialStateSettled(),
                   "(15b) ...and opens once the settle has landed, so the gate is a wait and not a block",
                   juce::String (r.h.pendingSettleJobs()) + " job(s) left");
        }
        else
        {
            // NOT a silent skip: the term is asserted where it CAN be, and the gap is printed.
            check (! r.h.dialStateSettled() || r.h.pendingSettleJobs() == 0,
                   "(15b) the gate and the settle-job count agree: it is never settled while a job is queued "
                   "(no deferred settle could be provoked on this machine - AUDelay's writes read back immediately, "
                   "so the WaveShell case this term exists for is not reproducible here; the term itself is pinned "
                   "by the arithmetic above)",
                   juce::String (r.h.pendingSettleJobs()) + " job(s), settled="
                       + (r.h.dialStateSettled() ? "y" : "n"));
        }
    }
    {
        std::printf ("\n-- (15c) the dead list keeps its names, and a normal close is not a cancel --\n");
        // "The dead list is local to each sweep and setLoopsAlive overwrites it every window, so a hold that ended
        // earlier loses its name the next window." And: "check that a primary that closed normally is not labelled
        // 'the primary (cancelled)'."
        Rig r (ChannelType::LeadVocal, 6.0f);
        const auto* lim = BuiltinDeviceRegistry::instance().findByName ("EchoJay Limiter");
        if (lim != nullptr) EchoJayBorrowHostTestAccess::loadBuiltin (r.h, BuiltinDeviceRegistry::descriptionFor (*lim));
        pumpMs (200);
        feed (r.proc, r.prog, 6.0);
        const juce::String uid;
        std::vector<echojay::CalibLoop::Config> cfgs;
        for (int sl = 0; sl < 2; ++sl)
        { auto c = passiveDriveCfg (sl == 0 ? "Compressor 1" : "Compressor 2");
          c.slot = sl; c.purpose = echojay::CalibLoop::Purpose::buildHold; cfgs.push_back (c); }
        r.proc.calibStartMany (uid, cfgs);
        // Run to the end of both holds, then keep ticking well past it.
        for (int k = 0; k < 40; ++k) { feed (r.proc, r.prog, 3.0); r.virtualMs += 3050.0; r.proc.calibTick (uid); }
        const auto lineJustAfter = r.h.loopsWatchdogLine();
        for (int k = 0; k < 6; ++k) { feed (r.proc, r.prog, 3.0); r.virtualMs += 3050.0; r.proc.calibTick (uid); }
        const auto lineMuchLater = r.h.loopsWatchdogLine();
        check (lineJustAfter.contains ("ended:"),
               "(15c) the headline names what ended", lineJustAfter.fromFirstOccurrenceOf ("loops", true, false));
        check (lineMuchLater.contains ("ended:"),
               "(15c) ...and STILL names it six windows later  (RED as it stood: the dead list was local to each "
               "sweep, so setLoopsAlive overwrote it with an empty one on the next window)",
               lineMuchLater.fromFirstOccurrenceOf ("loops", true, false));
        check (! lineMuchLater.contains ("cancelled"),
               "(15c) ...and a hold that CLOSED normally is not called cancelled",
               lineMuchLater.fromFirstOccurrenceOf ("ended:", true, false));
    }

    // ---- (16) A COMPRESSOR BUILD IS SET AS DIALLED (letter (q), 30 Sep 2026 ruling) -----------------------
    {
        std::printf ("\n-- (16) on a build, a compressor slot gets NO drive seek --\n");
        // Ruled while compressor calibration is redesigned around measured profiles: "on a build, a compressor slot
        // gets NO drive seek. IN stays 0, the hold matches level on OUT once, and the closing line says 'set as
        // dialled, level matched' per compressor. Keep the loop code for non-compressor slots."
        // THE SLOT HAS TO READ AS DYNAMICS for the rule to apply, and an EchoJay Gain does not - the first cut of
        // this leg used one, so `dynamicsSlot` was false and the build seeked exactly as (m) says a non-compressor
        // should. The limiter's category carries "limit", which is one of the words slotIsDynamics reads.
        Rig r (ChannelType::LeadVocal, 6.0f);
        const auto* lim = BuiltinDeviceRegistry::instance().findByName ("EchoJay Limiter");
        check (lim != nullptr, "(16) precondition: a dynamics built-in is registered");
        if (lim != nullptr) EchoJayBorrowHostTestAccess::loadBuiltin (r.h, BuiltinDeviceRegistry::descriptionFor (*lim));
        pumpMs (200);
        check (r.h.slotIsDynamics (1), "(16) precondition: slot 2 reads as dynamics");
        feed (r.proc, r.prog, 6.0);
        const juce::String uid;
        auto cfg = passiveDriveCfg ("EchoJay Limiter");
        cfg.slot = 1;
        cfg.purpose = echojay::CalibLoop::Purpose::buildHold;
        cfg.working = true;
        cfg.startDb = 0.0f;
        const int started = r.proc.calibStartMany (uid, { cfg });
        check (started == 1, "(16) precondition: the hold started", juce::String (started));
        check (r.proc.calibLoad (uid).dynamicsSlot,
               "(16) precondition: the slot reads as dynamics, so the no-seek rule applies to it");
        juce::String said; bool replaces = false; int windows = 0;
        for (int k = 0; k < 20 && said.isEmpty(); ++k)
        {
            feed (r.proc, r.prog, 3.0); r.virtualMs += 3050.0; r.proc.calibTick (uid); ++windows;
            said = r.proc.calibTakeAsk (uid, &replaces);
        }
        const auto done = r.proc.calibLoad (uid);
        check (std::abs (r.h.getSlotPreTrimDb (1)) < 0.05f,
               "(16) IN STAYS 0 - no drive is written at all  (RED as it stood after (m): the build seeked the band "
               "and drove IN up to the slot range)",
               f1 (r.h.getSlotPreTrimDb (1)) + " dB on IN");
        check (done.steps == 0 && done.settleSteps == 0,
               "(16) ...and it took no rung",
               juce::String (done.steps) + " step(s), settle " + juce::String (done.settleSteps));
        // The hold writes AT MOST ONCE and the figure is whatever this slot's own residual is - the limiter at its
        // defaults may owe nothing, and "level already matched" is then the honest line.
        check (done.holdWrites <= 1,
               "(16) ...while the HOLD runs at most once on OUT",
               juce::String (done.holdWrites) + " write(s), OUT " + f1 (r.h.getSlotOutGainDb (1)) + " dB");
        check (said.containsIgnoreCase ("set as dialled")
                   && (said.contains ("level matched") || said.contains ("level already matched")),
               "(16) ...and the closing line says \"set as dialled\" with what it did about the level",
               said.isEmpty() ? juce::String ("(nothing said)") : said);
        check (! said.contains ("Drive ") && ! said.contains ("band was not reached"),
               "(16) ...with no drive figure and no band claim, because it was not driven", said);
        check (! done.active(),
               "(16) ...and the loop ends at its close",
               done.active() ? juce::String ("still active") : juce::String ("ended"));
        // TWO COMPRESSORS: the phrase is per compressor.
        Rig r2 (ChannelType::LeadVocal, 6.0f);
        if (lim != nullptr)
        {   // two dynamics slots, so the no-seek rule applies to both
            EchoJayBorrowHostTestAccess::loadBuiltin (r2.h, BuiltinDeviceRegistry::descriptionFor (*lim));
            EchoJayBorrowHostTestAccess::loadBuiltin (r2.h, BuiltinDeviceRegistry::descriptionFor (*lim));
        }
        pumpMs (300);
        // ...and a dynamics slot that OWES something, so the hold's write is proved and not merely permitted:
        // the limiter's input_db makes it genuinely louder out than in.
        {
            auto* pp = new juce::DynamicObject(); pp->setProperty ("input_db", 6.0);
            auto* wv = new juce::DynamicObject(); wv->setProperty ("params", juce::var (pp));
            r2.h.setSlotStructuredSettings (1, juce::var (wv)); pumpMs (300);
        }
        check (r2.h.getNumSlots() == 3 && r2.h.slotIsDynamics (1) && r2.h.slotIsDynamics (2),
               "(16) precondition: two dynamics slots for the per-compressor line",
               juce::String (r2.h.getNumSlots()) + " slot(s)");
        feed (r2.proc, r2.prog, 6.0);
        std::vector<echojay::CalibLoop::Config> two;
        for (int sl = 1; sl <= 2; ++sl)
        { auto c = passiveDriveCfg (sl == 1 ? "Comp A" : "Comp B");
          c.slot = sl; c.purpose = echojay::CalibLoop::Purpose::buildHold; c.startDb = 0.0f; two.push_back (c); }
        r2.proc.calibStartMany (uid, two);
        juce::String said2;
        for (int k = 0; k < 20 && said2.isEmpty(); ++k)
        { feed (r2.proc, r2.prog, 3.0); r2.virtualMs += 3050.0; r2.proc.calibTick (uid);
          said2 = r2.proc.calibTakeAsk (uid, &replaces); }
        // 6 Oct 2026 (Sean's ruling): THE PLUGINS ARE NAMED, not numbered. This asserted the literal "Compressor 1"
        // / "Compressor 2", which is the wording the ruling replaced - and the fixture names them "Comp A" and
        // "Comp B", so asserting the ordinals hid the real defect: the companion's record was WIPED when its hold
        // ended, so the line could only ever say "Compressor 2" for it.
        check (said2.contains ("Comp A set as dialled") && said2.contains ("Comp B set as dialled"),
               "(16) the phrase is PER COMPRESSOR and names each plugin  (RED as it stood: the companion's record "
               "was reset by endedWith, so its name and its written figure were gone by the time the line was built)",
               said2);
        check (std::abs (r2.h.getSlotPreTrimDb (1)) < 0.05f && std::abs (r2.h.getSlotPreTrimDb (2)) < 0.05f,
               "(16) ...and neither IN was written",
               "IN1 " + f1 (r2.h.getSlotPreTrimDb (1)) + " IN2 " + f1 (r2.h.getSlotPreTrimDb (2)));
        // THE HOLD DOES WRITE when there is something to match - the slot driven +6 dB by its own input gain gets
        // its OUT pulled down, with IN still untouched. Without this the rule would only be shown as "permitted".
        check (r2.h.getSlotOutGainDb (1) < -1.0f,
               "(16) ...while the slot that IS louder out than in has its OUT pulled down by the hold",
               "OUT1 " + f1 (r2.h.getSlotOutGainDb (1)) + " dB with IN at "
                   + f1 (r2.h.getSlotPreTrimDb (1)));
        check (said2.containsIgnoreCase ("level matched"),
               "(16) ...and its line says the level was matched, not that it already was", said2);
    }

    // ---- (17) MAKE-UP ABOVE 6 dB IS OVER-COMPRESSION, AND THE LINE SAYS SO (2 Oct 2026 ruling) -------------
    {
        std::printf ("\n-- (17) the hold made up more than 6 dB: the line names it, not \"matched\" --\n");
        // Sean's 11:35 session, Tube-Tech CL 1B, profiles OFF: the hold wrote OUT +12.0 dB - the ceiling - and the
        // closing line said "Set as dialled, level matched, Output +12.0 dB." The level WAS matched, so the line
        // was not false; it was useless. A compressor needing 12 dB of make-up is pulling 12 dB down, and the
        // sentence told him everything was fine. Ruled: above 6 dB of make-up the line says what it is taking off
        // and to check the threshold.
        auto lineFor = [] (float outDb, bool dynamics) -> juce::String
        {
            echojay::CalibLoop l;
            echojay::CalibLoop::Config c;
            c.plugin = "Tube-Tech CL 1B"; c.slot = 1;
            c.purpose = echojay::CalibLoop::Purpose::buildHold;
            c.dynamicsSlot = dynamics; c.mode = echojay::CalibLoop::Mode::Passive;
            beginDriven (l, c);
            l.slotGainDb = outDb; l.levelTrimmedDb = outDb; l.levelHeld = true;
            return l.completedLine();
        };
        const auto hot = lineFor (12.0f, true);
        check (hot.contains ("too much, check the threshold"),
               "(17) +12 dB of make-up: the line says it is taking too much off and names the cause  (RED as it "
               "stood: \"level matched, Output +12.0 dB\" - true, and no use to anyone)", hot);
        check (hot.contains ("about 12 dB off"),
               "(17) ...and quotes the figure the compressor is actually taking off", hot);
        check (! hot.containsIgnoreCase ("level matched"),
               "(17) ...and does NOT also claim the level matched, which is what hid it", hot);
        check (hot.contains ("ceiling"),
               "(17) ...and says the trim is at its ceiling, so the real figure may be larger still", hot);
        // THE OTHER DIRECTION, so this cannot fire on an ordinary build: a verifier proven one way is unfalsifiable.
        const auto ok = lineFor (3.0f, true);
        check (ok.containsIgnoreCase ("level matched") && ! ok.contains ("too much"),
               "(17) ...while 3 dB of make-up is ordinary and still reports a plain match", ok);
        // ...and it is a COMPRESSOR rule: a non-dynamics slot with a big trim is not over-compressing, it is staged.
        const auto nd = lineFor (12.0f, false);
        check (! nd.contains ("too much"),
               "(17) ...and a NON-dynamics slot with the same +12 dB is not accused of compressing at all", nd);
    }

    // ---- (18) BLOCKS ARE KEYED TO IDENTITY, NOT INDEX (2 Oct 2026 ruling) ---------------------------------
    {
        std::printf ("\n-- (18) a rack that compacted under a block: remap, never the wrong plugin --\n");
        // Sean's 19:43 rack, exactly: EQ, Vocal De-Esser, CL 1B, Lustrous Plates. The de-esser failed on its
        // licence, the rack compacted to three, and the CL 1B's block - shipped slot 3, index 2 - pointed at
        // Lustrous Plates. He was told "I could not start landing Lustrous Plates" and the CL 1B was never checked.
        const juce::StringArray shipped { "EchoJay EQ", "Vocal De-Esser", "Tube-Tech CL 1B", "Lustrous Plates" };
        const juce::StringArray live    { "EchoJay EQ", "Tube-Tech CL 1B", "Lustrous Plates" };   // compacted
        std::vector<echojay::CalibLoop::Config> cfgs;
        {
            echojay::CalibLoop::Config c; c.plugin = "Tube-Tech CL 1B"; c.slot = 2;   // shipped index
            cfgs.push_back (c);
        }
        const int moved = echojay::CalibLoop::remapBlocksToIdentity (cfgs, shipped, live);
        check (moved == 1 && cfgs.size() == 1 && cfgs[0].slot == 1,
               "(18) the CL 1B's block follows the CL 1B to its live slot  (RED as it stood: it kept index 2, "
               "which after the compaction was Lustrous Plates - a plugin nothing asked to land)",
               "moved " + juce::String (moved) + ", slot now " + juce::String (cfgs[0].slot));
        // ...and the block for the plugin that never arrived is REFUSED, by identity, so it cannot land on a
        // neighbour either. slot < 0 is the caller's signal to drop it silently.
        std::vector<echojay::CalibLoop::Config> gone;
        { echojay::CalibLoop::Config c; c.plugin = "Vocal De-Esser"; c.slot = 1; gone.push_back (c); }
        echojay::CalibLoop::remapBlocksToIdentity (gone, shipped, live);
        check (gone[0].slot < 0,
               "(18) ...while the block for the plugin that did NOT load is refused, not pointed at its neighbour",
               "slot " + juce::String (gone[0].slot));
        // AND THE OTHER DIRECTION: an untouched rack must not be reshuffled by this.
        std::vector<echojay::CalibLoop::Config> same;
        { echojay::CalibLoop::Config c; c.plugin = "Tube-Tech CL 1B"; c.slot = 1; same.push_back (c); }
        check (echojay::CalibLoop::remapBlocksToIdentity (same, live, live) == 0 && same[0].slot == 1,
               "(18) ...and a rack that matches the shipped chain is left exactly as it is",
               "slot " + juce::String (same[0].slot));
    }

    // ---- (19) WITHDRAWN 2 Oct 2026 -------------------------------------------------------------------------
    // This leg asserted that windows with a frozen heard clock move nothing, for a gate I added to EJCalibLoop and
    // have since withdrawn. Both were wrong: Sean's session showed the slot clock advancing three seconds per
    // window throughout (171.9 -> 174.9 -> 177.9 ...), so the loop was hearing and the premise did not hold; and
    // the mechanism - a delta against lastHeardS - had already been abandoned for causing 187 consecutive stale
    // windows after a handover, which case (4) below exists to hold fixed. Removing the leg with the gate rather
    // than leaving a test for behaviour the product deliberately does not have. The real fault in that session was
    // the GR sensor, and case (20) covers it.

    // ---- (20) WITHDRAWN 2 Oct 2026, and OWED elsewhere --------------------------------------------------
    // This asserted that 6 dB of level reduction reads as 6 dB of GR, on a rig with no profile. Two things were
    // wrong with it. It read st.lastGr, which stays NaN until a window is actually JUDGED, and two ticks did not
    // produce one - so it compared against NaN and would have failed whatever the product did. And the rule it
    // asserted is not the rule: GR by LEVEL counts every level change between in and out, including a static one
    // the compressor did not cause, which is what case (9) caught when a fixture slot 6 dB down read as 6 dB of
    // compression and the loop backed the drive to -6.
    //
    // The sensor now chooses: LEVEL minus static_gain_db when a profile supplies that offset, CREST when nothing
    // does, and grVia= in the window line says which ran. Testing the level path needs a rig with a REAL profile
    // attached, which comp_profile_guard has and this one does not - so the coverage is OWED there, not faked here.
    // Sean's CL 1B case is the level path; case (9) is the crest path; both must hold.

    // ---- (21) B's PRODUCTION BLOCK SHAPES (3723e80) ARE SAFE (2 Oct 2026) ----------------------------------
    {
        std::printf ("\n-- (21) actuator \"amount\" holds and checks; it never drives EchoJay's input --\n");
        // B's production server ships actuator "amount" or "threshold" with param, start_db, min/max_db and
        // output_param, and profiled blocks carry actuator "amount", from_profile, set_directly and
        // expected_gr_db. The old parser recognised only threshold/input/drive and its else branch set
        // Actuator::Drive - so a profiled block naming the amount control ran EchoJay's own INPUT PRE-GAIN and
        // ignored the plugin control the server had already set from the profile. It warned into whyOut and then
        // did the wrong thing anyway, which is the worst of both.
        auto blockWith = [] (const char* actuator, bool setDirectly, bool withParam) -> juce::var
        {
            auto* c = new juce::DynamicObject();
            c->setProperty ("slot", 1);
            c->setProperty ("actuator", actuator);
            if (withParam) c->setProperty ("param", "Comp Thresh");
            c->setProperty ("min_db", -60.0); c->setProperty ("max_db", 0.0);
            c->setProperty ("output_param", "Makeup");
            c->setProperty ("expected_gr_db", 2.0);
            c->setProperty ("from_profile", true);
            if (setDirectly) c->setProperty ("set_directly", true);
            auto* chain = new juce::DynamicObject();
            chain->setProperty ("calibration", juce::var (c));
            return juce::var (chain);
        };
        auto parse = [] (const juce::var& blk, echojay::CalibLoop::Config& out) -> bool
        {
            juce::String why;
            return echojay::CalibLoop::configFromBlock (blk.getProperty ("calibration", juce::var()),
                                                        4, false, "Tube-Tech CL 1B", out, why);
        };
        {   // the PROFILED shape: amount + from_profile + set_directly + expected_gr_db
            echojay::CalibLoop::Config c;
            check (parse (blockWith ("amount", true, true), c), "(21) the profiled \"amount\" block parses");
            check (c.actuator != echojay::CalibLoop::Actuator::Drive,
                   "(21) ...and the actuator is NOT the drive  (RED as it stood: the else branch ran EchoJay's own "
                   "input pre-gain in front of a control the server had already set from the profile)",
                   c.actuator == echojay::CalibLoop::Actuator::Threshold ? "Threshold" : "Input/Drive");
            check (c.holdOnly, "(21) ...it HOLDS rather than stepping a control whose direction the block never states");
            check (c.params.contains ("Comp Thresh"),
                   "(21) ...and the plugin control it names is carried, not dropped", c.params.joinIntoString ("|"));
            check (c.expectedGrDb == c.expectedGrDb && std::abs (c.expectedGrDb - 2.0f) < 0.01f,
                   "(21) ...and expected_gr_db survives, so the section 7 check still has its figure",
                   juce::String (c.expectedGrDb, 2));
            check (c.outputParams.contains ("Makeup"),
                   "(21) ...and output_param is carried for the make-up work that is owed",
                   c.outputParams.joinIntoString ("|"));
        }
        {   // "amount" WITHOUT set_directly: still held, never driven
            echojay::CalibLoop::Config c;
            parse (blockWith ("amount", false, true), c);
            check (c.actuator != echojay::CalibLoop::Actuator::Drive && c.holdOnly,
                   "(21) an \"amount\" block with no set_directly is held too - a sense we would have to guess is "
                   "not a sense");
        }
        {   // THE OTHER DIRECTION: a plain drive block with no param still runs the drive, unchanged.
            echojay::CalibLoop::Config c;
            parse (blockWith ("drive", false, false), c);
            check (c.actuator == echojay::CalibLoop::Actuator::Drive && ! c.holdOnly,
                   "(21) ...while a real drive block with no control named still runs the DRIVE, as before");
        }
    }


    // ---- (22) THE AGREEMENT RULE: WITH NO PROFILE, ONE SENSOR'S WORD MOVES NOTHING (3 Oct 2026 ruling) -------
    {
        std::printf ("\n-- (22) no profile: the loop moves only where LEVEL and CREST agree --\n");
        // Sean's ruling after two faults with the same shape. Without a profile there is no static_gain_db, so
        // neither sensor is sound on its own:
        //   the 19:43 CL 1B  crest read ~0 on a slow unit over sustained material (SHORTMAX and SHORT90 fall
        //                    together, so the crest does not shrink) while the level figure read several dB -
        //                    and acting on crest alone walked the drive to +6 dB hunting gain reduction that
        //                    was already there;
        //   case (9)         a fixture slot 6 dB DOWN reads 6 dB of level change and no crest change at all -
        //                    and acting on level alone backed the drive to -6 dB for compression that never
        //                    happened.
        // One condition covers both: move only where the two agree, and when they do not, HOLD and print both.
        //
        // Driven through onWindow rather than the end-to-end rig because the point is the DECISION, and a
        // fixture that fed real audio would also have to fake a disagreement in the taps to produce one.
        auto runWith = [] (float crest, float level, bool levelKnown, bool profile, int windows)
        {
            echojay::CalibLoop l;
            auto cfg = passiveDriveCfg ("Tube-Tech CL 1B");
            cfg.mode = echojay::CalibLoop::Mode::Listen;   // the stepping path: this is where it matters
            cfg.purpose = echojay::CalibLoop::Purpose::askRung;
            beginDriven (l, cfg);
            l.hasProfile = profile;
            l.profileStaticGainDb = profile ? 0.0f : std::numeric_limits<float>::quiet_NaN();
            l.dynamicsSlot = true;                         // a compressor, so no input-headroom ceiling (l)
            echojay::CalibLoop::Window w;
            w.measured = true; w.silent = false;
            // grDb IS the acted-on figure, and with no profile the host sets it FROM the crest (see
            // fillCalibWindow): keeping that true here is what makes the leg a test of the product's own wiring.
            w.grDb = profile ? level : crest;
            w.grCrestDb = crest; w.grLevelDb = level; w.grLevelKnown = levelKnown;
            w.grSensor = profile ? "level-static" : "crest";
            w.levelChangeDb = 0.0f;
            w.inTruePeakDb = -30.0f;                       // nowhere near the ceiling: not what is under test
            w.slotOutGainDb = 0.0f; w.slotPreTrimDb = 0.0f;
            float heard = 30.0f;
            juce::StringArray lines; int driveWrites = 0;
            for (int i = 0; i < windows; ++i)
            {
                w.heardSeconds = (heard += 4.0f);
                w.slotPreTrimDb = l.preDb;
                const auto st = l.onWindow (w, 3000.0);
                if (st.logLine.isNotEmpty()) lines.add (st.logLine);
                if (st.writeDrive) ++driveWrites;
                if (st.finished) break;
            }
            struct R { float preDb; int writes; juce::StringArray lines; };
            return R { l.preDb, driveWrites, lines };
        };
        const float band = 2.5f;   // passiveDriveCfg's band is 2.0-3.0
        juce::ignoreUnused (band);

        {   // DISAGREEMENT, Sean's CL 1B shape: crest under the band, level over it.
            const auto r = runWith (0.0f, 6.0f, true, false, 12);
            check (r.writes == 0 && std::abs (r.preDb) < 0.01f,
                   "(22) crest 0.0 UNDER the band and level 6.0 OVER it: nothing moves  (RED as it stood: the "
                   "crest sensor alone wanted more, and the drive walked to +6.0 dB on Sean's CL 1B)",
                   juce::String (r.writes) + " drive write(s), drive " + f1 (r.preDb) + " dB");
            const auto held = r.lines.strings.size() > 0 ? r.lines[r.lines.size() - 1] : juce::String();
            bool saidBoth = false;
            for (const auto& ln : r.lines)
                if (ln.contains ("disagree") && ln.contains ("level 6.0") && ln.contains ("crest 0.0"))
                    saidBoth = true;
            check (saidBoth,
                   "(22) ...and the line says they disagree and prints BOTH figures, so the next session can see "
                   "which sensor wanted what", held);
        }
        {   // ...AND THE OTHER DISAGREEMENT, case (9)'s shape: level over, crest flat, on a slot that only cuts.
            const auto r = runWith (0.2f, 6.0f, true, false, 12);
            check (r.writes == 0,
                   "(22) a slot 6 dB DOWN with no crest change moves nothing either  (RED as it stood on the "
                   "level sensor: it backed the drive to -6 dB for compression that never happened)",
                   juce::String (r.writes) + " drive write(s), drive " + f1 (r.preDb) + " dB");
        }
        {   // AGREED UNDER: both read below the band, so harder is allowed - this is the direction that must
            // still work, or the rule has simply disabled the loop.
            const auto r = runWith (0.4f, 0.6f, true, false, 24);
            check (r.writes >= 1 && r.preDb > 0.5f,
                   "(22) BOTH under the band: it steps HARDER, exactly as before the rule",
                   juce::String (r.writes) + " drive write(s), drive " + f1 (r.preDb) + " dB");
        }
        {   // AGREED OVER: both read above it, so softer is allowed.
            const auto r = runWith (6.0f, 6.4f, true, false, 24);
            check (r.writes >= 1 && r.preDb < -0.5f,
                   "(22) BOTH over the band: it steps SOFTER",
                   juce::String (r.writes) + " drive write(s), drive " + f1 (r.preDb) + " dB");
        }
        {   // NO LEVEL FIGURE AT ALL (the host could not close a SHORT90 window): the rule cannot apply, and it
            // must not freeze the loop instead. This is the difference between "the sensors disagree" and "there
            // is only one sensor", and conflating them would strand every host that reads no SHORT90.
            const auto r = runWith (0.4f, 0.0f, false, false, 24);
            check (r.writes >= 1,
                   "(22) with NO level figure the crest stands alone and the loop still moves - an unreadable "
                   "sensor is not a disagreement",
                   juce::String (r.writes) + " drive write(s), drive " + f1 (r.preDb) + " dB");
        }
        {   // WITH A PROFILE the rule is off: static_gain_db is what makes the level figure trustworthy, and the
            // profile is the only thing that supplies it. The same disagreement must step.
            const auto r = runWith (0.0f, 6.0f, true, true, 24);
            check (r.writes >= 1,
                   "(22) WITH a profile the same disagreement still steps: the level figure stands on its own "
                   "once static_gain_db is known, and the rule is for unprofiled slots only",
                   juce::String (r.writes) + " drive write(s), drive " + f1 (r.preDb) + " dB");
        }
    }

    // ---- (23) max_steps COMES FROM THE BLOCK, CAPPED BY OURS (3 Oct 2026 ruling) ----------------------------
    {
        std::printf ("\n-- (23) the step budget is the SMALLER of the block's max_steps and our 6 --\n");
        // The parser read max_steps nowhere, so a server that asked for two steps got six. The ruling is the
        // smaller of the two: the block may ask for less, never for more.
        auto stepsUnder = [] (int maxSteps)
        {
            echojay::CalibLoop l;
            auto cfg = passiveDriveCfg ("Tube-Tech CL 1B");
            cfg.mode = echojay::CalibLoop::Mode::Listen;
            cfg.purpose = echojay::CalibLoop::Purpose::askRung;
            cfg.maxStepsFromBlock = maxSteps;
            beginDriven (l, cfg);
            l.dynamicsSlot = true;
            echojay::CalibLoop::Window w;
            w.measured = true; w.silent = false;
            w.grDb = 0.0f; w.grCrestDb = 0.0f;              // never anywhere near the band: every window wants one
            w.grLevelDb = 0.0f; w.grLevelKnown = true;      // ...and the two AGREE, so (22) lets it run
            w.levelChangeDb = 0.0f; w.inTruePeakDb = -30.0f;
            w.slotOutGainDb = 0.0f;
            float heard = 30.0f; int writes = 0;
            for (int i = 0; i < 40; ++i)
            {
                w.heardSeconds = (heard += 4.0f);
                w.slotPreTrimDb = l.preDb;
                const auto st = l.onWindow (w, 3000.0);
                if (st.writeDrive) ++writes;
                if (st.finished) break;
            }
            struct R { int writes; float preDb; };
            return R { writes, l.preDb };
        };
        const auto two   = stepsUnder (2);
        const auto absent = stepsUnder (-1);
        const auto ten   = stepsUnder (10);
        check (std::abs (two.preDb - 2.0f) <= 0.01f,
               "(23) max_steps 2 spends TWO dB and stops  (RED as it stood: the field was parsed nowhere, so the "
               "block's 2 became our 6)", f1 (two.preDb) + " dB over " + juce::String (two.writes) + " write(s)");
        check (std::abs (absent.preDb - 6.0f) <= 0.01f,
               "(23) ...no max_steps at all keeps our own cap of 6", f1 (absent.preDb) + " dB");
        check (std::abs (ten.preDb - 6.0f) <= 0.01f,
               "(23) ...and max_steps 10 is still 6: the block may ask for LESS, never for more",
               f1 (ten.preDb) + " dB");
    }

    // ---- (24) 03a ITEM 3: holdOnly BEATS LISTEN, where the stepping actually is ------------------------------
    {
        std::printf ("\n-- (24) a held block in LISTEN mode writes nothing at all --\n");
        // Sean's 08:03 session: the block logged "actuator amount names a plugin control: HOLDING, not stepping
        // it" and then walked the CL 1B's Threshold from -1 to -6 dB in six steps. Mapping holdOnly onto
        // Purpose::buildHold was not enough, because the automatic stepping is the LISTEN path and ran whatever
        // the purpose said. (21) covers the PARSE; this covers the RUN, which is where the writes came from.
        echojay::CalibLoop l;
        auto cfg = passiveDriveCfg ("Tube-Tech CL 1B");
        cfg.mode = echojay::CalibLoop::Mode::Listen;             // LISTEN, as the 08:03 block was
        cfg.actuator = echojay::CalibLoop::Actuator::Threshold;
        cfg.params.add ("Threshold");
        cfg.startDb = -1.0f; cfg.minDb = -60.0f; cfg.maxDb = 0.0f;
        cfg.holdOnly = true;                                     // ...and held, as configFromBlock marked it
        cfg.setDirectly = true; cfg.fromProfile = true; cfg.expectedGrDb = 2.0f;
        beginDriven (l, cfg);
        l.dynamicsSlot = true;
        echojay::CalibLoop::Window w;
        w.measured = true; w.silent = false;
        w.grDb = 0.0f; w.grCrestDb = 0.0f;                       // far under the band: a stepping loop would move
        w.grLevelDb = 0.0f; w.grLevelKnown = true;
        w.levelChangeDb = 0.0f; w.inTruePeakDb = -30.0f;
        w.slotOutGainDb = 0.0f; w.slotPreTrimDb = 0.0f;
        int paramWrites = 0, driveWrites = 0; float heard = 30.0f;
        juce::String anAsk;
        for (int i = 0; i < 20; ++i)
        {
            w.heardSeconds = (heard += 4.0f);
            const auto st = l.onWindow (w, 3000.0);
            if (st.writeParams) ++paramWrites;
            if (st.writeDrive)  ++driveWrites;
            if (st.ask.isNotEmpty()) anAsk = st.ask;
        }
        check (paramWrites == 0,
               "(24) twenty windows 2.5 dB under the band and the named control is never written  (RED as it "
               "stood: six writes, Threshold -1 -> -6 dB, after the log had said it was holding)",
               juce::String (paramWrites) + " write(s) to Threshold");
        check (driveWrites == 0,
               "(24) ...and EchoJay's own drive is not written either - a held block steps NOTHING",
               juce::String (driveWrites) + " drive write(s)");
        check (std::abs (l.value + 1.0f) <= 0.01f,
               "(24) ...so the control is still where the server set it", f1 (l.value) + " dB");
        check (anAsk.isNotEmpty(),
               "(24) ...and it does what a held block is FOR: it measures and reports", anAsk);
    }


    // ---- (25) 03a ITEM 5: A NAMED CONTROL WITH NO START IS READ, OR HELD -----------------------------------
    {
        std::printf ("\n-- (25) no start_db on a named control: read it, or hold rather than jump --\n");
        // Sean's 08:03 session, item 5: the block carried start_db=(none) for actuator "threshold" and the loop
        // logged "dialling Threshold from nan dB", then its first window read "Threshold=+0.0" - a write to a
        // value it had never read. The drive had had its own substitution since 30 Sep (open from the staging);
        // a NAMED control needs the control's own position, and when that cannot be read the honest answer is to
        // hold, because a step from an unknown position is a jump.
        Rig r (ChannelType::LeadVocal, 0.0f);
        // A SECOND BUILT-IN beside the rig's Gain, so the ground-truth assertion below is about a rack rather
        // than about one device.
        if (const auto* cp = BuiltinDeviceRegistry::instance().findByName ("EchoJay Limiter"))
            EchoJayBorrowHostTestAccess::loadBuiltin (r.h, BuiltinDeviceRegistry::descriptionFor (*cp));
        pumpMs (200);
        feed (r.proc, r.prog, 6.0);
        const juce::String uid;
        // The readable control is found by ASKING every slot, not by naming one: readControlDb matches on the
        // parameter's own name and parses its display text, and hard-coding a built-in's parameter name here
        // would make the leg fail the day that name changes for an unrelated reason.
        // GROUND TRUTH FIRST, because it decides what this leg can honestly assert: ChainHost::readControlDb
        // iterates the slot processor's JUCE parameters, and NO EchoJay built-in publishes one - they are driven
        // entirely by structured settings (not a single addParameter in any Eed*Processor.cpp). So readControlDb
        // can never read a built-in, and the "read the control off the plugin" half of item 5 cannot be exercised
        // on a rig made of built-ins. The first cut of this leg tried, picked a slot by probing, found nothing,
        // and failed its own precondition - twice, once per built-in it tried.
        //
        // What that leaves: the HOLD half is the safety-critical direction and it IS exercisable here, because a
        // built-in is exactly the "cannot be read" case. The read half is OWED in a guard that hosts a real
        // plugin (comp_profile_guard, which has one for the profile join).
        int paramsSeen = 0;
        for (int sl = 0; sl < r.h.getNumSlots(); ++sl)
            if (auto* p0 = r.h.getSlotProcessor (sl))
                paramsSeen += p0->getParameters().size();
        float ignored = 0.0f;
        check (paramsSeen == 0 && ! r.h.readControlDb (0, "level_db", ignored)
               && ! r.h.readControlDb (0, "ceiling_db", ignored),
               "(25) ground truth: a built-in publishes no JUCE parameters, so readControlDb refuses on one - "
               "which is what makes this rig the UNREADABLE case and the read case owed elsewhere",
               juce::String (paramsSeen) + " parameter(s) across " + juce::String (r.h.getNumSlots()) + " slot(s)");

        auto named = [&] (const juce::String& param)
        {
            auto c = passiveDriveCfg ("EchoJay Limiter");
            c.slot = juce::jmax (0, r.h.getNumSlots() - 1);
            c.actuator = echojay::CalibLoop::Actuator::Threshold;
            c.params.add (param);
            c.startDb = std::numeric_limits<float>::quiet_NaN();   // absent and null both arrive as NaN
            c.minDb = -60.0f; c.maxDb = 12.0f;
            c.purpose = echojay::CalibLoop::Purpose::buildHold;
            return c;
        };
        r.proc.calibStore (uid, echojay::CalibLoop{});
        r.proc.calibStartMany (uid, { named ("Threshold") });
        const auto held = r.proc.calibLoad (uid);
        check (held.holdOnly,
               "(25) a named control with NO start_db that cannot be read HOLDS instead of stepping  (RED as it "
               "stood: \"dialling Threshold from nan dB\", and the first window wrote Threshold=+0.0 - a jump "
               "from a position the loop had never read)");
        // THE POSITION STAYS UNKNOWN, and that is the point: the loop holds BECAUSE it does not know where the
        // control is, so claiming a number here would be the invented-0 failure wearing the fix's clothes. What
        // must never happen is a LINE quoting one - Sean's log read "dialling Threshold from nan dB" and then
        // "Threshold=+0.0". So the assertion is on the line, not on the member.
        check (! (held.value == held.value),
               "(25) ...and its position is deliberately still UNKNOWN - a held block holds because nobody read "
               "the control, and a 0 here would be an invented reading");
        {
            echojay::CalibLoop::Window probe;
            probe.measured = true; probe.silent = false; probe.heardSeconds = 34.0f;
            probe.grDb = 0.0f; probe.grCrestDb = 0.0f; probe.grLevelDb = 0.0f; probe.grLevelKnown = true;
            probe.levelChangeDb = 0.0f; probe.inTruePeakDb = -30.0f;
            probe.slotOutGainDb = 0.0f; probe.slotPreTrimDb = 0.0f;
            auto probeLoop = held;
            const auto line = probeLoop.onWindow (probe, 3000.0).logLine;
            check (! line.containsIgnoreCase ("nan"),
                   "(25) ...and NO line prints \"nan\"  (RED as it stood: \"dialling Threshold from nan dB\", and "
                   "the window line read Threshold=+0.0 - a number for a position never read)", line);
            check (line.contains ("unknown"),
                   "(25) ...it says the position is unknown instead, which is the truth and is unmistakable", line);
        }
        {
            echojay::CalibLoop::Window w;
            w.measured = true; w.silent = false;
            w.grDb = 0.0f; w.grCrestDb = 0.0f; w.grLevelDb = 0.0f; w.grLevelKnown = true;
            w.levelChangeDb = 0.0f; w.inTruePeakDb = -30.0f; w.slotOutGainDb = 0.0f; w.slotPreTrimDb = 0.0f;
            auto loop = held; int writes = 0; float heard = 30.0f;
            for (int i = 0; i < 12; ++i)
            { w.heardSeconds = (heard += 4.0f); if (loop.onWindow (w, 3000.0).writeParams) ++writes; }
            check (writes == 0, "(25) ...and twelve windows 2.5 dB under the band later it has written nothing",
                   juce::String (writes) + " write(s)");
        }
        r.proc.calibStore (uid, echojay::CalibLoop{});
    }

    // ---- (26) THE SIDECAR ROUND TRIP KEEPS EVERY FIELD A DECISION IS MADE ON (3 Oct 2026) ------------------
    {
        std::printf ("\n-- (26) toVar/fromVar: a Link rack's loop survives its own tick --\n");
        // Found while writing (24): calibTick is calibLoad -> decide -> calibStore, and for a NON-EMPTY uid both
        // ends go through toVar/fromVar. holdOnly was in neither, so item 3's fix held for exactly one tick on a
        // Link or borrowed rack and the loop then stepped the control again - on the rack Sean's sessions run on.
        // maxStepsBlock had the same hole, and the whole profile group did too, which is why COMP_PROFILE_SPEC_v1's
        // one check could never run on a Link: `hasProfile` was false again by the second tick.
        //
        // The leg sets every such field to a NON-DEFAULT value and compares after the trip, so the next field
        // added to the loop is caught here rather than in a session log three weeks later.
        echojay::CalibLoop l;
        auto cfg = passiveDriveCfg ("Tube-Tech CL 1B");
        cfg.actuator = echojay::CalibLoop::Actuator::Threshold;
        cfg.params.add ("Comp Thresh");
        cfg.startDb = -14.0f;
        cfg.holdOnly = true;
        cfg.maxStepsFromBlock = 2;
        beginDriven (l, cfg);
        l.hasProfile = true; l.profilesFeatureOn = true; l.blockFromProfile = true;
        l.profileChecked = true; l.profileCorrected = true; l.profileNotEngaging = true;
        l.profileCorrectionDb = 1.5f; l.profileObservedDropDb = 2.25f;
        l.profileExpectedGrDb = 2.0f; l.profileExpectedLevelDb = -1.5f;
        l.profileStaticGainDb = -3.5f;
        l.profileAmountControl = "Comp Thresh"; l.profileAmountNormAfter = 0.42f; l.profileCurrentNorm = 0.37f;
        l.levelHoldClamped = true; l.levelHoldLimitDb = -24.0f; l.levelResidualDb = -2.75f;
        l.landedInBand = true;
        // 3 Oct 13:52: lastSensor stopped being log-only (measuredGrDb reads it to refuse a crest figure on a
        // from_profile slot) and blockStaticGainDb is the block's own offset, so both are decision fields now.
        l.lastSensor = "level-static"; l.blockStaticGainDb = -3.5f;
        const auto back = echojay::CalibLoop::fromVar (l.toVar());
        check (back.holdOnly,
               "(26) holdOnly survives the round trip  (RED as it stood: the one field that says \"never write "
               "this control\" was in neither direction, so a held block started stepping on its second tick)");
        check (back.maxStepsBlock == 2,
               "(26) ...and the block's own max_steps, which became our 6 again after one tick",
               juce::String (back.maxStepsBlock));
        check (back.hasProfile && back.profilesFeatureOn && back.blockFromProfile,
               "(26) ...and the three profile flags, without which section 7's check never runs on a Link rack");
        check (back.profileChecked && back.profileCorrected && back.profileNotEngaging,
               "(26) ...and what the check already found, so it is not re-run every second");
        check (std::abs (back.profileStaticGainDb + 3.5f) < 0.01f,
               "(26) ...and static_gain_db, which is what makes the LEVEL sensor trustworthy and decides whether "
               "the agreement rule applies at all", f1 (back.profileStaticGainDb));
        check (std::abs (back.profileCorrectionDb - 1.5f) < 0.01f
               && std::abs (back.profileObservedDropDb - 2.25f) < 0.01f
               && std::abs (back.profileExpectedGrDb - 2.0f) < 0.01f
               && std::abs (back.profileExpectedLevelDb + 1.5f) < 0.01f,
               "(26) ...and the four figures the check's own line quotes");
        check (back.profileAmountControl == "Comp Thresh"
               && std::abs (back.profileAmountNormAfter - 0.42f) < 0.001f
               && std::abs (back.profileCurrentNorm - 0.37f) < 0.001f,
               "(26) ...and the amount control with both its positions", back.profileAmountControl);
        check (back.levelHoldClamped && std::abs (back.levelHoldLimitDb + 24.0f) < 0.01f
               && std::abs (back.levelResidualDb + 2.75f) < 0.01f && back.landedInBand,
               "(26) ...and the four fields the CLOSING SENTENCE is composed from, so a handover cannot change "
               "what Sean is told about what happened");
        // NaN MUST COME BACK AS NaN, not as 0: "nobody stated an expected gain reduction" and "the expected gain
        // reduction is 0 dB" are different claims, and juce::JSON writes a NaN double as null.
        echojay::CalibLoop n;
        beginDriven (n, passiveDriveCfg ("Tube-Tech CL 1B"));
        const auto nback = echojay::CalibLoop::fromVar (n.toVar());
        check (! (nback.profileExpectedGrDb == nback.profileExpectedGrDb)
               && ! (nback.profileObservedDropDb == nback.profileObservedDropDb),
               "(26) ...while an absent figure comes back ABSENT, not as 0 dB",
               juce::String (nback.profileExpectedGrDb, 2));
        // ...AND AN OLD SIDECAR, written before these fields existed, keeps the member defaults rather than
        // reading a void var as 0 - which for maxStepsBlock would mean "no steps at all".
        // THE VAR IS HELD IN A NAMED LOCAL FIRST. getDynamicObject() hands back a raw pointer into the var, and
        // on a temporary that object is released at the end of the statement - this leg segfaulted on its last
        // assertion for exactly that reason, which is the 18 Sep Pro Tools crash in a test harness.
        const auto stored = l.toVar();
        auto* o = stored.getDynamicObject();
        o->removeProperty ("maxStepsBlock"); o->removeProperty ("holdOnly");
        const auto older = echojay::CalibLoop::fromVar (stored);
        check (older.maxStepsBlock == -1 && ! older.holdOnly,
               "(26) ...and a sidecar written by an older build keeps the defaults, not zeroes",
               juce::String (older.maxStepsBlock));
        check (back.lastSensor == "level-static",
               "(26) ...and lastSensor travels now it is a DECISION field - measuredGrDb reads it to refuse a "
               "crest figure on a from_profile slot, and a field like that reset on the second tick is this "
               "morning's defect again", back.lastSensor);
        check (std::abs (back.blockStaticGainDb + 3.5f) < 0.01f,
               "(26) ...and so does the block's own static_gain_db, which is the whole of what the level sensor "
               "needs", f1 (back.blockStaticGainDb));
    }


    // ---- (27) static_gain_db OFF THE BLOCK, AND NO CREST FIGURE ON A from_profile CARD (3 Oct 13:52) --------
    {
        std::printf ("\n-- (27) a from_profile block supplies its own static_gain_db; crest never poses as GR --\n");
        // Sean's 13:52 session: the map had NO comp_profile (B is fixing that), the calibration block carried
        // from_profile, and the loop fell back to the crest sensor - which reads about 0 on a slow unit over
        // sustained material. The card then said "about 0 dB on the loud phrases, from its profile" on a
        // compressor measurably doing 2-3 dB. His grLevel figures were 1.5/3.0/2.5 against the profile's
        // 2.0/3.0/3.0, so the level sensor was right the whole time and only lacked the one offset.
        auto blockWith = [] (bool fromProfile, juce::var staticGain) -> juce::var
        {
            auto* c = new juce::DynamicObject();
            c->setProperty ("slot", 1);
            c->setProperty ("actuator", "amount");
            c->setProperty ("param", "Comp Thresh");
            c->setProperty ("min_db", -60.0); c->setProperty ("max_db", 0.0);
            c->setProperty ("expected_gr_db", 2.0);
            if (fromProfile) c->setProperty ("from_profile", true);
            if (! staticGain.isVoid()) c->setProperty ("static_gain_db", staticGain);
            auto* chain = new juce::DynamicObject();
            chain->setProperty ("calibration", juce::var (c));
            return juce::var (chain);
        };
        auto parse = [] (const juce::var& blk, echojay::CalibLoop::Config& out)
        {
            juce::String why;
            return echojay::CalibLoop::configFromBlock (blk.getProperty ("calibration", juce::var()),
                                                       4, false, "Tube-Tech CL 1B", out, why);
        };
        {   // THE PARSE: static_gain_db is read, and -3.5 survives as -3.5.
            echojay::CalibLoop::Config c;
            check (parse (blockWith (true, juce::var (-3.5)), c), "(27) a block carrying static_gain_db parses");
            check (c.staticGainFromBlock == c.staticGainFromBlock && std::abs (c.staticGainFromBlock + 3.5f) < 0.01f,
                   "(27) ...and static_gain_db is read off the block  (RED as it stood: the field was parsed "
                   "nowhere, so the only source was a map profile and a map without one cost the sensor)",
                   f1 (c.staticGainFromBlock));
            echojay::CalibLoop l; beginDriven (l, c);
            check (l.blockFromProfile,
                   "(27) ...and begin() carries from_profile onto the loop  (RED as it stood: the flag was set by "
                   "PluginProcessor only, so the LINK twin never set it and every from_profile rule read false "
                   "on a Link rack)");
            check (l.staticGainKnown() && std::abs (l.staticGainDb() + 3.5f) < 0.01f,
                   "(27) ...so the loop has a static offset with NO map profile attached at all",
                   f1 (l.staticGainDb()));
            // ZERO IS A REAL VALUE - a unity-gain compressor - and must not read as "absent".
            echojay::CalibLoop::Config z;
            parse (blockWith (true, juce::var (0.0)), z);
            echojay::CalibLoop lz; beginDriven (lz, z);
            check (lz.staticGainKnown() && std::abs (lz.staticGainDb()) < 0.01f,
                   "(27) ...and static_gain_db 0 is a READING, not an absence: a unity-gain compressor states 0");
            // ...while a block that does NOT say from_profile has no business supplying the sensor's offset.
            echojay::CalibLoop::Config n;
            parse (blockWith (false, juce::var (-3.5)), n);
            echojay::CalibLoop ln; beginDriven (ln, n);
            check (! ln.staticGainKnown(),
                   "(27) ...and a block with static_gain_db but NO from_profile supplies nothing - the offset is "
                   "only meaningful as part of a profile's own claim about the plugin");
            // ...and absent stays absent.
            echojay::CalibLoop::Config a;
            parse (blockWith (true, juce::var()), a);
            echojay::CalibLoop la; beginDriven (la, a);
            check (! la.staticGainKnown(),
                   "(27) ...and from_profile with no static_gain_db still has no offset, so the crest fallback "
                   "stands - which is the state Sean's session was actually in");
        }
        {   // THE CARD: a from_profile slot never quotes a crest figure as gain reduction.
            echojay::CalibLoop::Config c;
            parse (blockWith (true, juce::var()), c);      // from_profile, NO static: the crest fallback
            echojay::CalibLoop l; beginDriven (l, c);
            l.lastGr = 0.2f; l.lastSensor = "crest";
            check (! (l.measuredGrDb() == l.measuredGrDb()),
                   "(27) a from_profile slot reading by CREST reports NO gain-reduction figure  (RED as it stood: "
                   "\"about 0 dB on the loud phrases, from its profile\" on a compressor doing 2-3 dB - a reading "
                   "from the wrong sensor, attributed to the profile)",
                   f1 (l.measuredGrDb()));
            const auto card = l.card();
            check (! card.containsIgnoreCase ("about 0 dB"),
                   "(27) ...so the card cannot print \"about 0 dB ... from its profile\"", card);
            // ...and the SAME loop reading by level-minus-static does quote it, or the rule has just silenced
            // the feature instead of fixing it.
            l.lastGr = 2.4f; l.lastSensor = "level-static";
            check (l.measuredGrDb() == l.measuredGrDb() && std::abs (l.measuredGrDb() - 2.4f) < 0.01f,
                   "(27) ...while the level-minus-static reading IS quoted", f1 (l.measuredGrDb()));
            // ...and a slot with no profile at all still reports its crest figure, unchanged.
            echojay::CalibLoop::Config p;
            parse (blockWith (false, juce::var()), p);
            echojay::CalibLoop lp; beginDriven (lp, p);
            lp.lastGr = 0.2f; lp.lastSensor = "crest";
            check (lp.measuredGrDb() == lp.measuredGrDb(),
                   "(27) ...and an UNPROFILED slot still reports its crest reading, as before - the rule is about "
                   "attributing a figure to a profile, not about hiding figures", f1 (lp.measuredGrDb()));
        }
        {   // AND THE AGREEMENT RULE IS OFF once the block has supplied the offset: it exists for slots with no
            // static figure, and asking "hasProfile" would have kept it on for exactly this case.
            echojay::CalibLoop::Config c;
            parse (blockWith (true, juce::var (0.0)), c);
            c.mode = echojay::CalibLoop::Mode::Listen;
            c.actuator = echojay::CalibLoop::Actuator::Drive;   // the stepping path
            c.holdOnly = false; c.params.clear();
            c.startDb = 0.0f;
            echojay::CalibLoop l; beginDriven (l, c);
            l.dynamicsSlot = true;
            echojay::CalibLoop::Window w;
            w.measured = true; w.silent = false;
            w.grDb = 6.0f; w.grLevelDb = 6.0f; w.grLevelKnown = true;   // level says OVER
            w.grCrestDb = 0.0f;                                        // crest says UNDER: they disagree
            w.grSensor = "level-static";
            w.levelChangeDb = 0.0f; w.inTruePeakDb = -30.0f;
            w.slotOutGainDb = 0.0f; w.slotPreTrimDb = 0.0f;
            int writes = 0; float heard = 30.0f;
            for (int i = 0; i < 20; ++i)
            {
                w.heardSeconds = (heard += 4.0f); w.slotPreTrimDb = l.preDb;
                if (l.onWindow (w, 3000.0).writeDrive) ++writes;
                if (l.state == echojay::CalibLoop::State::Idle && l.window > 2) break;
            }
            check (writes >= 1,
                   "(27) with static_gain_db from the BLOCK the agreement rule is off and the loop acts on the "
                   "level figure  (the rule exists for slots that have NO static offset; testing hasProfile would "
                   "have frozen exactly the case the block just fixed)",
                   juce::String (writes) + " drive write(s), drive " + f1 (l.preDb) + " dB");
        }
    }

    // ---- (28) A HOLD WRITES NOTHING, NOT EVEN WHAT IT JUST READ (3 Oct 13:52 item 3) ----------------------
    {
        std::printf ("\n-- (28) the opening write is refused for a hold --\n");
        // 13:52:00 "wrote Threshold = 0.10", 13:53:11 "wrote Threshold = -1.20", both on hold-only blocks. The
        // write is not a no-op: it goes through the readback search and the map, so what lands can differ from
        // what was read - a hold had moved the compressor it was supposed to leave alone.
        auto cfg = [] (bool holdOnly, float startDb)
        {
            echojay::CalibLoop::Config c;
            c.plugin = "Tube-Tech CL 1B"; c.slot = 1;
            c.actuator = echojay::CalibLoop::Actuator::Threshold;
            c.params.add ("Threshold");
            c.holdOnly = holdOnly; c.startDb = startDb;
            return c;
        };
        const float nan = std::numeric_limits<float>::quiet_NaN();
        check (! echojay::CalibLoop::writesOpeningValue (cfg (true, 0.10f)),
               "(28) a hold-only block with a readable position writes NOTHING  (RED as it stood: \"wrote "
               "Threshold = 0.10\" - the value item 5 had just read, written straight back)");
        check (! echojay::CalibLoop::writesOpeningValue (cfg (true, nan)),
               "(28) ...and a hold with no position writes nothing either");
        check (echojay::CalibLoop::writesOpeningValue (cfg (false, -1.20f)),
               "(28) ...while a STEPPING block with an opening value still writes it - the opening write is how a "
               "threshold loop starts, and this must not have switched that off");
        check (! echojay::CalibLoop::writesOpeningValue (cfg (false, nan)),
               "(28) ...and a stepping block with no opening value sent still writes nothing, as before");
    }

    // ---- (29) gr_target_db AND last_gr_db IN [CURRENT CHAIN] (3 Oct 13:52 item 2) -------------------------
    {
        std::printf ("\n-- (29) the turn states what the loop is after and what it last read --\n");
        // Sean asked "harder" twice; the second turn came back asking for the same 3.0 dB, because nothing in the
        // turn said what the loop was already targeting - so a comparative had no current value to move from.
        Rig r (ChannelType::LeadVocal, 0.0f);
        feed (r.proc, r.prog, 6.0);
        const juce::String uid;
        {   // NO LOOP: the block says nothing about gain reduction at all.
            const auto before = EchoJayAPI::buildCurrentChainInjection (r.h);
            check (! before.contains ("gr_target_db") && ! before.contains ("last_gr_db"),
                   "(29) a rack with no loop states neither field  (ABSENT STAYS ABSENT: a 0 here would say the "
                   "compressor is doing nothing, which is a claim and the one the model would act on)",
                   before.substring (0, 90).replace ("\n", " "));
        }
        auto c = passiveDriveCfg ("EchoJay Gain");
        c.slot = 0; c.lo = 2.0f; c.hi = 3.0f;
        c.purpose = echojay::CalibLoop::Purpose::buildHold;
        r.proc.calibStartMany (uid, { c });
        {   // A LOOP WITH A TARGET BUT NO READING YET: the target rides, the reading does not.
            const auto mid = EchoJayAPI::buildCurrentChainInjection (r.h);
            check (mid.contains ("gr_target_db 2.0"),
                   "(29) a running loop states its target band  (RED as it stood: the field was never emitted, so "
                   "a second \"harder\" re-asked for the same 3.0 dB)",
                   mid.contains ("gr_target_db") ? mid.fromFirstOccurrenceOf ("gr_target_db", true, false)
                                                      .substring (0, 40) : juce::String ("no gr_target_db"));
            check (! mid.contains ("last_gr_db"),
                   "(29) ...and states NO last_gr_db before anything has been measured - a loop that has just "
                   "started has a target and no reading, which is exactly the state worth seeing");
        }
        {   // ...AND ONCE A WINDOW HAS BEEN JUDGED, the reading rides too, as a magnitude.
            auto loop = r.proc.calibLoad (uid);
            loop.lastGr = -2.4f;                 // the loop stores it signed; the block states the magnitude
            r.proc.calibStore (uid, loop);
            const auto after = EchoJayAPI::buildCurrentChainInjection (r.h);
            check (after.contains ("last_gr_db 2.4"),
                   "(29) a measured reading rides as its MAGNITUDE, the way \"N dB of gain reduction\" reads "
                   "everywhere else", after.contains ("last_gr_db")
                       ? after.fromFirstOccurrenceOf ("last_gr_db", true, false).substring (0, 30)
                       : juce::String ("no last_gr_db"));
            check (after.contains ("gr_target_db 2.0"),
                   "(29) ...beside the target it is being judged against");
        }
        {   // AN ENDED LOOP keeps its last reading and drops its target: the reading is still the last thing
            // measured on that compressor, while a band nobody is pursuing is a false claim about the present.
            auto loop = r.proc.calibLoad (uid);
            loop.lastGr = -2.4f;
            loop.state = echojay::CalibLoop::State::Idle;
            r.proc.calibStore (uid, loop);
            const auto ended = EchoJayAPI::buildCurrentChainInjection (r.h);
            check (ended.contains ("last_gr_db 2.4") && ! ended.contains ("gr_target_db"),
                   "(29) an ENDED loop keeps last_gr_db and drops gr_target_db - the reading still stands, the "
                   "target does not", ended.contains ("gr_target") ? juce::String ("target still stated")
                                                                   : juce::String ("target dropped, reading kept"));
        }
        r.proc.calibStore (uid, echojay::CalibLoop{});
    }


    // ---- (30) THE 1 dB POINT MOVES WITH THE CONTROL (4 Oct 2026 ruling) -----------------------------------
    {
        std::printf ("\n-- (30) in_at_gr1 is re-derived at the control's position, never frozen --\n");
        // The low-level-gain measurement only counts windows 6 dB under the unit's own 1 dB point. That point is a
        // property of the DIALLED POSITION, not of the plugin: the profile's amount.curve carries
        // in_at_gr_dbfs {"1","2","3"} per point. The first cut derived it once and guarded re-derivation with
        // `if (! isfinite(inAtGr1Dbfs))`, so after "harder" moved the threshold down the gate kept the old, HIGHER
        // ceiling and began admitting windows where the unit was compressing. That does not fail loudly - both
        // input bands are contaminated together, so they can still agree and report a confident wrong figure.
        //
        // A synthetic ladder, because the arithmetic is the thing under test: point 1 runs -30 dBFS at norm 0.2 to
        // -10 dBFS at norm 0.8, with nulls at both ends where no input produces 1 dB (the CL 1B prints "Off" at 0).
        auto profileWithLadder = [] () -> juce::var
        {
            auto pt = [] (double norm, juce::var gr1)
            {
                auto* o = new juce::DynamicObject();
                o->setProperty ("norm", norm);
                auto* lad = new juce::DynamicObject();
                lad->setProperty ("1", gr1);
                o->setProperty ("in_at_gr_dbfs", juce::var (lad));
                return juce::var (o);
            };
            juce::Array<juce::var> curve;
            curve.add (pt (0.0, juce::var()));        // "Off": no input produces 1 dB
            curve.add (pt (0.2, -30.0));
            curve.add (pt (0.5, -20.0));
            curve.add (pt (0.8, -10.0));
            curve.add (pt (1.0, juce::var()));        // past the top: not stated
            auto* amount = new juce::DynamicObject();
            amount->setProperty ("control", "Threshold");
            amount->setProperty ("curve", curve);
            auto* prof = new juce::DynamicObject();
            prof->setProperty ("amount", juce::var (amount));
            return juce::var (prof);
        };
        const auto prof = profileWithLadder();
        using CC = echojay::CompCheck;
        // AT THE STATED POINTS, exactly.
        check (std::abs (CC::inAtGr1At (prof, 0.2f) + 30.0f) < 0.01f,
               "(30) the ladder reads -30 dBFS at norm 0.2", f1 (CC::inAtGr1At (prof, 0.2f)));
        check (std::abs (CC::inAtGr1At (prof, 0.8f) + 10.0f) < 0.01f,
               "(30) ...and -10 dBFS at norm 0.8", f1 (CC::inAtGr1At (prof, 0.8f)));
        // DERIVE AT A, MOVE TO B: the figure must CHANGE and must match the ladder at B. This is the ruling's leg.
        const float atA = CC::inAtGr1At (prof, 0.35f);
        const float atB = CC::inAtGr1At (prof, 0.65f);
        check (std::abs (atA + 25.0f) < 0.2f,
               "(30) interpolated between stated points at norm 0.35", f1 (atA));
        check (std::abs (atB + 15.0f) < 0.2f,
               "(30) ...and at norm 0.65", f1 (atB));
        check (std::abs (atB - atA) > 1.0f,
               "(30) moving the control MOVES the 1 dB point  (RED as it stood: derived once and frozen, so the "
               "gate kept the old ceiling and admitted windows where the unit was compressing)",
               f1 (atA) + " -> " + f1 (atB));
        // THE NULLS AT THE ENDS ARE NOT ZERO. Reading a null as 0 dBFS would put the 1 dB point at full scale and
        // make every window look below threshold - the failure that would have looked like a working measurement.
        check (std::abs (CC::inAtGr1At (prof, 0.0f) + 30.0f) < 0.01f,
               "(30) at norm 0 the ladder is NULL, so the nearest STATED point stands - a null is never read as "
               "0 dBFS", f1 (CC::inAtGr1At (prof, 0.0f)));
        check (std::abs (CC::inAtGr1At (prof, 1.0f) + 10.0f) < 0.01f,
               "(30) ...and the same past the top", f1 (CC::inAtGr1At (prof, 1.0f)));
        // A PROFILE WITH NO LADDER states nothing, and the caller keeps its fallback.
        check (! (CC::inAtGr1At (juce::var(), 0.5f) == CC::inAtGr1At (juce::var(), 0.5f)),
               "(30) no profile -> NaN, so the gate falls back rather than inventing a ceiling");
        auto* bare = new juce::DynamicObject();
        check (! (CC::inAtGr1At (juce::var (bare), 0.5f) == CC::inAtGr1At (juce::var (bare), 0.5f)),
               "(30) ...and a profile with no amount.curve does the same");
        // AND THE PROVENANCE RULE: a block-stated figure is marked as such and must never be re-derived.
        echojay::CalibLoop::Config cb;
        cb.plugin = "Tube-Tech CL 1B"; cb.slot = 0;
        cb.inAtGr1Dbfs = -18.0f;
        echojay::CalibLoop lb; beginDriven (lb, cb);
        check (lb.inAtGr1FromBlock && std::abs (lb.inAtGr1Dbfs + 18.0f) < 0.01f,
               "(30) a BLOCK-stated figure is flagged as the block's, so nothing overwrites it",
               f1 (lb.inAtGr1Dbfs));
        echojay::CalibLoop::Config cd;
        cd.plugin = "Tube-Tech CL 1B"; cd.slot = 0;      // no in_at_gr1_dbfs
        echojay::CalibLoop ld; beginDriven (ld, cd);
        check (! ld.inAtGr1FromBlock,
               "(30) ...while an absent one is NOT, which is what licenses re-derivation on every move");
        // ...and both survive the sidecar round trip, or a Link rack loses the distinction on its second tick.
        lb.lastSensor = "level-static";
        const auto backB = echojay::CalibLoop::fromVar (lb.toVar());
        check (backB.inAtGr1FromBlock && std::abs (backB.inAtGr1Dbfs + 18.0f) < 0.01f,
               "(30) ...and the provenance rides the sidecar, so a Link tick cannot turn the block's figure into a "
               "re-derivable one", f1 (backB.inAtGr1Dbfs));
    }

    std::printf ("\n==== level_loop_guard: %s (%d assertion(s) failed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures);
}
} // namespace

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    guardMain();
    return failures == 0 ? 0 : 1;
}
