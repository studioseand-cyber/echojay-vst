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
    static WsChat* chat (EchoJayEditor& e, const juce::String& id) { return e.workspace.findChatById (id); }
    static const std::vector<WsChat>& chats (EchoJayEditor& e) { return e.workspace.getChats(); }
    static EchoJayWorkspace& store (EchoJayEditor& e) { return e.workspace; }
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

    std::printf ("==== chat_store_guard: %s (%d assertion(s) failed, %d passed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures, passes);
    return failures == 0 ? 0 : 1;
}
