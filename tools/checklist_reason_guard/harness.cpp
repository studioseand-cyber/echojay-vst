// checklist_reason_guard (18 Sep 2026): the Settings disabled list shows WHY a plugin is disabled and lets the user
// CLEAR a hangs-on-load mark. On the REAL PluginChecklistComponent (with the real scanner): a uid with a
// hangs-on-load mark -> the row text carries the detail and the expiry; the row is clearable; clearMark lifts the
// reasons entry and re-enables the uid on the scanner; a settings/load-failure reason is shown but not clearable.
// RED today: the component has no reason text and no Clear action.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginScanner.h"
#include "PluginChecklist.h"
#include "EJDisableReasons.h"
#include <cstdio>
namespace { int failures = 0; void check (bool ok, const juce::String& w, const juce::String& d = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; } }
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ej_checklist_" + juce::String (juce::Time::getMillisecondCounter()));
    tmp.createDirectory(); setenv ("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);
    echojay::userAppData().getChildFile ("EchoJay").createDirectory();
    std::printf ("checklist_reason_guard: the disabled list shows the reason and can clear a hangs-on-load mark\n");
#ifdef EJ_GUARD_TODAY
    check (false, "the row text carries the hangs-on-load reason", "TODAY: PluginChecklistComponent draws the name only; no reason, no Clear");
#else
    PluginScanner scanner;
    PluginChecklistComponent list (scanner);
    const juce::String uid = "avox_sybil_antares", uid2 = "some_plugin_vendor";
    echojay::recordDisableReasons ({ uid }, echojay::kDisableWhyHangsOnLoad, "hangs on load (Rosetta static initialisers, 17 Sep sample)");
    echojay::recordDisableReasons ({ uid2 }, echojay::kDisableWhyLoadFailure);
    scanner.setPluginEnabled (uid, false);
    const auto text = list.rowReasonText (uid);
    // 24 Sep 2026: the expiry is a DATE COMPUTED FROM TODAY, and this leg asserted the literal month "2026-09-".
    // A 14-day window from 24 September lands in OCTOBER, so the leg started failing on the calendar rather than
    // on the code. It now asserts the SHAPE of the date, which is what the row actually promises.
    const bool expiryShape = text.contains ("re-checked after 20") && text.length() > 20
                             && text.fromFirstOccurrenceOf ("re-checked after ", false, false).length() >= 10;
    check (text.contains ("hangs on load (Rosetta static initialisers, 17 Sep sample)") && expiryShape,
           "the row text carries the hangs-on-load reason and its expiry date", text);
    check (list.rowMarkClearable (uid), "a hangs-on-load mark is clearable");
    check (! list.rowMarkClearable (uid2) && list.rowReasonText (uid2) == "load-failure", "a load-failure reason is shown but NOT clearable", list.rowReasonText (uid2));
    list.clearMark (uid);
    check (echojay::disableReasonFor (uid).isEmpty() && list.rowReasonText (uid).isEmpty(), "Clear lifts the mark: no reason remains");
    check (! list.rowMarkClearable (uid), "...and the row is no longer clearable");
    const auto src = juce::File ("/Users/SeanD/echojay-vst/Source/PluginChecklist.cpp").loadFileAsString();
    check (src.contains ("rowReasonText(e.uid)") && src.contains ("g.drawText(\"Clear\"") && src.contains ("clearPillBounds(ln, getWidth()).contains(ev.x, ev.y)"), "(pin) paint draws the reason and the Clear pill; mouseDown on the pill clears");
#endif
    std::printf ("\n==== checklist_reason_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
