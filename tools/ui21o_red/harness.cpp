// ui21o_red (23 Sep 2026): the BEHAVIOURAL known-bad for the 21o UI items, written against the SHIPPING surface only -
// it walks the editor's component tree and reads button texts, so it compiles on BOTH trees and the difference is
// behaviour, not API. Run by hand against each tree; not in the gate (a RED-by-construction leg cannot sit in a suite
// that must be GREEN - the same written exclusion as gain_staging_red.cpp).
//   pre-21o tree  -> RED: two TooltipWindows; the chain strip carries the undo/redo pair; the header pair sits in the
//                    tab row left of "Hide AI"'s neighbours; an "Apply changes" button stays VISIBLE on Settings.
//   21o tree      -> GREEN on all four.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cstdio>
#include <functional>
// both trees have these members and this friend grant, so the harness compiles against either
struct EchoJayTabStripTestAccess
{
    static void toTab (EchoJayEditor& e, int idx) { e.switchToTab ((EchoJayEditor::Tab) idx, true); }
    static juce::TextButton& applyBtn (EchoJayEditor& e, int i) { return e.editApplyBtns[(size_t) i]; }
};
using A = EchoJayTabStripTestAccess;
namespace {
int failures = 0; void check (bool ok, const juce::String& w, const juce::String& d = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
void pumpMs (double ms) { const double t0 = juce::Time::getMillisecondCounterHiRes(); while (juce::Time::getMillisecondCounterHiRes() - t0 < ms) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, false); } }
int countTooltips (juce::Component& c)
{ int n = 0; for (int i = 0; i < c.getNumChildComponents(); ++i) { auto* k = c.getChildComponent (i); if (dynamic_cast<juce::TooltipWindow*> (k) != nullptr) ++n; n += countTooltips (*k); } return n; }
void collectButtons (juce::Component& c, std::vector<juce::TextButton*>& out)
{ for (int i = 0; i < c.getNumChildComponents(); ++i) { auto* k = c.getChildComponent (i); if (auto* b = dynamic_cast<juce::TextButton*> (k)) out.push_back (b); collectButtons (*k, out); } }
juce::TextButton* byText (juce::Component& c, const juce::String& t)
{ std::vector<juce::TextButton*> all; collectButtons (c, all); for (auto* b : all) if (b->getButtonText() == t) return b; return nullptr; }
int countArrowButtons (juce::Component& c)
{ std::vector<juce::TextButton*> all; collectButtons (c, all); int n = 0;
  for (auto* b : all) if (b->getButtonText().containsAnyOf (juce::String::fromUTF8 ("\xe2\x86\xb6\xe2\x86\xb7"))) ++n; return n; }
}
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("ui21o_red: the four 21o UI claims, read off the shipping component tree\n");
    EchoJayProcessor proc; proc.prepareToPlay (48000.0, 512);
    std::unique_ptr<juce::AudioProcessorEditor> edBase (proc.createEditor());
    auto* ed = dynamic_cast<EchoJayEditor*> (edBase.get()); if (! ed) { std::printf ("no editor\n"); return 2; }
    ed->setSize (2000, 1100); pumpMs (120);
    A::toTab (*ed, 5); ed->resized(); pumpMs (60);   // the Link tab, laid out: the top bar's buttons are authored here
    // (1) ONE tooltip window in the tree
    const int tips = countTooltips (*ed);
    check (tips == 1, "R1. exactly ONE TooltipWindow in the editor tree (two = every hint drawn twice)", juce::String (tips));
    // (2) the chain strip carries no undo/redo of its own: count the arrow buttons anywhere BELOW the top bar
    {
        std::vector<juce::TextButton*> all; collectButtons (*ed, all);
        int belowHeader = 0, inHeader = 0;
        for (auto* b : all)
            if (b->getButtonText().containsAnyOf (juce::String::fromUTF8 ("\xe2\x86\xb6\xe2\x86\xb7")))
            {
                const auto p = ed->getLocalPoint (b->getParentComponent(), b->getBounds().getCentre());
                if (p.y <= 40) ++inHeader; else ++belowHeader;
            }
        check (belowHeader == 0, "R2. NO undo/redo arrow buttons below the header (the chain strip's pair is gone)", juce::String (belowHeader));
        check (inHeader == 2, "R2. ...and the header carries exactly the two", juce::String (inHeader));
        // the header GEOMETRY (the pills right of everything, left of "Hide AI") is asserted by ui_guard leg (12e) on a
        // fully exercised editor: this headless one never reaches the top-bar layout path, so its header rects are 0x0
        // and a comparison here would pass on zeros - a leg that cannot fail is not a leg.
    }
    // (4) a pending "Apply changes" must not be visible on the Settings tab (Tab::Chat = 3, Tab::Settings = 7)
    {
        A::toTab (*ed, 3);
        auto& apply = A::applyBtn (*ed, 0);
        apply.setButtonText ("Apply changes"); apply.setVisible (true);
        check (apply.isVisible(), "R4. a pending \"Apply changes\" is visible on the Chat tab", juce::String ((int) apply.isVisible()));
        A::toTab (*ed, 7); pumpMs (60);
        juce::ignoreUnused (ed->createComponentSnapshot (ed->getLocalBounds(), false, 1.0f));
        check (! apply.isVisible(), "R4. on the SETTINGS tab it is NOT visible", juce::String ((int) apply.isVisible()));
    }
    std::printf ("\n==== ui21o_red: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
