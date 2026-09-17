// ui_round_guard/ui_guard — round C guards (17 Sep 2026) on the real editor.
// (2) "Build this chain" selects the Chain tab BEFORE the build starts, from
//     the button handler (own rack), synchronously  (RED today: the switch
//     happens at the END of the async load).
// (1) the chain panel is visible only when Chain is current, at every tick of
//     the build (never over the transcript).
// (3) the borrowed-rack status line says "when you leave this rack" ONCE
//     (BorrowStatusText.h helper; RED today: the helper does not exist).
// (4) the ask shelf / brief card never extends past the chat column's right
//     edge (AskShelfLayout.h helper; RED today: absent).
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#ifndef UI_GUARD_NO_HELPERS
#include "BorrowStatusText.h"
#include "AskShelfLayout.h"
#include "ChatBubbleStyle.h"    // hurdle 1 item 3
#include "NotDialableText.h"    // hurdle 1 item 3
#endif
#include <cstdio>
struct EchoJayTabStripTestAccess
{
    static void toChat (EchoJayEditor& e)   { e.switchToTab (EchoJayEditor::Tab::Chat, true); }
    static int  tab (EchoJayEditor& e)      { return (int) e.currentTab; }
    static bool panelVisible (EchoJayEditor& e) { return e.chainListPanel.isVisible(); }
    static int  chatTab()  { return (int) EchoJayEditor::Tab::Chat; }
    static int  chainTab() { return (int) EchoJayEditor::Tab::Chain; }
    static void build (EchoJayEditor& e, const juce::String& json) { e.loadChainFromJson (json, true); }
};
namespace {
int failures = 0; void check (bool ok, const juce::String& w, const juce::String& d = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
void pumpMs (double ms) { const double t0 = juce::Time::getMillisecondCounterHiRes(); while (juce::Time::getMillisecondCounterHiRes() - t0 < ms) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false); } }
}
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ej_uiguard_" + juce::String (juce::Time::getMillisecondCounter()));
    tmp.createDirectory(); setenv ("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);
    { auto af = tmp.getChildFile ("Library/Application Support/EchoJay/auth.json"); af.getParentDirectory().createDirectory();
      af.replaceWithText ("{\"endpoint\":\"https://localhost.invalid\",\"token\":\"harness-token\",\"email\":\"ui@test.local\",\"tier\":\"pro\",\"tierLevel\":2,\"messageLimit\":999,\"credits\":999,\"displayName\":\"UI\",\"messagesUsedToday\":0,\"usageDate\":\"2026-09-17\",\"autoDialMode\":false,\"dialWritesBlocked\":false}"); }
    using A = EchoJayTabStripTestAccess;
    std::printf ("== (2)(1) Build from the CHAT tab ==\n");
    {
        EchoJayProcessor proc; proc.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> edBase (proc.createEditor());
        auto* ed = dynamic_cast<EchoJayEditor*> (edBase.get()); if (! ed) return 2;
        ed->setSize (2000, 1100); A::toChat (*ed); pumpMs (60);
        check (A::tab (*ed) == A::chatTab(), "Chat tab current before Build");
        A::build (*ed, "{\"chain\":[{\"name\":\"EchoJay EQ\",\"role\":\"eq\",\"settings\":\"\"}]}");   // the own-rack arm the Build button takes
        check (A::tab (*ed) == A::chainTab(), "(2) Chain tab selected SYNCHRONOUSLY when the build starts", "tab=" + juce::String (A::tab (*ed)));
        bool panelOverChat = false;
        for (int k = 0; k < 20; ++k) { pumpMs (30); if (A::panelVisible (*ed) && A::tab (*ed) != A::chainTab()) panelOverChat = true; }
        check (! panelOverChat, "(1) the chain panel is never visible while a tab other than Chain is current");
        check (A::tab (*ed) == A::chainTab() && A::panelVisible (*ed), "after the build: Chain current, panel visible");
    }
#ifndef UI_GUARD_NO_HELPERS
    std::printf ("== (3) the borrowed-rack status line ==\n");
    for (int inCtx = 0; inCtx < 2; ++inCtx) for (int cap = 0; cap < 2; ++cap) for (int kept = 0; kept < 2; ++kept)
    {
        const juce::String s = borrowStatusSentence ("Nafe Lead Vocal", inCtx != 0, cap != 0, kept != 0, {});
        int n = 0; for (int at = 0; (at = s.indexOf (at, "when you leave this rack")) >= 0; ++at) ++n;
        check (n == 1, "inCtx=" + juce::String (inCtx) + " ctxCapable=" + juce::String (cap) + " restoredKept=" + juce::String (kept) + " -> exactly one 'when you leave this rack'", s);
    }
    std::printf ("== (4) the ask shelf never passes the chat column's right edge ==\n");
    {
        const juce::Rectangle<int> chatBox { 218, 990, 1774, 86 }, chatCol { 242, 99, 1373, 883 };   // the narrowed column (right edge 1615)
        const auto r = askShelfBounds (chatBox, chatCol, 200);
        check (r.getRight() <= chatCol.getRight(), "shelf right edge <= chat column right edge (1615)", r.toString());
        check (r.getX() == chatBox.getX() && r.getHeight() == 200 && r.getBottom() == chatBox.getY(), "shelf keeps x, height and sits on the composer", r.toString());
        const juce::Rectangle<int> wide { 242, 99, 1756, 883 };
        check (askShelfBounds (chatBox, wide, 200).getWidth() == chatBox.getWidth(), "with a full-width column the shelf keeps the composer width");
    }
#else
    std::printf ("== (3)(4) helper legs: ABSENT in this build (UI_GUARD_NO_HELPERS: BorrowStatusText.h / AskShelfLayout.h do not exist yet) ==\n");
    check (false, "helpers compiled in");
#endif
    std::printf ("== hurdle 1 item 3: the NOT DIALABLE report paints coral, names the built-in by role ==\n");
#ifndef UI_GUARD_NO_HELPERS
    {
        const juce::Colour userC (0xffffffff), asstC (0xffa0a0b8);
        check (chatBubbleTextColour (false, true, userC, asstC) == chatBubbleWarningColour(), "a dial-warning assistant bubble paints CORAL (0xfff87171)", chatBubbleTextColour (false, true, userC, asstC).toString());
        check (chatBubbleTextColour (false, false, userC, asstC) == asstC, "an ordinary assistant bubble keeps the assistant grey");
        check (chatBubbleTextColour (true, true, userC, asstC) == userC, "a user bubble is never coral");
        check (builtinAlternativeForRole ("pitch") == "EchoJay Pitch", "role pitch -> EchoJay Pitch");
        check (builtinAlternativeForRole ("reverb") == "EchoJay Reverb" && builtinAlternativeForRole ("compressor") == "EchoJay Compressor" && builtinAlternativeForRole ("eq") == "EchoJay EQ" && builtinAlternativeForRole ("de-esser") == "EchoJay De-Esser", "reverb/compressor/eq/de-esser -> the matching built-ins");
        const auto sentence = notDialableSentence ("Auto-Tune Pro", "no map for fp, no near map", "EchoJay Pitch");
        check (sentence.contains ("Auto-Tune Pro is NOT DIALABLE (no map for fp, no near map)") && sentence.contains ("EchoJay Pitch"), "the sentence says NOT DIALABLE in those words and names the built-in", sentence);
    }
#else
    std::printf ("  (helpers absent in this build - RED by construction)\n"); ++failures;
#endif
    std::printf ("\n==== ui_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
