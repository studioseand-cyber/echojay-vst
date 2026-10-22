// =============================================================================
//  chat_store_guard - THE CHAT STORE'S WHOLE RULE SET (Sean, 18 Oct 2026, 16:39)
// =============================================================================
//  He stopped taking symptom fixes and wrote the rules out:
//
//    1. "Every chat belongs to ONE project and ONE track: Main (the main rack)
//       or a Link (by uid). A new chat is filed under the open project, in that
//       track's folder, dated today. Never Ungrouped while a project is open."
//    2. "A message (mine or the agent's) is saved only into the chat it was sent
//       in. Opening or switching chats never moves, copies or appends anything."
//    3. "A chat keeps its target: reopening it puts the chip back to that track."
//    4. (the one-off clean-up, which is EchoJayWorkspace::repairChatFiling)
//
//  And he named the guard: "three projects x (Main + two Links), create a chat
//  in each, send in each, switch between all of them in random order, then check
//  each chat holds exactly its own messages, sits in the right folder, and keeps
//  its target. Red first on today's code."
//
//  WHY ITS OWN BINARY. ui_guard holds the editor legs, and its run is ~200 s of
//  real UI waits; this one has to be runnable BEFORE an install on every chat
//  fix, so it carries only what these rules need and finishes in seconds.
//
//  The access struct is named EchoJayTabStripTestAccess because friendship is by
//  NAME: PluginEditor.h befriends that name, and this file's definition carries
//  only the handful of members these legs use.
// =============================================================================
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cstdio>

// The clean-up (rule 4) is part of the FIX, so it cannot exist in the code the red-first pass runs
// against. This switch is 0 for that pass and 1 ever after - stated here rather than hidden in a
// build flag, so anyone reading the file can see which legs the red pass could and could not hold.
#define EJ_CHATSTORE_REPAIR_PRESENT 1

namespace
{
int failures = 0, passes = 0;
void check (bool ok, const juce::String& what, const juce::String& detail = {})
{
    if (ok) { ++passes; std::printf ("  ok    %s\n", what.toRawUTF8()); }
    else    { ++failures; std::printf ("  FAIL  %s%s\n", what.toRawUTF8(),
                                       detail.isNotEmpty() ? ("  [" + detail + "]").toRawUTF8() : ""); }
    std::fflush (stdout);
}
void pumpMs (double ms)
{   // ui_guard's own pump, for the same reason: a plugin harness has no dispatch loop of its own.
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    while (juce::Time::getMillisecondCounterHiRes() - t0 < ms)
    { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false); }
}
} // namespace

struct EchoJayAlignTestAccess
{
    static void setLinks (EchoJayProcessor& p, std::vector<EchoJayProcessor::LinkSlotInfo> v)
    {   // the processor's 1 Hz timer would rebuild this from the (empty) registry - stop it first,
        // exactly as ui_guard's copy of this struct does, and for the same reason.
        static_cast<juce::Timer&> (p).stopTimer(); p.linkSlotInfos = std::move (v);
    }
};

