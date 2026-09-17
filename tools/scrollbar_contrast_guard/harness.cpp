// scrollbar_contrast_guard (17 Sep 2026, rides 17e): the horizontal scrollbars under the chain strip (Chain tab)
// and under the Link rack row (Link tab) must be visible. The REAL editor is created; each viewport's horizontal
// juce::ScrollBar is painted through the editor's LookAndFeel into an image with a range that yields a thumb, the
// thumb pixel is read back, and its contrast ratio against the panel background must be >= 3:1 (RED today: the
// thumb was Colours::bg4 at 0.4 alpha over a near-identical panel, ~1.05:1). Hover must be lighter than rest; the
// thumb no thinner than 8 px; both viewports share one thickness.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "EchoJayLookAndFeel.h"
#include <cstdio>
struct EchoJayTabStripTestAccess
{
    static juce::ScrollBar& stripBar (EchoJayEditor& e) { return e.chainListPanel.stripView.getHorizontalScrollBar(); }
    static juce::ScrollBar& linkBar (EchoJayEditor& e)  { return e.linkMixerViewport_.getHorizontalScrollBar(); }
    static int stripThickness (EchoJayEditor& e) { return e.chainListPanel.stripView.getScrollBarThickness(); }
    static int linkThickness (EchoJayEditor& e)  { return e.linkMixerViewport_.getScrollBarThickness(); }
};
namespace {
int failures = 0; void check (bool ok, const juce::String& w, const juce::String& d = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
double lum (juce::Colour c) { auto lin = [] (double v) { return v <= 0.03928 ? v / 12.92 : std::pow ((v + 0.055) / 1.055, 2.4); }; return 0.2126 * lin (c.getFloatRed()) + 0.7152 * lin (c.getFloatGreen()) + 0.0722 * lin (c.getFloatBlue()); }
double ratio (juce::Colour a, juce::Colour b) { const double la = lum (a), lb = lum (b); return (juce::jmax (la, lb) + 0.05) / (juce::jmin (la, lb) + 0.05); }
// Paint the REAL bar through the editor's LookAndFeel over the panel background; return the thumb's centre pixel
// and the thumb's measured thickness (rows of thumb colour at the thumb's centre column).
struct Sample { juce::Colour thumb; int thick = 0; juce::Colour track; };
Sample paintBar (juce::ScrollBar& bar, juce::Colour panel, bool hover)
{
    const int w = 400, h = juce::jmax (1, bar.getHeight());
    bar.setBounds (0, 0, w, h); bar.setRangeLimits (0.0, 1000.0); bar.setCurrentRange (0.0, 250.0);   // a 25% thumb
    juce::Image img (juce::Image::ARGB, w, h, true);
    juce::Graphics g (img); g.fillAll (panel);
    auto& lf = bar.getLookAndFeel();
    const double thumbStart = bar.getCurrentRangeStart() / bar.getMaximumRangeLimit() * w, thumbSize = bar.getCurrentRangeSize() / bar.getMaximumRangeLimit() * w;
    lf.drawScrollbar (g, bar, 0, 0, w, h, false, (int) thumbStart, (int) thumbSize, hover, false);
    Sample s; const int cx = (int) (thumbStart + thumbSize / 2), cy = h / 2;
    s.thumb = img.getPixelAt (cx, cy); s.track = img.getPixelAt (w - 4, cy);
    for (int y = 0; y < h; ++y) if (img.getPixelAt (cx, y) == s.thumb) ++s.thick;
    return s;
}
void pumpMs (double ms) { const double t0 = juce::Time::getMillisecondCounterHiRes(); while (juce::Time::getMillisecondCounterHiRes() - t0 < ms) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false); } }
}
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ej_scroll_" + juce::String (juce::Time::getMillisecondCounter()));
    tmp.createDirectory(); setenv ("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);
    { auto af = tmp.getChildFile ("Library/Application Support/EchoJay/auth.json"); af.getParentDirectory().createDirectory();
      af.replaceWithText ("{\"endpoint\":\"https://localhost.invalid\",\"token\":\"harness-token\",\"email\":\"ui@test.local\",\"tier\":\"pro\",\"tierLevel\":2,\"messageLimit\":999,\"credits\":999,\"displayName\":\"UI\",\"messagesUsedToday\":0,\"usageDate\":\"2026-09-17\",\"autoDialMode\":false,\"dialWritesBlocked\":false}"); }
    std::printf ("scrollbar_contrast_guard: the chain-strip and Link-rack horizontal scrollbars are visible (thumb >= 3:1 against the panel)\n");
    EchoJayProcessor proc; proc.prepareToPlay (48000.0, 512);
    std::unique_ptr<juce::AudioProcessorEditor> edBase (proc.createEditor());
    auto* ed = dynamic_cast<EchoJayEditor*> (edBase.get()); if (! ed) return 2;
    ed->setSize (2000, 1100); pumpMs (60);
    using A = EchoJayTabStripTestAccess;
    const juce::Colour panel = EchoJayLookAndFeel::Colours::bg3;   // the panel behind both bars
    struct Bar { const char* name; juce::ScrollBar* bar; int thickness; };
    Bar bars[] = { { "chain strip (Chain tab)", &A::stripBar (*ed), A::stripThickness (*ed) }, { "Link rack row (Link tab)", &A::linkBar (*ed), A::linkThickness (*ed) } };
    for (auto& b : bars)
    {
        b.bar->setSize (400, b.thickness);
        const auto rest = paintBar (*b.bar, panel, false), hover = paintBar (*b.bar, panel, true);
        const double cr = ratio (rest.thumb, panel), ch = ratio (hover.thumb, panel), ct = ratio (rest.track, panel);
        check (cr >= 3.0, juce::String (b.name) + ": thumb contrast vs the panel >= 3:1", "thumb " + rest.thumb.toString() + " panel " + panel.toString() + " ratio " + juce::String (cr, 2));
        check (lum (hover.thumb) > lum (rest.thumb), juce::String (b.name) + ": hover thumb is LIGHTER than rest", juce::String (lum (rest.thumb), 3) + " -> " + juce::String (lum (hover.thumb), 3));
        check (ct > 1.05, juce::String (b.name) + ": the track is a step lighter than the panel", "track " + rest.track.toString() + " ratio " + juce::String (ct, 2));
        check (rest.thick >= 8, juce::String (b.name) + ": thumb thickness >= 8 px", juce::String (rest.thick) + " px (bar " + juce::String (b.thickness) + " px)");
    }
    check (bars[0].thickness == bars[1].thickness && bars[0].thickness >= 8, "both viewports share ONE scrollbar thickness", juce::String (bars[0].thickness) + " / " + juce::String (bars[1].thickness));
    std::printf ("\n==== scrollbar_contrast_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
