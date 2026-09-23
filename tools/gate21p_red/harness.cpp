// gate21p_red (23 Sep 2026): the BEHAVIOURAL known-bad for 21p items 1 and 4, written against surface that exists on
// BOTH trees (the 3-argument measureUnityTrims and EchoJay_NSLog), so the difference is behaviour, not API.
//   pre-21p -> RED: a trim is written from a SILENT reading, and no log file exists.
//   21p     -> GREEN: silence writes nothing, and the plugin's own log is on disk.
// Items 2 and 3 (the per-slot picture, the pre-trim) are new API and are compile-refusal RED there; this file is not
// in the gate, for the usual reason - a RED-by-construction leg cannot sit in a suite that must be GREEN.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "EedDeviceRegistry.h"
#include "EedLevelProcessor.h"
#include "EedLimiterProcessor.h"
#include "EedGainProcessor.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
extern "C" void EchoJay_NSLog (const char* msg);
struct EchoJayBorrowHostTestAccess { static juce::String loadBuiltin (ChainHost& h, const juce::PluginDescription& d) { return h.loadBuiltinNow (d); } };
namespace {
int failures = 0; void check (bool ok, const juce::String& w, const juce::String& d = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
void feedSilence (EchoJayProcessor& p, int blocks)
{ juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer m; for (int i = 0; i < blocks; ++i) { b.clear(); p.processBlock (b, m); } }
void feedNoise (EchoJayProcessor& p, int blocks, float amp)
{ juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer m; juce::Random r (7);
  for (int i = 0; i < blocks; ++i) { for (int ch = 0; ch < 2; ++ch) { auto* d = b.getWritePointer (ch); for (int k = 0; k < 512; ++k) d[k] = (r.nextFloat() * 2.0f - 1.0f) * amp; } p.processBlock (b, m); } }
}
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    (void) EedLevelProcessor::schema(); (void) EedLimiterProcessor::schema(); (void) EedGainProcessor::schema();
    auto ph = std::make_unique<EchoJayProcessor>(); auto& p = *ph; p.prepareToPlay (48000.0, 512); auto& h = p.getChainHost();
    for (const char* n : { "EchoJay Level", "EchoJay Gain", "EchoJay Limiter" })
        EchoJayBorrowHostTestAccess::loadBuiltin (h, BuiltinDeviceRegistry::descriptionFor (*BuiltinDeviceRegistry::instance().findByName (n)));
    { auto* pp = new juce::DynamicObject(); pp->setProperty ("level_db", 4.0); auto* w = new juce::DynamicObject(); w->setProperty ("params", juce::var (pp)); h.setSlotStructuredSettings (1, juce::var (w)); }
    // (1) a real window first, so the tallies are "measured", then SILENCE - the state the noon panel was in
    feedNoise (p, 600, 0.05f);
    h.setSlotTrimDb (1, 0.0f);
    h.resetAllLevels();
    feedSilence (p, 900);
    const auto lv = h.getSlotLevels (1);
    std::printf ("  silent slot reads: in %.1f LUFS-S / %.1f dBTP, out %.1f / %.1f, measured=%d\n",
                 lv.in.shortTermDb, lv.in.truePeakDb, lv.out.shortTermDb, lv.out.truePeakDb, (int) lv.measured);
    juce::StringArray lines;
    const int changed = h.measureUnityTrims (0, 2, &lines);
    // THE CLAIM IS ACCEPTANCE, not the number: on a silent window the old pass treats the floor as a measurement and
    // says so ("out-in 0.0 dB -> trim 0.0 dB"); the gated pass refuses it and names the reason. In this fixture both
    // arrive at a trim of 0.0, which is exactly why the leg reads the LINE - a wrong number is not the only harm, and
    // "it happened to be zero" is not a gate.
    const juce::String line = lines.joinIntoString (" | ");
    check (changed == 0 && line.contains ("NO READING"),
           "R1. a SILENT reading is REFUSED, not accepted as a measurement (the -245 hole)", "changed " + juce::String (changed) + " | " + line.substring (0, 140));
    // (4) the plugin's own rolling log
    EchoJay_NSLog ("EJGuard: gate21p marker");
    const char* iso = std::getenv ("ECHOJAY_STATE_HOME");
    const juce::File dir = juce::File (juce::String (iso != nullptr && *iso != 0 ? iso : std::getenv ("HOME"))).getChildFile ("Library").getChildFile ("EchoJay").getChildFile ("logs");
    const juce::File f = dir.getChildFile ("echojay-0.log");
    check (f.existsAsFile() && f.loadFileAsString().contains ("EJGuard: gate21p marker"),
           "R4. the plugin writes its own rolling log and the line is in it", f.getFullPathName() + (f.existsAsFile() ? " (" + juce::String (f.getSize()) + " B)" : " (absent)"));
    std::printf ("\n==== gate21p_red: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
