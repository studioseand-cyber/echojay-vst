// level_loop_guard PRE-FIX RED (21t-m, 29 Sep 2026). The SAME rig and the SAME case (1) as harness.cpp, cut
// down to what exists on the PRE-FIX tree, so the wind-up can be shown happening before it is fixed. It paces on
// the real clock because calibClockMsForTest is one of this round's additions and is not there to hook. It is
// registered with NO ctest label: it is run by hand against a stashed Source/ and its output is quoted in MERGE.
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
    Rig (ChannelType role, float slotGainDb) : h (proc.getChainHost())
    {
        proc.prepareToPlay (48000.0, 512);
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
    float chainInDb()  const { const auto s = h.getChainInLevels();  return s.known ? s.levelDb : std::numeric_limits<float>::quiet_NaN(); }
    float chainOutDb() const { const auto s = h.getChainOutLevels(); return s.known ? s.levelDb : std::numeric_limits<float>::quiet_NaN(); }
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
    double lastTickMs = juce::Time::getMillisecondCounterHiRes();
    for (int k = 0; k < maxWindows; ++k)
    {
        feed (r.proc, r.prog, secondsPerWindow);
        while (juce::Time::getMillisecondCounterHiRes() - lastTickMs < 3050.0) pumpMs (50);
        lastTickMs = juce::Time::getMillisecondCounterHiRes();
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


void guardMain()
{
    std::printf ("== level_loop_guard PRE-FIX RED: the hold on a +6 dB channel slot ==\n");
    Rig r (ChannelType::LeadVocal, 6.0f);
    feed (r.proc, r.prog, 6.0);
    r.proc.calibStart ({}, passiveDriveCfg ("Fake Comp +6"));
    const auto rr = runLoop (r, 28);
    reportRun ("PRE-FIX", rr);
    check ((int) rr.slotGainWrites.size() <= 2,
           "AT MOST TWO WRITES for the whole settle", juce::String ((int) rr.slotGainWrites.size()) + " write(s)");
    if (! rr.slotGainWrites.empty())
        check (std::abs (rr.slotGainWrites.back()) <= 7.05f,
               "THE WRITTEN TOTAL never exceeds the first measured excess plus 1 dB",
               f1 (rr.slotGainWrites.back()) + " dB of 7.00 allowed");
    const float d = rr.chainOutAtEnd - rr.chainInAtEnd;
    check (d == d && std::abs (d) <= 0.5f, "the chain out equals the chain in", f1 (d) + " dB");
    check (rr.closing.contains ("in total to hold the level"),
           "the closing sentence states the WRITTEN TOTAL", rr.closing);

    // ---- 21t-m item 6a: THE MAP'S CLASSIFICATION, on the PRE-FIX count ----------------------------------
    // dynamicsSlotCount() exists on the pre-fix tree, so this leg compiles both sides of the fix and is the
    // direct RED for 6a. Apple's AUDelay (on every Mac, not PACE-wrapped) is a slot that HAS a fingerprint -
    // built-ins carry none - and its own category is a delay, so only the map can make it count.
    {
        std::printf ("\n-- item 6a: a dynamics slot only the MAP knows about --\n");
        Rig r6 (ChannelType::LeadVocal, 6.0f);
        juce::PluginDescription dly;
        dly.name = "AUDelay"; dly.pluginFormatName = "AudioUnit";
        dly.fileOrIdentifier = "AudioUnit:Effects/aufx,dely,appl";
        dly.uniqueId = dly.deprecatedUid = (int) (juce::int64) juce::String ("64607a6d").getHexValue64();
        r6.h.onNeedParamMaps = [] (const juce::StringArray&) {};   // pre-fix: called bare, and an empty one aborts
        r6.h.loadPluginAsync (dly, ChainHost::LoadOrigin::User, [] (const juce::String&) {});
        for (int k = 0; k < 40 && r6.h.getNumSlots() < 2; ++k) pumpMs (100);
        check (r6.h.getNumSlots() == 2, "precondition: AUDelay loaded as slot 2",
               juce::String (r6.h.getNumSlots()) + " slot(s)");
        const auto fp = r6.h.getSlotIdentity (1).fp;
        check (fp.isNotEmpty(), "precondition: it carries a fingerprint", fp.substring (0, 16));
        if (fp.isNotEmpty())
        {
            auto* m = new juce::DynamicObject(); m->setProperty ("category", "compressor");
            auto* maps = new juce::DynamicObject(); maps->setProperty (fp, juce::var (m));
            r6.h.storeParamMaps (juce::var (maps));
            pumpMs (150);
            check (r6.h.dynamicsSlotCount() == 1,
                   "item 6a: a slot whose MAP says category=compressor counts as a dynamics slot",
                   juce::String (r6.h.dynamicsSlotCount()) + " of " + juce::String (r6.h.getNumSlots())
                       + "  <-- PRE-FIX READS 0, which is Sean's \"loops started 1 of 0 dynamics slots\"");
        }
    }

    std::printf ("\n==== level_loop_guard PRE-FIX RED: %s (%d assertion(s) failed) ====\n",
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
