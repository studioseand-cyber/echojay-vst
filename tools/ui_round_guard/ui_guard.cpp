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
struct EchoJayAlignTestAccess { static void setLinks (EchoJayProcessor& p, std::vector<EchoJayProcessor::LinkSlotInfo> v) { p.linkSlotInfos = std::move (v); } };   // 21m ruling 1: the roster's Link rows
struct EchoJayRosterTestAccess { static EchoJayEditor::LastLinkActiveCmd last (EchoJayEditor& e) { return e.lastLinkActiveCmd_; } };
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
    static auto& panel (EchoJayEditor& e) { return e.chainListPanel; }        // 22 Sep 2026 (item 7)
    static void picker (EchoJayEditor& e) { e.showChainPluginPicker(); }     // 22 Sep 2026 (item 7)
    static bool forcedVerb (EchoJayEditor& e, const juce::String& t) { return e.handleLoudnessVerb (t, true); }   // 22 Sep 2026 (item 2 client half): the server's loop_verb
    static void send (EchoJayEditor& e, const juce::String& t) { e.sendChatMessage (t); }
    static void refreshPanel (EchoJayEditor& e) { e.refreshChainPanelForView (true); }   // 21m ruling 3 (keep-level grey-out)
    using Block = EchoJayEditor::ChainListPanel::Block;   // the friend names the private nested type (as Msg)
    static void tapPill (EchoJayEditor& e, int msgIdx) { e.onResultChipTapped (msgIdx, 2); }
    static juce::StringArray chips (EchoJayEditor& e, const Msg& m) { juce::StringArray out; for (const auto& c : e.resultChipList (m)) out.add (c.label + "#" + juce::String (c.kind)); return out; }
    // 18h (1): the chip layout at a given width, the row count, the on-screen chip buttons
    static std::vector<juce::Rectangle<int>> layout (EchoJayEditor& e, const Msg& m, int w) { std::vector<juce::Rectangle<int>> r; e.layoutResultChips (m, { 0, 0, w, 26 }, r); return r; }
#ifdef EJ_LOUDNESSLOOP_VERBS18H
    static int rows (EchoJayEditor& e, const Msg& m, int w) { return e.chipRows (m, w); }
#endif
    static juce::StringArray visibleChips (EchoJayEditor& e) { juce::StringArray o; for (auto& b : e.resultChipBtns) if (b.isVisible()) o.add (b.getButtonText() + "@" + juce::String (b.getX()) + "," + juce::String (b.getY()) + " " + juce::String (b.getWidth()) + "x" + juce::String (b.getHeight())); return o; }
    static juce::Rectangle<int> scrollBounds (EchoJayEditor& e) { return e.chatScroll.getBounds(); }
    // 18g (5): the ONE build-bubble composer, on the host's dial infos
    static juce::String compose (EchoJayEditor& e, ChainHost& ch, const juce::String& json) { return e.composeBuildBubble (ch, json).text; }
};
struct EchoJayBorrowHostTestAccess
{
#ifdef EJ_LOUDNESSLOOP_MANNERS
    // 18g (5): record an apply REPORT on a slot exactly as the build does (ChainHost::recordApplyReport), so the bubble the
    // composer writes from it can be read
    static void record (ChainHost& h, int i, std::vector<ChainHost::ApplyReport>& report) { h.recordApplyReport (i, juce::var(), report); }
#endif
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
        check (loop.everArmed() && lastLoop() != nullptr && (lastLoop()->content.startsWith ("Chain built. Play the loudest part") || lastLoop()->content.startsWith ("Cue the loudest section")), "(4) the ARM bubble is shown after the build (before any Listening...)", lastLoop() ? lastLoop()->content.substring (0, 60) : "no loop bubble");
        // progress bubbles: two ticks -> ONE progress bubble; a proposal -> a NEW bubble, the progress bubble stays
        const size_t n0 = M.size();
        LoudnessLoop::Bubble pb; pb.kind = LoudnessLoop::Bubble::Kind::progress; pb.replace = true; pb.text = "Listening..."; pb.progress = 0.2f; loop.onBubble (pb);
        pb.progress = 0.5f; loop.onBubble (pb);
        check (M.size() == n0 + 1 && M.back().content == "Listening..." && M.back().loopBubbleId > 0, "(2) two progress ticks -> one progress bubble (replaced in place)", juce::String ((int) (M.size() - n0)));
        LoudnessLoop::Bubble prop; prop.kind = LoudnessLoop::Bubble::Kind::proposal; prop.text = "Measured -12.0 LUFS (loudest 3 s). Push +3.0 dB to reach -9.0? limiter working 1.0 dB average, up to 3.0 dB on the hits."; prop.pills = LoudnessLoop::proposalPills(); loop.onBubble (prop);
        check (M.size() == n0 + 2 && M[n0].content == "Listening..." && M[n0].loopBubbleId == 0 && M.back().content.startsWith ("Measured -12.0"), "(2) the proposal is a NEW bubble; the progress bubble stays as history (closed)", juce::String ((int) (M.size() - n0)));
        check (M.back().loopPills.joinIntoString ("|") == "Go|Leave it" && A::chips (*ed, M.back()).joinIntoString ("|") == "Go#2|Leave it#2", "(1) the proposal bubble renders [Go] [Leave it] as pills (kind 2)", A::chips (*ed, M.back()).joinIntoString ("|"));
        LoudnessLoop::Bubble res; res.kind = LoudnessLoop::Bubble::Kind::result; res.final = true; res.text = "Hitting -9.0 LUFS (loudest 3 s), target -9.0 - on target."; res.pills = LoudnessLoop::resultPills(); loop.onBubble (res);
        check (A::chips (*ed, M.back()).joinIntoString ("|").startsWith ("Undo#2|A bit louder#2|A bit softer#2"), "(1) the result bubble renders [Undo] [A bit louder] [A bit softer] (+ Done since 18g; Push it only when short since 18h)", A::chips (*ed, M.back()).joinIntoString ("|"));
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

