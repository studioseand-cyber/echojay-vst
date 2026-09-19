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
    // 18f (loop pills + cleared composer)
    using Msg = EchoJayEditor::ChatMsg;   // the friend names the private type; the harness body uses A::Msg
    static std::vector<EchoJayEditor::ChatMsg>& msgs (EchoJayEditor& e) { return e.chatMessages; }
    static juce::TextEditor& input (EchoJayEditor& e) { return e.chatInput; }
    static bool verb (EchoJayEditor& e, const juce::String& t) { return e.handleLoudnessVerb (t); }
    static void send (EchoJayEditor& e, const juce::String& t) { e.sendChatMessage (t); }
    static void tapPill (EchoJayEditor& e, int msgIdx) { e.onResultChipTapped (msgIdx, 2); }
    static juce::StringArray chips (EchoJayEditor& e, const Msg& m) { juce::StringArray out; for (const auto& c : e.resultChipList (m)) out.add (c.label + "#" + juce::String (c.kind)); return out; }
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
    std::printf ("== 18f: loop bubbles carry their verbs as pills; a pill runs the typed handler; the composer is cleared; bubbles are history ==\n");
#ifdef EJ_LOUDNESSLOOP_PILLS
    {
        EchoJayProcessor proc; proc.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> edBase (proc.createEditor());
        auto* ed = dynamic_cast<EchoJayEditor*> (edBase.get()); if (! ed) return 2;
        ed->setSize (2000, 1100); A::toChat (*ed); pumpMs (60);
        auto& loop = proc.loudnessLoop(); juce::StringArray logs;
        // a built chain with a target arms the loop through the editor (armLoudnessLoopIfTargeted wires onBubble)
        A::build (*ed, "{\"chain\":[{\"name\":\"EchoJay Level\",\"role\":\"level\",\"settings_structured\":{\"params\":{\"gain_db\":0,\"target_lufs\":-9,\"loudness_option\":0}}},{\"name\":\"EchoJay Limiter\",\"role\":\"limiter\",\"settings_structured\":{\"params\":{\"ceiling_db\":-0.1}}}]}");
        pumpMs (2500);
        loop.logLine = [&] (const juce::String& l) { logs.add (l); };   // installed AFTER the arm (the editor wires NSLog at arm time; the capture must be the live hook)
        auto& M = A::msgs (*ed);
        auto lastLoop = [&] () -> const A::Msg* { for (int i = (int) M.size() - 1; i >= 0; --i) if (M[(size_t) i].role == "assistant" && (M[(size_t) i].content.startsWith ("Chain built. Play") || ! M[(size_t) i].loopPills.isEmpty() || M[(size_t) i].loopBubbleId > 0)) return &M[(size_t) i]; return nullptr; };
        check (loop.everArmed() && lastLoop() != nullptr && lastLoop()->content.startsWith ("Chain built. Play the loudest part"), "(4) the ARM bubble is shown after the build (before any Listening...)", lastLoop() ? lastLoop()->content.substring (0, 60) : "no loop bubble");
        // progress bubbles: two ticks -> ONE progress bubble; a proposal -> a NEW bubble, the progress bubble stays
        const size_t n0 = M.size();
        LoudnessLoop::Bubble pb; pb.kind = LoudnessLoop::Bubble::Kind::progress; pb.replace = true; pb.text = "Listening..."; pb.progress = 0.2f; loop.onBubble (pb);
        pb.progress = 0.5f; loop.onBubble (pb);
        check (M.size() == n0 + 1 && M.back().content == "Listening..." && M.back().loopBubbleId > 0, "(2) two progress ticks -> one progress bubble (replaced in place)", juce::String ((int) (M.size() - n0)));
        LoudnessLoop::Bubble prop; prop.kind = LoudnessLoop::Bubble::Kind::proposal; prop.text = "Measured -12.0 LUFS (loudest 3 s). Push +3.0 dB to reach -9.0? limiter working 1.0 dB average, up to 3.0 dB on the hits."; prop.pills = LoudnessLoop::proposalPills(); loop.onBubble (prop);
        check (M.size() == n0 + 2 && M[n0].content == "Listening..." && M[n0].loopBubbleId == 0 && M.back().content.startsWith ("Measured -12.0"), "(2) the proposal is a NEW bubble; the progress bubble stays as history (closed)", juce::String ((int) (M.size() - n0)));
        check (M.back().loopPills.joinIntoString ("|") == "Go|Leave it" && A::chips (*ed, M.back()).joinIntoString ("|") == "Go#2|Leave it#2", "(1) the proposal bubble renders [Go] [Leave it] as pills (kind 2)", A::chips (*ed, M.back()).joinIntoString ("|"));
        LoudnessLoop::Bubble res; res.kind = LoudnessLoop::Bubble::Kind::result; res.final = true; res.text = "Hitting -9.0 LUFS (loudest 3 s), target -9.0 - on target."; res.pills = LoudnessLoop::resultPills(); loop.onBubble (res);
        check (A::chips (*ed, M.back()).joinIntoString ("|") == "Undo#2|A bit louder#2|A bit softer#2|Push it#2", "(1) the result bubble renders [Undo] [A bit louder] [A bit softer] [Push it]", A::chips (*ed, M.back()).joinIntoString ("|"));
        LoudnessLoop::Bubble st; st.kind = LoudnessLoop::Bubble::Kind::stuck; st.text = "Stuck at -10.0 LUFS after 4 rounds"; st.pills = LoudnessLoop::stuckPills(); loop.onBubble (st);
        check (A::chips (*ed, M.back()).joinIntoString ("|") == "Push it#2|Leave it#2", "(1) the stuck bubble renders [Push it] [Leave it]", A::chips (*ed, M.back()).joinIntoString ("|"));
        LoudnessLoop::Bubble q; q.kind = LoudnessLoop::Bubble::Kind::quiet; q.text = "This is quieter than the section the chain was built on (-24.0 now vs -18.0 at build). Is this the loudest part of the song?"; q.pills = LoudnessLoop::quietPills(); loop.onBubble (q);
        check (A::chips (*ed, M.back()).joinIntoString ("|") == "Listen again#2|This is the loudest part#2", "(quiet) the quiet-window bubble renders [Listen again] [This is the loudest part]", A::chips (*ed, M.back()).joinIntoString ("|"));
        // typed verb: the composer is cleared, the handler ran (EJLoudness verb line), no chat send
        logs.clear(); A::input (*ed).setText ("go", juce::dontSendNotification); A::send (*ed, "go"); pumpMs (30);
        const bool typedLogged = logs.joinIntoString ("\n").contains ("verb \"go\"");
        check (A::input (*ed).getText().isEmpty(), "(1) the composer is CLEARED after a typed verb", "input=\"" + A::input (*ed).getText() + "\"");
        check (logs.joinIntoString ("\n").contains ("verb \"go\""), "(1) the typed word reached handleLoudnessVerb (EJLoudness verb line)", logs.joinIntoString (" | ").substring (0, 120));
        // pill: the same handler, the same EJLoudness sequence, the composer cleared
        juce::StringArray typedSeq = logs; logs.clear();
        A::input (*ed).setText ("stale text", juce::dontSendNotification);
        int propIdx = -1; for (int i = (int) M.size() - 1; i >= 0; --i) if (M[(size_t) i].loopPills.joinIntoString ("|") == "Go|Leave it") { propIdx = i; break; }
        check (propIdx >= 0, "the proposal bubble is findable for the pill tap");
        if (propIdx >= 0) A::tapPill (*ed, propIdx); pumpMs (30);
        juce::StringArray pillSeq = logs;
        auto verbLines = [] (const juce::StringArray& a) { juce::StringArray o; for (const auto& l : a) if (l.contains ("verb \"")) o.add (l.upToFirstOccurrenceOf (" state", false, false)); return o; };
        check (verbLines (pillSeq) == verbLines (typedSeq) && ! pillSeq.isEmpty(), "(1) a pill tap produces the SAME EJLoudness verb sequence as the typed word", verbLines (pillSeq).joinIntoString (" | ") + " vs " + verbLines (typedSeq).joinIntoString (" | "));
        check (A::input (*ed).getText().isEmpty(), "(1) the composer is CLEARED after a pill tap", "input=\"" + A::input (*ed).getText() + "\"");
        juce::ignoreUnused (typedLogged);
    }
#else
    for (const char* leg : { "(4) the ARM bubble is shown after the build (before any Listening...)", "(2) two progress ticks -> one progress bubble (replaced in place)", "(2) the proposal is a NEW bubble; the progress bubble stays as history (closed)", "(1) the proposal bubble renders [Go] [Leave it] as pills (kind 2)", "(1) the result bubble renders [Undo] [A bit louder] [A bit softer] [Push it]", "(1) the stuck bubble renders [Push it] [Leave it]", "(quiet) the quiet-window bubble renders [Listen again] [This is the loudest part]", "(1) the composer is CLEARED after a typed verb", "(1) a pill tap produces the SAME EJLoudness verb sequence as the typed word", "(1) the composer is CLEARED after a pill tap" })
        check (false, leg, "no loop pills on this build (18e)");
#endif
    std::printf ("\n==== ui_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
