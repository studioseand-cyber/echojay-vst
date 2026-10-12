// settings_toggles_guard (11 Oct 2026). Two Settings rulings from Sean's in-host pass, each with the leg he
// asked for.
//
//  (1) "YOUR PLUGINS" painted UNDER the fourth toggle. The paint cursor walked past three rows because the step
//      was the literal `2 * (fh + 8)` plus the one label() covers, and the agent toggle made four. Note where
//      the fault was: resized() placed all four correctly the entire time. So this leg asserts the HEADING'S
//      PAINTED Y against the toggles' real bounds - a leg on component geometry alone would have passed on the
//      broken build, and a leg that re-derived the step from the toggle count would pass by construction.
//      It is the second time this drifted (aa455ff added the do-not-dial row to resized() and not to paint()),
//      which is why the fix derives the step from the list both passes now share.
//
//  (2) "Only suggest plugins EchoJay can auto-dial" and "Suggest settings but never dial them" could both be
//      on. They contradict - narrow the list by a capability, then forbid using it - so they are now exclusive
//      in the SAVED STATE, which is where it has to hold: a UI-only rule leaves a settings file that re-creates
//      the pair on the next load. EchoJay-only is independent of both, by the same ruling, and this asserts
//      that it stays independent rather than being swept into the rule.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "EchoJayAPI.h"
#include <cstdio>

struct EchoJayTabStripTestAccess
{
    static void toSettings (EchoJayEditor& e) { e.switchToTab (EchoJayEditor::Tab::Settings, true); }
    static EchoJayAPI& api (EchoJayEditor& e) { return e.api; }
    static int headingY (EchoJayEditor& e) { return e.settingsPluginsHeadingY_; }
    static std::array<juce::ToggleButton*, 4> toggles (EchoJayEditor& e) { return e.chainSuggestionToggles(); }
    static juce::Component& scanBtn (EchoJayEditor& e) { return e.settingsScanBtn; }
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
}
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("ej_settoggle_" + juce::String (juce::Time::getMillisecondCounter()));
    tmp.createDirectory();
    setenv ("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);
    { auto af = tmp.getChildFile ("Library/Application Support/EchoJay/auth.json");
      af.getParentDirectory().createDirectory();
      af.replaceWithText ("{\"endpoint\":\"https://localhost.invalid\",\"token\":\"harness-token\","
                          "\"email\":\"ui@test.local\",\"tier\":\"pro\",\"tierLevel\":2,\"messageLimit\":999,"
                          "\"credits\":999,\"displayName\":\"UI\",\"messagesUsedToday\":0,"
                          "\"usageDate\":\"2026-10-11\",\"autoDialMode\":false,\"dialWritesBlocked\":false}"); }

    std::printf ("settings_toggles_guard: the YOUR PLUGINS heading clears the toggles; the two dial settings are exclusive\n");
    EchoJayProcessor proc; proc.prepareToPlay (48000.0, 512);
    std::unique_ptr<juce::AudioProcessorEditor> edBase (proc.createEditor());
    auto* ed = dynamic_cast<EchoJayEditor*> (edBase.get());
    if (ed == nullptr) { std::printf ("no EchoJayEditor\n"); return 2; }
    ed->setSize (2000, 1100);
    A::toSettings (*ed); pumpMs (400);
    ed->resized();
    // A real paint, because the number under test is one paint() produces.
    {
        juce::Image img (juce::Image::ARGB, ed->getWidth(), ed->getHeight(), true);
        juce::Graphics g (img);
        ed->paintEntireComponent (g, false);
    }

    // ---- (1) the heading clears every toggle ----
    const auto tg = A::toggles (*ed);
    int lastBottom = -1, placed = 0;
    for (auto* t : tg)
    {
        if (t == nullptr) continue;
        const auto b = t->getBounds();
        if (b.isEmpty()) continue;
        ++placed;
        lastBottom = juce::jmax (lastBottom, b.getBottom());
        std::printf ("    toggle %-22s bounds=%s\n",
                     t->getButtonText().substring (0, 20).toRawUTF8(), b.toString().toRawUTF8());
    }
    const int hy = A::headingY (*ed);
    std::printf ("    toggles placed=%d (list size %d)  last bottom=%d  \"YOUR PLUGINS\" painted at y=%d\n",
                 placed, (int) tg.size(), lastBottom, hy);
    check (placed == (int) tg.size(),
           "every toggle in the shared list was actually placed by resized()",
           juce::String (placed) + " of " + juce::String ((int) tg.size()));
    check (hy >= 0, "Settings painted, so the heading recorded where it landed", juce::String (hy));
    check (hy >= lastBottom,
           "\"YOUR PLUGINS\" is painted BELOW the last toggle, not on top of it",
           "heading y=" + juce::String (hy) + " vs last toggle bottom=" + juce::String (lastBottom));
    // ...and it has not over-corrected past the control it heads.
    check (hy <= A::scanBtn (*ed).getY(),
           "the heading still sits above the scan button it heads (the step did not overshoot)",
           "heading y=" + juce::String (hy) + " vs scan button y=" + juce::String (A::scanBtn (*ed).getY()));

    // ---- (2) the two dial settings are mutually exclusive, in the saved state ----
    auto& api = A::api (*ed);
    api.setEchoJayOnly (true);                      // independent: must survive everything below
    api.setAutoDialMode (true);
    check (api.getAutoDialMode() && ! api.getDialWritesBlocked(),
           "auto-dial on turns do-not-dial off",
           "autoDial=" + juce::String ((int) api.getAutoDialMode())
               + " dialWritesBlocked=" + juce::String ((int) api.getDialWritesBlocked()));
    api.setDialWritesBlocked (true);
    check (api.getDialWritesBlocked() && ! api.getAutoDialMode(),
           "do-not-dial on turns auto-dial off",
           "autoDial=" + juce::String ((int) api.getAutoDialMode())
               + " dialWritesBlocked=" + juce::String ((int) api.getDialWritesBlocked()));
    api.setAutoDialMode (true);
    check (api.getAutoDialMode() && ! api.getDialWritesBlocked(), "and back the other way");
    check (api.getEchoJayOnly(),
           "EchoJay-only is INDEPENDENT: it survived both exclusive flips untouched");
    check (! api.resolveDialExclusivity(),
           "the load-time tie-break is a no-op once the pair is already exclusive");
    // Turning one OFF must not turn the other on - exclusivity is not a radio pair.
    api.setAutoDialMode (false);
    check (! api.getAutoDialMode() && ! api.getDialWritesBlocked(),
           "both OFF is a legal state: turning one off does not switch the other on");

    std::printf ("\n==== settings_toggles_guard: %s (%d assertion(s) failed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