    std::printf ("== 18g: Listen / Check / Done pills; the arm bubble asks for Listen; a duplicate-ceiling fixture never says \"needs hand-dialing\" ==\n");
#ifdef EJ_LOUDNESSLOOP_MANNERS
    {
        EchoJayProcessor proc; proc.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> edBase (proc.createEditor());
        auto* ed = dynamic_cast<EchoJayEditor*> (edBase.get()); if (! ed) return 2;
        ed->setSize (2000, 1100); A::toChat (*ed); pumpMs (60);
        auto& loop = proc.loudnessLoop(); juce::StringArray logs;
        A::build (*ed, "{\"chain\":[{\"name\":\"EchoJay Level\",\"role\":\"level\",\"settings_structured\":{\"params\":{\"gain_db\":0,\"target_lufs\":-9,\"loudness_option\":0}}},{\"name\":\"EchoJay Limiter\",\"role\":\"limiter\",\"settings_structured\":{\"params\":{\"ceiling_db\":-0.1,\"true_peak\":1}}}]}");
        pumpMs (2500);
        loop.logLine = [&] (const juce::String& l) { logs.add (l); };
        auto& M = A::msgs (*ed);
        int armIdx = -1; for (int i = (int) M.size() - 1; i >= 0; --i) if (M[(size_t) i].role == "assistant" && M[(size_t) i].content.startsWith ("Cue the loudest section")) { armIdx = i; break; }
        check (loop.everArmed() && loop.state() == LoudnessLoop::State::armed && armIdx >= 0 && A::chips (*ed, M[(size_t) armIdx]).joinIntoString ("|") == "Listen#2", "18g (1) the arm bubble reads \"Cue the loudest section, press play, then tap Listen\" with the [Listen] pill; no window runs yet", armIdx >= 0 ? A::chips (*ed, M[(size_t) armIdx]).joinIntoString ("|") : juce::String ("no arm bubble; state ") + juce::String ((int) loop.state()));
        // [Listen] runs the same handler as the typed word and starts the window
        A::input (*ed).setText ("stale", juce::dontSendNotification);
        if (armIdx >= 0) A::tapPill (*ed, armIdx); pumpMs (30);
        check (logs.joinIntoString ("\n").contains ("verb \"listen\"") && loop.state() == LoudnessLoop::State::waitAudio && A::input (*ed).getText().isEmpty(), "18g (1) tapping [Listen] -> verb \"listen\" -> the window starts (state waitAudio), composer cleared", "state " + juce::String ((int) loop.state()));
        // the Check and Done pills render, and Done reaches the handler
        LoudnessLoop::Bubble ck; ck.kind = LoudnessLoop::Bubble::Kind::info; ck.text = "Tap Check when the loud part is playing."; ck.pills = LoudnessLoop::checkPills(); loop.onBubble (ck);
        check (A::chips (*ed, M.back()).joinIntoString ("|") == "Check#2", "18g (1) the Check bubble renders [Check]", A::chips (*ed, M.back()).joinIntoString ("|"));
        LoudnessLoop::Bubble res; res.kind = LoudnessLoop::Bubble::Kind::result; res.final = true; res.text = "Hitting -9.0 LUFS (loudest 3 s), target -9.0 - on target."; res.pills = LoudnessLoop::resultPills(); loop.onBubble (res);
        check (A::chips (*ed, M.back()).joinIntoString ("|") == "Undo#2|A bit louder#2|A bit softer#2|Done#2", "18g (3) the result bubble renders [Undo] [A bit louder] [A bit softer] [Done] (Push it only when short, 18h)", A::chips (*ed, M.back()).joinIntoString ("|"));
        logs.clear(); A::send (*ed, "done"); pumpMs (30);
        check (logs.joinIntoString ("\n").contains ("verb \"done\"") && loop.state() == LoudnessLoop::State::hold, "18g (3) typed \"done\" reaches the handler and ends the watch (state hold)", "state " + juce::String ((int) loop.state()));
        LoudnessLoop::Bubble bo; bo.kind = LoudnessLoop::Bubble::Kind::backoff; bo.text = "That section is louder - back off -1.4 dB?"; bo.pills = LoudnessLoop::backoffPills(); loop.onBubble (bo);
        check (A::chips (*ed, M.back()).joinIntoString ("|") == "Back off#2|Leave it#2", "18g (3) the watch's back-off bubble renders [Back off] [Leave it]", A::chips (*ed, M.back()).joinIntoString ("|"));
        // (5) BUBBLE TRUTH: the 20 Sep fixture - controls{Ceiling, Limiter Mode} applied, a flat ceiling_db beside them with no mapping
        auto& ch = proc.getChainHost();
        { auto* co = new juce::DynamicObject(); co->setProperty ("Ceiling", -0.1); co->setProperty ("Limiter Mode", "Modern");
          auto* pp = new juce::DynamicObject(); pp->setProperty ("ceiling_db", -0.1); pp->setProperty ("true_peak", 1);
          auto* w = new juce::DynamicObject(); w->setProperty ("params", juce::var (pp)); w->setProperty ("controls", juce::var (co)); w->setProperty ("ceiling_db", -0.1);
          ch.setSlotStructuredSettings (1, juce::var (w)); }
        std::vector<ChainHost::ApplyReport> report;
        { ChainHost::ApplyReport r; r.semantic = "ceiling_db"; r.applied = false; r.normalized = 0.0f; r.note = "no mapping for this control on this plugin"; report.push_back (r); }
        { ChainHost::ApplyReport r; r.semantic = "Ceiling"; r.applied = true; r.normalized = 0.945f; r.requestedValue = -0.1; r.note = "applied (display unverifiable on this plugin)"; r.landedText = "-0.09 dB"; report.push_back (r); }
        { ChainHost::ApplyReport r; r.semantic = "Limiter Mode"; r.applied = true; r.normalized = 1.0f; r.requestedValue = "Modern"; r.note = "applied, reads \"Modern\""; r.landedText = "Modern"; report.push_back (r); }
        EchoJayBorrowHostTestAccess::record (ch, 1, report);
        const auto infos = ch.getDialInfos();
        check (infos.size() >= 2 && infos[1].status == ChainHost::DialStatus::applied && infos[1].manual.isEmpty() && infos[1].applied.joinIntoString ("|") == "Ceiling|Limiter Mode", "18g (5) the duplicate flat ceiling_db collapses to the APPLIED Ceiling: status applied, nothing manual, applied = Ceiling | Limiter Mode", infos.size() >= 2 ? "status " + juce::String ((int) infos[1].status) + " manual [" + infos[1].manual.joinIntoString ("|") + "] applied [" + infos[1].applied.joinIntoString ("|") + "]" : juce::String ("no infos"));
        const auto bubble = A::compose (*ed, ch, "{\"chain\":[{\"name\":\"EchoJay Level\",\"role\":\"level\"},{\"name\":\"EchoJay Limiter\",\"role\":\"limiter\"}]}");
        check (! bubble.contains ("hand-dialing") && bubble.startsWith ("Chain built"), "18g (5) the build bubble from that readback never says \"needs hand-dialing\" (a clean build line)", bubble.substring (0, 160));
    }
#else
    for (const char* leg : { "18g (1) the arm bubble reads \"Cue the loudest section, press play, then tap Listen\" with the [Listen] pill; no window runs yet", "18g (1) tapping [Listen] -> verb \"listen\" -> the window starts (state waitAudio), composer cleared",
                             "18g (1) the Check bubble renders [Check]", "18g (3) the result bubble renders [Undo] [A bit louder] [A bit softer] [Push it] [Done]", "18g (3) typed \"done\" reaches the handler and ends the watch (state hold)",
                             "18g (3) the watch's back-off bubble renders [Back off] [Leave it]", "18g (5) the duplicate flat ceiling_db collapses to the APPLIED Ceiling: status applied, nothing manual, applied = Ceiling | Limiter Mode", "18g (5) the build bubble from that readback never says \"needs hand-dialing\"" })
        check (false, leg, "no 18g on this build");
#endif

