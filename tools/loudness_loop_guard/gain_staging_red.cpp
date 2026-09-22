// gain_staging_red (22 Sep 2026, 21m ruling 2): the BEHAVIOURAL known-bad for gain staging, using only pre-21m APIs.
// Chain = [EchoJay Level, EchoJay Gain +4 dB, EchoJay Limiter]; -18 LUFS programme; arm; one Listen window; then feed and
// read ChainHost::getSlotLevels(1): out - in over the short-term window.
//   pre-21m tree (EJ_LIB=libV2_pre21m.a EJ_SRC_ROOT=asis21m): the Gain slot STILL reads +4 dB after Listen -> RED (exit 1)
//   21m tree: the unity trim brought it to 0.0 +-0.3 -> GREEN (exit 0)
// NOT in the gate (build_and_run.sh runs harness.cpp): this file is run by hand against both trees and its output is
// quoted in the record. Written exclusion: a RED-by-construction leg cannot sit in a suite that must be GREEN.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "EedLimiterProcessor.h"
#include "EedGainProcessor.h"
#include "EedDeviceRegistry.h"
#include "EedLevelProcessor.h"
#include <cstdio>
#include <cmath>
struct EchoJayBorrowHostTestAccess { static juce::String loadBuiltin (ChainHost& h, const juce::PluginDescription& d) { return h.loadBuiltinNow (d); } };
namespace {
struct Programme { juce::Random rng { 4242 }; float amp = 0.1f; int blockCount = 0; };
void feed (EchoJayProcessor& p, Programme& prog, int blocks, LoudnessLoop* loop)
{
    juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
    for (int b = 0; b < blocks; ++b)
    {
        for (int ch = 0; ch < 2; ++ch) { auto* d = buf.getWritePointer (ch); for (int i = 0; i < 512; ++i) d[i] = (prog.rng.nextFloat() * 2.0f - 1.0f) * prog.amp; }
        ++prog.blockCount; p.processBlock (buf, midi);
        if (loop != nullptr && (b % 23) == 22) loop->tickNow();
    }
}
}
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    (void) EedLimiterProcessor::schema(); (void) EedGainProcessor::schema();
    auto proc = std::make_unique<EchoJayProcessor>(); proc->prepareToPlay (48000.0, 512); auto& h = proc->getChainHost(); auto& loop = proc->loudnessLoop();
    const auto* lv = BuiltinDeviceRegistry::instance().findByName ("EchoJay Level");
    const auto* gn = BuiltinDeviceRegistry::instance().findByName ("EchoJay Gain");
    const auto* lm = BuiltinDeviceRegistry::instance().findByName ("EchoJay Limiter");
    if (! lv || ! gn || ! lm) { std::printf ("precondition failed: devices not registered\n"); return 2; }
    EchoJayBorrowHostTestAccess::loadBuiltin (h, BuiltinDeviceRegistry::descriptionFor (*lv));
    EchoJayBorrowHostTestAccess::loadBuiltin (h, BuiltinDeviceRegistry::descriptionFor (*gn));
    EchoJayBorrowHostTestAccess::loadBuiltin (h, BuiltinDeviceRegistry::descriptionFor (*lm));
    { auto* pp = new juce::DynamicObject(); pp->setProperty ("gain_db", 0.0); pp->setProperty ("target_lufs", -9.0); pp->setProperty ("loudness_option", 0); auto* w = new juce::DynamicObject(); w->setProperty ("params", juce::var (pp)); h.setSlotStructuredSettings (0, juce::var (w)); }
    { auto* pp = new juce::DynamicObject(); pp->setProperty ("level_db", 4.0); auto* w = new juce::DynamicObject(); w->setProperty ("params", juce::var (pp)); h.setSlotStructuredSettings (1, juce::var (w)); }
    { auto* pp = new juce::DynamicObject(); pp->setProperty ("input_db", 0.0); pp->setProperty ("ceiling_db", -0.1); pp->setProperty ("true_peak", 1); auto* w = new juce::DynamicObject(); w->setProperty ("params", juce::var (pp)); h.setSlotStructuredSettings (2, juce::var (w)); }
    loop.isPlaying = [] { return true; };
    Programme prog;
    h.resetAllLevels(); feed (*proc, prog, 600, nullptr); const float have = h.getChainInLevels().levelDb; prog.amp *= std::pow (10.0f, (-18.0f - have) / 20.0f);
    h.resetAllLevels(); feed (*proc, prog, 600, nullptr);
    std::printf ("programme at the chain input: %.2f LUFS; chain = [%s, %s, %s]\n", h.getChainInLevels().levelDb, h.getSlotInfo (0).name.toRawUTF8(), h.getSlotInfo (1).name.toRawUTF8(), h.getSlotInfo (2).name.toRawUTF8());
    { const auto lvl = h.getSlotLevels (1); std::printf ("before Listen: Gain slot out - in = %+.2f dB\n", lvl.out.shortTermDb - lvl.in.shortTermDb); }
    const bool armed = loop.armFromChain(); std::printf ("armed: %d (Level slot %d, limiter slot %d)\n", (int) armed, loop.levelSlot(), loop.limiterSlot());
    if (loop.state() == LoudnessLoop::State::armed) loop.listen();
    for (int k = 0; k < 16 && (loop.state() == LoudnessLoop::State::waitAudio || loop.state() == LoudnessLoop::State::measuring); ++k) feed (*proc, prog, 100, &loop);
    std::printf ("after the window: loop state %d\n", (int) loop.state());
    feed (*proc, prog, 400, nullptr);
    const auto lvl = h.getSlotLevels (1); const float d = lvl.out.shortTermDb - lvl.in.shortTermDb;
    std::printf ("after Listen: Gain slot out - in = %+.2f dB (short-term, measured=%d)\n", d, (int) lvl.measured);
    const bool unity = lvl.measured && std::abs (d) < 0.3f;
    std::printf ("\n==== gain_staging_red: %s (%s) ====\n", unity ? "GREEN" : "RED", unity ? "the slot was trimmed to unity after Listen" : "the +4 dB slot still reads its own gain after Listen - no unity trim on this tree");
    return unity ? 0 : 1;
}
