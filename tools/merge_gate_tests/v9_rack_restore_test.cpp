/*  V9 EXTENDED (6 Sep 2026): a reload with DO NOT DIAL on must restore not only
    a device's params but the RACK'S SLOT WET VALUES, through the real path -
    EchoJayProcessor::setStateInformation -> ChainHost restore -> loadPluginAsync
    (LoadOrigin::Restore) -> the callback's setSlotWet(.., WetSource::Restore)
    and applyRestoredState -> the device's setStateInformation
    (ParamSource::Restore). A built-in slot (EchoJay Pitch) so no third-party
    plugin is needed. Positive control = the same round-trip, setting OFF.
    Everything runs ON THE MESSAGE THREAD as a chain of async steps (this JUCE
    build has JUCE_MODAL_LOOPS_PERMITTED off, so there is no runDispatchLoopUntil,
    and constructing EchoJayProcessor from another thread under a
    MessageManagerLock deadlocks); main() just runs the dispatch loop.        */
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "ChainHost.h"
#include "EedPitchProcessor.h"
#include "EJDialWrites.h"
#include <cstdio>

struct Leg : juce::Timer
{
    Leg (bool b, const char* l) : blocked (b), label (l) {}
    bool blocked; const char* label; std::function<void (int)> done;
    std::unique_ptr<EchoJayProcessor> a, b; juce::MemoryBlock st; int polls = 0;

    void start()
    {
        std::printf ("[%s] start (dialWritesBlocked=%s during setStateInformation)\n", label, blocked ? "TRUE" : "false");
        a = std::make_unique<EchoJayProcessor>(); a->prepareToPlay (48000.0, 512);
        std::printf ("[%s] processor built, loading the builtin slot\n", label);
        a->getChainHost().loadPluginAsync (ChainHost::builtinDescriptionFor ("EchoJay Pitch"), ChainHost::LoadOrigin::User,
            [this] (const juce::String& err) { juce::MessageManager::callAsync ([this, err] { loaded (err); }); });
    }
    void loaded (const juce::String& err)
    {
        auto& ch = a->getChainHost();
        if (err.isNotEmpty() || ch.getNumSlots() != 1) { std::printf ("[%s] builtin slot did not load (%s, slots %d)\n", label, err.toRawUTF8(), ch.getNumSlots()); return finish (99); }
        auto* dev = dynamic_cast<EedPitchProcessor*> (ch.getSlotProcessor (0));
        if (dev == nullptr) { std::printf ("[%s] slot 0 is not the pitch device\n", label); return finish (99); }
        dev->setParamValue (EedPitchProcessor::kRetune, 200.0);      // on the curve: 150 ms / depth 15
        dev->setParamValue (EedPitchProcessor::kNaturalVib, 50.0);
        dev->setParamValue (EedPitchProcessor::kFlex, 30.0);
        ch.setSlotWet (0, 0.4f, ChainHost::WetSource::User);        // the hand on the wet knob
        a->getStateInformation (st);
        std::printf ("[%s] state saved (%d bytes), reloading into a fresh processor\n", label, (int) st.getSize());
        a.reset();
        echojay::setDialWritesBlocked (blocked);
        b = std::make_unique<EchoJayProcessor>(); b->prepareToPlay (48000.0, 512);
        b->setStateInformation (st.getData(), (int) st.getSize());
        startTimer (50);
    }
    void timerCallback() override
    {
        auto& ch = b->getChainHost();
        const bool back = ch.getNumSlots() == 1 && ch.getSlotProcessor (0) != nullptr;
        if (! back && ++polls < 200) return;                         // up to 10 s
        stopTimer();
        // one more beat so the restore callback's setSlotWet / applyRestoredState land
        juce::Timer::callAfterDelay (500, [this] { readBack(); });
    }
    void readBack()
    {
        auto& ch = b->getChainHost();
        echojay::setDialWritesBlocked (false);
        if (ch.getNumSlots() != 1) { std::printf ("[%s] slot did not come back (slots %d)\n", label, ch.getNumSlots()); return finish (99); }
        int bad = 0;
        const float wet = ch.getSlotWet (0); const bool wetOk = std::abs (wet - 0.4f) < 0.005f; bad += ! wetOk;
        std::printf ("  %-28s saved %7.2f  reloaded %7.2f  %s\n", "slot 0 wet", 0.4, wet, wetOk ? "ok" : "LOST");
        auto* dev = dynamic_cast<EedPitchProcessor*> (ch.getSlotProcessor (0));
        if (dev == nullptr) { std::printf ("  restored slot 0 is not the pitch device\n"); return finish (99); }
        struct Row { const char* id; double want; };
        for (auto& r : { Row { EedPitchProcessor::kRetune, 200.0 }, Row { EedPitchProcessor::kRetuneMs, 150.0 },
                         Row { EedPitchProcessor::kNaturalVib, 50.0 }, Row { EedPitchProcessor::kFlex, 30.0 } })
        {
            const double got = dev->getParamValue (r.id); const bool ok = std::abs (got - r.want) < 0.5; bad += ! ok;
            std::printf ("  %-28s saved %7.2f  reloaded %7.2f  %s\n", r.id, r.want, got, ok ? "ok" : "LOST (default written)");
        }
        finish (bad);
    }
    void finish (int r) { b.reset(); a.reset(); auto d = done; juce::MessageManager::callAsync ([d, r] { d (r); }); }
};

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI init;
    int ctl = -1, on = -1;
    Leg control (false, "POSITIVE CONTROL: setting OFF");
    Leg blockedLeg (true, "V9 RACK: setting ON");
    blockedLeg.done = [&] (int r) { on = r; juce::MessageManager::getInstance()->stopDispatchLoop(); };
    control.done   = [&] (int r) { ctl = r; if (r == 0) blockedLeg.start(); else juce::MessageManager::getInstance()->stopDispatchLoop(); };
    juce::MessageManager::callAsync ([&] { control.start(); });
    juce::MessageManager::getInstance()->runDispatchLoop();
    std::printf ("control: %s   V9 rack: %s\n",
                 ctl == 0 ? "PASS" : (ctl == 99 ? "UNREACHABLE (harness could not drive the path)" : "FAIL (harness invalid)"),
                 on == 0 ? "PASS" : (on == 99 ? "UNREACHABLE" : (on < 0 ? "NOT RUN" : "FAIL - rack values lost on reload with DO NOT DIAL on")));
    return (ctl != 0) ? 2 : (on != 0 ? 1 : 0);
}