    std::printf ("== 18h: pills wrap (420 / 1200 px); no stray panel after a pill in the Chat tab; Push it only when short; the after-verb bubble; Check reports ==\n");
#ifdef EJ_LOUDNESSLOOP_VERBS18H
    {
        EchoJayProcessor proc; proc.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> edBase (proc.createEditor());
        auto* ed = dynamic_cast<EchoJayEditor*> (edBase.get()); if (! ed) return 2;
        ed->setSize (1400, 900); ed->setVisible (true); A::toChat (*ed); pumpMs (60);
        auto& loop = proc.loudnessLoop(); juce::StringArray logs;
        A::build (*ed, "{\"chain\":[{\"name\":\"EchoJay Level\",\"role\":\"level\",\"settings_structured\":{\"params\":{\"gain_db\":0,\"target_lufs\":-9,\"loudness_option\":0}}},{\"name\":\"EchoJay Limiter\",\"role\":\"limiter\",\"settings_structured\":{\"params\":{\"ceiling_db\":-0.1,\"true_peak\":1}}}]}");
        pumpMs (2500); A::toChat (*ed); pumpMs (100);
        loop.logLine = [&] (const juce::String& l) { logs.add (l); };
        auto& M = A::msgs (*ed);
        // (1) the layout: five pills, then [Go] [Leave it], at 420 and 1200 px - every chip inside the width, natural size, rows counted
        A::Msg five; five.role = "assistant"; five.content = "x"; five.loopPills = { "Check", "A bit louder", "A bit softer", "Undo", "Done" };
        A::Msg two;  two.role = "assistant";  two.content = "x";  two.loopPills = LoudnessLoop::proposalPills();
        A::Msg three; three.role = "assistant"; three.content = "x";
#ifdef EJ_LOUDNESSLOOP_MANNERS21
        three.loopPills = LoudnessLoop::proposalAfterApplyPills();   // 21 Sep: [Go] [Leave it] [Undo] on a proposal that follows an apply
#else
        three.loopPills = { "Go", "Leave it", "Undo" };
#endif
        for (int w : { 420, 1200 })
            for (const auto* m : { &five, &two, &three })
            {
                const auto rects = A::layout (*ed, *m, w); bool inside = true, natural = true; int maxRight = 0;
                for (size_t i = 0; i < rects.size(); ++i) { inside = inside && rects[i].getX() >= 0 && rects[i].getRight() <= w; natural = natural && rects[i].getWidth() <= 140 && rects[i].getWidth() >= 40; maxRight = juce::jmax (maxRight, rects[i].getRight()); }
                const int rows = A::rows (*ed, *m, w);
                check (rects.size() == (size_t) m->loopPills.size() && inside && natural && rows >= 1 && rows == (rects.back().getY() / 32) + 1 && (w == 1200 ? rows == 1 : true), "18h (1) " + juce::String ((int) m->loopPills.size()) + " pills at " + juce::String (w) + " px: every chip inside the width at its natural size (40-140 px), rows = " + juce::String (rows), "rows " + juce::String (rows) + " maxRight " + juce::String (maxRight) + " widths " + [&] { juce::StringArray o; for (auto& r : rects) o.add (juce::String (r.getWidth())); return o.joinIntoString ("/"); }());
            }
        check (A::layout (*ed, two, 1200)[0].getWidth() <= 80, "18h (2) [Go] is a 40-80 px pill, not the bubble's width (the 1,130 px bar in the Chat tab)", juce::String (A::layout (*ed, two, 1200)[0].getWidth()) + " px");
        // a real proposal in the Chat tab: Listen, audio, the proposal bubble - then every visible chip button is inside the transcript and narrow
        loop.listen(); { juce::Random rng (7); juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi; int b = 0;
          while (loop.state() != LoudnessLoop::State::proposed && loop.state() != LoudnessLoop::State::tracking && b < 6000) { for (int ch = 0; ch < 2; ++ch) { auto* d = buf.getWritePointer (ch); for (int i = 0; i < 512; ++i) d[i] = (rng.nextFloat() * 2.0f - 1.0f) * 0.1f; } proc.processBlock (buf, midi); if ((++b % 23) == 0) loop.tickNow(); } }
        pumpMs (300); ed->repaint(); pumpMs (100);
        // the chip pool is POSITIONED by the transcript's paint pass; a headless editor never paints, so paint it into an image first
        auto paintOnce = [&] { juce::ignoreUnused (ed->createComponentSnapshot (ed->getLocalBounds(), false, 1.0f)); };
        paintOnce();
        check (loop.state() == LoudnessLoop::State::proposed && M.back().loopPills.joinIntoString ("|") == "Go|Leave it", "18h (2) a real proposal in the Chat tab", M.back().content.substring (0, 80));
        auto chipsOk = [&] (const char* leg) { const auto vis = A::visibleChips (*ed); const auto sb = A::scrollBounds (*ed); bool ok = ! vis.isEmpty(); int wide = 0;
            for (const auto& v : vis) { const auto geo = v.fromFirstOccurrenceOf ("@", false, false); const int x = geo.upToFirstOccurrenceOf (",", false, false).getIntValue(); const int w = geo.fromFirstOccurrenceOf (" ", false, false).upToFirstOccurrenceOf ("x", false, false).getIntValue(); if (w > 160) ++wide; if (x < sb.getX() || x + w > sb.getRight() + 2) ok = false; }
            check (ok && wide == 0, leg, vis.joinIntoString (" | ") + " | scroll " + sb.toString()); };
        chipsOk ("18h (2) every visible loop pill sits inside the transcript and is at most 160 px wide (no bar) - before the tap");
        // tap a pill (the same path as the click), then: no stray panel - the only visible components >= 20000 px^2 are the ones before
        auto bigVisible = [&] { juce::StringArray o; std::function<void (juce::Component&)> walk = [&] (juce::Component& c) { for (int i = 0; i < c.getNumChildComponents(); ++i) { auto* k = c.getChildComponent (i); if (! k->isVisible()) continue; if (k->getWidth() * k->getHeight() >= 20000) o.add (juce::String (typeid (*k).name()) + " " + k->getBounds().toString()); walk (*k); } }; walk (*ed); return o; };
        const auto before = bigVisible();
        int propIdx = (int) M.size() - 1; A::input (*ed).setText ("stale", juce::dontSendNotification); A::tapPill (*ed, propIdx); pumpMs (200); ed->repaint(); pumpMs (100); paintOnce();
        const auto after = bigVisible();
        check (after == before, "18h (2) no stray component after a pill tap in the Chat tab (the large visible components are the same set as before)", "before " + before.joinIntoString (" ; ") + " || after " + after.joinIntoString (" ; "));
        chipsOk ("18h (2) ...and the pills after the tap are still inside the transcript, none wider than 160 px");
        // 21 Sep 2026 (loop manners): the tapped pill was [Go] - a level verb like every other: ONE bubble, the five pills, and NOTHING
        // measures until Check (RED as it stood: "Applied - checking while it plays." then "Checking..." and a second proposal)
#ifdef EJ_LOUDNESSLOOP_MANNERS21
        { const auto goMsg = M.back();
          check (goMsg.role == "assistant" && goMsg.content.startsWith ("Applied +") && goMsg.content.endsWith ("). How's it sounding?") && A::chips (*ed, goMsg).joinIntoString ("|") == "Check#2|A bit louder#2|A bit softer#2|Undo#2|Done#2" && loop.state() == LoudnessLoop::State::hold,
                 "21 Sep (Go) the [Go] tap answers \"Applied +X dB (Level now +Y). How's it sounding?\" with [Check] [A bit louder] [A bit softer] [Undo] [Done] and the loop holds", goMsg.content.substring (0, 90) + " | " + A::chips (*ed, goMsg).joinIntoString ("|") + " | state " + juce::String ((int) loop.state()));
          juce::Random rng (12); juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi; logs.clear(); const size_t nMsgs = M.size();
          for (int b = 0; b < 800; ++b) { for (int ch = 0; ch < 2; ++ch) { auto* d = buf.getWritePointer (ch); for (int i = 0; i < 512; ++i) d[i] = (rng.nextFloat() * 2.0f - 1.0f) * 0.1f; } proc.processBlock (buf, midi); if ((b % 23) == 22) loop.tickNow(); }
          pumpMs (100);
          check (loop.state() == LoudnessLoop::State::hold && ! logs.joinIntoString ("\n").contains ("measured:") && M.size() == nMsgs, "21 Sep (Go) 8 s of audio after Go: nothing measured, no new bubble, the loop holds until Check", "state " + juce::String ((int) loop.state()) + " msgs +" + juce::String ((int) (M.size() - nMsgs))); }
#else
        check (false, "21 Sep (Go) the [Go] tap answers \"Applied +X dB (Level now +Y). How's it sounding?\" with [Check] [A bit louder] [A bit softer] [Undo] [Done] and the loop holds", "no MANNERS21 on this build: " + M.back().content.substring (0, 80));
        check (false, "21 Sep (Go) 8 s of audio after Go: nothing measured, no new bubble, the loop holds until Check", "no MANNERS21 on this build");
#endif
        // (3)+(4): the verbs after a Go: "Applied +-X dB (Level now +Y). How's it sounding?" with exactly the five pills, no auto-check
        // (the tapped pill above was [Go] - the first pill - so the loop is checking; let it finish on audio)
        { juce::Random rng (8); juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi; int b = 0;
          while ((loop.state() == LoudnessLoop::State::waitAudio || loop.state() == LoudnessLoop::State::measuring) && b < 6000) { for (int ch = 0; ch < 2; ++ch) { auto* d = buf.getWritePointer (ch); for (int i = 0; i < 512; ++i) d[i] = (rng.nextFloat() * 2.0f - 1.0f) * 0.1f; } proc.processBlock (buf, midi); if ((++b % 23) == 0) loop.tickNow(); } }
        pumpMs (100);
        for (const char* v : { "a bit louder", "a bit softer", "push it", "undo" })
        {
            const size_t n0 = M.size(); logs.clear(); A::send (*ed, v); pumpMs (100);
            const auto& last = M.back();
            check (M.size() == n0 + 2 && last.role == "assistant" && last.content.startsWith ("Applied ") && last.content.endsWith ("). How's it sounding?") && A::chips (*ed, last).joinIntoString ("|") == "Check#2|A bit louder#2|A bit softer#2|Undo#2|Done#2" && loop.state() == LoudnessLoop::State::hold, juce::String ("18h (4) \"") + v + "\": one bubble \"Applied +-X dB (Level now +Y). How's it sounding?\" with [Check] [A bit louder] [A bit softer] [Undo] [Done], no window", last.content.substring (0, 70) + " [" + A::chips (*ed, last).joinIntoString ("|") + "] state " + juce::String ((int) loop.state()));
        }
        // no auto-check after a verb: 8 s of audio, still holding, no measurement
        { juce::Random rng (9); juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi; logs.clear();
          for (int b = 0; b < 800; ++b) { for (int ch = 0; ch < 2; ++ch) { auto* d = buf.getWritePointer (ch); for (int i = 0; i < 512; ++i) d[i] = (rng.nextFloat() * 2.0f - 1.0f) * 0.1f; } proc.processBlock (buf, midi); if ((b % 23) == 22) loop.tickNow(); } }
        check (loop.state() == LoudnessLoop::State::hold && ! logs.joinIntoString ("\n").contains ("measured:"), "18h (4) no automatic check after a verb (8 s of audio: nothing measured)", "state " + juce::String ((int) loop.state()));
        // (3) Push it is absent on an on-target result
        LoudnessLoop::Bubble res; res.kind = LoudnessLoop::Bubble::Kind::result; res.final = true; res.text = "Hitting -9.0 LUFS (loudest 3 s), target -9.0 - on target."; res.pills = LoudnessLoop::resultPills(); loop.onBubble (res);
        check (! A::chips (*ed, M.back()).joinIntoString ("|").contains ("Push it"), "18h (3) Push it is absent on an on-target result", A::chips (*ed, M.back()).joinIntoString ("|"));
        check (LoudnessLoop::shortPills().contains ("Push it") && LoudnessLoop::stuckPills().contains ("Push it") && ! LoudnessLoop::resultPills().contains ("Push it"), "18h (3) ...and present on the short / at-the-limit sets");
    }
#else
    for (const char* leg : { "18h (1) 5 pills at 420 px: every chip inside the width at its natural size (40-140 px), rows = 2", "18h (1) 2 pills at 420 px: every chip inside the width at its natural size (40-140 px), rows = 1", "18h (1) 5 pills at 1200 px: every chip inside the width at its natural size (40-140 px), rows = 1", "18h (1) 2 pills at 1200 px: every chip inside the width at its natural size (40-140 px), rows = 1",
                             "18h (2) [Go] is a 40-80 px pill, not the bubble's width (the 1,130 px bar in the Chat tab)", "18h (2) no stray component after a pill tap in the Chat tab (the large visible components are the same set as before)", "18h (2) every visible loop pill sits inside the transcript and is at most 160 px wide (no bar) - before the tap",
                             "18h (4) \"a bit louder\": one bubble \"Applied +-X dB (Level now +Y). How's it sounding?\" with [Check] [A bit louder] [A bit softer] [Undo] [Done], no window", "18h (4) no automatic check after a verb (8 s of audio: nothing measured)", "18h (3) Push it is absent on an on-target result" })
        check (false, leg, "no 18h on this build");
    {   // AS IT STOOD: [Go] at 1200 px
        EchoJayProcessor proc; proc.prepareToPlay (48000.0, 512); std::unique_ptr<juce::AudioProcessorEditor> edBase (proc.createEditor()); auto* ed = dynamic_cast<EchoJayEditor*> (edBase.get()); if (! ed) return 2;
        A::Msg two; two.role = "assistant"; two.content = "x"; two.loopPills = { "Go", "Leave it" };
        const auto rects = A::layout (*ed, two, 1200);
        check (rects.size() == 2 && rects[0].getWidth() <= 80 && rects[1].getRight() <= 1200, "18h (2) [Go] is a 40-80 px pill, not the bubble's width (the 1,130 px bar in the Chat tab)", "Go " + juce::String (rects.empty() ? -1 : rects[0].getWidth()) + " px, Leave it right edge " + juce::String (rects.size() > 1 ? rects[1].getRight() : -1));
    }
#endif