struct EchoJayTabStripTestAccess
{
    /** 19 Oct 2026 (his 17:20): the mode hint under the composer. A pure static string function,
        reached through the friend struct rather than by widening the editor's interface - the hint
        is the editor's own presentation and has no business being public just to be read here. */
    static juce::String approvalLine (EchoJayEditor::AgentApproval m)
    { return EchoJayEditor::agentApprovalLine (m); }
    static void toChat (EchoJayEditor& e) { e.switchToTab (EchoJayEditor::Tab::Chat, true); }
    static void newChat (EchoJayEditor& e) { e.createNewChat (e.effectiveChannelUid()); }
    static void send (EchoJayEditor& e, const juce::String& t) { e.sendChatMessage (t); }
    static juce::String chatId (EchoJayEditor& e) { return e.currentChatId; }
    static void openChat (EchoJayEditor& e, const juce::String& id) { e.loadChatFromWorkspace (id); }
    /** THE ROAD HE TAKES IN LOGIC: a rack selection. "" is the main rack - and it must be this
        call, not openChannelByUid, which returns early on an empty uid and is Link-only. */
    static void selectRack (EchoJayEditor& e, const juce::String& uid) { e.selectRack (uid); }
    /** VIEW only, which is what he did in Logic: a rack selection, saying nothing about the chat. */
    static void viewRack (EchoJayEditor& e, const juce::String& uid) { e.selectRackForView (uid); }
    static juce::String effectiveUid (EchoJayEditor& e) { return e.effectiveChannelUid(); }
    static juce::String chipLabel (EchoJayEditor& e) { return e.chatTargetLabel(); }
    static juce::String banner (EchoJayEditor& e) { e.refreshChannelBannerCache(); return e.chanBannerText_; }
    /** The agent's road in: the client emits the goal as a "user" line and the bridge lands here. */
    static void agentUserLine (EchoJayEditor& e, const juce::String& t) { e.appendLocalUserBubble (t); }
    static int onScreen (EchoJayEditor& e, const juce::String& text)
    {
        int n = 0;
        for (const auto& m : e.chatMessages) if (m.role == "user" && m.content == text) ++n;
        return n;
    }
    /** The run's card as the RECORD holds it, and as the editor would draw it. */
    static juce::String storedCard (EchoJayEditor& e, const juce::String& id)
    {
        auto* c = e.workspace.findChatById (id);
        if (c == nullptr) return {};
        for (const auto& m : c->messages) if (m.agentJson.isNotEmpty()) return m.agentJson;
        return {};
    }
    static const EchoJayEditor::ChatMsg* rowWithCard (EchoJayEditor& e)
    {
        for (const auto& m : e.chatMessages) if (m.agentData.isNotEmpty()) return &m;
        return nullptr;
    }
    static echojay::agentui::RunView viewOf (EchoJayEditor& e, const EchoJayEditor::ChatMsg& m)
    { return e.agentRunViewFor (m); }
    static int rowWidth (EchoJayEditor& e) { return e.agentRowWidth(); }
    static int inStore (EchoJayEditor& e, const juce::String& id, const juce::String& text)
    {
        auto* c = e.workspace.findChatById (id);
        if (c == nullptr) return -1;
        int n = 0;
        for (const auto& m : c->messages) if (m.role == "user" && m.content == text) ++n;
        return n;
    }
    static void clearChat (EchoJayEditor& e) { e.currentChatId = {}; e.chatMessages.clear(); }
    static void pend (EchoJayProcessor& p, const juce::String& uid) { p.pendingChannelUid = uid; }
    static juce::String channelLabel (EchoJayEditor& e, const juce::String& uid) { return e.channelDisplayLabel (uid); }
    static std::vector<EchoJayEditor::ChatMsg>& msgs (EchoJayEditor& e) { return e.chatMessages; }
    /** 20 Oct 2026: push a run CARD into the open chat. ChatMsg is private to the editor, so the
        harness body cannot name it - only this friend can, which is why the factory lives here. */
    static void pushCard (EchoJayEditor& e, const juce::String& agentJson, int runId)
    {
        EchoJayEditor::ChatMsg m;
        m.role       = "assistant";
        m.agentData  = agentJson;
        m.agentRunId = runId;
        e.chatMessages.push_back (m);
    }
    static WsChat* chat (EchoJayEditor& e, const juce::String& id) { return e.workspace.findChatById (id); }
    static const std::vector<WsChat>& chats (EchoJayEditor& e) { return e.workspace.getChats(); }
    static EchoJayWorkspace& store (EchoJayEditor& e) { return e.workspace; }
    // 19 Oct 2026: the interrupted-run save, driven without a server - the liveness question is
    // one line in saveInterruptedAgentRun; this is the transform and the store write it calls.
    static void stopCard (EchoJayEditor& e, EchoJayEditor::ChatMsg& m, const juce::String& why)
    { e.writeStoppedCardForRun (m, why); }
    static void appendRow (EchoJayEditor& e, const juce::String& content, const juce::String& json)
    {
        EchoJayEditor::ChatMsg m;
        m.role = "assistant"; m.content = content; m.agentData = json;
        m.agentRunId = 77;
        // ---- RE-AIMED BY STAGE D (his chat-log ruling, 21 Oct 2026) --------------------------
        // The gates a live run sets for itself when its row is born are now the RUN'S KEY and its
        // chat - not a per-window integer, which the chat replay reissues (his 14:15). The row
        // carries the key as its log id, which is what the recorder looks it up by.
        const auto key = echojay::agentui::fromJson (juce::JSON::parse (json)).runKey;
        m.entryId = key;
        e.chatMessages.push_back (m);
        e.workspace.appendMessageToChat (e.currentChatId, "assistant", content);
        e.agentRunMsgId_  = 77;                 // still the DRAWING handle the zones use
        e.agentRunKey_    = key;                // ...and the identity the recorder gates on
        e.agentRunChatId_ = e.currentChatId;
    }
};

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("ej_chatstore_" + juce::String (juce::Time::getMillisecondCounter()));
    tmp.createDirectory();
    setenv ("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);
    {
        auto af = tmp.getChildFile ("Library/Application Support/EchoJay/auth.json");
        af.getParentDirectory().createDirectory();
        af.replaceWithText ("{\"endpoint\":\"https://localhost.invalid\",\"token\":\"harness-token\","
                            "\"email\":\"store@test.local\",\"tier\":\"pro\",\"tierLevel\":2,"
                            "\"messageLimit\":999,\"credits\":999,\"displayName\":\"Store\","
                            "\"messagesUsedToday\":0,\"usageDate\":\"2026-10-18\","
                            "\"autoDialMode\":false,\"dialWritesBlocked\":false}");
    }
    std::printf ("==== chat_store_guard - every chat its own messages, its own folder, its own target ====\n");

    using A = EchoJayTabStripTestAccess;
    EchoJayProcessor proc; proc.prepareToPlay (48000.0, 512);
    std::unique_ptr<juce::AudioProcessorEditor> edBase (proc.createEditor());
    auto* ed = dynamic_cast<EchoJayEditor*> (edBase.get());
    if (ed == nullptr) { std::printf ("  FAIL  no editor\n"); return 2; }
    ed->setSize (2000, 1100);
    A::toChat (*ed); pumpMs (60);

    // Two Links on the roster, as his session has: the Kick and the Vox.
    const juce::String kickUid = "lnk_kick", voxUid = "lnk_vox";
    {
        std::vector<EchoJayProcessor::LinkSlotInfo> rows;
        EchoJayProcessor::LinkSlotInfo k;
        k.name = "88_7even Kick_2_01"; k.uid = kickUid; k.active = true; k.connected = true; k.regIdx = 0;
        EchoJayProcessor::LinkSlotInfo v;
        v.name = "88_7even Vox_Lead_01"; v.uid = voxUid; v.active = true; v.connected = true; v.regIdx = 1;
        rows.push_back (k); rows.push_back (v);
        EchoJayAlignTestAccess::setLinks (proc, rows); pumpMs (40);
    }

    // ---- his scenario: three projects x (Main + two Links) ------------------------------------
    struct Made { juce::String project, uid, chatId, sent; };
    std::vector<Made> made;
    const char* projects[] = { "project nafe aitch", "project two", "project three" };
    for (const auto* project : projects)
    {
        proc.setProjectName (project); pumpMs (20);
        for (const auto& uid : { juce::String(), kickUid, voxUid })
        {
            // A rack selection, which is what he did: the main rack for Main, the Link's rack for a
            // Link. Both through selectRack, the one road a strip click takes.
            A::selectRack (*ed, uid); pumpMs (30);
            A::newChat (*ed); pumpMs (30);
            const auto text = juce::String ("in ") + project + " on "
                            + (uid.isEmpty() ? juce::String ("main") : uid);
            A::send (*ed, text); pumpMs (60);
            made.push_back ({ project, uid, A::chatId (*ed), text });
        }
    }
    check (made.size() == 9, "SP0 PRECONDITION: nine chats were made - three projects x (Main + two Links)",
           juce::String ((int) made.size()));
    {
        juce::StringArray ids;
        for (const auto& m : made) if (m.chatId.isNotEmpty()) ids.addIfNotAlreadyThere (m.chatId);
        check (ids.size() == 9, "SP0b ...and each one is its OWN record, nine distinct ids",
               juce::String (ids.size()) + " distinct");
    }

    // ---- RULE 1: filed under the project, in the right folder, dated today --------------------
    for (const auto& m : made)
    {
        auto* c = A::chat (*ed, m.chatId);
        const auto today = juce::Time::getCurrentTime().toISO8601 (true).substring (0, 10);
        check (c != nullptr && c->trackName == m.project && c->linkUid == m.uid
               && c->created.startsWith (today),
               "SP1 \"" + m.project + "\" / " + (m.uid.isEmpty() ? juce::String ("Main") : m.uid)
                 + ": filed under its project, in that track's folder, dated today (RED as it stood: "
                   "a new chat on a Link's rack was filed under Main, and a chat the store minted "
                   "itself had no project at all - UNGROUPED)",
               c == nullptr ? juce::String ("no record")
                            : ("project=\"" + c->trackName + "\" uid=\""
                               + (c->linkUid.isEmpty() ? juce::String ("(main)") : c->linkUid)
                               + "\" created=" + c->created.substring (0, 10)));
    }
    check ([&] { for (auto& c : A::chats (*ed)) if (c.trackName.trim().isEmpty()) return false; return true; }(),
           "SP1b and NO chat in the store is Ungrouped - \"never Ungrouped while a project is open\"");
    {
        // ---- HIS UNGROUPED CHAT, WHERE IT ACTUALLY COMES FROM --------------------------------
        // Not from + New chat, which files properly: from the STORE minting one when a message
        // arrives for a chat id its sync has not seen yet. That is an ordinary state - the id is
        // minted locally and the sync follows - and the chat it made was bare: no project, no
        // channel, no album. The sidebar draws that as UNGROUPED and no chip can point at it.
        proc.setProjectName ("project nafe aitch"); pumpMs (20);
        A::selectRack (*ed, kickUid); pumpMs (30);
        const juce::String unseen = "chat_the_sync_has_not_seen";
        A::store (*ed).appendMessageToChat (unseen, "user", "test two");
        auto* minted = A::chat (*ed, unseen);
        check (minted != nullptr && minted->trackName == "project nafe aitch",
               "SP1c a chat the STORE mints for an unseen id is filed under the open project (RED as "
               "it stood: it was minted bare - \"a new chat on the Mix Bus in 'project nafe aitch' "
               "went to Ungrouped\")",
               minted == nullptr ? juce::String ("no record")
                                 : ("project=\"" + minted->trackName + "\""));
        check (minted != nullptr && minted->linkUid == kickUid,
               "SP1d ...and on the track whose rack is open, so it has a target to keep",
               minted == nullptr ? juce::String ("no record")
                                 : ("uid=\"" + (minted->linkUid.isEmpty() ? juce::String ("(main)")
                                                                           : minted->linkUid) + "\""));
    }

    // ---- RULE 2: switching never moves, copies or appends anything ----------------------------
    // His own move: click between all of them in an order that is not the order they were made.
    const int order[] = { 4, 0, 8, 2, 6, 1, 7, 3, 5, 0, 8, 4 };
    for (const int i : order)
    {
        A::openChat (*ed, made[(size_t) i].chatId);
        pumpMs (40);
    }
    for (const auto& m : made)
    {
        auto* c = A::chat (*ed, m.chatId);
        int userRows = 0; bool hasOwn = false, hasForeign = false;
        if (c != nullptr)
            for (const auto& ms : c->messages)
                if (ms.role == "user")
                {
                    ++userRows;
                    if (ms.content == m.sent) hasOwn = true;
                    else                      hasForeign = true;
                }
        check (c != nullptr && userRows == 1 && hasOwn && ! hasForeign,
               "SP2 " + m.chatId + " holds exactly its OWN message after twelve switches (RED as it "
               "stood: the processor's unsynced rows were carried into whichever chat was opened, and "
               "carried again on the next switch - his log read \"merged 2 row(s)\", then 3, 4, 5...)",
               c == nullptr ? juce::String ("no record")
                            : (juce::String (userRows) + " user row(s)"
                               + (hasForeign ? ", one of them another chat's" : "")));
    }
    {
        // and the TRANSCRIPT on screen is that chat's too, not the last one's
        A::openChat (*ed, made[0].chatId); pumpMs (40);
        int onScreen = 0; bool foreign = false;
        for (const auto& m : A::msgs (*ed))
            if (m.role == "user")
            {
                ++onScreen;
                if (m.content != made[0].sent) foreign = true;
            }
        check (onScreen == 1 && ! foreign,
               "SP2b ...and the transcript on screen is that chat's alone (RED as it stood: clicking "
               "any of three chats showed the same three messages)",
               juce::String (onScreen) + " user row(s) on screen"
                 + (foreign ? ", one of them another chat's" : ""));
    }

    // ---- RULE 3: a chat keeps its target ------------------------------------------------------
    for (const auto& m : made)
    {
        A::openChat (*ed, m.chatId); pumpMs (40);
        const auto uid = A::effectiveUid (*ed);
        check (uid == m.uid,
               "SP3 reopening " + (m.uid.isEmpty() ? juce::String ("the Main chat") : "the " + m.uid + " chat")
                 + " puts the target back on that track",
               uid.isEmpty() ? juce::String ("(main)") : uid);
        // The chip NAMES the track when the display name can be resolved. Under this harness's
        // isolated state root there is no Link registry to resolve one, so the leg asks the weaker
        // honest question: the chip must not be the mix bus's words while the chat is on a Link.
        if (m.uid.isNotEmpty() && A::channelLabel (*ed, m.uid).isNotEmpty()
            && A::channelLabel (*ed, m.uid) != m.uid)
            check (A::chipLabel (*ed) != "This channel",
                   "SP3b ...and the composer's chip is that track, never the mix bus's words",
                   A::chipLabel (*ed));
    }

    // ---- RULE 4: the one-off clean-up, both halves and its refusal ----------------------------
#if EJ_CHATSTORE_REPAIR_PRESENT
    {
        // A chat the 14:53 build left with no project, and the em-dash twin it wrote twice.
        WsChat broken;
        broken.id = "chat_broken"; broken.title = "Broken";
        broken.created = juce::Time::getCurrentTime().toISO8601 (true);
        WsMessage q; q.role = "user"; q.content = "what did you do?";
        WsMessage a1; a1.role = "assistant"; a1.content = "Added the CL 1B - threshold -10 dB.";
        WsMessage a2; a2.role = "assistant";
        a2.content = juce::String::fromUTF8 ("Added the CL 1B \xe2\x80\x94 threshold -10 dB.");
        broken.messages = { q, a1, a2 };
        A::store (*ed).addChat (broken);
        const auto rep = A::store (*ed).repairChatFiling ("project nafe aitch", {});
        auto* fixed = A::chat (*ed, "chat_broken");
        check (fixed != nullptr && fixed->trackName == "project nafe aitch" && rep.refiled >= 1,
               "SP4 the clean-up files a chat the old build left with no project",
               fixed == nullptr ? juce::String ("gone") : fixed->trackName);
        check (fixed != nullptr && fixed->messages.size() == 2 && rep.duplicatesDropped == 1,
               "SP4b ...and drops the em-dash twin of an answer already in that chat, keeping one",
               fixed == nullptr ? juce::String ("gone")
                                : juce::String ((int) fixed->messages.size()) + " message(s) left");
        check (fixed != nullptr && fixed->messages.size() == 2
               && fixed->messages[0].role == "user" && fixed->messages[1].role == "assistant",
               "SP4c ...and what it keeps is the question and ONE answer, in order");
    }
#endif

    // ---- SURVIVAL IS NOT CAPTURE (20 Oct 2026, his 11:59 run) --------------------------------
    //
    // *"an earlier agent run was left waiting on 'How's it sounding?'. He opened a NEW chat on a
    // DIFFERENT Link, typed 'adjust the compressors' and pressed Enter. The message was delivered to
    // the old waiting run as its answer: the old card ... was drawn in the new chat and carried on
    // from there. Survival must not mean capture."*
    //
    // His leg, as he specified it: a run waiting in chat A on Link X, a send in chat B on Link Y,
    // and afterwards a new run in B with A untouched.
    //
    // This is the cost of 0b being on. While a window close ended every run, a waiting ask could
    // not outlive the chat it was asked in; now it can, and the first thing typed ANYWHERE became
    // its answer. The guard is the run's own chat id, remembered at the row's birth - an identity,
    // not the sentence heuristic that was standing in for one.
    std::printf ("== CAPTURE: a send in another chat starts its own run and leaves the waiting one ==\n");
    {
        using namespace echojay::agentui;
        A::toChat (*ed);

        // CHAT A, on Link X, with a run waiting on its question.
        A::newChat (*ed);
        const auto chatA = A::chatId (*ed);
        A::selectRack (*ed, "linkX");
        RunView waiting;
        waiting.running = true;
        waiting.closing.clear();
        { RunStep st; st.label = "Which compressor should I add to the Mix Bus?";
          st.chips = { "EchoJay Compressor", "dbx 160" }; waiting.steps.push_back (st); }
        { RunStep st; st.label = "add EchoJay Compressor"; waiting.steps.push_back (st); }
        { RunStep st; st.label = "How's it sounding?"; st.chips = { "Sounds good", "Run a check" };
          waiting.steps.push_back (st); }
        A::pushCard (*ed, juce::JSON::toString (toJson (waiting), true), 1);
        const int msgsInA = (int) A::msgs (*ed).size();

        // CHAT B, on Link Y: a NEW chat, a different rack, and a new instruction.
        A::newChat (*ed);
        const auto chatB = A::chatId (*ed);
        A::selectRack (*ed, "linkY");
        check (chatB != chatA && chatB.isNotEmpty(),
               "CAP0 the fixture: two different chats", chatA + " vs " + chatB);
        A::send (*ed, "adjust the compressors");
        pumpMs (120);

        // THE OLD CARD IS NOT HERE. Its steps are the tell: "Which compressor should I add to the
        // Mix Bus?" belongs to chat A and must not be drawn in B.
        juce::String bWords;
        for (const auto& m : A::msgs (*ed)) bWords << m.content << " " << m.agentData << " | ";
        check (! bWords.contains ("Which compressor should I add to the Mix Bus?"),
               "CAP1 the waiting run's card is NOT drawn in the new chat  (RED as it stood: the old "
               "card appeared in chat B and carried on from there)",
               bWords.substring (0, 140));
        check (! bWords.contains ("How's it sounding?"),
               "CAP2 ...and neither is its pending question");

        // AND CHAT A IS UNTOUCHED.
        //
        // NOT BY REOPENING IT: this fixture pushed the card straight into chatMessages and never
        // persisted it to the workspace, so switching away and back legitimately loses it - which is
        // what CAP3 reported on its first run ("1 message(s), was 1"). That was my fixture telling me
        // about itself, not the product losing a card. The thing worth asserting is that the send in
        // B did not REACH into A, and the way to see that without a round trip through the store is
        // the client: the run that was waiting in A is no longer waiting on ITS question, and nothing
        // answered it - no "You said:" was recorded on any of its steps.
        {
            bool anyAnswered = false;
            juce::String steps;
            for (const auto& m : A::msgs (*ed))
                if (m.agentData.isNotEmpty())
                {
                    const auto v = fromJson (juce::JSON::parse (m.agentData));
                    for (const auto& st : v.steps)
                    {
                        steps << st.label << " / ";
                        if (st.answered.isNotEmpty()) anyAnswered = true;
                    }
                }
            check (! anyAnswered,
                   "CAP3 nothing was recorded as an ANSWER on the waiting run's steps - the send in "
                   "chat B did not reach into chat A  (RED as it stood: \"adjust the compressors\" was "
                   "recorded as the answer to \"How's it sounding?\")",
                   steps.isEmpty() ? juce::String ("no steps in view") : steps.substring (0, 120));
            juce::ignoreUnused (msgsInA);
        }
    }

    // ---- FOLD ON THE PULL (20 Oct 2026, his 17:11 and 18:07 reports) -------------------------
    //
    // *"The closing showed twice again."*  Three times in one day, and twice AFTER the key-minting
    // fix of 16 Oct closed the cause everyone knew about.
    //
    // THE ROAD IN IS THE SYNC. `requestMutationSync` POSTs and then calls `requestSync()` to "pull
    // any concurrent web-app edits", so a line the SERVER persisted beside the card comes back down
    // as its own assistant message. The plugin never drew it twice - it was handed a second copy.
    //
    // AND THE OLD DEDUPE COULD NOT CATCH IT: it compared a message against earlier messages'
    // `content`, and a MID-ROUND sentence lives on a STEP inside `agentJson` while the card's own
    // content has moved on to a later round's closing. His 15:29 case exactly: "Added EchoJay EQ.
    // Settings applied." was round 8's, and the card by then said "I cut the low-mids...".
#if EJ_CHATSTORE_REPAIR_PRESENT
    {
        using namespace echojay::agentui;
        RunView v;
        v.closing = "I cut the low-mids around 300 Hz by about 3 dB, so it should read less boxy.";
        { RunStep st; st.label = "Added EchoJay EQ. Settings applied."; v.steps.push_back (st); }
        { RunStep st; st.label  = "set slot 1: bell 300 Hz";
          st.detail = "read back band 1: bell 300.0 Hz, -3.0 dB, Q 1.40"; v.steps.push_back (st); }

        WsChat ch;
        ch.id = "chat_fold"; ch.title = "Fold"; ch.trackName = "Aitch_4_01";
        ch.created = juce::Time::getCurrentTime().toISO8601 (true);
        WsMessage card; card.role = "assistant";
        card.content   = v.closing;
        card.agentJson = juce::JSON::toString (toJson (v), true);
        // THE PULLED ECHO: the server's own copy of a MID-ROUND step line, which the card carries on
        // a step and not in its content. This is the message he saw under the closing.
        WsMessage echoStep; echoStep.role = "assistant";
        echoStep.content = "Added EchoJay EQ. Settings applied.";
        // ...and an echo of the CLOSING, which the old rule would have caught.
        WsMessage echoClosing; echoClosing.role = "assistant"; echoClosing.content = v.closing;
        // A REAL later answer that happens to be short, and a real long one: neither may be folded.
        WsMessage shortReal; shortReal.role = "assistant"; shortReal.content = "Done.";
        WsMessage realReply; realReply.role = "assistant";
        realReply.content = "The kick is sitting about 2 dB behind the snare now, if you want it up.";
        ch.messages = { card, echoStep, echoClosing, shortReal, realReply };
        A::store (*ed).addChat (ch);

        const auto rep = A::store (*ed).repairChatFiling ("Aitch_4_01", {});
        auto* f = A::chat (*ed, "chat_fold");
        juce::StringArray left;
        if (f != nullptr) for (const auto& m : f->messages) left.add (m.content.substring (0, 40));

        check (f != nullptr && rep.duplicatesDropped == 2,
               "FP1 BOTH pulled echoes are folded - the mid-round STEP line and the closing  (RED as "
               "it stood: the step line was not in any earlier message's content, so the old rule "
               "could not see it and he read the closing twice)",
               juce::String (rep.duplicatesDropped) + " dropped; left: " + left.joinIntoString (" | "));
        check (f != nullptr && f->messages.size() == 3,
               "FP1b ...leaving the card, the short real answer and the real reply",
               juce::String (f == nullptr ? 0 : (int) f->messages.size()) + " left");
        check (f != nullptr && ! f->messages.empty() && f->messages[0].agentJson.isNotEmpty(),
               "FP2 the CARD is never the one dropped - a message carrying its own agentJson is a "
               "record, not an echo");
        bool keptShort = false, keptReal = false;
        if (f != nullptr)
            for (const auto& m : f->messages)
            {
                if (m.content == "Done.") keptShort = true;
                if (m.content.startsWith ("The kick is sitting")) keptReal = true;
            }
        check (keptShort,
               "FP3 a SHORT real answer is never folded - \"Done.\" can honestly be said by a step "
               "and again by a later turn, and folding on it would lose a real answer");
        check (keptReal,
               "FP4 and a real reply that the card does NOT say is kept, which is the whole point of "
               "the length floor and the agentJson test");
    }
    // ---- FP5: THE DASH TWIN, WHICH I FAILED TO LEG LAST NIGHT (20 Oct 2026, his 09:36) --------
    //
    // *"fold-on-pull missed a duplicate closing because the two copies differ only in "-" vs " - "."*
    //
    // FP1-FP4 above assert the MECHANISM - a mid-round step line, a closing, a short answer, a real
    // reply - and never the one difference the repair's own comment, three lines from my new code,
    // says was the only way two copies of a sentence ever differed. So the fold shipped with the
    // case it was most likely to meet untested, and it met it within the hour.
#if EJ_CHATSTORE_REPAIR_PRESENT
    {
        using namespace echojay::agentui;
        RunView v;
        v.closing = juce::String::fromUTF8 ("I cut the low-mids \xe2\x80\x94 about 3 dB \xe2\x80\x94 "
                                            "so it reads less boxy.");
        WsChat ch;
        ch.id = "chat_dash"; ch.title = "Dash"; ch.trackName = "Aitch_4_01";
        ch.created = juce::Time::getCurrentTime().toISO8601 (true);
        WsMessage card; card.role = "assistant";
        card.content   = v.closing;                       // EM DASHES, as the card has them
        card.agentJson = juce::JSON::toString (toJson (v), true);
        // THE PULLED ECHO, spelled with spaced hyphens - the only difference, and his 09:36 case.
        WsMessage echoDash; echoDash.role = "assistant";
        echoDash.content = "I cut the low-mids - about 3 dB - so it reads less boxy.";
        ch.messages = { card, echoDash };
        A::store (*ed).addChat (ch);

        const auto rep = A::store (*ed).repairChatFiling ("Aitch_4_01", {});
        auto* f = A::chat (*ed, "chat_dash");
        check (f != nullptr && rep.duplicatesDropped == 1 && f->messages.size() == 1,
               "FP5 a copy differing ONLY in \"-\" versus \" - \" is folded  (RED as it stood: the "
               "dashes were mapped but the SPACES around them were not, so \"a-b\" and \"a - b\" "
               "stayed different keys)",
               juce::String (rep.duplicatesDropped) + " dropped, "
                 + juce::String (f == nullptr ? 0 : (int) f->messages.size()) + " left");
        check (f != nullptr && ! f->messages.empty() && f->messages[0].agentJson.isNotEmpty(),
               "FP5b ...and the CARD is the one kept, not the plain echo");
    }
#endif
    {
        // AND THE KEY ITSELF, both directions, because a key that collapses too much folds real
        // answers: three spellings of one sentence agree, and two DIFFERENT sentences do not.
        using namespace echojay::agentui;
        const auto a = normalisedProse (juce::String::fromUTF8 ("low-mids \xe2\x80\x94 about 3 dB"));
        const auto b = normalisedProse ("low-mids - about 3 dB");
        const auto c = normalisedProse ("low-mids-about 3 dB");
        check (a == b && b == c,
               "FP6 em dash, spaced hyphen and bare hyphen all normalise to ONE key",
               a + " | " + b + " | " + c);
        check (normalisedProse ("low-mids - about 3 dB")
                 != normalisedProse ("low-mids - about 4 dB"),
               "FP6b ...and two sentences that genuinely differ still have different keys");
    }
#endif

    // ---- SP5 (18 Oct 2026, 17:01): A RUN ON A LINK, AND THE SWITCH TO MAIN AND BACK -----------
    //
    // *"The Kick chat's answer is lost: reopened, it shows only 'what EQs do I have?'. That question
    // leaked into the Main chat. The Kick chat doesn't restore its target on reopen: chip says 'This
    // channel', no 'Working on Link' banner."*  And his own diagnosis, which was right: *"a run on a
    // Link writes my message into the previously open chat and its reply is gated out."*
    //
    // The scenario he named: a chat on a Link, a run in it, switch to Main and back.
    {
        std::printf ("\n-- SP5: a run in a Link chat keeps its question AND its answer --\n");
        proc.setProjectName ("test project nafe aitch"); pumpMs (20);

        // A Main chat with one message of its own, so a leak into it is visible.
        A::selectRack (*ed, {}); pumpMs (30);
        A::newChat (*ed); pumpMs (30);
        A::send (*ed, "what compressors do I have?"); pumpMs (60);
        const auto mainChat = A::chatId (*ed);
        // BY CONTENT, NOT BY COUNT. There is no server here, so the send's own failure bubble lands
        // in this chat a moment later - and a count measured against that race decided this leg
        // twice with no product change between the runs. The question he asked is about WHOSE
        // message is in which chat, so that is what is asked.
        auto holds = [&] (const juce::String& chatIdIn, const juce::String& text)
        {
            auto* c = A::chat (*ed, chatIdIn);
            if (c == nullptr) return false;
            for (const auto& m : c->messages) if (m.content.contains (text)) return true;
            return false;
        };
        pumpMs (200);                                   // let the failed send settle, bubble and all
        check (mainChat.isNotEmpty() && holds (mainChat, "what compressors do I have?"),
               "SP5a PRECONDITION: a Main chat holding its own message");

        // Now the Kick: aimed at, with NO chat record yet - HIS STATE EXACTLY, and the state my
        // first cut of this leg got wrong. Selecting a Link with no chat holds it as PENDING and
        // leaves the Main chat open, so currentChatId is still Main when the send happens. Clearing
        // it here (as I first did) models a case he was never in, and the leg then passed on the
        // broken code - which is what the second red-first pass caught.
        A::pend (proc, kickUid);
        A::agentUserLine (*ed, "what EQs do I have?"); pumpMs (40);
        const auto linkChat = A::chatId (*ed);
        check (linkChat.isNotEmpty(),
               "SP5b the agent's first line MINTS the Link's chat before writing anything (RED as it "
               "stood: currentChatId was empty, so the store minted a record under an empty id - his "
               "log read \"EJChat: filed chat  under project ... on Link 88_7even Kick_2_01\")",
               linkChat.isEmpty() ? juce::String ("(empty id)") : linkChat);
        check ([&] { for (auto& c : A::chats (*ed)) if (c.id.isEmpty()) return false; return true; }(),
               "SP5c ...and NO record in the store has an empty id - nothing can be keyed to one, "
               "which is why the answer was gated out and the question surfaced elsewhere");
        {
            auto* lc = A::chat (*ed, linkChat);
            check (lc != nullptr && lc->linkUid == kickUid && lc->trackName == "test project nafe aitch",
                   "SP5d ...and it is the KICK's chat, under the open project",
                   lc == nullptr ? juce::String ("no record")
                                 : ("uid=" + lc->linkUid + " project=" + lc->trackName));
        }

        // The run's answer, written the way the bridge writes it when a run ends.
        {
            const juce::String answer = "You have 41 EQs - 12 I can set.";
            A::store (*ed).appendMessageToChat (linkChat, "assistant", answer);
            // switch to Main and back, which is the move that lost it
            A::openChat (*ed, mainChat); pumpMs (40);
            A::openChat (*ed, linkChat); pumpMs (40);
            auto* lc = A::chat (*ed, linkChat);
            bool hasQ = false, hasA = false;
            if (lc != nullptr)
                for (const auto& m : lc->messages)
                {
                    if (m.role == "user"      && m.content == "what EQs do I have?") hasQ = true;
                    if (m.role == "assistant" && m.content == answer)                hasA = true;
                }
            check (lc != nullptr && hasQ && hasA,
                   "SP5e the Link chat keeps its question AND its answer across a switch to Main and "
                   "back (RED as it stood: \"reopened, it shows only 'what EQs do I have?'\")",
                   lc == nullptr ? juce::String ("no record")
                                 : (juce::String (hasQ ? "question " : "NO question ")
                                    + (hasA ? "answer" : "NO answer")));
            auto* mc = A::chat (*ed, mainChat);
            juce::String mainRows;
            if (mc != nullptr)
                for (const auto& m : mc->messages)
                    mainRows << "[" << m.role << ": " << m.content.substring (0, 40) << "] ";
            check (mc != nullptr && ! holds (mainChat, "what EQs do I have?")
                   && ! holds (mainChat, answer) && holds (mainChat, "what compressors do I have?"),
                   "SP5f ...and the Link chat's question and answer are NOT in the Main chat, which "
                   "still has its own (RED as it stood: \"that question leaked into the Main chat\")",
                   mc == nullptr ? juce::String ("no record") : mainRows);
        }
        // ...and the target comes back with it
        A::openChat (*ed, linkChat); pumpMs (40);
        check (A::effectiveUid (*ed) == kickUid,
               "SP5g reopening the Link chat puts the target back on that Link",
               A::effectiveUid (*ed).isEmpty() ? juce::String ("(main)") : A::effectiveUid (*ed));
        check (A::banner (*ed).isNotEmpty(),
               "SP5h ...and the banner says which channel the chat is working on (RED as it stood: "
               "\"no Working on Link banner\")",
               A::banner (*ed).isEmpty() ? juce::String ("(no banner)") : A::banner (*ed));
    }

    // ---- DUP (18 Oct 2026, 18:35): "MY 'Add a compressor' SHOWS TWICE (sent once)" ------------
    //
    // His log settles the half that matters first: ONE `START goal="Add a compressor"` and ONE
    // POST /api/agent/start in that session, so B saw one request and the chat is drawing two. In
    // agent mode the composer's own send path returns before it appends anything (hook E5), so the
    // bubble has exactly one author: the client emits the goal as a "user" transcript line and the
    // bridge lands it in appendLocalUserBubble, which writes BOTH the on-screen row and the store's.
    // That is one of each - and then a reload has to put them back together without making two.
    std::printf ("\n-- DUP: one send, one bubble, before and after a reload --\n");
    {
        proc.setProjectName ("test project nafe aitch"); pumpMs (20);
        A::selectRack (*ed, kickUid); pumpMs (30);
        A::newChat (*ed); pumpMs (30);
        const juce::String goal = "Add a compressor";
        A::agentUserLine (*ed, goal); pumpMs (60);
        const auto chat = A::chatId (*ed);
        check (A::onScreen (*ed, goal) == 1,
               "DUP1 the agent's road draws the goal ONCE on screen",
               juce::String (A::onScreen (*ed, goal)) + " on screen");
        check (A::inStore (*ed, chat, goal) == 1,
               "DUP1b ...and writes it once to the chat's record",
               juce::String (A::inStore (*ed, chat, goal)) + " in the record");
        // THE RELOAD, which is where a store copy and a held copy meet. His session reloads this
        // chat constantly: openChannelByUid does it on every rack selection.
        A::openChat (*ed, chat); pumpMs (60);
        check (A::onScreen (*ed, goal) == 1,
               "DUP2 ...and after a reload it is STILL once - the store's copy and the processor's "
               "held copy are the same row, not two (RED as it stood: \"my 'Add a compressor' shows "
               "twice in the chat, sent once\")",
               juce::String (A::onScreen (*ed, goal)) + " on screen");
        check (A::inStore (*ed, chat, goal) == 1,
               "DUP2b ...and the record still holds one",
               juce::String (A::inStore (*ed, chat, goal)) + " in the record");
        // and twice more, because his session switches racks repeatedly
        A::selectRack (*ed, voxUid); pumpMs (30);
        A::selectRack (*ed, kickUid); pumpMs (30);
        A::openChat (*ed, chat); pumpMs (60);
        check (A::onScreen (*ed, goal) == 1 && A::inStore (*ed, chat, goal) == 1,
               "DUP3 ...and it does not grow a copy per switch",
               juce::String (A::onScreen (*ed, goal)) + " on screen, "
                 + juce::String (A::inStore (*ed, chat, goal)) + " in the record");
    }

    // ---- CARD (18 Oct 2026): A RUN COMES BACK AS A CARD, NOT AS ITS LAST SENTENCE -------------
    //
    // *"When I reopen a chat, an agent run comes back as just its last message in a plain bubble. It
    // should come back exactly as it looked live: the ASK card with every step, the 'You said'
    // lines, the read-back lines, the red failures, Details, Hide steps, Undo, and the closing.
    // Save the run card in the chat store and redraw it on reopen (finished runs only, chips shown
    // as answered, not tappable)."*  And his leg: finish a run, switch chat, switch back, reload,
    // and the card matches what was on screen.
    std::printf ("\n-- CARD: the run's card survives a switch, a switch back and a reload --\n");
    {
        proc.setProjectName ("test project nafe aitch"); pumpMs (20);
        A::selectRack (*ed, kickUid); pumpMs (30);
        A::newChat (*ed); pumpMs (30);
        const auto cardChat = A::chatId (*ed);

        // A finished run as the row holds it: four steps, an answered ask, a read-back, a failure
        // with a technical message behind Details, and a closing.
        juce::String snapshot;
        {
            echojay::agentui::RunView v;
            v.sessionId = "ag_card"; v.statusWord = "Done"; v.finished = true; v.modeBadge = "ASK";
            v.closing = "Added the CL 1B on the kick - threshold -15, ratio 4.";
            v.changeCount = 2; v.stepCount = 4; v.stepsOpen = true;
            using St = EJAgentClient::Step::Status;
            echojay::agentui::RunStep look;
            look.id = "tc_look"; look.label = "read the kick's rack"; look.status = (int) St::Done;
            look.detail = "2 slots";
            echojay::agentui::RunStep ask;
            ask.id = "tc_ask"; ask.label = "Which compressor?"; ask.status = (int) St::Done;
            ask.chips.add ("Tube-Tech CL 1B"); ask.chips.add ("the EchoJay Compressor");
            ask.answered = "You said: Tube-Tech CL 1B";
            echojay::agentui::RunStep add;
            add.id = "tc_add"; add.label = "add Tube-Tech CL 1B"; add.status = (int) St::Done;
            add.detail = "Added Tube-Tech CL 1B. Read back: threshold -15, ratio 4, attack 10.";
            add.canUndo = true; add.right = "applied";
            echojay::agentui::RunStep bad;
            bad.id = "tc_fail"; bad.label = "set the release"; bad.status = (int) St::Failed;
            bad.detail = "the release could not be set"; bad.modelDetail = "unknown position \"500\"";
            bad.hasDetails = true;
            v.steps = { look, ask, add, bad };
            v.showUndoAll = true; v.showGotIt = true;     // as a live finished run would have them
            snapshot = juce::JSON::toString (echojay::agentui::toJson (v), true);
        }
        A::store (*ed).appendMessageToChat (cardChat, "assistant",
                                            "Added the CL 1B on the kick - threshold -15, ratio 4.",
                                            {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, snapshot);
        check (A::storedCard (*ed, cardChat).isNotEmpty(),
               "CARD0 PRECONDITION: the record holds the run's card beside its prose");

        // ...switch away, switch back, and reload - his three moves.
        A::openChat (*ed, made[0].chatId); pumpMs (40);
        A::openChat (*ed, cardChat); pumpMs (60);
        A::openChat (*ed, cardChat); pumpMs (60);

        const auto* row = A::rowWithCard (*ed);
        check (row != nullptr,
               "CARD1 the reopened chat has a ROW with the run's card on it, not a bare bubble (RED as "
               "it stood: the snapshot died with the window, so the record held only the last sentence)");
        if (row != nullptr)
        {
            const auto v = A::viewOf (*ed, *row);
            check (v.steps.size() == 4,
                   "CARD2 ...with every step back", juce::String ((int) v.steps.size()) + " step(s)");
            bool said = false, readBack = false, failed = false, details = false;
            for (const auto& st : v.steps)
            {
                if (st.answered.contains ("You said: Tube-Tech CL 1B")) said = true;
                if (st.detail.contains ("Read back: threshold -15"))    readBack = true;
                if (st.status == (int) EJAgentClient::Step::Status::Failed) failed = true;
                if (st.hasDetails && st.modelDetail.isNotEmpty())        details = true;
            }
            check (said && readBack && failed && details,
                   "CARD3 ...the \"You said\" line, the read-back line, the red failure and its Details",
                   juce::String (said ? "said " : "NO said ") + (readBack ? "readback " : "NO readback ")
                     + (failed ? "failed " : "NO failed ") + (details ? "details" : "NO details"));
            check (v.closing.contains ("threshold -15, ratio 4"),
                   "CARD4 ...and the closing", v.closing.substring (0, 50));
            // FINISHED RUNS ONLY, CHIPS NOT TAPPABLE: the card is a record, so nothing on it offers
            // to act on a run that is over.
            check (v.fromRecord && v.finished && ! v.running && ! v.showStop && ! v.showRetry
                   && ! v.showUndoAll,
                   "CARD5 ...drawn as a RECORD: finished, and no Stop, Retry or Undo all - they would "
                   "act on a run that no longer exists");
            const auto L = echojay::agentui::layoutRun (v, 0, 0, juce::jmax (220, A::rowWidth (*ed)));
            int chipZones = 0, applyZones = 0, undoZones = 0, detailZones = 0;
            for (const auto& z : L.zones)
            {
                using H = echojay::agentui::RunHit;
                if (z.kind == H::Chip)       ++chipZones;
                if (z.kind == H::ApplyLine)  ++applyZones;
                if (z.kind == H::Undo)       ++undoZones;
                if (z.kind == H::Details)    ++detailZones;
            }
            check (chipZones == 0 && applyZones == 0 && undoZones == 0,
                   "CARD6 ...and NOTHING on it is pressable that would act: no chip, no Apply, no Undo",
                   "chips=" + juce::String (chipZones) + " apply=" + juce::String (applyZones)
                     + " undo=" + juce::String (undoZones));
            check (detailZones > 0,
                   "CARD7 ...while Details IS still pressable, because it is a question about the "
                   "picture and not about the run", juce::String (detailZones) + " Details zone(s)");
            check (echojay::agentui::checkRun (v, L, 0, juce::jmax (220, A::rowWidth (*ed))).isEmpty(),
                   "CARD8 ...and the whole card fits its column - the same self-check the live one passes",
                   echojay::agentui::checkRun (v, L, 0, juce::jmax (220, A::rowWidth (*ed))));
        }
        // and the record still holds exactly one copy of it after all that switching
        check (A::storedCard (*ed, cardChat).isNotEmpty() && A::inStore (*ed, cardChat, "Add a compressor") <= 1,
               "CARD9 ...and the switching added nothing to the record");
    }

    // ---- INT: A RUN THE WINDOW TOOK WITH IT (19 Oct 2026, Sean at 11:08) ---------------------
    //
    // *"I closed the plugin window mid-run (during wait for playback) and on reopen the run had
    // vanished; only my question bubble remained, no card. A run interrupted by closing the editor
    // (or Logic) must be saved and come back as its card with the steps so far and 'Stopped: the
    // plugin was closed mid-run', never disappear."*
    //
    // The store had the prose and NOT the card, which is exactly what he read. This drives the save
    // the destructor now calls, then reads the STORE - the thing a reopen reads - rather than the
    // editor's own list, which is what died with the window.
    std::printf ("\n== INT: an interrupted run is in the STORE as a stopped card, with its steps ==\n");
    {
        A::toChat (*ed);
        A::newChat (*ed);
        const auto chatId = A::chatId (*ed);
        A::send (*ed, "add a compressor to the kick");

        echojay::agentui::RunView live;
        live.sessionId = "ses_int"; live.runKey = "key_int";
        live.statusWord = "Working"; live.running = true; live.finished = false;
        live.playbackActive = true; live.playbackLine = "Listening... 3 of 8 s";
        {
            echojay::agentui::RunStep a;
            a.id = "st1"; a.label = "Added Tube-Tech CL 1B to slot 1"; a.right = "applied";
            a.status = (int) EJAgentClient::Step::Status::Done;
            live.steps.push_back (a);
            echojay::agentui::RunStep b;
            b.id = "st2"; b.label = "Waiting for playback";
            b.status = (int) EJAgentClient::Step::Status::Running;
            live.steps.push_back (b);
        }
        A::appendRow (*ed, "Working on the kick.",
                      juce::JSON::toString (echojay::agentui::toJson (live), true));

        auto& rows = A::msgs (*ed);
        A::stopCard (*ed, rows.back(), "Stopped: the plugin was closed mid-run.");

        auto* chat = A::chat (*ed, chatId);
        juce::String storedJson;
        if (chat != nullptr)
            for (const auto& m : chat->messages)
                if (m.role == "assistant" && m.agentJson.isNotEmpty()) storedJson = m.agentJson;
        check (storedJson.isNotEmpty(),
               "INT1 the interrupted run's CARD is in the store  (RED as it stood: only his question "
               "bubble was, and the card went with the window)",
               storedJson.isEmpty() ? juce::String ("no agentJson on any assistant row")
                                    : juce::String ("present"));
        const auto back = echojay::agentui::fromJson (juce::JSON::parse (storedJson));
        check (back.closing == "Stopped: the plugin was closed mid-run.",
               "INT2 ...marked stopped, in the words he asked for", back.closing);
        check (back.steps.size() == 2
               && back.steps[0].label.contains ("Tube-Tech CL 1B")
               && back.steps[1].label.contains ("playback"),
               "INT3 ...with the steps as far as they got",
               juce::String ((int) back.steps.size()) + " step(s)");
        check (back.finished && ! back.running && ! back.playbackActive,
               "INT4 ...and it comes back finished, not spinning: no live playback on a dead run");
    }

    // ---- THE MODE HINTS SAY WHAT THIS BUILD DOES (19 Oct 2026, his 17:20 run) ----------------------
    //
    // *"The Ask mode hint reads 'small moves just happen; adding, removing or touching another track
    // waits for your Apply.' Under Sean's rule Go approves every step of a plan and there's no second
    // Apply, so reword to match."*
    //
    // The old line described the approval model this build had before the plan card: a per-op gate
    // with an Apply behind it. Go is the approval now and it covers every step, so the line sent him
    // looking for a button that is gone. Here rather than in agent_client_guard because the hint is
    // the EDITOR's and this guard already links it.
    std::printf ("== THE MODE HINTS ==\n");
    {
        const auto askLine = EchoJayTabStripTestAccess::approvalLine (EchoJayEditor::AgentApproval::Ask);
        check (! askLine.containsIgnoreCase ("Apply"),
               "AH1 the Ask hint promises no second Apply  (RED as it stood: \"...waits for your "
               "Apply.\", a button Go replaced)", askLine);
        check (askLine.containsIgnoreCase ("Go"),
               "AH1b ...and it names the control that DOES approve the plan", askLine);
        const auto offLine = EchoJayTabStripTestAccess::approvalLine (EchoJayEditor::AgentApproval::Off);
        check (offLine.containsIgnoreCase ("Build"),
               "AH2 and Off still names Build, which it does have", offLine);
    }

    // ---- CL: STAGE A OF THE CHAT LOG (his ruling of 21 Oct 2026) -----------------------------
    //
    // *"One stored log per chat, where every entry (user message, agent card, notice, Stopped line)
    // gets its chat id, a monotonic order number and a stable id at the moment it's created."*
    //
    // Stage A writes the log and nothing reads it. So what can be held now is exactly the two
    // properties the later stages will rest on: a place that is ALLOCATED rather than computed, and
    // an identity that is the RUN'S KEY for a card. His five cases are named against each leg.
    std::printf ("== CL: the stored log's place and identity (stage A) ==\n");
    {
        WsChat c; c.id = "chatA"; c.title = "Mix bus";

        WsMessage u1; u1.role = "user"; u1.content = "add a compressor";
        EchoJayWorkspace::stampLogEntry (c, u1); c.messages.push_back (u1);
        WsMessage u2; u2.role = "user"; u2.content = "add a delay";
        EchoJayWorkspace::stampLogEntry (c, u2); c.messages.push_back (u2);

        check (u1.seq == 1 && u2.seq == 2,
               "CL1 seq is allocated in creation order, from the CHAT's allocator - not from the "
               "size of a list that can be filtered, merged or half-loaded",
               juce::String (u1.seq) + ", " + juce::String (u2.seq));
        check (u1.entryId.isNotEmpty() && u2.entryId.isNotEmpty() && u1.entryId != u2.entryId,
               "CL2 ...and each has its own id at creation");
        check (c.nextSeq == 3, "CL3 the allocator moved past them", juce::String (c.nextSeq));

        // AN AGENT CARD'S ID IS THE RUN'S KEY - the identity that survives a replay and a reopen,
        // which is what the per-window integer could not do (his 14:15 chips).
        WsMessage card; card.role = "assistant";
        card.agentJson = R"({"runKey":"3afc6930-4c62-45a3","sessionId":"ag_1","steps":[]})";
        EchoJayWorkspace::stampLogEntry (c, card); c.messages.push_back (card);
        check (card.entryId == "3afc6930-4c62-45a3",
               "CL4 RED-FIRST: an agent card's id IS the run's key, so the live run updates its own "
               "entry by id instead of being re-identified by an integer a replay reissues",
               card.entryId);
        check (card.seq == 3, "CL5 ...and it takes its place in the same order as everything else",
               juce::String (card.seq));

        // TWO WINDOWS: both append through the same allocator, so they cannot mint the same number.
        WsMessage w1; w1.role = "user"; w1.content = "from window one";
        WsMessage w2; w2.role = "user"; w2.content = "from window two";
        EchoJayWorkspace::stampLogEntry (c, w1);
        EchoJayWorkspace::stampLogEntry (c, w2);
        check (w1.seq != w2.seq && w1.seq == 4 && w2.seq == 5,
               "CL6 two appends cannot collide - the allocator is the chat's, which is his "
               "\"two windows open\" case",
               juce::String (w1.seq) + " vs " + juce::String (w2.seq));
    }
    {
        // MIGRATION (his fifth case): rows written before today have no place and no id. THE FILE
        // ORDER IS THE TRUTH, so they are stamped in the order they are already in.
        WsChat old; old.id = "chatOld";
        for (const char* t : { "first", "second", "third" })
        { WsMessage m; m.role = "user"; m.content = t; old.messages.push_back (m); }
        // ...and one of them is a run that was live across the upgrade.
        WsMessage card; card.role = "assistant";
        card.agentJson = R"({"runKey":"live-run-key","steps":[]})";
        old.messages.push_back (card);

        const int stamped = EchoJayWorkspace::migrateChatLog (old);
        check (stamped == 4, "CL7 every unstamped row is migrated", juce::String (stamped));
        check (old.messages[0].seq == 1 && old.messages[1].seq == 2
                 && old.messages[2].seq == 3 && old.messages[3].seq == 4,
               "CL8 ...in the order the file already had, which is the only order that is evidence",
               juce::String (old.messages[0].seq) + juce::String (old.messages[1].seq)
                 + juce::String (old.messages[2].seq) + juce::String (old.messages[3].seq));
        check (old.messages[3].entryId == "live-run-key",
               "CL9 ...and a run that was live across the upgrade keeps its KEY as its id, so it is "
               "still found", old.messages[3].entryId);
        check (old.nextSeq == 5, "CL10 the allocator is set past the last row", juce::String (old.nextSeq));

        // IDEMPOTENT: running it again stamps nothing and moves nothing.
        const auto seqsBefore = juce::String (old.messages[0].seq) + juce::String (old.messages[3].seq);
        const int again = EchoJayWorkspace::migrateChatLog (old);
        check (again == 0, "CL11 a second migration stamps NOTHING - it runs on every parse, so it "
                           "has to be free and harmless", juce::String (again));
        check (seqsBefore == juce::String (old.messages[0].seq) + juce::String (old.messages[3].seq),
               "CL12 ...and renumbers nothing: a place, once recorded, is never recomputed");
    }
    {
        // A HALF-MIGRATED CHAT: some rows stamped (a live append landed first), the rest not. The
        // new rows must not collide with the stamped ones - which is what the max() is for.
        WsChat half; half.id = "chatHalf";
        WsMessage a; a.role = "user"; a.seq = 7; a.entryId = "kept"; half.messages.push_back (a);
        WsMessage b; b.role = "user"; half.messages.push_back (b);
        const int stamped = EchoJayWorkspace::migrateChatLog (half);
        check (stamped == 1 && half.messages[0].seq == 7 && half.messages[0].entryId == "kept",
               "CL13 an already-stamped row is left exactly as it is", juce::String (stamped));
        check (half.messages[1].seq == 8 && half.nextSeq == 9,
               "CL14 ...and the unstamped one is given a place PAST it, never on top of it",
               juce::String (half.messages[1].seq) + ", next " + juce::String (half.nextSeq));
    }

    // ---- A WINDOWLESS RUN'S CARD REACHES THE STORED LOG (his 09:24 fault, 22 Oct 2026) ------
    //
    // *"a run with no window open does NOT come back in the chat ... the build WAS on the rack, but
    //  the chat showed only my message: no agent card, no steps, no closing, no chips, no Undo all."*
    //
    // WHY THESE LEGS USE THEIR OWN PROCESSOR AND NO EDITOR: that is the fault's condition. Until
    // today the chat store was a MEMBER OF THE EDITOR, so with no window there was no store to
    // append to - the card lived only on the processor's replay list and the first chat switch
    // dropped it ("EJStores: dropped 1 unsynced row(s) that belong to chat 1792657430720"). Every
    // leg below is red against that build for the most basic reason: it could not be written.
    std::printf ("== A RUN WITH NO WINDOW: its card in the STORED log (his 09:24 fault) ==\n");
    {
        EchoJayProcessor pw;                       // no createEditor() anywhere in this block
        WsChat c; c.id = "chatNoWindow"; c.title = "Mix bus"; c.trackName = "test project nafe aitch";
        pw.workspace.addChat (c);

        const juce::String key = "7272e523-0a37-40ce-a0bf-897a3ea0837e";   // his run's own key
        echojay::agentui::RunView v;
        v.runKey  = key;
        v.opening = "Building a mix bus.";
        { echojay::agentui::RunStep s; s.label = "add bx_townhouse Buss Compressor";
          s.status = (int) EJAgentClient::Step::Status::Done; v.steps.push_back (s); }
        EchoJayProcessor::ChatEntry e;
        e.role = "assistant";
        e.agentJson = juce::JSON::toString (echojay::agentui::toJson (v), true);
        pw.appendChatEntry (std::move (e), "chatNoWindow", key);

        auto* stored = pw.workspace.findChatById ("chatNoWindow");
        const WsMessage* card = nullptr;
        if (stored != nullptr)
            for (const auto& m : stored->messages)
                if (m.entryId == key) card = &m;
        check (card != nullptr && card->agentJson.isNotEmpty(),
               "NW1 a card stamped with NO WINDOW OPEN is in the chat's STORED log, found by the run's "
               "own key  (RED as it shipped: the store was the editor's, so a windowless run had "
               "nowhere to write and his reopened chat showed only his message)",
               stored == nullptr ? juce::String ("the store lost the chat itself")
                                 : "rows " + juce::String ((int) stored->messages.size()));
        check (card != nullptr && card->seq > 0,
               "NW2 ...and the STORE placed it: a row the processor could not number (no window, so no "
               "seq allocator) still gets its place from the chat's own allocator, which is what "
               "\"the store appends first\" buys",
               card != nullptr ? "seq " + juce::String (card->seq) : juce::String ("no card"));
        check (card != nullptr
               && echojay::agentui::fromJson (juce::JSON::parse (card->agentJson)).steps.size() == 1,
               "NW3 ...carrying the STEPS, not just a sentence - \"no agent card, no steps\" is what he "
               "saw, and the snapshot is what redraws them",
               card != nullptr ? juce::JSON::toString (juce::JSON::parse (card->agentJson), true).substring (0, 90)
                               : juce::String ("no card"));

        // The run goes on: a later round adds its closing. ONE row, updated by id - not a second one.
        v.closing = "Built a mix bus: compressor, EQ, limiter.";
        { echojay::agentui::RunStep s2; s2.label = "add bx_clipper";
          s2.status = (int) EJAgentClient::Step::Status::Done; v.steps.push_back (s2); }
        pw.storeAgentCard ("chatNoWindow", key,
                           juce::JSON::toString (echojay::agentui::toJson (v), true), v.closing);
        stored = pw.workspace.findChatById ("chatNoWindow");
        int withKey = 0; const WsMessage* grown = nullptr;
        if (stored != nullptr)
            for (const auto& m : stored->messages)
                if (m.entryId == key) { ++withKey; grown = &m; }
        check (withKey == 1 && grown != nullptr && grown->content == v.closing
               && echojay::agentui::fromJson (juce::JSON::parse (grown->agentJson)).steps.size() == 2,
               "NW4 a later round UPDATES that one row by its id - one run is one row, with its closing "
               "and both steps  (the DUP2 fault's shape: two writers for one card is how a line showed "
               "twice, and an id is the only handle that works before the prose exists)",
               "rows with that key " + juce::String (withKey)
               + (grown != nullptr ? ", text \"" + grown->content.substring (0, 40) + "\"" : ""));

        // ...AND NOTHING IS INVENTED. A card for a chat the store does not hold must not mint one:
        // an invented chat has no project and no channel, which is his rule 1's exact failure.
        const int chatsBefore = (int) pw.workspace.getChats().size();
        pw.storeAgentCard ("chatTheStoreNeverHeardOf", "some-other-key", "{\"runKey\":\"x\"}", "hi");
        check ((int) pw.workspace.getChats().size() == chatsBefore,
               "NW5 and a card whose chat the store does not hold invents NO chat - an invented one has "
               "no project and no channel, which is Ungrouped (his rule 1, 18 Oct)",
               "chats " + juce::String (chatsBefore) + " -> "
               + juce::String ((int) pw.workspace.getChats().size()));

        // THE OTHER DIRECTION of "only a card": a plain row must NOT be store-appended by the stamp.
        // The editor's send path appends it there first ("THE STORE GOES FIRST"), so doing it here
        // too would be two writers for one row - the duplicate his 16:00 report was about.
        auto* before = pw.workspace.findChatById ("chatNoWindow");
        const int rowsBefore = before != nullptr ? (int) before->messages.size() : -1;
        EchoJayProcessor::ChatEntry plain; plain.role = "user"; plain.content = "build a mix bus";
        pw.appendChatEntry (std::move (plain), "chatNoWindow", {});
        auto* after = pw.workspace.findChatById ("chatNoWindow");
        check (after != nullptr && (int) after->messages.size() == rowsBefore,
               "NW6 a PLAIN row is not store-appended by the stamp - its writer is the send path, which "
               "already appends to the store first; two writers for one row is the duplicate fault",
               "stored rows " + juce::String (rowsBefore) + " -> "
               + juce::String (after != nullptr ? (int) after->messages.size() : -1)
               + ", replay list " + juce::String ((int) pw.chatHistory.size()));
    }

    std::printf ("==== chat_store_guard: %s (%d assertion(s) failed, %d passed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures, passes);
    return failures == 0 ? 0 : 1;
}
