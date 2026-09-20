// capture_width_guard (20 Sep 2026): a capture's mix-bus width is the live meter's width averaged over the window - on a STEREO
// fixture the two agree within 5 points; a DUAL-MONO fixture (left = right) reads ~0 % / 1.0 and is NAMED a mono input, never
// "narrow" (Sean's 19:10 BST capture: 0.0 % / 1.0 from identical channels).
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cstdio>
struct EchoJayTabStripTestAccess { static juce::String line (float w, float c, bool full) {
#ifdef EJ_CAPTURE_MONO_LINE
    return EchoJayEditor::stereoFlagLine (w, c, full);
#else
    juce::ignoreUnused (w, c, full); return {};
#endif
} };
namespace {
int failures = 0; void check (bool ok, const juce::String& w, const juce::String& d = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
void pumpMs (int ms) { const double end = juce::Time::getMillisecondCounterHiRes() + ms; while (juce::Time::getMillisecondCounterHiRes() < end) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false); } }
void feed (EchoJayProcessor& p, juce::Random& rng, int blocks, bool dualMono, float amp = 0.2f)
{
    juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
    for (int b = 0; b < blocks; ++b)
    {
        auto* l = buf.getWritePointer (0); auto* r = buf.getWritePointer (1);
        for (int i = 0; i < 512; ++i) { l[i] = (rng.nextFloat() * 2.0f - 1.0f) * amp; r[i] = dualMono ? l[i] : (rng.nextFloat() * 2.0f - 1.0f) * amp; }
        p.processBlock (buf, midi);
        if ((b % 8) == 7) pumpMs (1);
    }
}
float liveWidth (EchoJayProcessor& p) { return p.getMeterEngine().getMeterData().width; }
float liveCorr (EchoJayProcessor& p)  { return p.getMeterEngine().getMeterData().correlation; }
}
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ej_capwidth_" + juce::String (juce::Time::getMillisecondCounter())); tmp.createDirectory(); setenv ("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);
    std::printf ("capture_width_guard: capture width vs live width; a mono input is named\n");
    for (int fixture = 0; fixture < 2; ++fixture)
    {
        const bool dualMono = fixture == 1;
        std::printf ("== %s fixture ==\n", dualMono ? "DUAL-MONO (left = right)" : "STEREO (independent noise)");
        EchoJayProcessor p; p.prepareToPlay (48000.0, 512); juce::Random rng (99 + fixture);
        feed (p, rng, 400, dualMono);                       // ~4 s: the live meter settles
        const float wLiveBefore = liveWidth (p);
        p.startCapture(); feed (p, rng, 300, dualMono); p.stopCapture(); pumpMs (300);
        const float wLive = liveWidth (p), cLive = liveCorr (p);
        const auto snaps = p.getSnapshots();
        check (! snaps.empty(), "a capture snapshot exists", juce::String ((int) snaps.size()));
        if (snaps.empty()) continue;
        const auto& d = snaps.back().averagedData;
        std::printf ("  live width %.1f %% (before %.1f) corr %.2f | capture width %.1f %% corr %.2f\n", wLive, wLiveBefore, cLive, d.width, d.correlation);
        if (! dualMono)
        {
            check (wLive > 20.0f, "the stereo fixture reads a real width on the live meter (> 20 %)", juce::String (wLive, 1));
            check (std::abs (d.width - wLive) <= 5.0f, "the capture's width matches the live meter's within 5 points", juce::String (d.width, 1) + " vs " + juce::String (wLive, 1));
#ifdef EJ_CAPTURE_MONO_LINE
            check (! EchoJayTabStripTestAccess::line (d.width, d.correlation, true).contains ("MONO INPUT"), "...and its stereo line does not say mono", EchoJayTabStripTestAccess::line (d.width, d.correlation, true));
#else
            check (false, "...and its stereo line does not say mono", "no stereoFlagLine on this build");
#endif
        }
        else
        {
            check (d.width < 1.0f && d.correlation > 0.99f, "the dual-mono fixture captures ~0 % width / correlation ~1.0 (a real reading of identical channels)", juce::String (d.width, 1) + " / " + juce::String (d.correlation, 2));
#ifdef EJ_CAPTURE_MONO_LINE
            const auto line = EchoJayTabStripTestAccess::line (d.width, d.correlation, true);
            check (line.contains ("MONO INPUT") && ! line.contains ("narrow"), "...and the capture's stereo line names a MONO INPUT, never \"narrow\" (RED as it stood: \"Width 0.0% - narrow\")", line.trim());
#else
            check (false, "...and the capture's stereo line names a MONO INPUT, never \"narrow\" (RED as it stood: \"Width 0.0% - narrow\")", "as it stood the flags line read \"Stereo: Width " + juce::String (d.width, 1) + "% - narrow\"");
#endif
        }
    }
    std::printf ("\n==== capture_width_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