    std::printf ("== (7) 22 Sep 2026: the add-plugin picker is IN FRONT of a hosted editor pop-out (the pop-out is lowered while the picker is open, raised again when it goes) ==\n");
    {
        EchoJayProcessor proc; proc.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> edBase (proc.createEditor());
        auto* ed = dynamic_cast<EchoJayEditor*> (edBase.get()); if (! ed) return 2;
        ed->setSize (2000, 1100); ed->setVisible (true); ed->addToDesktop (0); pumpMs (60);
        A::build (*ed, "{\"chain\":[{\"name\":\"EchoJay Level\",\"role\":\"level\",\"settings\":\"\"},{\"name\":\"EchoJay Limiter\",\"role\":\"limiter\",\"settings\":\"\"}]}");
        pumpMs (400);
        auto& panel = A::panel (*ed);
        panel.selectedIdx = 0; panel.openPopoutForSelected(); pumpMs (100);
        const bool havePopout = panel.popout != nullptr;
        check (havePopout && panel.popout->isAlwaysOnTop(), "(7) precondition: a hosted-editor pop-out window is open and always-on-top", havePopout ? "always-on-top=" + juce::String ((int) panel.popout->isAlwaysOnTop()) : "no pop-out (" + panel.statusText + ")");
        if (havePopout)
        {
            A::picker (*ed); pumpMs (100);
            juce::CallOutBox* box = nullptr; for (auto* c : ed->getChildren()) if (auto* b = dynamic_cast<juce::CallOutBox*> (c)) box = b;
            check (box != nullptr, "(7) the picker's call-out is open, embedded in the editor");
            check (! panel.popout->isAlwaysOnTop(), "(7) WHILE the picker is open the pop-out is LOWERED (always-on-top off) so the picker is in front", "always-on-top=" + juce::String ((int) panel.popout->isAlwaysOnTop()));
            if (box != nullptr) box->dismiss();
            pumpMs (200);
            check (panel.popout != nullptr && panel.popout->isAlwaysOnTop(), "(7) when the picker goes (dismissed) the pop-out is raised again (always-on-top back on)", panel.popout ? "always-on-top=" + juce::String ((int) panel.popout->isAlwaysOnTop()) : "pop-out gone");
        }
        panel.closeAllEditors(); pumpMs (60); ed->removeFromDesktop();
    }
    std::printf ("== (2c) 22 Sep 2026: a complaint the server classified loop_verb (forced) backs off - the softer step twice - never a chat ==\n");
    {
        EchoJayProcessor proc; proc.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> edBase (proc.createEditor());
        auto* ed = dynamic_cast<EchoJayEditor*> (edBase.get()); if (! ed) return 2;
        ed->setSize (2000, 1100); A::toChat (*ed); pumpMs (60);
        auto& loop = proc.loudnessLoop();
        A::build (*ed, "{\"chain\":[{\"name\":\"EchoJay Level\",\"role\":\"level\",\"settings_structured\":{\"params\":{\"gain_db\":0,\"target_lufs\":-9,\"loudness_option\":0}}},{\"name\":\"EchoJay Limiter\",\"role\":\"limiter\",\"settings_structured\":{\"params\":{\"ceiling_db\":-0.1,\"true_peak\":1}}}]}");
        pumpMs (2500); A::toChat (*ed); pumpMs (100);
        loop.listen(); { juce::Random rng (7); juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi; int b = 0;
          while (loop.state() != LoudnessLoop::State::proposed && b < 6000) { for (int ch = 0; ch < 2; ++ch) { auto* d = buf.getWritePointer (ch); for (int i = 0; i < 512; ++i) d[i] = (rng.nextFloat() * 2.0f - 1.0f) * 0.1f; } proc.processBlock (buf, midi); if ((++b % 23) == 22) loop.tickNow(); } }
        check (loop.state() == LoudnessLoop::State::proposed && loop.go(), "(2c) precondition: a proposal, then Go (the apply bubble)");
        const auto& M = A::msgs (*ed); const size_t nBefore = M.size();
        auto* lv = dynamic_cast<EedLevelProcessor*> (proc.getChainHost().getSlotProcessor (loop.levelSlot())); const float gBefore = lv ? (float) lv->gainDb() : 0.0f; const float tBefore = loop.target();
        check (A::forcedVerb (*ed, "too squashed now"), "(2c) the forced verb is CONSUMED locally (never sent as a chat)");
        pumpMs (60);
        check (lv != nullptr && std::abs ((float) lv->gainDb() - (gBefore - 2.0f)) < 0.05f && std::abs (loop.target() - (tBefore - 2.0f)) < 0.01f, "(2c) the Level moved -2 dB and the target -2 (the softer step twice)", "Level " + juce::String (gBefore, 2) + " -> " + juce::String (lv ? lv->gainDb() : 0.0, 2));
        check (M.size() >= nBefore + 2 && M.back().content.startsWith ("Applied -2.0 dB (Level now "), "(2c) one after-verb bubble \"Applied -2.0 dB (Level now ...)\" (plus the local user bubble)", M.back().content.substring (0, 60));
    }
    std::printf ("== (8) 22 Sep 2026 (21m ruling 1): the roster tick is the Link's ACTIVE flag alone; a separate lamp says audio is flowing; the click sends the inverse of the flag it paints ==\n");
    {
        EchoJayProcessor proc; proc.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> edBase (proc.createEditor());
        auto* ed = dynamic_cast<EchoJayEditor*> (edBase.get()); if (! ed) return 2;
        ed->setSize (2000, 1100); pumpMs (60);
        std::vector<EchoJayProcessor::LinkSlotInfo> links;
        for (int i = 0; i < 5; ++i) { EchoJayProcessor::LinkSlotInfo li; li.name = "BV " + juce::String (i + 1); li.uid = "lnk_0" + juce::String (i + 1); li.active = true; li.connected = false; li.audioFlowing = false; li.regIdx = i; links.push_back (li); }
        EchoJayAlignTestAccess::setLinks (proc, links); pumpMs (60);
        auto& panel = A::panel (*ed);
        bool allOn = true, allLampsOff = true; for (int i = 0; i < 5; ++i) { const auto tv = panel.msLamps.tickFor ("lnk_0" + juce::String (i + 1)); allOn = allOn && tv.has && tv.active; allLampsOff = allLampsOff && ! tv.audio; }
        check (allOn && allLampsOff, "(8) five ACTIVE Links with UNBOUND rings paint ON (tick from the active flag alone), audio lamps OFF  (RED as it stood: the tick needed the ring)", "on=" + juce::String ((int) allOn) + " lampsOff=" + juce::String ((int) allLampsOff));
        panel.msLamps.onTick ("lnk_03"); pumpMs (20);
        const auto sent = EchoJayRosterTestAccess::last (*ed);
        check (sent.count == 1 && sent.addr == "lnk_03" && sent.active == false, "(8) a click on an ACTIVE Link sends set-active(false) - the inverse of the flag it paints, never a turn-on of a Link already on", sent.addr + " active=" + juce::String ((int) sent.active) + " count=" + juce::String (sent.count));
        { const auto tv = panel.msLamps.tickFor ("lnk_03"); check (tv.pending && ! tv.target, "(8) ...and the tick shows the pending target OFF until the Link answers", "pending=" + juce::String ((int) tv.pending) + " target=" + juce::String ((int) tv.target)); }
        links[0].connected = true; links[0].audioFlowing = true; EchoJayAlignTestAccess::setLinks (proc, links); pumpMs (20);
        { const auto tv = panel.msLamps.tickFor ("lnk_01"); check (tv.active && tv.audio, "(8) the ring binds later and frames arrive -> the lamp comes ON, the tick unchanged (still active)", "active=" + juce::String ((int) tv.active) + " audio=" + juce::String ((int) tv.audio)); }
        // ---- (10) 21m ruling 3, on THIS editor (a third editor in the process aborted in setSize: "mutex lock failed", so the leg shares (8)'s) ----
        std::printf ("== (10) 22 Sep 2026 (21m ruling 3): the keep-level toggle on a borrowed or held remote rack is GREYED with \"available on this rack only for now\", never a silent no-op ==\n");
        { A::Block b; b.keepAvailable = false; const auto km = b.keepMenu();
          check (! km.enabled && km.note == "available on this rack only for now", "(10) a block with keepAvailable=false reports the item DISABLED with the note", km.note);
          b.keepAvailable = true; check (b.keepMenu().enabled && b.keepMenu().note.isEmpty(), "(10) ...and enabled with no note when available"); }
        proc.pendingChannelUid = {}; A::refreshPanel (*ed);
        check (panel.keepLevelAvailable, "(10) the LOCAL rack view: keep-level available", juce::String ((int) panel.keepLevelAvailable));
        proc.pendingChannelUid = "lnk_01"; A::refreshPanel (*ed);   // the chain view = that Link's rack (held remote, no borrow)
        check (! panel.keepLevelAvailable, "(10) a Link's rack in view (chainViewUid non-empty): keep-level GREYED (RED as it stood: the flag did not exist - compile refusal)", juce::String ((int) panel.keepLevelAvailable));
        proc.pendingChannelUid = {}; A::refreshPanel (*ed);
        check (panel.keepLevelAvailable, "(10) back on the local rack: available again");
    }
    std::printf ("== (9) 22 Sep 2026 (21m): rename alias - a V2-session alias for a Link, shown by getLinkDisplayList (the one source), Reset name clears it, persisted with the session state ==\n");
    {
        EchoJayProcessor proc; proc.prepareToPlay (48000.0, 512);
        std::vector<EchoJayProcessor::LinkSlotInfo> links;
        for (int i = 0; i < 2; ++i) { EchoJayProcessor::LinkSlotInfo li; li.name = "BV " + juce::String (i + 1); li.uid = "lnk_0" + juce::String (i + 1); li.active = true; li.connected = true; links.push_back (li); }
        EchoJayAlignTestAccess::setLinks (proc, links);
        auto nameOf = [] (EchoJayProcessor& p, const juce::String& uid) { for (const auto& e : p.getLinkDisplayList()) if (e.info.uid == uid) return e.displayName; return juce::String ("<absent>"); };
        check (nameOf (proc, "lnk_01") == "BV 1", "(9) before: the Link's own name", nameOf (proc, "lnk_01"));
        proc.setLinkAlias ("lnk_01", "Lead Vox");
        check (nameOf (proc, "lnk_01") == "Lead Vox" && nameOf (proc, "lnk_02") == "BV 2" && proc.linkAlias ("lnk_01") == "Lead Vox", "(9) the alias shows everywhere V2 names the Link (getLinkDisplayList is the one source); the other Link untouched", nameOf (proc, "lnk_01") + " / " + nameOf (proc, "lnk_02"));
        juce::MemoryBlock mb; proc.getStateInformation (mb);
        { EchoJayProcessor p2; p2.prepareToPlay (48000.0, 512); p2.setStateInformation (mb.getData(), (int) mb.getSize()); EchoJayAlignTestAccess::setLinks (p2, links);
          check (p2.linkAlias ("lnk_01") == "Lead Vox" && nameOf (p2, "lnk_01") == "Lead Vox", "(9) the alias persists in the V2 session state", p2.linkAlias ("lnk_01")); }
        proc.setLinkAlias ("lnk_01", {});
        check (nameOf (proc, "lnk_01") == "BV 1" && proc.linkAlias ("lnk_01").isEmpty(), "(9) Reset name clears it", nameOf (proc, "lnk_01"));
    }
    std::printf ("\n==== ui_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
