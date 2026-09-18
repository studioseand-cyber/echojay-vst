// loudness_loop_guard (18 Sep 2026 ruling G) on the REAL objects: EchoJayProcessor -> ChainHost -> EchoJay Limiter,
// the real chain-output tally and the real LoudnessLoop. A -18 LUFS programme (noise, calibrated through the chain
// INPUT tally) with a 2 s silence gap runs through a chain whose limiter carries "to the -9 LUFS target" and an
// open-loop input gain deliberately 3 dB short. RED (-DEJ_GUARD_TODAY = no loop armed): the output stays at the
// open-loop miss. GREEN: within +-1 dB of the target after two passes, the gap not counted. Edge: a -24 programme
// asserts the "would need +15 dB" bubble and NO gain change.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "EedLimiterProcessor.h"   // force-link the built-in's registrar
#include "EedDeviceRegistry.h"
#include <cstdio>
struct EchoJayBorrowHostTestAccess { static juce::String loadBuiltin (ChainHost& h, const juce::PluginDescription& d) { return h.loadBuiltinNow (d); } };
namespace {
int failures = 0; void check (bool ok, const juce::String& w, const juce::String& d = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
juce::String f1 (float v) { return juce::String (v, 2); }
struct Programme { juce::Random rng { 4242 }; float amp = 0.1f; };
void feed (EchoJayProcessor& p, Programme& prog, int blocks, bool silent, LoudnessLoop* loop)
{
    juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
    for (int b = 0; b < blocks; ++b)
    {
        for (int ch = 0; ch < 2; ++ch) { auto* d = buf.getWritePointer (ch); for (int i = 0; i < 512; ++i) d[i] = silent ? 0.0f : (prog.rng.nextFloat() * 2.0f - 1.0f) * prog.amp; }
        p.processBlock (buf, midi);
        if (loop != nullptr && (b % 23) == 22) loop->tickNow();   // ~250 ms of audio per tick, as the plugin's timer would
    }
}
float calibrate (EchoJayProcessor& p, Programme& prog, float wantLufs)
{   // measure the programme at the chain INPUT (K-weighted, gated) and scale the amplitude to the wanted LUFS
    p.getChainHost().resetAllLevels();
    feed (p, prog, 600, false, nullptr);
    const auto in = p.getChainHost().getChainInLevels();
    const float have = in.levelDb;
    prog.amp *= std::pow (10.0f, (wantLufs - have) / 20.0f);
    p.getChainHost().resetAllLevels();
    feed (p, prog, 600, false, nullptr);
    return p.getChainHost().getChainInLevels().levelDb;
}
} // namespace
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ej_loudloop_" + juce::String (juce::Time::getMillisecondCounter()));
    tmp.createDirectory(); setenv ("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);
    std::printf ("loudness_loop_guard: a -18 LUFS programme, target -9, open-loop gain 3 dB short\n");
    const auto* dev = BuiltinDeviceRegistry::instance().findByName ("EchoJay Limiter");
    check (dev != nullptr, "precondition: EchoJay Limiter is registered");
    if (! dev) return 2;

    auto runScenario = [&] (float programmeLufs, float target, double openLoopDb, bool armLoop, juce::StringArray& bubbles, float& outAfter, double& inputDbAfter, LoudnessLoop::State& st, float& gapCountedDelta)
    {
        EchoJayProcessor proc; proc.prepareToPlay (48000.0, 512);
        auto& h = proc.getChainHost();
        const auto err = EchoJayBorrowHostTestAccess::loadBuiltin (h, BuiltinDeviceRegistry::descriptionFor (*dev));
        check (err.isEmpty() && h.getNumSlots() == 1 && h.getSlotInfo (0).name == "EchoJay Limiter", "the limiter is slot 0", err);
        { auto* pp = new juce::DynamicObject(); pp->setProperty ("input_db", openLoopDb); pp->setProperty ("ceiling_db", -0.1); pp->setProperty ("true_peak", 1);
          auto* w = new juce::DynamicObject(); w->setProperty ("params", juce::var (pp)); h.setSlotStructuredSettings (0, juce::var (w)); }
        h.setSlotSettings (0, "input gain " + juce::String (openLoopDb, 1) + " dB to the " + juce::String (target, 0) + " LUFS target, ceiling -0.1 dBTP");
        Programme prog;
        const float cal = calibrate (proc, prog, programmeLufs);
        check (std::abs (cal - programmeLufs) < 0.6f, "the programme is calibrated at the chain input to " + f1 (programmeLufs) + " LUFS", f1 (cal));
        auto& loop = proc.loudnessLoop();
        loop.isPlaying = [] { return true; };
        loop.onBubble = [&bubbles] (const LoudnessLoop::Bubble& b) { if (b.text.startsWith ("Listening") || b.text.startsWith ("Checking...")) return; bubbles.add (b.text); };   // milestones only; the progress texts replace in place in the plugin
        LoudnessLoop* lp = armLoop ? &loop : nullptr;
        if (armLoop) check (loop.armFromChain(), "armed from the chain (the limiter's settings name the target)", h.getSlotInfo (0).settings);
        // pass 1 with a 2 s silence gap in the middle: 4 s audio, 2 s silence, 8 s audio
        feed (proc, prog, 375, false, lp);
        const float countedBeforeGap = h.getChainOutLevels().heardAboveSeconds;
        feed (proc, prog, 188, true, lp);
        const float countedAfterGap = h.getChainOutLevels().heardAboveSeconds;
        gapCountedDelta = countedAfterGap - countedBeforeGap;
        feed (proc, prog, 750, false, lp);
        // pass 2
        feed (proc, prog, 1000, false, lp);
        // settle: measure the output as it now stands, loop or not
        h.resetChainOutLevels(); feed (proc, prog, 940, false, nullptr);
        outAfter = h.getChainOutLevels().levelDb;
        inputDbAfter = dynamic_cast<EedLimiterProcessor*> (h.getSlotProcessor (0))->inputDb();
        st = loop.state();
    };

    std::printf ("== the loop: -18 programme, target -9, open loop +6 (3 dB short) ==\n");
    {
        juce::StringArray bubbles; float outAfter = 0; double inDb = 0; LoudnessLoop::State st; float gapDelta = 0;
#ifdef EJ_GUARD_TODAY
        runScenario (-18.0f, -9.0f, 6.0, false, bubbles, outAfter, inDb, st, gapDelta);
        check (std::abs (outAfter - (-9.0f)) <= 1.0f, "output within +-1 dB of the -9 target after two passes", "TODAY (no loop): output " + f1 (outAfter) + " LUFS, input_db still " + juce::String (inDb, 1) + " - the open-loop miss");
#else
        runScenario (-18.0f, -9.0f, 6.0, true, bubbles, outAfter, inDb, st, gapDelta);
        check (std::abs (outAfter - (-9.0f)) <= 1.0f, "output within +-1 dB of the -9 target after two passes", "output " + f1 (outAfter) + " LUFS, input_db " + juce::String (inDb, 1));
        check (st == LoudnessLoop::State::hold, "the loop HOLDS after pass 2 (never a third pass)", juce::String ((int) st));
        check (gapDelta < 0.3f, "the 2 s silence gap was NOT counted toward the 10 s", "counted during the gap: " + f1 (gapDelta) + " s");
        check (bubbles.size() >= 3 && bubbles[0].startsWith ("Chain built. Play the loudest part"), "bubble 1: the arm text", bubbles.joinIntoString (" | ").substring (0, 300));
        check (bubbles.size() >= 3 && bubbles[bubbles.size() - 2].startsWith ("Measured -1") && bubbles[bubbles.size() - 2].contains ("Pushing +") && bubbles[bubbles.size() - 2].contains ("- checking."), "bubble after pass 1: \"Measured -x. Pushing +y dB - checking.\"", bubbles[juce::jmax (0, bubbles.size() - 2)]);
        check (bubbles.size() >= 3 && bubbles[bubbles.size() - 1].startsWith ("Hitting -") && bubbles[bubbles.size() - 1].contains ("target -9.0") && bubbles[bubbles.size() - 1].contains ("Peaks ") && bubbles[bubbles.size() - 1].contains ("limiter working "), "final bubble: \"Hitting -x LUFS, target -9. Peaks -y dBTP, limiter working a-b dB.\"", bubbles[bubbles.size() - 1]);
        check (std::abs (inDb - 6.0) > 0.5, "the limiter input_db was trimmed from the open-loop +6", juce::String (inDb, 2));
#endif
    }
    std::printf ("== the edge: -24 programme, target -9 (+15 needed) ==\n");
#ifndef EJ_GUARD_TODAY
    {
        juce::StringArray bubbles; float outAfter = 0; double inDb = 0; LoudnessLoop::State st; float gapDelta = 0;
        runScenario (-24.0f, -9.0f, 6.0, true, bubbles, outAfter, inDb, st, gapDelta);
        const auto last = bubbles.isEmpty() ? juce::String() : bubbles[bubbles.size() - 1];
        check (last.contains ("Your mix reads -24") && last.contains ("would need +15") && last.contains ("say 'push it'"), "the bubble says what it would take (\"your mix reads -24; would need +15 dB\") and offers 'push it'", last);
        check (std::abs (inDb - 6.0) < 0.01, "and NO clamp was applied: input_db unchanged at +6", juce::String (inDb, 2));
        check (st == LoudnessLoop::State::hold, "the loop holds (no change)");
    }
#else
    check (false, "the edge bubble exists", "TODAY: no loop");
#endif
    std::printf ("\n==== loudness_loop_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
