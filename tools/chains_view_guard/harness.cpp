// chains_view_guard (11 Oct 2026). Sean's leg: "render the CHAINS view over several frames and assert the
// header and list are drawn every frame."
//
// WHY FRAMES, PLURAL. The in-host symptom was a FLICKER, and a flicker is never visible in one frame. Two
// authorities disagreed about the chat furniture - resized() hid it for the chains list, and timerCallback()
// restored it on the next tick because its trigger was `!chatInput.isVisible()` with no chains-mode test - so
// the column's state depended on which ran last. A single-frame check passes on that build roughly half the
// time, which is worse than no check. This one interleaves layout, a REAL paint, and the timer, and asserts the
// claims on EVERY iteration.
//
// WHY PIXELS. "The header and the AI|CHAINS switch vanish" was the report, and the cause was that both painted
// inside `if (assistantVisible)` - a branch chatReplyControlsAllowed() makes false precisely when the chains
// list is showing. Asserting on rects alone would have passed: resized() authored them correctly the whole
// time, and nothing consumed them. So each frame is painted into an image CLEARED TO MAGENTA, and a region that
// is still magenta afterwards was not drawn by anybody. That is the one question a rect cannot answer.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "EedDeviceRegistry.h"
#include <cstdio>

struct EchoJayTabStripTestAccess
{
    static void toChain (EchoJayEditor& e) { e.switchToTab (EchoJayEditor::Tab::Chain, true); }
    static int  tab (EchoJayEditor& e)     { return (int) e.currentTab; }
    static void chainsMode (EchoJayEditor& e, bool on) { e.setChainSidebarMode (on); }
    static bool showing (EchoJayEditor& e) { return e.chainsSidebarShowing(); }
    static bool replyAllowed (EchoJayEditor& e) { return e.chatReplyControlsAllowed(); }
    static juce::Rectangle<int> aiRect (EchoJayEditor& e)     { return e.chainModeAiRect_; }
    static juce::Rectangle<int> chainsRect (EchoJayEditor& e) { return e.chainModeChainsRect_; }
    static juce::Rectangle<int> statusRect (EchoJayEditor& e) { return e.chainListStatusRect_; }
    static int  numRowRects (EchoJayEditor& e) { return (int) e.chainRowRects_.size(); }
    static int  numRows (EchoJayEditor& e)     { return (int) e.chainDisplayRows_.size(); }
    static bool inputVisible (EchoJayEditor& e) { return e.chatInput.isVisible(); }
    static bool aaVisible (EchoJayEditor& e)    { return e.chatTextSizeBtn.isVisible(); }
};
using A = EchoJayTabStripTestAccess;
namespace {
int failures = 0;
void check (bool ok, const juce::String& w, const juce::String& d = {})
{ std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
void pumpMs (double ms)
{ const double t0 = juce::Time::getMillisecondCounterHiRes();
  while (juce::Time::getMillisecondCounterHiRes() - t0 < ms)
  { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false); } }
const juce::Colour kUnpainted { 0xffff00ff };   // magenta: nobody's colour
// How much of a rect is still the clear colour. 1.0 = nothing drew here at all.
double unpaintedFraction (const juce::Image& img, juce::Rectangle<int> r)
{
    r = r.getIntersection (img.getBounds());
    if (r.isEmpty()) return 1.0;
    long long hits = 0, n = 0;
    for (int y = r.getY(); y < r.getBottom(); ++y)
        for (int x = r.getX(); x < r.getRight(); ++x)
        { ++n; if (img.getPixelAt (x, y) == kUnpainted) ++hits; }
    return n > 0 ? (double) hits / (double) n : 1.0;
}
}
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("ej_chainsview_" + juce::String (juce::Time::getMillisecondCounter()));
    tmp.createDirectory();
    setenv ("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);
    // Logged in, so the editor constructs the MAIN screen with real geometry rather than the login screen.
    // The endpoint is unroutable and the token is a fixture; nothing can leave this process.
    { auto af = tmp.getChildFile ("Library/Application Support/EchoJay/auth.json");
      af.getParentDirectory().createDirectory();
      af.replaceWithText ("{\"endpoint\":\"https://localhost.invalid\",\"token\":\"harness-token\","
                          "\"email\":\"ui@test.local\",\"tier\":\"pro\",\"tierLevel\":2,\"messageLimit\":999,"
                          "\"credits\":999,\"displayName\":\"UI\",\"messagesUsedToday\":0,"
                          "\"usageDate\":\"2026-10-11\",\"autoDialMode\":false,\"dialWritesBlocked\":false}"); }

    std::printf ("chains_view_guard: the CHAINS column over several frames (layout, a real paint, and the timer)\n");
    EchoJayProcessor proc; proc.prepareToPlay (48000.0, 512);
    std::unique_ptr<juce::AudioProcessorEditor> edBase (proc.createEditor());
    auto* ed = dynamic_cast<EchoJayEditor*> (edBase.get());
    if (ed == nullptr) { std::printf ("no EchoJayEditor\n"); return 2; }
    ed->setSize (2000, 1100);
    A::toChain (*ed); pumpMs (200);
    A::chainsMode (*ed, true); pumpMs (200);

    check (A::showing (*ed), "the CHAINS column is showing after the switch was pressed");
    check (! A::replyAllowed (*ed),
           "chatReplyControlsAllowed() says NO while CHAINS is showing - the two predicates agree");
    if (! A::showing (*ed))
    { std::printf ("\n==== chains_view_guard: RED (the column never entered chains mode) ====\n"); return 1; }

    const int kFrames = 8;
    int badHeader = 0, badSwitch = 0, badRects = 0, furnitureBack = 0, modeLost = 0;
    juce::Image img (juce::Image::ARGB, ed->getWidth(), ed->getHeight(), true);
    for (int f = 0; f < kFrames; ++f)
    {
        // THE TIMER RUNS BETWEEN FRAMES. This is the whole point: the flicker was a layout pass and a timer
        // disagreeing, so a frame is only evidence if a tick has had its chance in between.
        pumpMs (120);
        ed->resized();
        {
            juce::Graphics g (img);
            g.fillAll (kUnpainted);
            ed->paintEntireComponent (g, false);
        }
        const auto sw = A::chainsRect (*ed);
        const auto ai = A::aiRect (*ed);
        const auto st = A::statusRect (*ed);
        // The header STRIP is the 32 px band the column's title and switch share; taking it from the switch's
        // own rect avoids this guard re-deriving a geometry the layout already owns.
        const juce::Rectangle<int> headerBand = sw.isEmpty() ? juce::Rectangle<int>()
                                                             : sw.expanded (0, 6);
        const double uH = unpaintedFraction (img, headerBand);
        const double uS = unpaintedFraction (img, sw);
        if (! A::showing (*ed)) ++modeLost;
        if (sw.isEmpty() || ai.isEmpty() || st.isEmpty()) ++badRects;
        if (headerBand.isEmpty() || uH > 0.5) ++badHeader;
        if (sw.isEmpty() || uS > 0.5) ++badSwitch;
        if (A::inputVisible (*ed) || A::aaVisible (*ed)) ++furnitureBack;
        std::printf ("    frame %d: switch=%s unpainted header %.0f%% / switch %.0f%%  rows %d/%d  input=%d Aa=%d\n",
                     f, sw.toString().toRawUTF8(), uH * 100.0, uS * 100.0,
                     A::numRowRects (*ed), A::numRows (*ed),
                     (int) A::inputVisible (*ed), (int) A::aaVisible (*ed));
    }

    check (modeLost == 0, "the column stayed in CHAINS mode across every frame",
           juce::String (modeLost) + " of " + juce::String (kFrames) + " frame(s) lost the mode");
    check (badRects == 0, "the layout authored the switch rects and the list's status rect on every frame",
           juce::String (badRects) + " frame(s) missing a rect");
    check (badHeader == 0, "the header band was DRAWN on every frame (not left as the clear colour)",
           juce::String (badHeader) + " of " + juce::String (kFrames) + " frame(s) undrawn");
    check (badSwitch == 0, "the AI|CHAINS switch was DRAWN on every frame - the control that chooses the mode "
                           "cannot disappear when the mode is chosen",
           juce::String (badSwitch) + " of " + juce::String (kFrames) + " frame(s) undrawn");
    check (furnitureBack == 0,
           "no second authority put the chat input or the Aa button back while CHAINS was showing (the flicker)",
           juce::String (furnitureBack) + " of " + juce::String (kFrames) + " frame(s) had it back");
    check (A::numRowRects (*ed) == A::numRows (*ed),
           "every row the list model holds got a rect, so none is dropped before paint sees it",
           juce::String (A::numRowRects (*ed)) + " rect(s) for " + juce::String (A::numRows (*ed)) + " row(s)");

    std::printf ("\n==== chains_view_guard: %s (%d assertion(s) failed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
