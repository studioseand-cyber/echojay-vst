/*  V9 EXTENDED (6 Sep 2026): a reload with DO NOT DIAL on must restore not only
    a device's params but the RACK'S SLOT WET VALUES, through the real path -
    EchoJayProcessor::setStateInformation -> ChainHost restore -> loadPluginAsync
    (LoadOrigin::Restore) -> the callback's setSlotWet(.., WetSource::Restore)
    and applyRestoredState -> the device's setStateInformation
    (ParamSource::Restore). A built-in slot (EchoJay Pitch) so no third-party
    plugin is needed; the message loop is pumped for the async callbacks.
    Positive control = the same round-trip with the setting OFF.               */
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "ChainHost.h"
#include "EedPitchProcessor.h"
#include "EJDialWrites.h"
#include <cstdio>

// This JUCE build has JUCE_MODAL_LOOPS_PERMITTED off (no runDispatchLoopUntil),
// so the test body runs on its own thread under a MessageManagerLock while the
// main thread runs the real dispatch loop; async load/restore callbacks land
// there between our locked sections.
static void pump (int ms) { juce::Thread::sleep (ms); }

static bool waitFor (std::function<bool()> cond, int timeoutMs)
{
    const auto t0 = juce::Time::getMillisecondCounter();
    for (;;)
    {
        { juce::MessageManagerLock mml; if (cond()) return true; }
        if ((int) (juce::Time::getMillisecondCounter() - t0) >= timeoutMs) { juce::MessageManagerLock mml; return cond(); }
        pump (20);
    }
}

static int roundTrip (bool blockedDuringLoad, const char* label)
{
    std::printf ("[%s] start\n", label);
    juce::MemoryBlock st;
    {
        std::unique_ptr<EchoJayProcessor> ap; { juce::MessageManagerLock mml; ap = std::make_unique<EchoJayProcessor>(); ap->prepareToPlay (48000.0, 512); }
        EchoJayProcessor& a = *ap;
        std::printf ("[%s] processor built\n", label);
        auto& ch = a.getChainHost();
        bool done = false; juce::String loadErr;
        { juce::MessageManagerLock mml;
          ch.loadPluginAsync (ChainHost::builtinDescriptionFor ("EchoJay Pitch"), ChainHost::LoadOrigin::User,
                              [&] (const juce::String& e) { loadErr = e; done = true; }); }
        std::printf ("[%s] builtin load requested\n", label);
        if (! waitFor ([&] { return done; }, 10000) || loadErr.isNotEmpty() || ch.getNumSlots() != 1)
        { std::printf ("%s: builtin slot did not load (%s, slots %d)\n", label, loadErr.toRawUTF8(), ch.getNumSlots()); return 99; }
        { juce::MessageManagerLock mml;
          auto* dev = dynamic_cast<EedPitchProcessor*> (ch.getSlotProcessor (0));
          if (dev == nullptr) { std::printf ("%s: slot 0 is not the pitch device\n", label); return 99; }
          dev->setParamValue (EedPitchProcessor::kRetune, 200.0);     // on the curve: 150 ms / depth 15
          dev->setParamValue (EedPitchProcessor::kNaturalVib, 50.0);
          dev->setParamValue (EedPitchProcessor::kFlex, 30.0);
          ch.setSlotWet (0, 0.4f, ChainHost::WetSource::User); }     // the hand on the wet knob
        pump (100);
        { juce::MessageManagerLock mml; a.getStateInformation (st); }
        { juce::MessageManagerLock mml; ap.reset(); }
    }

    std::unique_ptr<EchoJayProcessor> bp;
    { juce::MessageManagerLock mml;
      echojay::setDialWritesBlocked (blockedDuringLoad);
      bp = std::make_unique<EchoJayProcessor>(); bp->prepareToPlay (48000.0, 512);
      bp->setStateInformation (st.getData(), (int) st.getSize()); }
    EchoJayProcessor& b = *bp;
    std::printf ("[%s] state set on the fresh processor, waiting for the restore\n", label);
    auto& ch = b.getChainHost();
    const bool restored = waitFor ([&] { return ch.getNumSlots() == 1 && ch.getSlotProcessor (0) != nullptr; }, 10000);
    pump (500);   // let the restore callback's setSlotWet / applyRestoredState land
    juce::MessageManagerLock mml;   // the read-back below, and the processor's destruction, under the lock
    echojay::setDialWritesBlocked (false);

    std::printf ("%s (dialWritesBlocked=%s during setStateInformation)\n", label, blockedDuringLoad ? "TRUE" : "false");
    if (! restored) { std::printf ("  slot did not come back (slots %d)\n", ch.getNumSlots()); return 99; }
    int bad = 0;
    const float wet = ch.getSlotWet (0);
    const bool wetOk = std::abs (wet - 0.4f) < 0.005f; bad += ! wetOk;
    std::printf ("  %-28s saved %7.2f  reloaded %7.2f  %s\n", "slot 0 wet", 0.4, wet, wetOk ? "ok" : "LOST");
    auto* dev = dynamic_cast<EedPitchProcessor*> (ch.getSlotProcessor (0));
    if (dev == nullptr) { std::printf ("  restored slot 0 is not the pitch device\n"); return 99; }
    struct Row { const char* id; double want; };
    for (auto& r : { Row { EedPitchProcessor::kRetune, 200.0 }, Row { EedPitchProcessor::kRetuneMs, 150.0 },
                     Row { EedPitchProcessor::kNaturalVib, 50.0 }, Row { EedPitchProcessor::kFlex, 30.0 } })
    {
        const double got = dev->getParamValue (r.id); const bool ok = std::abs (got - r.want) < 0.5; bad += ! ok;
        std::printf ("  %-28s saved %7.2f  reloaded %7.2f  %s\n", r.id, r.want, got, ok ? "ok" : "LOST (default written)");
    }
    return bad;
}

struct Runner : juce::Thread
{
    Runner() : juce::Thread ("v9rack") {}
    int ctl = -1, on = -1;
    void run() override
    {
        ctl = roundTrip (false, "POSITIVE CONTROL: setting OFF");
        on  = roundTrip (true,  "V9 RACK: setting ON");
        juce::MessageManager::callAsync ([] { juce::MessageManager::getInstance()->stopDispatchLoop(); });
    }
};

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI init;
    Runner r; r.startThread();
    juce::MessageManager::getInstance()->runDispatchLoop();
    r.waitForThreadToExit (-1);
    const int ctl = r.ctl, on = r.on;
    std::printf ("control: %s   V9 rack: %s\n", ctl == 0 ? "PASS" : (ctl == 99 ? "UNREACHABLE (harness could not drive the path)" : "FAIL (harness invalid)"),
                 on == 0 ? "PASS" : (on == 99 ? "UNREACHABLE" : "FAIL - rack values lost on reload with DO NOT DIAL on"));
    return (ctl != 0) ? 2 : (on != 0 ? 1 : 0);
}
