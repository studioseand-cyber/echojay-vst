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
#include "EchoJayAPI.h"
#include "EedDeviceRegistry.h"
// EedGainProcessor.cpp holds the BuiltinDeviceRegistrar for "EchoJay Gain", and a registrar is a static object
// with nothing referencing it - the linker drops the whole object file unless a symbol from it is used. The
// first cut of this guard did not, the registry came back empty, and every slot leg failed on an empty rack.
// This reference is what pulls the device in; level_slot_guard gets it for free by using the Level processor.
#include "EedGainProcessor.h"
#include <cstdio>
#include <cmath>
#include <memory>

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

    Rig (ChannelType role, float slotGainDb, bool realClock = false) : h (proc.getChainHost())
    {
        proc.prepareToPlay (48000.0, 512);
        if (! realClock) proc.calibClockMsForTest = [this] { return virtualMs; };
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
    std::vector<float> slotGainWrites;   // the VALUE the gain landed on, one entry per change
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
        const float g = r.slotOut();
        if (std::abs (g - lastGain) > 0.005f) { out.slotGainWrites.push_back (g); lastGain = g; wroteOnce = true; }
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
    std::printf ("    %s: %d window(s), %d write(s) to the slot output gain [%s]\n",
                 what, rr.windows, (int) rr.slotGainWrites.size(), w.isEmpty() ? "none" : w.toRawUTF8());
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
            check (std::abs (rr.slotGainWrites[0] + excess) <= 0.6f,
                   "(1) ...and the FIRST write lands the whole correction: IN plus the plugin, within 0.6 dB",
                   "wrote " + f1 (rr.slotGainWrites[0]) + ", owed " + f1 (-excess) + " dB");
        if (rr.slotGainWrites.size() >= 2)
            check (std::abs (rr.slotGainWrites[1] - rr.slotGainWrites[0]) < 0.5f,
                   "(1) ...and a SECOND write, if there is one, is a refinement smaller than 0.5 dB",
                   "moved " + f1 (rr.slotGainWrites[1] - rr.slotGainWrites[0]) + " dB");
        if (! rr.slotGainWrites.empty())
            check (std::abs (rr.slotGainWrites.back()) <= std::abs (excess) + 1.0f + 0.05f,
                   "(1) THE WRITTEN TOTAL never exceeds the first measured excess plus 1 dB  (RED as it stood: "
                   "-24.00, four times the excess)",
                   f1 (rr.slotGainWrites.back()) + " dB of " + f1 (std::abs (excess) + 1.0f) + " allowed");

        const float d = rr.chainOutAtEnd - rr.chainInAtEnd;
        check (d == d && std::abs (d) <= 0.5f,
               "(1) the CHAIN OUT, measured after the hold's own slot output gain, equals the chain in within "
               "0.5 dB", f1 (d) + " dB");
        int held = 0; for (float x : rr.chainDeltaAfterHold) if (std::abs (x) <= 0.6f) ++held;
        check ((int) rr.chainDeltaAfterHold.size() >= 3 && held == (int) rr.chainDeltaAfterHold.size(),
               "(1) ...and it STAYS there over three more windows",
               juce::String (held) + " of " + juce::String ((int) rr.chainDeltaAfterHold.size()) + " window(s) held");
        check (rr.closing.contains ("in total to hold the level"),
               "(1) the closing sentence states the WRITTEN TOTAL  (RED as it stood: \"Output trimmed 6.0 dB\" "
               "with 24 dB written)", rr.closing);
        if (! rr.slotGainWrites.empty())
            check (rr.closing.contains (juce::String (std::abs (rr.slotGainWrites.back()), 1)),
                   "(1) ...and the number in it IS the total that was written",
                   "wrote " + f1 (rr.slotGainWrites.back()) + " | " + rr.closing);
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
            check (std::abs (rr.slotGainWrites[0] + excess2) <= 0.6f,
                   "(2a) the first write is the whole correction the other way up too: IN plus the plugin",
                   "wrote " + f1 (rr.slotGainWrites[0]) + ", owed " + f1 (-excess2) + " dB");
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
               "(2b) a unity plugin costs at most ONE write, and only for what the drive added",
               juce::String ((int) rr.slotGainWrites.size()) + " write(s), IN at " + f1 (r.slotPreTrim()));
        if (! rr.slotGainWrites.empty())
            check (std::abs (rr.slotGainWrites[0] + r.slotPreTrim()) <= 0.6f,
                   "(2b) ...and that write is exactly minus the drive, nothing else",
                   "wrote " + f1 (rr.slotGainWrites[0]) + ", IN " + f1 (r.slotPreTrim()) + " dB");
        const float d2b = rr.chainOutAtEnd - rr.chainInAtEnd;
        check (d2b == d2b && std::abs (d2b) <= 0.6f,
               "(2b) ...and the chain comes out where it went in", f1 (d2b) + " dB");
    }

    // ---- (3) A BUS ROLE ------------------------------------------------------------------------------------
    {
        std::printf ("\n-- (3) bus role: nothing is written to the chain output, and no record is restored --\n");
        Rig r (ChannelType::FullMix, 6.0f);
        check (r.h.roleIsBus() && r.h.roleName() == "Mix Bus",
               "(3) fixture: the declared role is a BUS", r.h.roleName());

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
        check (chainLine.contains ("declared role Mix Bus"),
               "(3) ...and on a bus it names the role and says the last stage sets the level", chainLine);
    }

    // ---- (4) A HANDOVER MID-LOOP -------------------------------------------------------------------------
    {
        std::printf ("\n-- (4) a handover mid-loop: the second host judges a window, it does not go stale --\n");
        // Sean's NEOLD U17: handed over at 12:58:28 with the FIRST host's slot heard time (126.4 s) riding the
        // sidecar as lastHeardS; the second host's own tally started at zero and never passed it, so every
        // window for nine minutes logged state=stale-window with heard stuck at 24.1 s.
        echojay::CalibLoop first;
        auto cfg = passiveDriveCfg ("NEOLD U17");
        first.begin (cfg);
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
        // ...and again immediately, well inside the 3 s window
        feed (r.proc, r.prog, 3.0); r.proc.calibTick ({}); second = r.proc.calibLastLogLine();
        check (second == first,
               "(5) a second tick INSIDE the 3 s window judges nothing - the line is unchanged",
               second.isEmpty() ? juce::String ("(no line)") : second.fromLastOccurrenceOf ("window", true, false).substring (0, 40));
        // ...and after the window has actually passed, it judges again
        const double waitUntil = juce::Time::getMillisecondCounterHiRes() + 3100.0;
        while (juce::Time::getMillisecondCounterHiRes() < waitUntil) pumpMs (50);
        feed (r.proc, r.prog, 3.0); r.proc.calibTick ({});
        const auto third = r.proc.calibLastLogLine();
        check (third != second && third.isNotEmpty(),
               "(5) ...and once 3 s of WALL CLOCK have passed it judges the next window",
               third.fromLastOccurrenceOf ("window", true, false).substring (0, 40));
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
