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
    // FIVE SINCE THE REAL EDITOR (27 Oct 2026). The array this forwards is the product's own list and
    // it grew a member; this copy said four and the guard stopped COMPILING, which is the loudest a
    // stale test access can be and still cheap. The fifth is the real-editor switch, and legs (1b) and
    // (1c) below are what it is worth: a switch that must not be offered in an installed build.
    static std::array<juce::ToggleButton*, 5> toggles (EchoJayEditor& e) { return e.chainSuggestionToggles(); }
    static juce::ToggleButton& realEditor (EchoJayEditor& e) { return e.realEditorToggle; }
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
    // ---- COUNTED AGAINST WHAT THIS BUILD OFFERS (27 Oct 2026) ------------------------------------
    // The claim is "nothing the user can see is left unplaced". A toggle this build deliberately does
    // NOT offer - the real-editor switch outside dev mode - is not a counter-example to that, and
    // counting it would turn Sean's "hidden in installed builds" rule into a red. So the denominator
    // is the visible ones, and the hidden one gets its own two legs below rather than disappearing.
    int offered = 0;
    for (auto* t : tg) if (t != nullptr && t->isVisible()) ++offered;
    std::printf ("    toggles placed=%d of %d offered (list size %d)  last bottom=%d  \"YOUR PLUGINS\" painted at y=%d\n",
                 placed, offered, (int) tg.size(), lastBottom, hy);
    check (placed >= offered,
           "every toggle this build OFFERS was actually placed by resized()",
           juce::String (placed) + " placed, " + juce::String (offered) + " offered, "
               + juce::String ((int) tg.size()) + " in the list");
    check (hy >= 0, "Settings painted, so the heading recorded where it landed", juce::String (hy));
    check (hy >= lastBottom,
           "\"YOUR PLUGINS\" is painted BELOW the last toggle, not on top of it",
           "heading y=" + juce::String (hy) + " vs last toggle bottom=" + juce::String (lastBottom));
    // ...and it has not over-corrected past the control it heads.
    check (hy <= A::scanBtn (*ed).getY(),
           "the heading still sits above the scan button it heads (the step did not overshoot)",
           "heading y=" + juce::String (hy) + " vs scan button y=" + juce::String (A::scanBtn (*ed).getY()));

    // ---- (1b) THE REAL-EDITOR SWITCH IS NOT OFFERED IN AN INSTALLED BUILD (27 Oct 2026) ---------
    //
    // Sean's standing condition for the whole real-editor road: *"the switch stays default off and
    // hidden from Settings in installed builds"*. Until now nothing held that but the line of code
    // that does it (`realEditorToggle.setVisible (ChainHost::devModeActive())`), so a careless
    // addAndMakeVisible anywhere would have shipped the switch to him with no row going red.
    //
    // Both directions are available to this guard, which is why the leg is worth having: it reads
    // devModeActive() and asserts the visibility that the build must have EITHER way.
    {
        const bool dev = ChainHost::devModeActive();
        const bool shown = A::realEditor (*ed).isVisible();
        check (shown == dev,
               dev ? "dev mode is on in this build, so the real-editor switch IS offered in Settings"
                   : "the real-editor switch is NOT offered in Settings: this is not a dev build, and "
                     "an installed build must never show it (his standing condition)",
               "devModeActive=" + juce::String ((int) dev) + " visible=" + juce::String ((int) shown));
        // ---- (1c) ...AND IT IS OFF UNTIL SOMETHING TURNS IT ON --------------------------------
        // This harness runs in its own ECHOJAY_STATE_HOME, so the switch file cannot exist: what is
        // measured is the DEFAULT, which is the half of his condition the visibility leg does not
        // cover. A build that read the switch as on with no file would put every live view on the
        // real road for every user at once.
        check (! echojay::realEditorSwitchOn(),
               "with no switch file in this isolated state home, the real editor reads OFF - the "
               "default his condition asks for",
               "switchOn=" + juce::String ((int) echojay::realEditorSwitchOn()));
        check (! A::realEditor (*ed).getToggleState(),
               "...and the tick in Settings agrees with it, so the two cannot drift apart");
    }

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

    // ---- (3) "DON'T USE THIS ONE", STORED (his 11:5x item 3, 21 Oct 2026) -------------------
    //
    // *"The MLimiterX is an expired demo. Can the plugin tell (it passes audio, so silent-slot
    // detection won't)? If not, give Sean a way to mark a plugin "don't use", stored like the silent
    // mark, and send it so B leaves it out."*
    //
    // A host CANNOT tell - parameters and audio look the same licensed or expired, which is why the
    // silent-slot detector cannot reach this case. So the mark is his, and it has to SURVIVE: a mark
    // that forgets itself when he quits Logic is worse than no mark, because he will not make it
    // twice. These legs hold the store, its reason, and the reload.
    {
        std::printf ("  -- the \"don't use\" mark --\n");
        check (! api.isPluginMarkedDontUse ("MLimiterX"),
               "DU-A nothing is marked to begin with");
        check (api.markPluginDontUse ("MLimiterX", "don't use it again - expired demo"),
               "DU-B marking it returns true (it was not marked before)");
        check (api.isPluginMarkedDontUse ("MLimiterX") && api.isPluginMarkedDontUse ("mlimiterx"),
               "DU-C ...and it reads back, case-insensitively - he types it either way");
        check (! api.markPluginDontUse ("MLimiterX", "again"),
               "DU-D marking it twice returns FALSE, so the caller can say \"already marked\" "
               "instead of the wrong sentence");
        check (api.dontUseReasonFor ("MLimiterX").containsIgnoreCase ("expired demo"),
               "DU-E the reason is HIS words, kept verbatim for the model to show back",
               api.dontUseReasonFor ("MLimiterX"));
        check (api.getDontUsePlugins().contains ("MLimiterX"),
               "DU-F ...and it is in the list the inventory and the start body read");

        // THE RELOAD. saveSettings() has already run (markPluginDontUse persists), so a second API
        // reading the same private state root must see it. This is the leg that answers "stored".
        {
            EchoJayAPI fresh;
            check (fresh.isPluginMarkedDontUse ("MLimiterX"),
                   "DU-G RED-FIRST: a NEW API object reading the same settings file sees the mark - "
                   "it survives a restart, which is the whole of \"stored like the silent mark\"");
            check (fresh.dontUseReasonFor ("MLimiterX").containsIgnoreCase ("expired demo"),
                   "DU-H ...with the reason", fresh.dontUseReasonFor ("MLimiterX"));
        }

        check (api.allowPluginAgain ("mlimiterx"),
               "DU-I lifting it returns true, and takes the name as typed");
        check (! api.isPluginMarkedDontUse ("MLimiterX"),
               "DU-J ...and it is back in");
        check (! api.allowPluginAgain ("MLimiterX"),
               "DU-K lifting what is not marked returns FALSE - nothing to say it did");
        check (! api.markPluginDontUse ("   ", "nothing"),
               "DU-L a blank name marks nothing: a mark with no plugin would withhold nothing and "
               "read like a setting that failed");
    }

    std::printf ("\n==== settings_toggles_guard: %s (%d assertion(s) failed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
