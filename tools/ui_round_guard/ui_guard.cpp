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
#include "EchoJayFileLog.h"   // 21t-e: the guard finds the log the way the WRITER does
#include "EedCompressorProcessor.h"   // 21t-d: force-link the built-in registrars the wiring leg racks
#include "SurgicalEqProcessor.h"
#include "EchoJayLevelTally.h"   // 21t-d (c)
#ifndef UI_GUARD_NO_HELPERS
#include "BorrowStatusText.h"
#include "AskShelfLayout.h"
#include "ChatBubbleStyle.h"    // hurdle 1 item 3
#include "NotDialableText.h"    // hurdle 1 item 3
#endif
#include <cstdio>
#include <typeinfo>
// The request body as the shipping client builds it (the same pin groups_guard uses).
struct EchoJayAPIRequestPin { static juce::String body (EchoJayAPI& a, const juce::StringArray& r, const juce::StringArray& c, const juce::String& sys, const juce::String& mb) { return a.buildChatRequestBody (r, c, sys, mb); } };
struct EchoJayAlignTestAccess { static void setLinks (EchoJayProcessor& p, std::vector<EchoJayProcessor::LinkSlotInfo> v)
    {   // 22 Sep 2026: STOP the processor's 1 Hz timer first - refreshLinkRegistry() rebuilds linkSlotInfos from the real
        // registry (empty under the isolated home) and would wipe the injected rows the moment it fires (a longer run is
        // all it takes). The guard owns this input; the product path is untouched.
        static_cast<juce::Timer&> (p).stopTimer(); p.linkSlotInfos = std::move (v); } };   // 21m ruling 1: the roster's Link rows
struct EchoJayRosterTestAccess { static EchoJayEditor::LastLinkActiveCmd last (EchoJayEditor& e) { return e.lastLinkActiveCmd_; } };
// 21t-a: the H1 leg has to dismiss the onboarding prompts (the overlay legitimately covers the whole editor
// while a prompt is up) and then put the instance back EXACTLY as it found it - item 7's leg asserts on
// isChannelChosen(), and dismissing IS an answer. A guard must not move the ground a later leg stands on.
struct EchoJayPromptTestAccess
{
    struct Saved { bool chanType = false, genre = false, project = false, chosen = false; };
    static Saved save (EchoJayProcessor& p)
    { return { p.channelTypePromptDismissed, p.genrePromptDismissed, p.projectPromptDismissed, p.channelChosen }; }
    static void restore (EchoJayProcessor& p, const Saved& s)
    {
        p.channelTypePromptDismissed = s.chanType; p.genrePromptDismissed = s.genre;
        p.projectPromptDismissed = s.project;      p.channelChosen = s.chosen;
    }
};

// 21t-a H3: a borrow SESSION the guard owns. The engage path needs a running Link (ring, lease, sidecar) and
// none exists under an isolated state home, so the state it produces is set here directly. What is under test is
// unchanged shipping code: what onMasterWet does when borrowHostIfActiveFor() answers yes - exactly the state
// every rack on screen is in, and the state H3 was wrong in.
// 21t-c: a LATCHED meter frame the guard owns. No Link process publishes under an isolated state home, and the
// latch is the same store the product reads when a member is not publishing right now - so a member WITH a frame
// is representable without faking the shared memory.
struct EchoJayLinkFrameTestAccess
{
    static void set (EchoJayProcessor& p, const juce::String& uid, float mom, float sht, float integ,
                     float tpMax, float shortTP)
    {
        LinkMeterFrame f;
        f.momentary = mom; f.shortTerm = sht; f.integrated = integ;
        f.truePeakMax = tpMax; f.shortTermTP = shortTP;
        p.linkLastGoodFrame_[uid] = EchoJayProcessor::LinkGoodFrame { f, juce::Time::getMillisecondCounter(), true };
        // 21t-i: ...and THE STORED RECORD, through the product's own frame feed. Every block line is composed from
        // the record now, so a fixture that seeds only the latch is seeding the surface that no longer composes
        // anything. Same frame, both places, one call - the way the 1 Hz tick does it.
        p.updateLevelRecordFromFrame (uid, f);
    }
    static void clear (EchoJayProcessor& p)
    {
        p.linkLastGoodFrame_.clear();
        // The RECORDS go with it: "known state in, known state out" has to include the thing the blocks read, or
        // one leg's figures become the next leg's fixture.
        for (const auto& kv : std::map<juce::String, echojay::LevelRecord> (p.levelRecordByUid_))
            p.levelRecordByUid_.erase (kv.first);
    }
};

struct EchoJayBorrowTestAccess
{
    static ChainHost& engage (EchoJayProcessor& p, const juce::String& uid)
    {
        static_cast<juce::Timer&> (p).stopTimer();   // the 1 Hz tick would tear a fixture borrow down
        if (p.borrowHost_ == nullptr) p.borrowHost_ = std::make_unique<ChainHost> (ChainHost::Mode::Borrowed);
        p.borrowSession_.uid = uid;
        p.borrowSession_.active.store (true, std::memory_order_relaxed);
        return *p.borrowHost_;
    }
    static void release (EchoJayProcessor& p)
    {
        p.borrowSession_.active.store (false, std::memory_order_relaxed);
        p.borrowSession_.uid.clear();
    }
    // ---- 21t-b R2 ----
    static void queuePush (EchoJayProcessor& p, int seq, const juce::String& id, const juce::String& op)
    {
        EchoJayProcessor::BorrowPendingPush b;
        b.seq = seq; b.id = id; b.op = op;
        p.borrowPendingPushes_.push_back (b);
    }
    static int  pending (EchoJayProcessor& p) { return (int) p.borrowPendingPushes_.size(); }
    static int  parked  (EchoJayProcessor& p, const juce::String& uid)
    {
        auto it = p.borrowParkedPushes_.find (uid);
        return it == p.borrowParkedPushes_.end() ? 0 : (int) it->second.size();
    }
    static juce::String banner (EchoJayProcessor& p) { return p.borrowStickyBanner_; }
    static void clearPending (EchoJayProcessor& p) { p.borrowPendingPushes_.clear(); }
};

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
    static std::vector<juce::String> rosterRows (EchoJayEditor& e) { return e.rosterAddresses(); }   // 21t-k item 5
    // 21t-l: the header undo pair - the path that cannot be swallowed by a host.
    static juce::String undoTip (EchoJayEditor& e) { return e.undoHdrBtn.getTooltip(); }
    static bool undoEnabled (EchoJayEditor& e) { return e.undoHdrBtn.isEnabled(); }
    static void refreshUndo (EchoJayEditor& e) { e.refreshUndoButtons(); }
    static void send (EchoJayEditor& e, const juce::String& t) { e.sendChatMessage (t); }
    static void refreshPanel (EchoJayEditor& e) { e.refreshChainPanelForView (true); }   // 21m ruling 3 (keep-level grey-out)
    // ---- 21o (23 Sep 2026) ----
    using Geom = EchoJayEditor::StripGeom;
    using Tb   = EchoJayEditor::Tab;
    static void toTab (EchoJayEditor& e, Tb t) { e.switchToTab (t, true); }
    static Tb tabChat() { return EchoJayEditor::Tab::Chat; }
    static Tb tabSettings() { return EchoJayEditor::Tab::Settings; }
    static void toLinkTab (EchoJayEditor& e) { e.switchToTab (EchoJayEditor::Tab::Link, true); e.resized(); }
    static void measureOnly (EchoJayEditor& e) { e.measureLinkStrips(); }   // the rows are injected between these two
    static const std::vector<Geom>& geom (EchoJayEditor& e) { return e.linkStripGeom_; }
    static bool replyAllowed (EchoJayEditor& e) { return e.chatReplyControlsAllowed(); }
    static void reattach (EchoJayEditor& e) { e.reattachLoopPills(); }   // 21r item 1
    static bool channelPrompt (EchoJayEditor& e) { return e.shouldShowChannelPrompt(); }   // 21r item 7
    // ---- 21s-a ----
    static juce::Component& replyLayer (EchoJayEditor& e) { return e.replyLayer; }                 // F1
    static juce::TextButton& chip (EchoJayEditor& e, int i) { return e.resultChipBtns[(size_t) i]; }
    static juce::TextButton& buildBtn (EchoJayEditor& e, int i) { return e.chainBuildBtns[(size_t) i]; }
    static int  activeBuilds (EchoJayEditor& e) { return e.activeChainBuildBtns; }                     // H1
    static void setActiveBuilds (EchoJayEditor& e, int n) { e.activeChainBuildBtns = n; }
    static void targetGroup (EchoJayEditor& e, const juce::String& id) { e.setChatTargetGroup (id); }   // F2
    static juce::String targetLabel (EchoJayEditor& e) { return e.chatTargetLabel(); }
    static juce::TextButton& scanTrigger (EchoJayEditor& e) { return e.scanBtn; }                  // F4
    static juce::TextButton& viewAllTrigger (EchoJayEditor& e) { return e.viewAllPluginsBtn; }     // F4
    static bool pillEligible (EchoJayEditor& e) { return e.targetPillEligible(); }                 // F1-rest
    static bool panelVisibleOn (EchoJayEditor& e) { return e.assistantSidebarVisible(); }          // F1 revised
    static juce::TextEditor& composer (EchoJayEditor& e) { return e.chatInput; }
    static juce::String chatId (EchoJayEditor& e) { return e.currentChatId; }                        // 21s-b contract
    static void setChatId (EchoJayEditor& e, const juce::String& id) { e.currentChatId = id; }
    static juce::String channelIdentity (EchoJayEditor& e) { return e.turnChannelIdentity(); }
    static juce::String injections (EchoJayEditor& e) { return e.standardChainInjections ("what is this chain", true, nullptr, {}); }
    static void newChat (EchoJayEditor& e) { e.createNewChat ({}); }
    static void masterWet (EchoJayEditor& e, float v) { e.chainListPanel.onMasterWet (v); }        // F5
    static juce::String viewUid (EchoJayEditor& e) { return e.chainViewUid(); }
    static void sendChainWet (EchoJayEditor& e, const juce::String& uid, float v) { e.sendLinkMasterWetCommand (uid, v); }
    static Tb tabVis()   { return EchoJayEditor::Tab::Visualisation; }
    static Tb tabComp()  { return EchoJayEditor::Tab::Compare; }
    static Tb tabMeters(){ return EchoJayEditor::Tab::Meters; }
    static std::vector<EchoJayEditor::ScanMenuItem> menuItems() { return EchoJayEditor::scanMenuItems(); }   // F4
    static Tb tabChain() { return EchoJayEditor::Tab::Chain; }
    static Tb tabLink()  { return EchoJayEditor::Tab::Link; }
    static juce::String banner (EchoJayEditor& e) { e.refreshChannelBannerCache(); return e.chanBannerText_; }
    // ---- 21t-a (25 Sep 2026) ----
    using BannerItem = EchoJayEditor::BannerMenuItem;
    static std::vector<BannerItem> bannerItems (EchoJayEditor& e) { return e.channelBannerMenuItems(); }   // H2a
    static void openChannel (EchoJayEditor& e, const juce::String& uid) { e.openChannelByUid (uid); }      // H2b
    static juce::String projName (EchoJayEditor& e) { return e.newChatProjectName(); }                     // H2c
    // ---- 21t-b R1 ----
    // 21t-e: the guard drives the SHARED rule, the way the menu and a strip click do. selectViewOnly exists for
    // the legs that are about the view half alone (H3's "which rack is on screen").
    static void selectRack (EchoJayEditor& e, const juce::String& uid) { e.selectRack (uid); }
    static void selectViewOnly (EchoJayEditor& e, const juce::String& uid) { e.selectRackForView (uid); }
    /** Every leg starts here and leaves it here: main chat, view unpinned, no group, no inline editor. */
    static void knownState (EchoJayEditor& e, EchoJayProcessor& p)
    {
        e.chainListPanel.closeAllEditors();
        if (p.chatTargetGroupId.isNotEmpty()) e.setChatTargetGroup ({});
        p.chatTargetLinkUid.clear(); p.chatTargetLinkName.clear();
        p.pendingChannelUid.clear();
        e.unpinRackView();
    }
    static juce::String workingOn (EchoJayEditor& e) { return e.workingOnUid(); }
    static void setBuildJson (EchoJayEditor& e, int i, const juce::String& j) { e.chainBuildJsons[(size_t) i] = j; }
    // ---- 21t-e ----
    static bool  pendingPreGain (EchoJayEditor& e) { return e.pendingLinkPreGain_.valid; }
    static juce::String stripLabel (const juce::String& n, const juce::StringArray& sibs) { return EchoJayEditor::collapsedStripLabel (n, sibs); }
    static bool  inlineEditorOpen (EchoJayEditor& e) { return e.chainListPanel.hasInlineEditor(); }
    static juce::String pill (EchoJayEditor& e) { return e.chatTargetLabel(); }
    static void applyEdit (EchoJayEditor& e, int msgIdx) { e.applyChainEditFromMsg (msgIdx); }
    static void unpinView (EchoJayEditor& e) { e.unpinRackView(); }
    static bool pinned (EchoJayEditor& e) { return e.viewRackPinned_; }
    // ---- 21t-c ----
    static juce::String groupLevels (EchoJayEditor& e) { return e.buildGroupLevelsContext(); }        // the block itself
    // 21t-g (6): the single-track block, and the composed body it rides in.
    static juce::String trackLevels (EchoJayEditor& e, const juce::String& uid = {}) { return e.buildTrackLevelsContext (uid); }
    // what the request's `channel` field carries for this turn - materialContextName(), the one the server matches
    static juce::String materialName (EchoJayEditor& e) { return e.materialContextName (e.mainContextLabel()); }
    static juce::String body (EchoJayEditor& e, const juce::String& msg, const juce::String& uid)
    { juce::StringArray mf; return e.testAssembleChainInjections (msg, uid, &mf); }
    static int calibFromChain (EchoJayEditor& e, const juce::String& uid, const juce::var& chain)
    { return e.startCalibrationFromChain (uid, chain); }
    // 21t-h: THE REAL BUILD PATH, the one the button calls. No guard drove it end to end, which is exactly why the
    // premature start shipped: every leg had called startCalibrationFromChain directly, on a rack already built.
    static void buildToLink (EchoJayEditor& e, const juce::String& uid, const juce::String& chainJson)
    { e.sendChainToLink (uid, chainJson); }
    // 21t-h (3): the REAL reply route, the one a chat turn comes back on.
    static void reply (EchoJayEditor& e, const juce::String& text, const juce::String& chatId = {})
    { e.handleChatReply (text, true, chatId, {}, {}, -1); }
    // 21t-i re-cut: the same route WITH the turn's target uid, which is what an ops-free calibration needs in order
    // to reach the rack the turn was about.
    static void replyFor (EchoJayEditor& e, const juce::String& text, const juce::String& uid)
    { e.handleChatReply (text, true, {}, uid, {}, -1); }
    static juce::String editDataOf (EchoJayEditor& e, int i)
    { return i >= 0 && i < (int) e.chatMessages.size() ? e.chatMessages[(size_t) i].editData : juce::String(); }
    // ---- 21t-d wiring ----
    // 30 Sep 2026: startCalibrationFromOps now carries the PURPOSE - the ops road was where a build lost it and
    // ran as an ask. This accessor keeps the ask meaning it always had.
    static int  calibFromOps (EchoJayEditor& e, const juce::String& uid, const juce::var& ops) { return e.startCalibrationFromOps (uid, ops, echojay::CalibLoop::Purpose::askRung); }
    static void calibTick (EchoJayEditor& e, const juce::String& uid) { e.calibTickAndPost (uid); }
    // 21t-i: THE EDITOR'S OWN TICK. The old legs called calibTickAndPost directly, which is exactly why none of
    // them could see that nothing in the product called it.
    static void tick (EchoJayEditor& e) { e.timerCallback(); }
    static juce::String panelStatus (EchoJayEditor& e) { return e.chainListPanel.statusText; }
    // ---- 21t-d ----
    static LinkMeterFrame stripFrame (EchoJayEditor& e, const juce::String& addr, int regIdx)
    {
        bool fresh = false; float dim = 0.0f;
        e.ingestLinkStripFrame (addr, regIdx, true, juce::Time::getMillisecondCounter(), fresh, dim);
        return e.linkStripStates_[addr].frame;
    }
    static int levelMatch (EchoJayEditor& e, const juce::var& members) { return e.applyGroupLevelMatch (members); }
    static void addChat (EchoJayEditor& e, const WsChat& c) { e.workspace.addChat (c); }
    static WsChat* chat (EchoJayEditor& e, const juce::String& id) { return e.workspace.findChatById (id); }
    static void openChat (EchoJayEditor& e, const juce::String& id) { e.loadChatFromWorkspace (id); }
    // The sidebar's own folder rows, from the shipped row builder: "<folder id>|<label>".
    static juce::StringArray folders (EchoJayEditor& e)
    {
        e.sidebarModel->refreshRows (e.workspace.getChats(), e.workspace.getAlbums(), e.workspace.getReviews(),
                                     e.workspace.getPinnedProjects(), e.collapsedAlbums, e.currentChatId);
        juce::StringArray out;
        for (const auto& r : e.sidebarModel->rows)
            if (r.kind == EchoJayEditor::ChatSidebarModel::Row::Kind::ChannelHeader)
                out.add (r.id + "|" + r.label);
        return out;
    }
    static bool stripSelected (EchoJayEditor& e, const juce::String& addr) { return e.linkSelection_.count (addr) > 0; }
    static juce::TextButton& applyBtn (EchoJayEditor& e, int i) { return e.editApplyBtns[(size_t) i]; }
    // ---- 21t-i: the EDIT-CARD Apply buttons, by the message they belong to --------------------------------
    // The card's height decides whether the button is laid out at all, so the guard reads BOTH: a card with no
    // height is the defect Sean hit (the level-match lines rendered and there was nothing to press).
    static int editCardH (EchoJayEditor& e, int i)
    { return i >= 0 && i < (int) e.chatMessages.size() ? e.editCardHeight (e.chatMessages[(size_t) i]) : -1; }
    static int applyBtnCount (EchoJayEditor& e) { return e.activeEditApplyBtns; }
    static int applyBtnMsg (EchoJayEditor& e, int i) { return e.editApplyMsgIdx[(size_t) i]; }
    static int msgCount (EchoJayEditor& e) { return (int) e.chatMessages.size(); }
    // ---- 21t-i: the stored level record ------------------------------------------------------------------
    static juce::String levelTokens (EchoJayEditor& e, const juce::String& uid)
    { juce::String n; float t = 0.0f; return e.levelsTokensFor (uid, &n, &t); }
    static juce::TextButton& undoHdr (EchoJayEditor& e) { return e.undoHdrBtn; }
    static juce::TextButton& redoHdr (EchoJayEditor& e) { return e.redoHdrBtn; }
    static juce::TextButton& collapseBtn (EchoJayEditor& e) { return e.chatCollapseBtn; }
    static juce::Component&  chainPanelOf (EchoJayEditor& e) { return e.chainListPanel; }
    static void setChainsMode (EchoJayEditor& e, bool on) { e.setChainSidebarMode (on); }
    static juce::Array<juce::Rectangle<int>> headerRow (EchoJayEditor& e)
    {   // the tab-row controls the undo pills must sit right of
        juce::Array<juce::Rectangle<int>> r;
        for (juce::Component* c : { (juce::Component*) &e.captureBtn, (juce::Component*) &e.scanBtn, (juce::Component*) &e.headerNewChatBtn, (juce::Component*) &e.settingsBtn })
            if (c->isVisible() && c->getWidth() > 2) r.add (c->getBounds());
        return r;
    }
    static int  targetWidth (EchoJayEditor& e) { return e.chatTargetChannelWidth(); }   // 21n ruling 1b
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
    // MOVED AHEAD OF THE ABORT (21t-d, 25 Sep 2026): ui_guard aborts immediately after the (13) legs in the
    // scope below, and everything after that point never ran - (12c), (12d), (11) and (9) were being skipped,
    // not passing. They are ordinary legs with their own processors, so they run here instead.
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
    std::printf ("== (12) 23 Sep 2026 (21o): roster selection, group-strip parity, chat-reply controls, the undo pair and ONE tooltip ==\n");
        // ---- (12a) the selection model ----
        proc.pendingChannelUid = "lnk_01";                       // A is the working Link
        ed->clearRosterSelection();
        const juce::ModifierKeys plain (0), cmd (juce::ModifierKeys::commandModifier);
        const bool consumedCmd = ed->applyRosterSelectionClick ("lnk_02", false, false, cmd);
        { const auto sel = ed->rosterSelection();
          check (consumedCmd && sel.size() == 2 && sel.count ("lnk_01") && sel.count ("lnk_02"), "(12a) click A, Cmd-click B -> the selection is A AND B (the first Cmd-click seeds from the working strip)  (RED as it stood: B alone)", juce::String ((int) sel.size())); }
        ed->applyRosterSelectionClick ("lnk_02", false, false, cmd);
        { const auto sel = ed->rosterSelection();
          check (sel.size() == 1 && sel.count ("lnk_01"), "(12a) Cmd-click the same strip again TOGGLES it out", juce::String ((int) sel.size())); }
        const bool consumedPlain = ed->applyRosterSelectionClick ("lnk_03", false, false, plain);
        check (! consumedPlain && ed->rosterSelection().empty(), "(12a) a plain click clears the multi-selection and falls through to the working-Link path -> exactly one strip selected", juce::String ((int) ed->rosterSelection().size()));
        ed->applyRosterSelectionClick ("lnk_02", false, false, cmd);
        check (ed->applyRosterSelectionClick ({}, false, false, plain) && ed->rosterSelection().empty(), "(12a) a click on empty roster space clears the selection");
        // ---- 21t-k item 5 (28/29 Sep 2026 ruling): SHIFT-CLICK RANGES, AND SELECT BY ROLE ------------------
        {
            std::printf ("== 21t-k item 5: shift-click ranges from the last plain click; select by role ==\n");
            const juce::ModifierKeys shift (juce::ModifierKeys::shiftModifier);
            const auto rows = A::rosterRows (*ed);
            juce::StringArray rowStr; for (const auto& r : rows) rowStr.add (r);
            std::printf ("    roster order: %s\n", rowStr.joinIntoString (", ").toRawUTF8());
            ed->clearRosterSelection();
            check (ed->rosterSelectionAnchor().isEmpty(),
                   "21t-k 5. with no plain click yet there is no anchor");
            const bool shiftNoAnchor = ed->applyRosterSelectionClick ("lnk_03", false, false, shift);
            check (! shiftNoAnchor && ed->rosterSelection().empty(),
                   "21t-k 5. ...so a shift-click with no anchor behaves as a plain click, never an invented range",
                   juce::String ((int) ed->rosterSelection().size()) + " selected");
            ed->applyRosterSelectionClick ("lnk_01", false, false, plain);
            check (ed->rosterSelectionAnchor() == "lnk_01",
                   "21t-k 5. a PLAIN click is the anchor", ed->rosterSelectionAnchor());
            const bool ranged = ed->applyRosterSelectionClick ("lnk_03", false, false, shift);
            { const auto sel = ed->rosterSelection();
              juce::StringArray got; for (const auto& u : sel) got.add (u);
              check (ranged && sel.size() == 3 && sel.count ("lnk_01") && sel.count ("lnk_02") && sel.count ("lnk_03"),
                     "21t-k 5. shift-click selects the contiguous run from the anchor, both ends included  (RED as "
                     "it stood: a shift-click was a plain click and the run did not exist)",
                     got.joinIntoString (",")); }
            // ...and backwards, because a range has no preferred direction.
            ed->applyRosterSelectionClick ("lnk_03", false, false, plain);
            ed->applyRosterSelectionClick ("lnk_01", false, false, shift);
            check (ed->rosterSelection().size() == 3,
                   "21t-k 5. ...and the same run backwards", juce::String ((int) ed->rosterSelection().size()));
            // SELECT BY ROLE. The fixture's Links are undeclared, so declare two channels and a bus first.
            {
                auto decl = links;
                for (size_t i = 0; i < decl.size(); ++i)
                    decl[i].placement = (i == 2 ? 1 : 2);        // lnk_03 a bus, the rest channels
                EchoJayAlignTestAccess::setLinks (proc, decl);
                const int nCh = (int) decl.size() - 1;        // the fixture's Links, less the one made a bus
                ed->selectRosterByRole ("channel");
                check ((int) ed->rosterSelection().size() == nCh && ! ed->rosterSelection().count ("lnk_03"),
                       "21t-k 5. \"Select all channels\" takes the channels and not the bus",
                       juce::String ((int) ed->rosterSelection().size()) + " of " + juce::String (nCh));
                ed->selectRosterByRole ("bus");
                check (ed->rosterSelection().size() == 1 && ed->rosterSelection().count ("lnk_03"),
                       "21t-k 5. \"Select all buses\" takes the bus",
                       juce::String ((int) ed->rosterSelection().size()));
                ed->selectRosterByRole ("all");
                check ((int) ed->rosterSelection().size() == (int) decl.size(),
                       "21t-k 5. \"Select all\" takes every DECLARED Link",
                       juce::String ((int) ed->rosterSelection().size()) + " of " + juce::String ((int) decl.size()));
                ed->selectRosterByRole ("none");
                check (ed->rosterSelection().empty() && ed->rosterSelectionAnchor().isEmpty(),
                       "21t-k 5. \"Select none\" clears the selection and its anchor");
            }
            ed->clearRosterSelection();
        }
        // ---- 21t-l item 1 (29 Sep 2026 ruling): A GROUP STRIP SHOWS THE COUNT ------------------------------
        // Sean's "Group 2" painted fourteen member names down the data band, over the GROUP label and over the
        // strip below it. The body is the count now, one line, inside its own rect; the names are the tooltip.
        {
            std::printf ("== 21t-l item 1: a group strip shows the COUNT ==\n");
            check (EchoJayEditor::groupStripBodyText (14) == "14 Links",
                   "21t-l 1. a 14-member group's body text is \"14 Links\"  (RED as it stood: fourteen names, one "
                   "per line, painted over the label and the strip below)",
                   EchoJayEditor::groupStripBodyText (14));
            check (EchoJayEditor::groupStripBodyText (1) == "1 Link"
                   && EchoJayEditor::groupStripBodyText (8) == "8 Links"
                   && EchoJayEditor::groupStripBodyText (0) == "0 Links",
                   "21t-l 1. ...and it is one line at every size, singular at one",
                   EchoJayEditor::groupStripBodyText (1) + " / " + EchoJayEditor::groupStripBodyText (8));
            check (! EchoJayEditor::groupStripBodyText (14).containsChar ('\n'),
                   "21t-l 1. ...with no line break in it, so it cannot grow past its rect");
        }

        // ---- 21t-l (29 Sep 2026 ruling): THE HEADER UNDO BUTTON IS THE PATH -------------------------------
        // Cmd-Z reaches rackUndoRedo from keyPressed, but only for key events the HOST forwards; Pro Tools binds
        // Cmd-Z to its own session undo and whether it passes it on is the host's decision, not determinable
        // from here. So the button is the promise and the tooltip says exactly that.
        {
            std::printf ("== 21t-l: the header undo button is the path ==\n");
            proc.recordLinkGainUndo ("lnk_01", 0.0f, -3.0f);
            A::refreshUndo (*ed);
            const auto tip = A::undoTip (*ed);
            std::printf ("    tooltip: %s\n", tip.toRawUTF8());
            check (A::undoEnabled (*ed), "21t-l. an undoable move enables the header undo button");
            check (tip.contains ("this button always works"),
                   "21t-l. ...and its tooltip promises the BUTTON, not a shortcut the host may keep", tip);
            check (tip.contains ("Cmd-Z does when the host passes it through"),
                   "21t-l. ...naming the shortcut as conditional rather than as a fact  (RED as it stood: it read "
                   "\"(Cmd-Z)\" flat, which is a promise this product cannot keep in Pro Tools)", tip);
            check (tip.contains ("Link trim"),
                   "21t-l. ...and saying what it will undo", tip);
        }

        // ---- 21t-k item 7 (28/29 Sep 2026 ruling): STRIP LABELS - one line, ellipsis, or the index ---------
        {
            std::printf ("== 21t-k item 7: strip labels never overflow ==\n");
            const juce::Font f (juce::FontOptions (10.5f, juce::Font::bold));
            auto w = [&f] (const juce::String& t) { return juce::GlyphArrangement::getStringWidth (f, t); };
            const juce::String longName ("Aitch Lead Vocal Double Left");
            const int wide = (int) w (longName) + 10;
            check (EchoJayEditor::stripNameLabel (longName, f, wide, 3) == longName,
                   "21t-k 7. a name that FITS is drawn whole",
                   EchoJayEditor::stripNameLabel (longName, f, wide, 3));
            const int narrow = (int) w ("Aitch Lead") ;
            const auto elided = EchoJayEditor::stripNameLabel (longName, f, narrow, 3);
            std::printf ("    %d px -> \"%s\"\n", narrow, elided.toRawUTF8());
            check (elided != longName && w (elided) <= (float) narrow,
                   "21t-k 7. a name that does not fit is ELLIPSISED, and what is drawn fits the strip",
                   elided + "  (" + juce::String (w (elided), 1) + " px of " + juce::String (narrow) + ")");
            check (elided.endsWith (juce::String::fromUTF8 ("\xe2\x80\xa6")),
                   "21t-k 7. ...with the ellipsis on the tail", elided);
            const int tiny = (int) w ("Ai\xe2\x80\xa6");
            const auto idx = EchoJayEditor::stripNameLabel (longName, f, tiny, 7);
            std::printf ("    %d px -> \"%s\"\n", tiny, idx.toRawUTF8());
            check (idx == "7",
                   "21t-k 7. ...and below four characters of the NAME the strip draws its INDEX instead, because "
                   "three letters identify nothing", idx);
            check (w (idx) <= (float) tiny,
                   "21t-k 7. ...which also fits", idx + " (" + juce::String (w (idx), 1) + " px)");
            check (EchoJayEditor::stripNameLabel ({}, f, 100, 2) == "2",
                   "21t-k 7. a Link with no name yet draws its index, never an empty strip",
                   EchoJayEditor::stripNameLabel ({}, f, 100, 2));
        }
        // ---- (12b) group strip parity ----
        const auto gid = proc.createLinkGroup ("the BVs", juce::StringArray { "lnk_01", "lnk_02", "lnk_03" });
        A::toLinkTab (*ed);
        EchoJayAlignTestAccess::setLinks (proc, links);   // the tab switch refreshes the registry: inject AFTER it, read with no pump between
        A::measureOnly (*ed);
        const A::Geom* linkSg = nullptr; const A::Geom* grpSg = nullptr;
        for (const auto& sg : A::geom (*ed)) { if (sg.isGroup) grpSg = &sg; else if (! sg.isBus && linkSg == nullptr) linkSg = &sg; }
        check (grpSg != nullptr && linkSg != nullptr, "(12b) the roster has both a Link strip and the group strip", juce::String ((int) A::geom (*ed).size()) + " strips");
        if (grpSg != nullptr && linkSg != nullptr)
        {
            check (grpSg->fader.getWidth() == linkSg->fader.getWidth() && grpSg->fader.getHeight() == linkSg->fader.getHeight()
                   && grpSg->fader.getY() == linkSg->fader.getY(), "(12b) the group strip's FADER rect equals a Link strip's in size and vertical position",
                   grpSg->fader.toString() + " vs " + linkSg->fader.toString());
            check (grpSg->full.getWidth() == linkSg->full.getWidth() && grpSg->name.getHeight() == linkSg->name.getHeight()
                   && grpSg->mute.getHeight() == linkSg->mute.getHeight() && grpSg->solo.getHeight() == linkSg->solo.getHeight(),
                   "(12b) ...and the same strip width, name band and M/S row", juce::String (grpSg->full.getWidth()) + "/" + juce::String (linkSg->full.getWidth()));
            bool allFit = true; juce::String worst;
            for (float db = -12.0f; db <= 12.0001f; db += 0.5f)
                if (! EchoJayEditor::groupReadoutFits (db, grpSg->full.getWidth() - 6)) { allFit = false; worst = EchoJayEditor::groupOffsetText (db); }
            check (allFit, "(12b) the offset readout FITS the strip at every offset from -12 to +12 (never \"+0....\")", allFit ? EchoJayEditor::groupOffsetText (0.0f) : "does not fit: " + worst);
            check (EchoJayEditor::groupOffsetText (0.0f) == "+0.0 dB" && EchoJayEditor::groupOffsetText (-2.5f) == "-2.5 dB", "(12b) the readout reads \"+0.0 dB\"", EchoJayEditor::groupOffsetText (0.0f));
        }
        // ---- (12c) chat-reply controls belong to the AI sub-view ----
        // the TAB SWITCH itself is the author (a repaint of the new surface can run before the next paint pass), so the
        // legs assert the switch's own effect: the rule (chatReplyControlsAllowed) and the button it hides.
        A::toTab (*ed, A::tabChat()); A::setChainsMode (*ed, false);
        A::applyBtn (*ed, 0).setVisible (true);
        check (A::replyAllowed (*ed) && A::applyBtn (*ed, 0).isVisible(), "(12c) a pending Apply on the Chat tab is visible", juce::String ((int) A::applyBtn (*ed, 0).isVisible()));
        A::toTab (*ed, A::tabSettings());
        check (! A::replyAllowed (*ed) && ! A::applyBtn (*ed, 0).isVisible(), "(12c) switch to SETTINGS -> the Apply button is NOT visible  (RED as it stood: it drew over the account panel)", juce::String ((int) A::applyBtn (*ed, 0).isVisible()));
        A::toTab (*ed, A::tabChat()); A::applyBtn (*ed, 0).setVisible (true);
        check (A::replyAllowed (*ed) && A::applyBtn (*ed, 0).isVisible(), "(12c) switch back -> the rule allows it again and the button stays visible", juce::String ((int) A::applyBtn (*ed, 0).isVisible()));
        A::setChainsMode (*ed, true);
        check (! A::replyAllowed (*ed) && ! A::applyBtn (*ed, 0).isVisible(), "(12c) the panel shows CHAINS -> not visible  (RED as it stood: Apply drew over the saved-chains list)", juce::String ((int) A::applyBtn (*ed, 0).isVisible()));
        A::setChainsMode (*ed, false); A::applyBtn (*ed, 0).setVisible (true);
        check (A::replyAllowed (*ed) && A::applyBtn (*ed, 0).isVisible(), "(12c) back to AI -> allowed and visible again");
        // ---- (12d) the chain strip has no undo/redo of its own ----
        {
            std::function<int (juce::Component&)> arrows = [&] (juce::Component& c)
            {
                int n = 0;
                for (int i = 0; i < c.getNumChildComponents(); ++i)
                {
                    auto* k = c.getChildComponent (i);
                    if (auto* b = dynamic_cast<juce::TextButton*> (k))
                        if (b->getButtonText().containsAnyOf (juce::String::fromUTF8 ("\xe2\x86\xb6\xe2\x86\xb7"))) ++n;
                    n += arrows (*k);
                }
                return n;
            };
            check (arrows (A::chainPanelOf (*ed)) == 0, "(12d) the chain strip carries NO undo/redo buttons  (RED as it stood: the pair sat beside \"+\")", juce::String (arrows (A::chainPanelOf (*ed))));
        }
        // ---- (12e) the header pair, far right, left of Hide AI ----
        ed->resized(); pumpMs (20);
        { const auto u = A::undoHdr (*ed).getBounds(), r = A::redoHdr (*ed).getBounds(), c = A::collapseBtn (*ed).getBounds();
          bool rightOfAll = true; juce::String blocker;
          for (const auto& hb : A::headerRow (*ed)) if (u.getX() <= hb.getRight()) { rightOfAll = false; blocker = hb.toString(); }
          check (rightOfAll, "(12e) the header Undo sits RIGHT of every other header control", rightOfAll ? u.toString() : "overlaps " + blocker);
          check (u.getRight() <= r.getX() && r.getRight() <= c.getX(), "(12e) ...Undo then Redo, both immediately LEFT of \"Hide AI\"", u.toString() + " " + r.toString() + " " + c.toString());
          check (u.getHeight() == c.getHeight(), "(12e) ...and the same height as the header's pill buttons", juce::String (u.getHeight()) + " vs " + juce::String (c.getHeight())); }
        // ---- (12f) ONE tooltip window in the editor tree ----
        {
            std::function<int (juce::Component&)> tips = [&] (juce::Component& c)
            {
                int n = 0;
                for (int i = 0; i < c.getNumChildComponents(); ++i)
                { auto* k = c.getChildComponent (i); if (dynamic_cast<juce::TooltipWindow*> (k) != nullptr) ++n; n += tips (*k); }
                return n;
            };
            check (tips (*ed) == 1, "(12f) exactly ONE TooltipWindow exists in the editor tree  (RED as it stood: 2 - the chain panel owned a second, so every hint drew twice)", juce::String (tips (*ed)));
        }
        proc.removeLinkGroup (gid); proc.pendingChannelUid = {}; ed->clearRosterSelection();   // the later legs start from the state they expect

        // ---- (11) 21n ruling 1b: channelWidth per TARGET, on this editor ----
        std::printf ("== (11) 22 Sep 2026 (21n ruling 1b): channelWidth follows the TARGET - a Link's registry width on a Link turn, never V2's own ==\n");
        check (A::targetWidth (*ed) == 2 && proc.getTotalNumInputChannels() == 2, "(11) local rack: V2's own bus width (stereo -> 2)", juce::String (A::targetWidth (*ed)));
        links[0].channels = 1; EchoJayAlignTestAccess::setLinks (proc, links); proc.pendingChannelUid = "lnk_01";
        check (A::targetWidth (*ed) == 1, "(11) V2 stereo, the target Link MONO -> the body carries channelWidth 1 (RED as it stood: 2, V2's own)", juce::String (A::targetWidth (*ed)));
        links[0].channels = 2; EchoJayAlignTestAccess::setLinks (proc, links);
        check (A::targetWidth (*ed) == 2, "(11) the target Link STEREO -> 2", juce::String (A::targetWidth (*ed)));
        links[0].channels = 0; EchoJayAlignTestAccess::setLinks (proc, links);
        check (A::targetWidth (*ed) == 0, "(11) an old Link (no width in its row) -> 0 = the field stays off the body", juce::String (A::targetWidth (*ed)));
        proc.pendingChannelUid = {};
    }
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
        // ---- F1 (21s-a): the reply pills cannot leak onto a tab that has no transcript -------------------------
        // THE COMPLAINT: loop pills floating in an empty panel on the Link tab. The controls used to be children of
        // the EDITOR, shown and hidden one by one, so any stale layout pass left one on screen. They are now
        // children of ONE layer whose visibility is the whole answer.
        {
            std::printf ("\n== F1: reply controls are owned by one layer, so they cannot outlive the transcript ==\n");
            A::toChat (*ed); pumpMs (40);
            auto& layer = A::replyLayer (*ed);
            check (A::chip (*ed, 0).getParentComponent() == &layer && A::buildBtn (*ed, 0).getParentComponent() == &layer,
                   "F1. every reply control is a child of the reply layer, not of the editor");
            // "On screen" headless: every ancestor up to the editor visible. (juce::Component::isShowing also
            // requires a window peer, which a headless guard has not got, so it cannot be the test here.)
            // The editor itself is not visible in a headless fixture (it has no window peer), so the walk stops AT
            // the reply layer: "would this control be drawn if the editor were on screen".
            auto onScreen = [&layer] (juce::Component& c)
            {
                for (juce::Component* p = &c; p != nullptr; p = p->getParentComponent())
                {
                    if (! p->isVisible()) return false;
                    if (p == &layer) return true;
                }
                return false;   // not inside the layer at all
            };
            // The defect, staged: a control left visible by a stale pass.
            A::chip (*ed, 0).setBounds (10, 10, 60, 20);
            A::chip (*ed, 0).setVisible (true);
            // Checked at this instant, with no pump: the editor's own housekeeping legitimately clears stale chips
            // on the Chat tab, and what this leg is about is the PARENT CHAIN, not that housekeeping.
            {   // name WHICH link of the chain is closed, so a failure says something
                juce::String chain;
                for (juce::Component* pc = &A::chip (*ed, 0); pc != nullptr; pc = pc->getParentComponent())
                    chain << (pc->getName().isEmpty() ? juce::String ("<unnamed>") : pc->getName())
                          << (pc->isVisible() ? "(vis) " : "(HIDDEN) ");
                check (onScreen (A::chip (*ed, 0)),
                       "F1. ...a pill whose own flag is set shows while the layer is visible (the chain is live)", chain);
            }
            // SUPERSEDED BY F1 REVISED (24 Sep 2026): Link now HAS the panel, so the tab that proves the rule is
            // one with no column at all. Settings is that tab, and it is the layout that says so.
            A::toTab (*ed, A::tabSettings()); pumpMs (60);
            check (! A::replyAllowed (*ed), "F1. a tab with no panel column is not a chat-reply tab (Settings)");
            check (! layer.isVisible(), "F1. ...so the layer is hidden");
            check (A::chip (*ed, 0).isVisible() && ! onScreen (A::chip (*ed, 0)),
                   "F1. ...and the stale pill CANNOT be on screen even with its own visible flag still true  "
                   "(RED as it stood: it showed)");
            A::toChat (*ed); pumpMs (60);
            check (layer.isVisible(), "F1. back on Chat the layer returns  (the one switch drives BOTH ways)");
            A::chip (*ed, 0).setVisible (false);
        }

        // ---- F5 (21s-b): the chain MIX is a RACK property ------------------------------------------------------
        // It used to be V2's: with a Link's rack on screen the knob simply returned, so the control was dead and
        // the only chain mix that existed was the main plugin's.
        {
            std::printf ("\n== F5: the chain MIX belongs to the rack on screen ==\n");
            auto& own = proc.getChainHost();
            own.setMasterWet (1.0f);
            check (std::abs (own.getMasterWet() - 1.0f) < 1e-4f, "F5. V2's own rack starts at 100%");
            // With no Link rack on screen the knob is V2's own.
            A::masterWet (*ed, 0.6f);
            check (A::viewUid (*ed).isEmpty() && std::abs (own.getMasterWet() - 0.6f) < 1e-4f,
                   "F5. with V2's own rack on screen the knob writes V2's own mix", juce::String (own.getMasterWet(), 2));
            own.setMasterWet (1.0f);
            // A REMOTE rack: the knob writes a ctrl-cmd for THAT Link, and V2's own mix is untouched.
            int err = 0; const auto dir = LinkShm::resolveDir (err);
            juce::File (dir + "ctrl-cmd-lnk_01.json").deleteFile();
            A::sendChainWet (*ed, "lnk_01", 0.6f);
            const auto cmdA = juce::JSON::parse (juce::File (dir + "ctrl-cmd-lnk_01.json").loadFileAsString());
            check (std::abs ((double) cmdA.getProperty ("chainWet", juce::var (-1.0)) - 0.6) < 1e-6,
                   "F5. Link A gets a chainWet command carrying ITS value  (RED as it stood: the knob returned)",
                   juce::JSON::toString (cmdA).substring (0, 80));
            juce::File (dir + "ctrl-cmd-lnk_02.json").deleteFile();
            A::sendChainWet (*ed, "lnk_02", 0.25f);
            const auto cmdB = juce::JSON::parse (juce::File (dir + "ctrl-cmd-lnk_02.json").loadFileAsString());
            check (std::abs ((double) cmdB.getProperty ("chainWet", juce::var (-1.0)) - 0.25) < 1e-6,
                   "F5. switching to Link B writes B's own value, not A's", juce::JSON::toString (cmdB).substring (0, 80));
            const auto cmdAstill = juce::JSON::parse (juce::File (dir + "ctrl-cmd-lnk_01.json").loadFileAsString());
            check (std::abs ((double) cmdAstill.getProperty ("chainWet", juce::var (-1.0)) - 0.6) < 1e-6,
                   "F5. ...and A's command still says 0.6 - one rack's mix is not the other's");
            check (std::abs (own.getMasterWet() - 1.0f) < 1e-4f,
                   "F5. V2's own rack is unaffected throughout  (it is at 100%)", juce::String (own.getMasterWet(), 2));
            juce::File (dir + "ctrl-cmd-lnk_01.json").deleteFile();
            juce::File (dir + "ctrl-cmd-lnk_02.json").deleteFile();
        }

        // ---- THE REQUEST CONTRACT (21s-b, 24 Sep 2026) --------------------------------------------------------
        // TONIGHT'S DEFECT: turns reached the server with channel null. The chat body had NO channel field at all -
        // only /api/classify had one - so a turn begun anywhere but the Chat tab told the server nothing about
        // where it was. Every request now carries the chat's stable id and the TARGET channel identity.
        {
            std::printf ("\n== the request contract: chatId + channel + the chain block, from every surface ==\n");
            // "+ New chat" empties the transcript, and the legs after this one assert on the arm bubble this scope
            // built - so the fixture is saved here and put back at the end. A guard must not move the ground the
            // next leg is standing on.
            const auto savedMsgs = A::msgs (*ed);
            const auto savedId   = A::chatId (*ed);
            if (A::chatId (*ed).isEmpty()) { A::newChat (*ed); pumpMs (60); }
            auto bodyNow = [&] ()
            {
                proc.getApi().setTurnIdentity (A::chatId (*ed), A::channelIdentity (*ed));
                proc.getApi().setGroupsContext (proc.linksBodyVar(), proc.groupsBodyVar());
                return EchoJayAPIRequestPin::body (proc.getApi(), juce::StringArray { "user" },
                                                   juce::StringArray { "what is this chain" }, "sys", {});
            };
            A::toLinkTab (*ed); pumpMs (40);
            {
                const auto b = bodyNow();
                check (b.contains ("\"chatId\""),
                       "contract. Link tab: the body carries chatId  (RED as it stood: no such field)",
                       b.fromFirstOccurrenceOf ("\"chatId\"", true, false).substring (0, 30));
                check (b.contains ("\"channel\":\"") && ! b.contains ("\"channel\":\"\""),
                       "contract. Link tab: ...and a NON-NULL channel  (RED as it stood: the server saw null)",
                       b.fromFirstOccurrenceOf ("\"channel\"", true, false).substring (0, 40));
            }
            proc.chatTargetLinkUid = "lnk_01"; proc.chatTargetLinkName = "BV 1";
            {
                const auto b = bodyNow();
                check (b.contains ("\"channel\""), "contract. Working-on menu: the chosen Link names the channel",
                       b.fromFirstOccurrenceOf ("\"channel\"", true, false).substring (0, 40));
            }
            proc.chatTargetLinkUid.clear(); proc.chatTargetLinkName.clear();
            const auto gidc = proc.createLinkGroup ("Group 3", juce::StringArray { "lnk_01", "lnk_02" });
            A::targetGroup (*ed, gidc);
            {
                const auto b = bodyNow();
                check (b.contains ("Group: Group 3"), "contract. group strip: the group names the channel",
                       b.fromFirstOccurrenceOf ("\"channel\"", true, false).substring (0, 40));
                check (b.contains ("\"selectedGroupId\""), "contract. ...and the group id rides with it");
            }
            const auto inj = A::injections (*ed);
            check (inj.contains ("CURRENT CHAIN"),
                   "contract. the turn carries a [CURRENT CHAIN] block for the target", inj.substring (0, 110));
            const auto id1 = A::chatId (*ed);
            check (id1.isNotEmpty(), "contract. the chat has a stable id", id1);
            check (A::chatId (*ed) == id1, "contract. a continued chat keeps it");
            // "+ New chat" mints a record only when the current chat HAS messages - an empty one already IS a
            // fresh chat, which is the product's rule, not a fixture detail. So give it one.
            { A::Msg m; m.role = "user"; m.content = "a turn, so this chat is not already fresh"; A::msgs (*ed).push_back (m); }
            A::newChat (*ed); pumpMs (60);
            const auto id2 = A::chatId (*ed);
            check (id2.isNotEmpty() && id2 != id1, "contract. \"+ New chat\" gets a NEW chatId", id1 + " -> " + id2);
            A::targetGroup (*ed, {});
            proc.removeLinkGroup (gidc);
            A::toChat (*ed); pumpMs (40);
            A::msgs (*ed) = savedMsgs;
            A::setChatId (*ed, savedId);
        }

        // ---- H1 (21t-a, 25 Sep 2026): a click at a reply control's own position must REACH it -----------------
        // F1 made every reply control a child of one layer and brought that layer to front ONCE, at the top of the
        // constructor - before the other 107 children were added, each of which then landed above it. The controls
        // ended up at the BOTTOM of the stack, under the transcript viewport, and nothing could be pressed.
        {
            std::printf ("\n== H1: a click at a Build button's position reaches the Build button ==\n");
            // getComponentAt walks only VISIBLE components, and an editor with no window peer starts invisible in a
            // headless fixture - so the editor is made visible here or the hit test answers "nothing" about
            // everything. This is the fixture catching up with the product, not a relaxation.
            const auto savedPrompts = EchoJayPromptTestAccess::save (proc);
            ed->setVisible (true);
            // ...and the onboarding overlay is dismissed, because it legitimately covers the whole editor while a
            // prompt is up. A fresh instance has answered nothing (21r item 7), so in a fixture it is always up -
            // and it was the first thing this leg found at the button's centre.
            proc.setChannelTypePromptDismissed (true);
            proc.setGenrePromptDismissed (true);
            proc.setProjectPromptDismissed (true);
            ed->resized(); pumpMs (40);
            for (auto tab : { A::tabChat(), A::tabChain() })
            {
                A::toTab (*ed, tab); ed->resized(); pumpMs (40);
                auto& btn = A::buildBtn (*ed, 0);
                A::setActiveBuilds (*ed, 1);
                btn.setBounds (60, 300, 120, 24);
                btn.setVisible (true);
                bool fired = false;
                // The REAL handler is put back at the end of this leg: R1's leg presses this same button and
                // asserts on what the shipped handler does, and a guard must not leave a control gutted.
                auto savedOnClick = btn.onClick;
                btn.onClick = [&fired] { fired = true; };
                // THE HIT TEST IS THE TEST: what does the editor say is at that point? A button under another
                // component never sees the click, whatever its own state says.
                const auto centre = btn.getBounds().getCentre();
                auto* hit = ed->getComponentAt (centre);
                check (hit == &btn,
                       juce::String ("H1. tab ") + juce::String ((int) tab)
                       + ": the component at the Build button's centre IS the Build button  (RED as it stood: the "
                         "layer was at the bottom, so something else was)",
                       hit == nullptr ? juce::String ("nothing")
                                      : (hit == &btn ? juce::String ("the button")
                                                     : juce::String (typeid (*hit).name()) + " name=\"" + hit->getName()
                                                       + "\" parentIsLayer=" + (hit->getParentComponent() == &A::replyLayer (*ed) ? "y" : "n")));
                btn.triggerClick(); pumpMs (30);
                check (fired, juce::String ("H1. tab ") + juce::String ((int) tab) + ": ...and pressing it runs the handler");
                btn.onClick = savedOnClick;
                btn.setVisible (false);
                A::setActiveBuilds (*ed, 0);
            }
            EchoJayPromptTestAccess::restore (proc, savedPrompts);   // the instance goes back as it was
        }

        // ---- H2 (21t-a, 25 Sep 2026): a group is a target you can choose, leave, and come back to ------------
        // (a) The Working-on banner's dropdown listed the Links and nothing else: the only way to choose a group
        // was the composer pill, so from the banner - the control that SAYS what you are working on - groups did
        // not exist. (b) One source of truth both ways: choosing a Link cleared nothing, so the banner kept
        // saying "Working on Group: ..." over a Link's chat. (c) A chat begun on a group fell into Main, with no
        // folder of its own, because a chat could only be bound to a channel.
        {
            std::printf ("\n== H2a: the Working-on menu lists the groups beside the channels ==\n");
            // SAVED HERE, before the first thing that moves it: H2b's Link click opens that channel's chat, and
            // the legs after this one assert on the transcript this fixture built. A guard must not move the
            // ground the next leg is standing on.
            const auto savedId   = A::chatId (*ed);
            const auto savedMsgs = A::msgs (*ed);
            const auto gA = proc.createLinkGroup ("Verses",   juce::StringArray { "lnk_01", "lnk_02" });
            const auto gB = proc.createLinkGroup ("Choruses", juce::StringArray { "lnk_02" });
            {
                const auto items = A::bannerItems (*ed);
                int groups = 0; juce::String names;
                for (const auto& it : items) if (it.isGroup) { ++groups; names << "[" << it.label << "]"; }
                check (groups == 2,
                       "H2a. both groups are rows of the menu  (RED as it stood: it listed none)",
                       juce::String (groups) + " of " + juce::String ((int) proc.linkGroups().size()) + " " + names);
                bool anyChan = false;
                for (const auto& it : items) if (! it.isGroup) { anyChan = true; break; }
                check (anyChan, "H2a. ...beside the channels, in one list (the channels are still there)");
            }
            A::targetGroup (*ed, gB);
            {
                const auto items = A::bannerItems (*ed);
                juce::String ticked;
                for (const auto& it : items) if (it.ticked) ticked << (it.isGroup ? "group:" : "chan:") << it.label << " ";
                check (ticked.trim() == "group:Choruses (1)",
                       "H2a. the selected group is the ticked row, and the ONLY ticked row  (one selection, one tick)",
                       ticked.isEmpty() ? juce::String ("nothing ticked") : ticked);
            }

            std::printf ("\n== H2b: choosing a Link leaves the group, and the banner follows ==\n");
            check (A::banner (*ed).startsWith ("Working on Group: Choruses"),
                   "H2b. fixture: the banner is on the group", A::banner (*ed));
            A::openChannel (*ed, "lnk_01"); pumpMs (40);
            check (proc.chatTargetGroupId.isEmpty(),
                   "H2b. clicking a Link clears the group selection  (RED as it stood: it survived)",
                   proc.chatTargetGroupId.isEmpty() ? juce::String ("cleared") : proc.chatTargetGroupName);
            check (! A::banner (*ed).contains ("Group:"),
                   "H2b. ...and the banner stops saying \"Working on Group\"  (RED as it stood: it stuck)",
                   A::banner (*ed));
            check (! A::stripSelected (*ed, "grp:" + gB),
                   "H2b. ...and the group strip is no longer the selected row");

            std::printf ("\n== H2c: a chat begun on a group gets its own folder, named after the group ==\n");
            // A send routes only out of a chat that HAS history (a virgin chat is assigned instead - the router's
            // own rule, unchanged), so the fixture gives the current chat a turn first.
            {
                WsChat seed;
                seed.id = "h2c_main"; seed.title = "a main chat"; seed.trackName = A::projName (*ed);
                seed.created = juce::Time::getCurrentTime().toISO8601 (true);
                WsMessage m; m.role = "user"; m.content = "a turn, so this chat is not virgin";
                seed.messages.push_back (m);
                A::addChat (*ed, seed);
                A::openChat (*ed, "h2c_main"); pumpMs (40);
            }
            A::targetGroup (*ed, gB); pumpMs (20);
            A::send (*ed, "make the choruses louder"); pumpMs (80);
            const auto cid = A::chatId (*ed);
            auto* gc = A::chat (*ed, cid);
            check (gc != nullptr && gc->groupId == gB,
                   "H2c. the send lands in a chat bound to the GROUP, not in the main chat  (RED as it stood: "
                   "there was no group binding at all)",
                   gc == nullptr ? juce::String ("no chat") : (cid + " groupId=\"" + gc->groupId + "\""));
            check (gc != nullptr && gc->groupName == "Choruses",
                   "H2c. ...carrying the group's name for display", gc == nullptr ? juce::String() : gc->groupName);
            if (gc != nullptr && gc->messages.empty())
            {   // offline fixture: the turn never reaches the server, so the record is given the message the
                // send would have written - the sidebar filters empty chats, and what is under test is the FOLDER.
                WsMessage m; m.role = "user"; m.content = "make the choruses louder";
                gc->messages.push_back (m);
            }
            {
                const auto folders = A::folders (*ed);
                bool own = false, named = false;
                for (const auto& f : folders)
                {
                    if (! f.startsWith ("group:" + gB + "|")) continue;
                    own = true;
                    named = f.fromFirstOccurrenceOf ("|", false, false).startsWith ("Choruses");
                }
                check (own, "H2c. the sidebar gives that chat a folder of its own, like a Link's  (RED as it "
                            "stood: it sat under Main)", folders.joinIntoString (" , "));
                check (named, "H2c. ...named after the group", folders.joinIntoString (" , "));
            }
            // The target follows the chat you open, both ways - the same one-source-of-truth rule as H2b.
            A::openChat (*ed, "h2c_main"); pumpMs (40);
            check (proc.chatTargetGroupId.isEmpty(),
                   "H2c. opening a non-group chat clears the group target", proc.chatTargetGroupName);
            A::openChat (*ed, cid); pumpMs (40);
            check (proc.chatTargetGroupId == gB,
                   "H2c. opening the group's chat selects that group again  (the banner says where you are)",
                   proc.chatTargetGroupId);
            A::targetGroup (*ed, {});
            proc.removeLinkGroup (gA); proc.removeLinkGroup (gB);
            A::setChatId (*ed, savedId);
            A::msgs (*ed) = savedMsgs;
            A::toChat (*ed); pumpMs (40);
        }

        // ---- H3 (21t-a, 25 Sep 2026): the chain MIX knob writes the rack on screen, BORROWED or not ------------
        // The rolling log had no "EJCtrl: chainWet" line at all, and repeated "EJBorrow: engaged uid=..." - every
        // rack on screen is borrowed, and F5's borrowed branch stopped at the in-process copy. So the move never
        // reached the Link and died with the lease.
        {
            std::printf ("\n== H3: the chain MIX knob writes the rack on screen, borrowed or not ==\n");
            int err = 0; const auto dir = LinkShm::resolveDir (err);
            juce::File (dir + "ctrl-cmd-lnk_01.json").deleteFile();
            auto& own = proc.getChainHost();
            own.setMasterWet (1.0f);
            const auto savedId = A::chatId (*ed);
            A::setChatId (*ed, {});
            A::unpinView (*ed);                       // 21t-e: start from a view that follows the chat
            proc.pendingChannelUid = "lnk_01";        // this is what puts lnk_01's rack on screen
            auto& bh = EchoJayBorrowTestAccess::engage (proc, "lnk_01");
            bh.setMasterWet (1.0f);
            check (A::viewUid (*ed) == "lnk_01", "H3. fixture: lnk_01's rack is the one on screen", A::viewUid (*ed));
            A::masterWet (*ed, 0.35f); pumpMs (30);
            const auto cmd = juce::JSON::parse (juce::File (dir + "ctrl-cmd-lnk_01.json").loadFileAsString());
            check (std::abs ((double) cmd.getProperty ("chainWet", juce::var (-1.0)) - 0.35) < 1e-6,
                   "H3. a BORROWED rack's knob still sends the move to the Link  (RED as it stood: the borrowed "
                   "host swallowed it and no command was written)",
                   juce::JSON::toString (cmd).substring (0, 80));
            check (std::abs (bh.getMasterWet() - 0.35f) < 1e-4f,
                   "H3. ...and the borrowed host takes it too, so what you hear follows the knob",
                   juce::String (bh.getMasterWet(), 3));
            check (std::abs (own.getMasterWet() - 1.0f) < 1e-4f,
                   "H3. ...and V2's own rack is untouched", juce::String (own.getMasterWet(), 3));
            EchoJayBorrowTestAccess::release (proc);
            proc.pendingChannelUid.clear();
            proc.pendingChannelUid.clear();
            A::setChatId (*ed, savedId);
            juce::File (dir + "ctrl-cmd-lnk_01.json").deleteFile();
            // OWED (recorded, not claimed): the other half of H3 - the borrowed host seeded from the rack's own
            // sidecar at engage - is only reachable through the real engage path (ring + lease + a running Link),
            // so it rides with F5's two-process leg. In-host evidence only until then.
        }

        // ---- 21t-e (25 Sep 2026): [CURRENT CHAIN], the retired pre-gain, the strip labels, the picker --------
        {
            std::printf ("\n== 21t-e: [CURRENT CHAIN] is the chat's channel, read from where that chain lives ==\n");
            const juce::String cuid = "cc_lnk";
            const auto savedId = A::chatId (*ed);
            A::setChatId (*ed, {});
            proc.pendingChannelUid = cuid;                   // the chat is on this Link
            auto& bh = EchoJayBorrowTestAccess::engage (proc, cuid);
            { EedCompressorProcessor fc; SurgicalEqProcessor fe; juce::ignoreUnused (fc, fe); }
            const auto* pitchLike = BuiltinDeviceRegistry::instance().findByName ("EchoJay EQ");
            if (pitchLike != nullptr)
            {
                while (bh.getNumSlots() > 0) bh.removeSlot (0);
                bh.insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*pitchLike), 0);
                // The SIDECAR is deliberately left empty/absent - which is exactly the state a parked Link keeps
                // rewriting it into while V2 holds the lease, and the state that dropped the block on 25 Sep.
                int e2 = 0; juce::File (LinkShm::resolveDir (e2) + "rack-" + cuid + ".json").deleteFile();
                const auto inj = A::injections (*ed);
                check (inj.contains ("[CURRENT CHAIN"),
                       "21t-e. a Link-chat turn carries [CURRENT CHAIN]  (RED as it stood: the sidecar was empty "
                       "because the parked Link had rewritten it, and the block was dropped)",
                       inj.contains ("[CURRENT CHAIN") ? juce::String ("present") : juce::String ("ABSENT"));
                check (inj.contains ("EchoJay EQ"),
                       "21t-e. ...listing the rack that is actually there, from the borrowed session host",
                       inj.fromFirstOccurrenceOf ("[CURRENT CHAIN", true, false).substring (0, 120));
            }
            else check (false, "21t-e. fixture: a built-in to rack");
            EchoJayBorrowTestAccess::release (proc);
            proc.pendingChannelUid.clear();
            A::setChatId (*ed, savedId);
        }

        {
            std::printf ("\n== 21t-e: no compose-time pre-gain reaches a Link any more ==\n");
            check (! A::pendingPreGain (*ed),
                   "21t-e. nothing is carried to a Link from the compose-time level match  (RED as it stood: a "
                   "tuner-only build sent +4.4 dB)");
        }

        {
            std::printf ("\n== 21t-e: a narrow strip shows what tells a channel apart from its siblings ==\n");
            juce::StringArray sibs { "Main vocal", "Main vocal 2", "Main vocal 3", "Main vocal 4",
                                     "Main vocal 5", "Main vocal 6", "v3_2" };
            juce::StringArray got;
            for (const auto& n : sibs) got.add (A::stripLabel (n, sibs));
            check (A::stripLabel ("Main vocal 2", sibs) == "MV 2" && A::stripLabel ("Main vocal", sibs) == "MV",
                   "21t-e. the shared words collapse to initials: \"Main vocal 2\" -> \"MV 2\", \"Main vocal\" -> \"MV\"",
                   got.joinIntoString (" | "));
            std::set<juce::String> distinct;
            for (const auto& g : got) distinct.insert (g);
            check ((int) distinct.size() == got.size(),
                   "21t-e. ...and every label is DISTINCT  (RED as it stood: seven strips all painted \"Main v...\")",
                   juce::String ((int) distinct.size()) + " of " + juce::String (got.size()));
            check (A::stripLabel ("Kick", juce::StringArray { "Kick", "Snare" }) == "Kick",
                   "21t-e. ...and names that share nothing are left alone", A::stripLabel ("Kick", juce::StringArray { "Kick", "Snare" }));
        }

        {
            std::printf ("\n== 21t-e: the members of the selected group are highlighted on the roster ==\n");
            std::vector<EchoJayProcessor::LinkSlotInfo> rows;
            juce::StringArray mem;
            for (int i = 0; i < 9; ++i)
            {
                EchoJayProcessor::LinkSlotInfo li;
                li.uid = "hl_" + juce::String (i); li.name = "Main vocal " + juce::String (i + 1);
                li.connected = true; rows.push_back (li);
                if (i < 7) mem.add (li.uid);
            }
            // KNOWN STATE IN, KNOWN STATE OUT: selecting a group VIEWS a member, and a view left pinned here is
            // what starved the picker leg below of an editor (createSlotEditorForView returns nullptr for a
            // remote rack, so the seam had nothing to place).
            A::knownState (*ed, proc);
            const auto gid = proc.createLinkGroup ("Main vocals", mem);
            A::targetGroup (*ed, gid);
            EchoJayAlignTestAccess::setLinks (proc, rows);
            int hit = 0; bool strays = false;
            for (const auto& r : rows)
            { if (A::stripSelected (*ed, r.uid)) { if (mem.contains (r.uid)) ++hit; else strays = true; } }
            check (hit == 7, "21t-e. all seven members are highlighted  (RED as it stood: only the group's own "
                   "strip was)", juce::String (hit) + " of 7");
            check (! strays, "21t-e. ...and no non-member is");
            check (A::stripSelected (*ed, "grp:" + gid), "21t-e. ...with the group's own strip still selected");
            check (A::pill (*ed) == "Main vocals", "21t-e. the composer pill names the group", A::pill (*ed));
            A::targetGroup (*ed, {});
            proc.removeLinkGroup (gid);
            A::knownState (*ed, proc);          // the view this leg moved goes back to the mix bus
        }

        {
            std::printf ("\n== 21t-e: the add-plugin picker never opens under a hosted editor ==\n");
            // The inline hosted editor is a heavyweight NSView; the picker is lightweight, so the only remedy is
            // to close it - which is what the rack menu has done since 2 Sep and the picker did not.
            // The rack on screen has to be the MIX BUS's own, because that is the only host whose slot editor this
            // process can create - a Link's instance lives in the Link's process.
            A::knownState (*ed, proc);
            auto& own = proc.getChainHost();
            { SurgicalEqProcessor fe; juce::ignoreUnused (fe); }
            const auto* eqd = BuiltinDeviceRegistry::instance().findByName ("EchoJay EQ");
            if (eqd != nullptr)
            {
                const int before = own.getNumSlots();
                own.insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*eqd), before);
                // The rack lives on the Chain tab: the panel has to be the laid-out, visible one before an inline
                // editor can be put in it, exactly as a user's click would have it.
                A::toTab (*ed, A::tabChain()); ed->resized();
                A::refreshPanel (*ed); pumpMs (60);
                // THE SEAM, and why it is needed: the built-in EQ is POP-OUT ONLY by design - the placement poll
                // looks for a hosted native view and a JUCE component has none - so showInline will never open it
                // inline, and a headless fixture has no third-party plugin to host. The leg proves the PICKER's
                // path; the compositing is a native-view fact, checked by hand on the installed build.
                const bool opened = A::panel (*ed).forceInlineForTest (before);
                pumpMs (60);
                check (opened && A::inlineEditorOpen (*ed),
                       "21t-e. fixture: a hosted editor is open INLINE in the rack",
                       A::inlineEditorOpen (*ed) ? juce::String ("open") : juce::String ("none"));
                const auto logBefore = juce::File (juce::String (echojay::FileLog::instance().currentPath())).loadFileAsString();
                A::picker (*ed); pumpMs (60);
                check (! A::inlineEditorOpen (*ed),
                       "21t-e. opening the picker closes it, so the picker cannot draw behind it  (RED as it "
                       "stood: only a popped-OUT editor was handled)");
                const auto logAfter = juce::File (juce::String (echojay::FileLog::instance().currentPath())).loadFileAsString();
                check (logAfter.substring (logBefore.length()).contains ("EJPicker: inline hosted editor closed"),
                       "21t-e. ...and it says so in the log, so the path is auditable in a real session",
                       logAfter.substring (logBefore.length()).fromFirstOccurrenceOf ("EJPicker:", true, false).upToFirstOccurrenceOf ("\n", false, false).substring (0, 110));
                while (own.getNumSlots() > before) own.removeSlot (own.getNumSlots() - 1);
                A::knownState (*ed, proc);
                A::refreshPanel (*ed); pumpMs (40);
            }
            else check (false, "21t-e. fixture: the EQ built-in is registered");
        }

        // ---- 21t-e (3): ONE RACK-SELECTION RULE ---------------------------------------------------------------
        // Selecting a rack switches the chat to that rack's channel UNLESS the rack is already inside what the
        // chat is working on - which is exactly the group case, and the only case that moves the view alone.
        {
            std::printf ("\n== 21t-e (3): selecting a rack moves the view AND the chat - except inside a group ==\n");
            std::vector<EchoJayProcessor::LinkSlotInfo> rows;
            juce::StringArray mem;
            for (int i = 0; i < 4; ++i)
            {
                EchoJayProcessor::LinkSlotInfo li;
                li.uid = "sel_" + juce::String (i); li.name = "Vox " + juce::String (i + 1);
                li.connected = true; rows.push_back (li);
                if (i < 3) mem.add (li.uid);          // sel_0..sel_2 are members; sel_3 is not
            }
            // EVERY LEG STARTS FROM A KNOWN STATE and leaves it there: main chat, view unpinned, no group, no
            // inline editor. The (3) leg used to leak a rack selection into the picker leg below.
            const auto savedId   = A::chatId (*ed);
            const auto savedMsgs = A::msgs (*ed);
            A::knownState (*ed, proc);
            A::setChatId (*ed, {});
            // INJECTED WITH NO PUMP BEFORE THE ASSERTIONS: the editor's tick rebuilds the registry from the real
            // one (empty under an isolated home) and drops a pending channel whose Link has "vanished".
            EchoJayAlignTestAccess::setLinks (proc, rows);

            // (a) FROM THE MAIN CHAT: selecting a Link moves both.
            A::selectRack (*ed, "sel_0");
            check (A::viewUid (*ed) == "sel_0"
                   && (A::workingOn (*ed) == "sel_0" || proc.pendingChannelUid == "sel_0"),
                   "21t-e (3a). from the main chat, selecting a Link moves the view AND the chat  (a channel with "
                   "no chat record yet is HELD as pending until the first send - the product's own rule)",
                   "view " + A::viewUid (*ed) + " / working on " + A::workingOn (*ed)
                   + " / pending \"" + proc.pendingChannelUid + "\" / chatId \"" + A::chatId (*ed)
                   + "\" / links " + juce::String ((int) proc.getLinkSlotInfos().size()));
            check (A::pill (*ed) != "This channel" && A::pill (*ed).isNotEmpty(),
                   "21t-e (3b). ...and the pill names the Link, not \"This channel\"", A::pill (*ed));

            // ...and selecting the mix bus goes back to the main chat.
            A::selectRack (*ed, {});
            check (A::viewUid (*ed).isEmpty() && A::workingOn (*ed).isEmpty(),
                   "21t-e (3a). selecting the mix bus moves both to the main context",
                   A::viewUid (*ed).isEmpty() ? juce::String ("(local)") : A::viewUid (*ed));
            check (A::pill (*ed) == "This channel",
                   "21t-e (3b). ...and \"This channel\" means the mix bus, and only that", A::pill (*ed));

            // (c) IN A GROUP CHAT: a MEMBER's rack moves the view only.
            const auto gid = proc.createLinkGroup ("The BVs", mem);
            EchoJayAlignTestAccess::setLinks (proc, rows); A::targetGroup (*ed, gid);
            check (A::pill (*ed) == "The BVs",
                   "21t-e (3b). a group chat's pill names the GROUP", A::pill (*ed));
            check (mem.contains (A::viewUid (*ed)),
                   "21t-e (3c). choosing the group views one of its members", A::viewUid (*ed));
            A::selectRack (*ed, "sel_2");
            check (A::viewUid (*ed) == "sel_2",
                   "21t-e (3c). selecting a MEMBER's rack moves the view", A::viewUid (*ed));
            check (proc.chatTargetGroupId == gid,
                   "21t-e (3c). ...and the chat STAYS on the group  (RED as it stood: it switched to the member "
                   "and the other six left the conversation)", proc.chatTargetGroupId);

            // ...while a NON-member leaves the group and switches, as from anywhere else.
            A::selectRack (*ed, "sel_3");
            check (proc.chatTargetGroupId.isEmpty() && A::viewUid (*ed) == "sel_3"
                   && (A::workingOn (*ed) == "sel_3" || proc.pendingChannelUid == "sel_3"),
                   "21t-e (3c). selecting a NON-member leaves the group and moves both",
                   "group \"" + proc.chatTargetGroupId + "\" working on " + A::workingOn (*ed));

            // (d) AN EDIT FROM A GROUP CHAT REACHES EVERY MEMBER, whichever member is on screen.
            A::targetGroup (*ed, gid); pumpMs (40);
            A::selectRack (*ed, "sel_1");
            int err = 0; const auto dir = LinkShm::resolveDir (err);
            for (const auto& m : mem) juce::File (dir + "chain-cmd-" + m + ".json").deleteFile();
            {
                A::Msg card;
                card.role = "assistant"; card.content = "a change for the BVs";
                card.editData = "{\"baseSlots\":[\"EchoJay EQ\"],\"edit\":[{\"op\":\"bypass\",\"slot\":1,\"on\":true}]}";
                A::msgs (*ed).push_back (card);
                A::applyEdit (*ed, (int) A::msgs (*ed).size() - 1);
            }
            int reached = 0;
            for (const auto& m : mem)
                if (juce::File (dir + "chain-cmd-" + m + ".json").existsAsFile()) ++reached;
            check (reached == mem.size(),
                   "21t-e (3d). an edit made in a group chat reaches EVERY member  (RED as it stood: it went to "
                   "the local rack, because a group chat has no channel of its own)",
                   juce::String (reached) + " of " + juce::String (mem.size()));
            check (A::viewUid (*ed) == "sel_1",
                   "21t-e (3d). ...and the rack on screen is still the one the user was looking at", A::viewUid (*ed));
            for (const auto& m : mem) juce::File (dir + "chain-cmd-" + m + ".json").deleteFile();
            proc.removeLinkGroup (gid);
            A::knownState (*ed, proc);          // leave it as it was found
            A::setChatId (*ed, savedId);
            A::msgs (*ed) = savedMsgs;
        }

        // ---- 21t-d wiring (25 Sep 2026): the trigger, the card from the sidecar, the closing posted once -----
        {
            std::printf ("\n== 21t-d wiring: an applied op starts the loop on the dynamics slot, and only there ==\n");
            // ON A BORROWED HOST, not this instance's own rack: the legs around this one assert on the fixture's
            // chain, and a leg that empties it to build its own two slots would be moving their ground. The
            // borrowed host is the guard's own, and it is also the host that actually runs a Link rack's loop.
            const juce::String tuid = "calib_trig";
            auto& own = EchoJayBorrowTestAccess::engage (proc, tuid);
            // Two built-ins: a compressor (dynamics) and an EQ (not). The op names both; only one may start.
            // FORCE-LINK: including the headers is not enough - the registrars live in the .cpp files and the
            // static archive drops an object nothing references. Constructing one of each pulls them in, which
            // is what registers "EchoJay Compressor" and "EchoJay EQ" with the registry.
            { EedCompressorProcessor forceComp; SurgicalEqProcessor forceEq; juce::ignoreUnused (forceComp, forceEq); }
            const auto* comp = BuiltinDeviceRegistry::instance().findByName ("EchoJay Compressor");
            const auto* eq   = BuiltinDeviceRegistry::instance().findByName ("EchoJay EQ");
            if (comp != nullptr && eq != nullptr)
            {
                while (own.getNumSlots() > 0) own.removeSlot (0);
                own.insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*eq),   0);
                own.insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*comp), 1);
                check (own.getNumSlots() == 2, "21t-d w. fixture: an EQ at slot 1 and a compressor at slot 2",
                       juce::String (own.getNumSlots()));
                juce::Array<juce::var> ops;
                auto op = [] (int slot1, juce::var band, double drive)
                {
                    auto* o = new juce::DynamicObject();
                    o->setProperty ("slot", slot1);
                    if (! band.isVoid()) o->setProperty ("gr_target_db", band);
                    o->setProperty ("slot_pre_gain_db", drive);
                    return juce::var (o);
                };
                juce::Array<juce::var> band; band.add (2.5); band.add (3.5);
                ops.add (op (1, juce::var(), 4.0));          // the EQ: must not start a loop
                ops.add (op (2, juce::var (band), 4.0));     // the compressor: must
                const int started = A::calibFromOps (*ed, tuid, juce::var (ops));
                check (started == 1,
                       "21t-d w. exactly ONE loop started - the dynamics slot, not the EQ  (RED as it stood: "
                       "nothing started a loop at all)", juce::String (started));
                const auto loop = proc.calibLoad (tuid);
                check (loop.slot == 1 && loop.plugin.containsIgnoreCase ("Compressor"),
                       "21t-d w. ...on the compressor's slot", juce::String (loop.slot) + " \"" + loop.plugin + "\"");
                check (std::abs (loop.lo - 2.5f) < 0.01f && std::abs (loop.hi - 3.5f) < 0.01f,
                       "21t-d w. ...with the band from the op's gr_target_db, not the fallback",
                       juce::String (loop.lo, 1) + "-" + juce::String (loop.hi, 1));
                check (std::abs (loop.preDb - 4.0f) < 0.01f,
                       "21t-d w. ...and slot_pre_gain_db as the opening drive", juce::String (loop.preDb, 1));
                const auto si = own.getSlotInfo (1);
                // 30 Sep 2026: the mirror lands on the LIVE OUT, not on the compare-only trim. 21t-m item 1
                // deleted that third gain - the drive used to mirror into it, which is only in circuit during an
                // A/B, so every rung raised the chain by a dB and nothing took it back. This leg asserted the
                // deleted behaviour and read "4.0 / 0.0"; outGainDb is the same reading getSlotOutGainDb gives.
                check (std::abs (si.preTrimDb - 4.0f) < 0.05f && std::abs (si.outGainDb + 4.0f) < 0.05f,
                       "21t-d w. ...written to the slot with the post-trim MIRRORED onto the LIVE out",
                       juce::String (si.preTrimDb, 1) + " / " + juce::String (si.outGainDb, 1));
                // A later op with a NEW band restarts from the drive already found, not from zero.
                juce::Array<juce::var> band2; band2.add (1.0); band2.add (2.0);
                juce::Array<juce::var> ops2; ops2.add (op (2, juce::var (band2), 0.0));
                A::calibFromOps (*ed, tuid, juce::var (ops2));
                const auto loop2 = proc.calibLoad (tuid);
                check (std::abs (loop2.lo - 1.0f) < 0.01f && std::abs (loop2.preDb - 4.0f) < 0.01f
                       && loop2.steps == 0,
                       "21t-d w. a new gr_target_db re-targets and CONTINUES from the current drive",
                       juce::String (loop2.lo, 1) + "-" + juce::String (loop2.hi, 1) + " at "
                       + juce::String (loop2.preDb, 1) + " dB, " + juce::String (loop2.steps) + " step(s)");
                while (own.getNumSlots() > 0) own.removeSlot (0);
                proc.calibStore (tuid, echojay::CalibLoop{});
                EchoJayBorrowTestAccess::release (proc);
                { int e2 = 0; juce::File (LinkShm::resolveDir (e2) + "rack-" + tuid + ".json").deleteFile(); }
            }
            else check (false, "21t-d w. fixture: the two built-ins are registered");
        }

        {
            std::printf ("\n== 21t-d wiring: V2 renders the card from a loop the OTHER host is running ==\n");
            int err = 0; const auto dir = LinkShm::resolveDir (err);
            const juce::String luid = "calib_lnk";
            // A stand-in "Link": the sidecar is written from outside this instance, exactly as the Link writes it
            // after deselect. V2 owns no tallies for this rack, so it must render and not advance.
            echojay::CalibLoop remote;
            // askRung: this fixture is a loop mid-hunt, which is what V2 renders after a deselect.
            remote.begin ("Tube-Tech CL 1B", 0, 2.0f, 3.0f, 3.0f, echojay::CalibLoop::Purpose::askRung);
            remote.lastGr = 2.4f;
            // revision >= 0 is what makes a sidecar VALID on read (LinkShm), so the fixture writes a real one -
            // a rack with no revision is not a rack anyone has described.
            LinkShm::RackSidecar rc; rc.valid = true; rc.uid = luid; rc.name = "Aitch Lead Vocal"; rc.revision = 1;
            rc.calib = remote.toVar();
            LinkShm::writeRackSidecar (dir, rc);
            A::calibTick (*ed, luid);
            check (A::panelStatus (*ed) == "Tube-Tech CL 1B working 2.4 dB",
                   "21t-d w. the card comes from the OTHER host's state  (RED as it stood: no card existed)",
                   A::panelStatus (*ed));
            // ...and the waiting state renders as ruled.
            remote.state = echojay::CalibLoop::State::Waiting;
            rc.calib = remote.toVar(); LinkShm::writeRackSidecar (dir, rc);
            A::calibTick (*ed, luid);
            check (A::panelStatus (*ed) == "Waiting for playback - play the loudest part of this channel",
                   "21t-d w. ...including \"Waiting for playback\"", A::panelStatus (*ed));
            // V2 did NOT advance a loop whose tallies it does not own.
            const auto still = proc.calibLoad (luid);
            check (still.steps == 0 && still.window == 0,
                   "21t-d w. ...and V2 advanced nothing - it owns no tallies for that rack",
                   juce::String (still.window) + " window(s)");

            std::printf ("\n== 21t-d wiring: a loop that ended while the editor was closed posts on next open ==\n");
            // 30 Sep 2026: AN ENDED LOOP IS WHAT endHere() LEAVES. (g) deleted the holding tail for every
            // purpose, so a loop that has finished is Idle with its closing TEXT captured - closingMessage() reads
            // `state`, and the chat takes the text later, so the text has to survive the ending. And its opening
            // line was posted before the editor closed, so it owes no ask: without clearing askOwed this fixture
            // posted TWO messages (the opening line from (d)'s begin(), then the closing) and read "4 -> 6".
            remote.state = echojay::CalibLoop::State::Adjusted;
            remote.closingOwed = true;
            remote.closingOwedText = remote.closingMessage();
            remote.state = echojay::CalibLoop::State::Idle;
            remote.askOwed.clear();
            rc.calib = remote.toVar(); LinkShm::writeRackSidecar (dir, rc);
            const int before = (int) A::msgs (*ed).size();
            A::calibTick (*ed, luid);   // the first tick after the editor opens
            const int after1 = (int) A::msgs (*ed).size();
            check (after1 == before + 1,
                   "21t-d w. the closing message posts when the editor opens  (RED as it stood: it was never "
                   "posted at all)", juce::String (before) + " -> " + juce::String (after1));
            check (A::msgs (*ed).back().content.contains ("Adjusted the Tube-Tech CL 1B")
                   && A::msgs (*ed).back().content.contains ("2.4"),
                   "21t-d w. ...naming what was adjusted and the figure measured",
                   A::msgs (*ed).back().content.substring (0, 110));
            A::calibTick (*ed, luid);
            check ((int) A::msgs (*ed).size() == after1,
                   "21t-d w. ...EXACTLY once - a second tick posts nothing",
                   juce::String ((int) A::msgs (*ed).size()));
            juce::File (dir + "rack-" + luid + ".json").deleteFile();
        }

        // ---- 21t-d (25 Sep 2026): the Link measures BEFORE its trim, and says so -----------------------------
        // A REAL registry slot and a REAL published frame: the guard claims a slot in its own isolated Link
        // directory and publishes through LinkShm exactly as a Link does, so the processor reads it through the
        // shipping path (readLinkMeterFrame) and nothing here is a stand-in for the transport.
        {
            std::printf ("\n== 21t-d: a pre-trim frame, the strip that adds the trim back, and the block that does not ==\n");
            int err = 0; const auto dir = LinkShm::resolveDir (err);
            int fd = -1, oerr = 0;
            void* reg = LinkShm::openRegistry (dir, fd, oerr);
            check (reg != nullptr, "21t-d. fixture: the guard's own Link registry opened", dir);
            if (reg != nullptr)
            {
                const juce::String uid = "pretrim01";
                const int slot = LinkShm::claimSlot (reg, "Pre-trim Vocal", "pretrim.wav", uid, 48000.0f, 2);
                check (slot >= 0, "21t-d. fixture: a slot was claimed", juce::String (slot));
                LinkShm::setSlotActive (reg, slot, true);
                LinkShm::setSlotGain (reg, slot, -6.0f);
                // A slot whose heartbeat never moves is SKIPPED by refreshLinkRegistry (liveness is observed in
                // time, not assumed) - so the fixture beats it like a Link would, and refreshes twice so the
                // observer sees an advance.
                // 21t-i: ...and the 1 Hz RECORD FEED, because every block line is composed from the stored record
                // now and the record is fed exactly here, from whatever frame the registry shows.
                auto beat = [&] { LinkShm::bumpHeartbeat (reg, slot); proc.refreshLinkRegistry();
                                  proc.updateLinkAudioRecency(); };
                beat(); beat();
                LinkMeterFrame f;
                f.momentary = -12.0f; f.shortTerm = -14.0f; f.integrated = -16.0f;
                f.truePeakMax = -3.0f; f.shortTermTP = -7.0f;
                f.shortTermMax = -11.5f; f.heardSeconds = 42.0f;
                f.fieldsMask = kFrameHasPreTrim | kFrameHasShortMax | kFrameHasHeard;
                frameSetShort90 (f, -13.5f);    // 21t-f item 5: on the quantum AND clear of the one-decimal
                                                // rounding of an exact .25 (-13.25 prints as -13.2)
                LinkShm::publishMeterFrame (reg, slot, f);
                beat();
                {
                    bool listed = false; float sawTrim = 0.0f; int sawIdx = -1;
                    for (const auto& li : proc.getLinkSlotInfos())
                        if (li.uid == uid) { listed = true; sawTrim = li.gainDb; sawIdx = li.regIdx; }
                    check (listed && std::abs (sawTrim - (-6.0f)) < 0.01f && sawIdx == slot,
                           "21t-d. fixture: the claimed slot is listed, with its trim and its frame index",
                           juce::String (listed ? "listed" : "absent") + " trim " + juce::String (sawTrim, 1)
                           + " regIdx " + juce::String (sawIdx));
                }

                // (a) THE STRIP SHOWS WHAT THE DAW HEARS: pre-trim -16 with a trim of -6 is -22.
                const auto st = A::stripFrame (*ed, uid, slot);
                check (std::abs (st.integrated - (-22.0f)) < 0.05f,
                       "21t-d (a). the strip adds the trim back: INT -16 pre-trim with trim -6 shows -22  "
                       "(RED as it stood: the frame was post-trim and the strip showed it raw)",
                       juce::String (st.integrated, 2));
                check (std::abs (st.shortTerm - (-20.0f)) < 0.05f && std::abs (st.truePeakMax - (-9.0f)) < 0.05f,
                       "21t-d (a). ...and so do SHORT and PEAK, by the same one conversion",
                       juce::String (st.shortTerm, 1) + " / " + juce::String (st.truePeakMax, 1));

                // ...and a frame WITHOUT the bit is untouched: an older Link still means what it always meant.
                {   // ...and a frame WITHOUT SHORTMAX falls back to the 3 s pair: -7.0 - (-14.0) = 7.0.
                    //
                    // 21t-i: ON ITS OWN uid. Every block line is composed from the stored RECORD now, and a record
                    // KEEPS what was heard - so publishing a reduced frame for a uid that published a full one a
                    // moment ago would be asserting that the record forgets, which is the opposite of the ruling.
                    // What is under test is that a figure is never INVENTED from a neighbouring field, and that
                    // needs a Link which has never published one.
                    const juce::String uidNo = "nomax1";
                    const int slotNo = LinkShm::claimSlot (reg, "Fallback Link", "nomax.wav", uidNo, 48000.0f, 2);
                    check (slotNo >= 0, "21t-i. fixture: a second Link that publishes no SHORTMAX and no SHORT90",
                           juce::String (slotNo));
                    if (slotNo >= 0)
                    {
                        LinkShm::setSlotActive (reg, slotNo, true);
                        LinkShm::setSlotGain (reg, slotNo, -6.0f);
                        LinkMeterFrame noMax = f; noMax.fieldsMask = kFrameHasPreTrim | kFrameHasHeard; noMax.seq = 0;
                        LinkShm::publishMeterFrame (reg, slotNo, noMax);
                        auto beatNo = [&] { LinkShm::bumpHeartbeat (reg, slotNo); proc.refreshLinkRegistry();
                                            proc.updateLinkAudioRecency(); };
                        beatNo(); beatNo();
                        const auto gidF = proc.createLinkGroup ("Fallback set", juce::StringArray { uidNo });
                        A::targetGroup (*ed, gidF);
                        const auto fb = A::groupLevels (*ed);
                        check (fb.contains ("PSR 7.0") && fb.contains ("SHORTMAX no reading"),
                               "21t-d. ...and a Link with no SHORTMAX falls back to shortTermTP minus SHORT",
                               fb.fromFirstOccurrenceOf ("SHORTMAX", true, false).substring (0, 36));
                        check (fb.contains ("SHORT90 no reading"),
                               "21t-f (5). ...and a Link that publishes no SHORT90 says \"no reading\", never a "
                               "neighbouring figure", fb.fromFirstOccurrenceOf ("SHORT90", true, false).substring (0, 26));
                        A::targetGroup (*ed, {}); proc.removeLinkGroup (gidF);
                        LinkShm::releaseSlot (reg, slotNo);
                    }
                    LinkShm::publishMeterFrame (reg, slot, f); beat();
                }
                LinkMeterFrame old = f; old.fieldsMask = 0; old.seq = 0;
                LinkShm::publishMeterFrame (reg, slot, old);
                const auto st2 = A::stripFrame (*ed, uid + "_old", slot);
                check (std::abs (st2.integrated - (-16.0f)) < 0.05f,
                       "21t-d (a). a frame with NO pre-trim bit is stored raw - an old Link is not double-counted",
                       juce::String (st2.integrated, 2));
                LinkShm::publishMeterFrame (reg, slot, f);   // back to the pre-trim frame
                beat();

                // (b) RE-RULED 21t-i (27 Sep 2026) and asserted here 28 Sep: THE BLOCK REPORTS WHAT IS HEARD.
                // It used to report the frame's own pre-trim figures, and Sean read the block's INT beside the
                // strip's INT and found them differing by exactly the trim - six for six. The record is taken
                // AFTER the trim now, the same point the strip meter is, so moving the trim MOVES the figures
                // with it. What does not move is the measurement underneath and any DIFFERENCE taken from it:
                // PSR is asserted unchanged a few lines below, on the same pair of blocks.
                const auto gid = proc.createLinkGroup ("Levelling set", juce::StringArray { uid });
                A::targetGroup (*ed, gid);
                const auto before = A::groupLevels (*ed);
                LinkShm::setSlotGain (reg, slot, -12.0f);
                beat();
                const auto after = A::groupLevels (*ed);
                check (before.contains ("INT -22.0") && after.contains ("INT -28.0"),
                       "21t-d (b) as re-ruled. the block's INT is the figure AS HEARD, so a 6 dB trim move moves "
                       "it 6 dB  (the frame reads INT -16.0 throughout)",
                       "before " + before.fromFirstOccurrenceOf ("INT", true, false).substring (0, 8)
                       + " -> after " + after.fromFirstOccurrenceOf ("INT", true, false).substring (0, 8));
                check (before.contains ("trim -6.0 dB") && after.contains ("trim -12.0 dB"),
                       "21t-d (b). ...and the trim it prints DOES move",
                       after.fromFirstOccurrenceOf ("trim", true, false).substring (0, 16));
                // PSR is PEAK minus SHORTMAX while SHORTMAX is published: -3.0 - (-11.5) = 8.5.
                {   // 21t-e (4): the block is LOGGED, one line per member, and the figures match what was sent.
                    // The log file is found the way the WRITER finds it (FileLog::currentPath), not by guessing
                    // a path from $HOME - the two can differ, and a guard that reads the wrong file proves
                    // nothing either way.
                    const juce::File cur (juce::String (echojay::FileLog::instance().currentPath()));
                    juce::String logs;
                    for (const auto& lf : cur.getParentDirectory().findChildFiles (juce::File::findFiles, false, "echojay-*.log"))
                        logs << lf.loadFileAsString();
                    check (logs.contains ("EJGroupLevels: Pre-trim Vocal (id " + uid + ")"),
                           "21t-e (4). every member of the block is logged beside it  (RED as it stood: the block "
                           "was sent and never appeared anywhere a reader could check it)",
                           logs.fromLastOccurrenceOf ("EJGroupLevels:", true, false).upToFirstOccurrenceOf ("\n", false, false).substring (0, 120));
                    // The logged line and the block are the SAME text, so the PEAK in the log is the as-heard
                    // one the block carries (-3.0 published, -12.0 of trim).
                    check (logs.contains ("PEAK -15.0") && after.contains ("PEAK -15.0"),
                           "21t-e (4) as re-ruled. ...with the same PEAK the block carried, as heard",
                           after.fromFirstOccurrenceOf ("PEAK", true, false).substring (0, 10));
                }
                check (after.contains ("PSR 8.5"),
                       "21t-d. PSR is PEAK minus SHORTMAX when SHORTMAX is there - whole programme, not the "
                       "last 3 s  (the 3 s pair would have read 7.0)",
                       after.fromFirstOccurrenceOf ("PSR", true, false).substring (0, 20));
                check (after.contains ("SHORTMAX -23.5") && after.contains ("HEARD 42"),
                       "21t-d (b) as re-ruled. ...and SHORTMAX and HEARD are real now, not \"no reading\" "
                       "(SHORTMAX as heard: -11.5 published, -12.0 of trim)",
                       after.fromFirstOccurrenceOf ("SHORTMAX", true, false).substring (0, 34));
                // 21t-f item 5: SHORT90 is on the line, AFTER SHORTMAX, with the value the Link published.
                check (after.contains ("SHORTMAX -23.5, SHORT90 -25.5, INT"),
                       "21t-f (5) as re-ruled. SHORT90 is on the member's line, immediately after SHORTMAX, both "
                       "as heard  (RED as it stood: the token did not exist)",
                       after.fromFirstOccurrenceOf ("SHORTMAX", true, false).substring (0, 40));
                {   // the MEMBER LINE must not be marked post-trim (the note still explains what that marking
                    // would mean on an older Link's line, which is why the whole block is not what is checked).
                    juce::StringArray al; al.addLines (after);
                    juce::String memberLine;
                    for (const auto& l : al) if (l.startsWith ("  ")) { memberLine = l; break; }
                    check (memberLine.isNotEmpty() && ! memberLine.contains ("POST-TRIM"),
                           "21t-d (b). ...and the member's line is NOT marked post-trim - this Link measures "
                           "before its gain", memberLine.substring (0, 120));
                }
                A::targetGroup (*ed, {});
                proc.removeLinkGroup (gid);
                LinkShm::releaseSlot (reg, slot);
            }
        }

        // ---- 21t-i: THE LOOP IS ADVANCED, EVERY WINDOW, ON A LEASED RACK -------------------------------------
        // Sean's 14:44 log: "leased build settled -> 1 loop(s) started", the block read, and then NOT ONE
        // EJThreshold window line while the song played. calibTickAndPost had been written and guarded and NEVER
        // CALLED - the same gap the trigger had before 21t-h, one step further down the same path. This leg drives
        // the editor's own tick (not calibTickAndPost directly, which is what the old legs did and why they could
        // not see this) and asserts the loop actually judges windows.
        {
            std::printf ("\n== 21t-i: the editor's tick advances a LEASED rack's loop, and every window logs ==\n");
            A::knownState (*ed, proc);
            const juce::String luid = "lease_i1";
            auto& bh = EchoJayBorrowTestAccess::engage (proc, luid);
            while (bh.getNumSlots() > 0) bh.removeSlot (0);
            { EedCompressorProcessor fc; juce::ignoreUnused (fc); }
            proc.pendingChannelUid = luid;
            // PREPARED FIRST, as the product prepares it (prepareToPlay, and again when the lease engages): a graph
            // that is already prepared prepares each node as it is ADDED, and an unprepared LevelTally silently
            // drops every push - which is exactly what the diagnostic showed (chain-in heard 3.70 s, the slot 0.00).
            bh.prepare (48000.0, 512);
            // THE RACK IS BUILT THE WAY THE PRODUCT BUILDS IT, through sendChainToLink - not by inserting a node
            // into the host by hand. The first cut of this leg did insert by hand, and the diagnostic said why that
            // is not the same thing: the borrowed host's chain-in and chain-out tallies heard 3.70 s of audio while
            // the SLOT's own tallies heard 0.00 s, so the node was in the slot list and not in the render sequence.
            // A leg that measures a graph the product never assembles measures nothing about the product.
            const juce::String chainJson =
                R"({"chain":[{"name":"EchoJay Compressor","settings_structured":{"params":{}}}],)"
                R"("calibration":{"source":"tally","heard_s":120,"measure":"short90","mode":"passive",)"
                R"("actuator":"drive","slot":1,"start_db":0.0,"sense":null,"gr_target_db":[2,3]}})";
            A::buildToLink (*ed, luid, chainJson);
            bool built = false;
            for (int i = 0; i < 80 && ! built; ++i) { pumpMs (100); built = bh.getNumSlots() >= 1; }
            bh.prepare (48000.0, 512);
            check (bh.getNumSlots() == 1, "21t-i. fixture: a compressor in the leased rack, built through the "
                   "product's own path", juce::String (bh.getNumSlots()));

            // REAL AUDIO through the borrowed host, so its per-slot tallies close real windows - the question
            // "does getSlotLevels report measured=true while audio runs" is answered by running audio, not by
            // asserting a flag somebody set.
            juce::AudioBuffer<float> buf (2, 512);
            auto pushSeconds = [&] (double seconds, float amp)
            {
                const int blocks = (int) (seconds * 48000.0 / 512.0);
                for (int b = 0; b < blocks; ++b)
                {
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < 512; ++i)
                            buf.setSample (ch, i, amp * std::sin (2.0f * juce::MathConstants<float>::pi * 220.0f
                                                                 * (float) ((b * 512 + i) % 48000) / 48000.0f));
                    juce::MidiBuffer mb;
                    bh.process (buf, mb);
                }
            };
            pumpMs (200);                 // the product always has a running message loop; an async insert needs one
            pushSeconds (4.0, 0.25f);
            {
                // WHERE THE AUDIO GOT TO, before any conclusion about the slot. The chain-in tally is fed at the
                // TOP of ChainHost::process, before the graph and before the try-lock branch, so it separates
                // "nothing reached this host" from "the host ran but the slot did not".
                const auto ci = bh.getChainInLevels();
                const auto co = bh.getChainOutLevels();
                std::printf ("    borrowed host after 4 s: chain-in heard=%.2f s (%.1f), chain-out heard=%.2f s\n",
                             ci.heardSeconds, ci.levelDb, co.heardSeconds);
                const auto lv = bh.getSlotLevels (0);
                // THE NUMBERS FIRST. "in unknown" can mean the tally heard nothing, or heard less than the 3 s
                // floor, or was never pushed at all, and those are three different defects.
                std::printf ("    slot levels after 4 s of audio: measured=%d  in heard=%.2f s known=%d level=%.1f"
                             "  out heard=%.2f s known=%d level=%.1f  slots=%d\n",
                             (int) lv.measured, lv.in.heardSeconds, (int) lv.in.known, lv.in.levelDb,
                             lv.out.heardSeconds, (int) lv.out.known, lv.out.levelDb, bh.getNumSlots());
                check (lv.measured && lv.in.known && lv.out.known,
                       "21t-i. getSlotLevels on the BORROWED host reports measured=true with audio running - both "
                       "legs known, which is what a window needs",
                       juce::String ("measured ") + (lv.measured ? "y" : "n") + ", in "
                       + (lv.in.known ? "known" : "unknown") + ", out " + (lv.out.known ? "known" : "unknown"));
            }

            // THE BUILD STARTS THE LOOP (21t-h's fix): the block rode the chain, so by the time the dial has
            // settled there is a loop on that slot. Started by hand only if the build's own start has not landed
            // yet, so this leg is about the TICK and not about the start.
            for (int i = 0; i < 40 && ! proc.calibLoad (luid).active(); ++i) pumpMs (100);
            if (! proc.calibLoad (luid).active())
            {
                echojay::CalibLoop::Config cfg;
                cfg.plugin = "EchoJay Compressor"; cfg.slot = 0; cfg.lo = 2.0f; cfg.hi = 3.0f;
                cfg.mode = echojay::CalibLoop::Mode::Passive;
                cfg.actuator = echojay::CalibLoop::Actuator::Drive;
                cfg.startDb = 0.0f;
                proc.calibStart (luid, cfg);
            }
            check (proc.calibLoad (luid).active(), "21t-i. fixture: the loop is running on the leased uid");

            const auto logStart = juce::File (juce::String (echojay::FileLog::instance().currentPath()))
                                     .loadFileAsString().length();
            // THE EDITOR'S OWN TICK, and audio between ticks, exactly as a playing session has it. The loop decides
            // once per 3 s window, so this covers several.
            const int windowsBefore = proc.calibLoad (luid).window;
            // THE 3 s WINDOW IS WALL CLOCK, not a count of ticks: calibTick refuses to judge twice inside one
            // window and it reads the millisecond counter to know. The first cut pushed audio and ticked eight
            // times inside about a second and a half of real time, so the gate never opened once - a leg that
            // measures its own loop speed, not the product. Four windows' worth of real time, with audio through
            // the rack before each tick, is what a playing session looks like.
            for (int t = 0; t < 4; ++t) { pushSeconds (1.2, 0.25f); pumpMs (1600); A::tick (*ed); pumpMs (40); }
            const auto after = proc.calibLoad (luid);
            check (after.window > windowsBefore,
                   "21t-i. the tick JUDGED windows  (RED as it stood: nothing called calibTickAndPost, so a running "
                   "loop never saw a window - Sean's 14:44 log, word for word)",
                   juce::String (windowsBefore) + " -> " + juce::String (after.window) + " window(s)");
            {
                // THE LINE ITSELF, from the processor that emitted it. Reading the rolling log FILE asserted about
                // the environment's logging as much as about the product: under the suite's sandbox seal the file
                // was unreadable, so a judged and logged window looked exactly like a silent one and this leg went
                // RED on working code. The product keeps the line on the same statement that logs it.
                const auto line = proc.calibLastLogLine();
                const juce::File cur (juce::String (echojay::FileLog::instance().currentPath()));
                const auto fresh = cur.loadFileAsString().substring (logStart);
                int inFile = 0;
                for (const auto& l : juce::StringArray::fromLines (fresh))
                    if (l.contains ("EJThreshold: \"EchoJay Compressor\" window")) ++inFile;
                std::printf ("    window lines in the rolling log file: %d (the file is %s here)\n",
                             inFile, cur.existsAsFile() ? "readable" : "NOT readable");
                check (line.contains ("EJThreshold: \"EchoJay Compressor\" window"),
                       "21t-i. ...and EVERY judged window printed its line, whether it moved anything or not - a "
                       "stalled loop and a quiet in-band loop must not look the same",
                       line.isEmpty() ? juce::String ("no window line") : line.substring (0, 110));
                check (line.contains ("gr=") && line.contains ("mode="),
                       "21t-i. ...with the reduction, the knob and the mode on it",
                       line.isEmpty() ? juce::String ("(none)") : line.fromFirstOccurrenceOf ("window", true, false).substring (0, 80));
            }
            EchoJayBorrowTestAccess::release (proc);
            A::knownState (*ed, proc);
        }

        // ---- 21t-h (3): THE LEVEL-MATCH BLOCK IS CONSUMED ON THE CHAT ROUTE ----------------------------------
        // Sean's 27 Sep session: a group turn came back on the CHAT route and the whole <<<ECHOJAY_LEVEL_MATCH>>>
        // block was printed to him as raw JSON, with "(sent as a chat, not a build - say 'build' to build)" under
        // it. The APPLY path for level_match had existed since 21t-c; nothing ever took the block out of a chat
        // reply, because the only extraction sat where a build card was expected. B emits it on every route by
        // ruling, so the client consumes it on every route.
        {
            std::printf ("\n== 21t-h (3): a level-match block on the CHAT route becomes the card, never raw text ==\n");
            A::knownState (*ed, proc);
            const auto savedId = A::chatId (*ed);
            const auto savedMsgs = A::msgs (*ed);
            const juce::String replyText =
                "Here is the levelling for the group.\n\n"
                "<<<ECHOJAY_LEVEL_MATCH>>>\n"
                "{\"members\":["
                "{\"uid\":\"lm_a\",\"name\":\"Main vocal\",\"int_lufs\":-19.4,\"delta_db\":1.4},"
                "{\"uid\":\"lm_b\",\"name\":\"Main vocal 2\",\"int_lufs\":-17.0,\"delta_db\":-1.0},"
                "{\"uid\":\"lm_c\",\"name\":\"Main vocal 3\",\"int_lufs\":null,\"delta_db\":0}]}\n"
                "<<<END_LEVEL_MATCH>>>\n";
            A::reply (*ed, replyText);
            pumpMs (120);
            const auto& M = A::msgs (*ed);
            juce::String last;
            for (int i = (int) M.size() - 1; i >= 0; --i)
                if (M[(size_t) i].role == "assistant") { last = M[(size_t) i].content; break; }
            check (last.isNotEmpty(), "21t-h (3). fixture: the reply landed as an assistant turn",
                   last.substring (0, 50).replace ("\n", " "));
            check (! last.contains ("ECHOJAY_LEVEL_MATCH") && ! last.contains ("delta_db")
                   && ! last.contains ("int_lufs"),
                   "21t-h (3). the marker and its JSON are NOT in the bubble  (RED as it stood: the whole block was "
                   "printed to the user)", last.substring (0, 90).replace ("\n", " | "));
            check (last.contains ("Main vocal: +1.4 dB") && last.contains ("Main vocal 2: -1.0 dB"),
                   "21t-h (3). ...and the card names each member and its delta",
                   last.fromFirstOccurrenceOf ("Level match", true, false).substring (0, 90).replace ("\n", " | "));
            check (last.contains ("Main vocal 3: no signal - left alone"),
                   "21t-h (3). ...including the member with no reading, which is left alone rather than moved on a guess",
                   last.contains ("no signal") ? juce::String ("said") : juce::String ("MISSING"));
            {
                // The card is looked up by CONTENT, not by index: the reply route replaces a provisional bubble in
                // place, so "the last assistant message" is not a safe handle for the turn that carried the block.
                juce::String ed2;
                for (int i = (int) M.size() - 1; i >= 0; --i)
                {
                    const auto d = A::editDataOf (*ed, i);
                    std::printf ("    msg %d role=%s editData=%s\n", i, M[(size_t) i].role.toRawUTF8(),
                                 d.isEmpty() ? "(empty)" : d.substring (0, 60).toRawUTF8());
                    if (d.contains ("level_match")) { ed2 = d; break; }
                }
                const auto members = juce::JSON::parse (ed2).getProperty ("level_match", juce::var())
                                        .getProperty ("members", juce::var());
                check (members.isArray() && members.size() == 3,
                       "21t-h (3). ...and the card carries the ops in the SHAPE the apply path already reads, so "
                       "Apply behaves identically on every route",
                       "editData members: " + juce::String (members.isArray() ? members.size() : -1));
            }
            // ---- 21t-i: THE CARD HAS AN APPLY BUTTON ON THIS ROUTE ---------------------------------------
            // Sean, 27 Sep: "the level-match card on the chat route renders its lines but has NO Apply button, so
            // it cannot be applied - the (3) fix wired the data and not the control". The button's existence is
            // decided by the CARD'S HEIGHT, and the height is the number of rows parseChainEditOps finds in an
            // "edit" array. 21t-h's payload had the block by name and no array, so: rows 0, height 0, no button.
            {
                int lmIdx = -1;
                for (int i = (int) M.size() - 1; i >= 0; --i)
                    if (A::editDataOf (*ed, i).contains ("level_match")) { lmIdx = i; break; }
                check (lmIdx >= 0, "21t-i (apply). fixture: the card's message is found",
                       "msg " + juce::String (lmIdx));
                check (lmIdx >= 0 && A::editCardH (*ed, lmIdx) > 0,
                       "21t-i (apply). the card HAS HEIGHT, so an Apply button can be laid out at all  (RED as it "
                       "stood: the level-match payload produced no rows, so height was 0 and no button existed)",
                       "height " + juce::String (lmIdx >= 0 ? A::editCardH (*ed, lmIdx) : -1) + " px");
                // The buttons are positioned in the paint pass, exactly as on the build route.
                juce::ignoreUnused (ed->createComponentSnapshot (ed->getLocalBounds(), false, 1.0f));
                pumpMs (60);
                int btn = -1;
                for (int i = 0; i < A::applyBtnCount (*ed); ++i)
                    if (A::applyBtnMsg (*ed, i) == lmIdx) { btn = i; break; }
                check (btn >= 0, "21t-i (apply). ...and the paint pass gives THAT message an Apply button",
                       "button " + juce::String (btn) + " of " + juce::String (A::applyBtnCount (*ed)));
                check (btn >= 0 && A::applyBtn (*ed, btn).getButtonText().isNotEmpty(),
                       "21t-i (apply). ...with a label on it",
                       btn >= 0 ? A::applyBtn (*ed, btn).getButtonText() : juce::String ("(none)"));
                // AND PRESSING IT MOVES THE TRIMS. Through the button's OWN onClick, not by calling the apply
                // function: the wiring between the two is the thing that was missing.
                if (btn >= 0)
                {
                    const auto before = A::editDataOf (*ed, lmIdx);
                    A::applyBtn (*ed, btn).onClick();
                    pumpMs (120);
                    juce::String result;
                    const auto& M2 = A::msgs (*ed);
                    for (int i = (int) M2.size() - 1; i >= 0; --i)
                        if (M2[(size_t) i].role == "assistant"
                            && (M2[(size_t) i].content.contains ("level")
                                || M2[(size_t) i].content.contains ("matched")
                                || M2[(size_t) i].content.contains ("moved")))
                        { result = M2[(size_t) i].content; break; }
                    check (result.isNotEmpty(),
                           "21t-i (apply). pressing the button runs the level-match apply and says what it did",
                           result.substring (0, 90));
                    juce::ignoreUnused (before);
                }
            }
            A::setChatId (*ed, savedId);
            A::msgs (*ed) = savedMsgs;
            A::knownState (*ed, proc);
        }

        // ---- 21t-i re-cut: AN OPS-FREE CALIBRATION BLOCK REACHES THE LOOP ------------------------------------
        // Ruled 27 Sep. The user types "ease off"; the server answers with an empty ops list and a calibration
        // block carrying the nudge, because there is nothing to edit in the rack - the block IS the instruction.
        // The 9 Aug "empty-ops edit block = no block" rule dropped the whole thing, so the nudge never arrived and
        // the knob never moved. There is no card to press either: a card with no ops has no Apply button.
        {
            std::printf ("\n== 21t-i re-cut: edit:[] + a calibration carrying a nudge still reaches the loop ==\n");
            A::knownState (*ed, proc);
            const auto savedId = A::chatId (*ed);
            const auto savedMsgs = A::msgs (*ed);
            const juce::String luid = "lease_n1";
            auto& bh = EchoJayBorrowTestAccess::engage (proc, luid);
            while (bh.getNumSlots() > 0) bh.removeSlot (0);
            { EedCompressorProcessor fc; juce::ignoreUnused (fc); }
            proc.pendingChannelUid = luid;
            bh.prepare (48000.0, 512);
            if (const auto* comp = BuiltinDeviceRegistry::instance().findByName ("EchoJay Compressor"))
                bh.insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*comp), 0);
            for (int i = 0; i < 60 && bh.getNumSlots() < 1; ++i) pumpMs (50);
            check (bh.getNumSlots() == 1, "21t-i (ops-free). fixture: a compressor in the leased rack",
                   juce::String (bh.getNumSlots()));

            // A loop already running on that slot, as it would be after the build.
            {
                echojay::CalibLoop::Config cfg;
                cfg.plugin = "EchoJay Compressor"; cfg.slot = 0; cfg.lo = 2.0f; cfg.hi = 3.0f;
                cfg.mode = echojay::CalibLoop::Mode::Passive;
                cfg.actuator = echojay::CalibLoop::Actuator::Drive;
                cfg.startDb = -6.0f; cfg.heardS = 110.0f;
                proc.calibStart (luid, cfg);
            }
            const auto before = proc.calibLoad (luid);
            check (before.active() && before.pendingStep == 0,
                   "21t-i (ops-free). fixture: the loop is running and owes no step",
                   juce::String (before.pendingStep));

            // THE REPLY: an EMPTY ops list, and a calibration carrying nudge "softer" with NO band at all.
            const juce::String reply =
                "Easing it off a little.\n\n"
                "<<<ECHOJAY_CHAIN_EDIT>>>\n"
                "{\"edit\":[],\"calibration\":{\"source\":\"tally\",\"heard_s\":110,\"mode\":\"passive\","
                "\"actuator\":\"drive\",\"slot\":1,\"sense\":null,\"nudge\":\"softer\"}}\n"
                "<<<END_CHAIN_EDIT>>>\n";
            A::replyFor (*ed, reply, luid);
            pumpMs (200);
            const auto after = proc.calibLoad (luid);
            check (after.pendingStep == -1 || after.stepsTaken > before.stepsTaken,
                   "21t-i (ops-free). the nudge REACHED the loop as a re-target  (RED as it stood: the empty-ops "
                   "rule cleared the block and the nudge was never seen)",
                   "pendingStep " + juce::String (after.pendingStep) + ", steps taken "
                   + juce::String (after.stepsTaken));
            check (after.active(),
                   "21t-i (ops-free). ...and the loop is still running, not closed by a block with no ops");

            // AN UNKNOWN NUDGE LITERAL: logged, and nothing owed.
            {
                echojay::CalibLoop::Config cfg;
                cfg.plugin = "EchoJay Compressor"; cfg.slot = 0; cfg.lo = 2.0f; cfg.hi = 3.0f;
                cfg.mode = echojay::CalibLoop::Mode::Passive;
                cfg.actuator = echojay::CalibLoop::Actuator::Drive;
                cfg.startDb = -6.0f; cfg.heardS = 110.0f;
                proc.calibStart (luid, cfg);            // a clean loop, nothing owed
                const juce::String odd =
                    "Trying something.\n\n"
                    "<<<ECHOJAY_CHAIN_EDIT>>>\n"
                    "{\"edit\":[],\"calibration\":{\"source\":\"tally\",\"heard_s\":110,\"mode\":\"passive\","
                    "\"actuator\":\"drive\",\"slot\":1,\"sense\":null,\"nudge\":\"a shade less\"}}\n"
                    "<<<END_CHAIN_EDIT>>>\n";
                A::replyFor (*ed, odd, luid);
                pumpMs (200);
                const auto odd2 = proc.calibLoad (luid);
                check (odd2.pendingStep == 0,
                       "21t-i (ops-free). an unknown nudge literal owes NO step - it is logged and ignored, never "
                       "guessed in a direction", "pendingStep " + juce::String (odd2.pendingStep));
            }
            EchoJayBorrowTestAccess::release (proc);
            proc.pendingChannelUid.clear();
            A::setChatId (*ed, savedId);
            A::msgs (*ed) = savedMsgs;
            A::knownState (*ed, proc);
        }

        // ---- 21t-i: A RE-SENT CHAT REPLY CARRYING A LEVEL-MATCH BLOCK GETS NO "not a build" FOOTER ------------
        // Ruled 27 Sep. The line tells the user nothing was built and to say "build"; a level-match turn has an
        // actionable card right above it, so the line contradicts the only control on the screen.
        {
            std::printf ("\n== 21t-i: the reroute footer, and the block that suppresses it ==\n");
            const juce::String plain = "Rolled off some low end.";
            const juce::String withLm =
                "Here is the levelling for the group.\n\n"
                "<<<ECHOJAY_LEVEL_MATCH>>>\n{\"members\":[{\"uid\":\"lm_a\",\"name\":\"A\",\"int_lufs\":-19.4,"
                "\"delta_db\":1.4}]}\n<<<END_LEVEL_MATCH>>>\n";
            const auto renderedPlain = EchoJayAPI::renderRerouteReply (plain);
            const auto renderedLm    = EchoJayAPI::renderRerouteReply (withLm);
            check (renderedPlain.contains ("sent as a chat"),
                   "21t-i (footer). an ordinary re-sent reply still carries the quiet line - it is true there",
                   renderedPlain.fromFirstOccurrenceOf ("(", true, false));
            check (! renderedLm.contains ("sent as a chat") && ! renderedLm.contains ("say 'build'"),
                   "21t-i (footer). a reply carrying a LEVEL_MATCH block does NOT  (RED as it stood: the footer "
                   "printed under the card and told the user to say \"build\")",
                   renderedLm.contains ("sent as a chat") ? juce::String ("footer still there")
                                                          : juce::String ("no footer"));
            check (EchoJayAPI::replyCarriesLevelMatch (withLm) && ! EchoJayAPI::replyCarriesLevelMatch (plain),
                   "21t-i (footer). ...and the test that decides it is a question about the reply, answered the "
                   "same way wherever it is asked", "block seen / not seen");
        }

        // ---- 21t-i: THE STORED LEVEL RECORD IS WHAT THE BLOCKS ARE COMPOSED FROM ------------------------------
        // Sean, 27 Sep 15:30: after a full play-through every strip showed an INT (v3_2 -18.9, Main vocal 6 -19.4)
        // and the [GROUP LEVELS] block sent on "balance these vocal channels" carried those two as having no
        // signal. A strip HOLDS its last smoothed value; the block read the live frame through a latch whose
        // conditions all mean "audio is playing now". These legs reproduce that shape exactly: a record present,
        // the frame gone.
        {
            std::printf ("\n== 21t-i: two Links, played in turn, transport off - the block still has both ==\n");
            A::knownState (*ed, proc);
            const juce::String uidA = "lrecA", uidB = "lrecB";
            auto frameFor = [] (float intLufs, float heard)
            {
                LinkMeterFrame f;
                f.momentary = intLufs + 3.0f; f.shortTerm = intLufs + 1.0f; f.integrated = intLufs;
                f.truePeakMax = -1.2f; f.shortTermTP = -3.0f; f.shortTermMax = intLufs + 4.0f;
                f.heardSeconds = heard;
                f.fieldsMask = kFrameHasPreTrim | kFrameHasShortMax | kFrameHasHeard;
                frameSetShort90 (f, intLufs + 2.0f);
                return f;
            };
            // A PLAYS, AND STOPS. The record is fed from the frame, exactly as the 1 Hz tick feeds it.
            proc.updateLevelRecordFromFrame (uidA, frameFor (-18.9f, 95.0f));
            // B PLAYS, AND STOPS.
            proc.updateLevelRecordFromFrame (uidB, frameFor (-19.4f, 60.0f));
            // TRANSPORT OFF: the publisher blanks the momentary group and V2 sees an audioStale frame. Under the
            // old rule this was the moment the figures vanished from the block.
            {
                auto stale = frameFor (-18.9f, 95.0f);
                stale.momentary = -100.0f; stale.shortTerm = -100.0f; stale.shortTermTP = -100.0f;
                stale.audioStale = 1u;
                proc.updateLevelRecordFromFrame (uidA, stale);
                auto staleB = frameFor (-19.4f, 60.0f);
                staleB.momentary = -100.0f; staleB.shortTerm = -100.0f; staleB.shortTermTP = -100.0f;
                staleB.audioStale = 1u;
                proc.updateLevelRecordFromFrame (uidB, staleB);
            }
            const auto lineA = A::levelTokens (*ed, uidA);
            const auto lineB = A::levelTokens (*ed, uidB);
            std::printf ("    A: %s\n    B: %s\n", lineA.toRawUTF8(), lineB.toRawUTF8());
            check (lineA.contains ("INT -18.9") && lineA.contains ("HEARD 95"),
                   "21t-i (record). with the transport OFF, the first Link's line carries its INT and HEARD  (RED "
                   "as it stood: the line read \"no signal\")", lineA);
            check (lineB.contains ("INT -19.4") && lineB.contains ("HEARD 60"),
                   "21t-i (record). ...and so does the second one", lineB);
            check (lineA != "no signal" && lineB != "no signal",
                   "21t-i (record). ...and neither is \"no signal\", which now means HEARD 0 and nothing else",
                   lineA.substring (0, 20) + " / " + lineB.substring (0, 20));
            check (lineA.contains (", AGE ") && lineB.contains (", AGE ")
                   && ! lineA.endsWith ("s") && lineA.fromLastOccurrenceOf ("AGE ", false, false).containsOnly ("0123456789"),
                   "21t-i (record). ...each line carrying the record's age after HEARD, as the ruled integer with "
                   "no unit suffix", lineA.fromFirstOccurrenceOf ("AGE", true, false));
            // A CHANNEL NOBODY HAS PLAYED is the only "no signal" there is.
            check (A::levelTokens (*ed, "lrecC") == "no signal",
                   "21t-i (record). a channel with no record at all reads \"no signal\"",
                   A::levelTokens (*ed, "lrecC"));

            // THE SESSION IS SAVED AND REOPENED. The records ride the plugin's state.
            {
                juce::MemoryBlock mb;
                proc.getStateInformation (mb);
                check (proc.resetLevelRecord (uidA) || true, "21t-i (record). fixture: A is cleared in memory");
                proc.resetLevelRecord (uidB);
                check (A::levelTokens (*ed, uidA) == "no signal",
                       "21t-i (record). fixture: cleared means cleared", A::levelTokens (*ed, uidA));
                proc.setStateInformation (mb.getData(), (int) mb.getSize());
                const auto backA = A::levelTokens (*ed, uidA);
                const auto backB = A::levelTokens (*ed, uidB);
                std::printf ("    reloaded A: %s\n    reloaded B: %s\n", backA.toRawUTF8(), backB.toRawUTF8());
                check (backA.contains ("INT -18.9") && backA.contains ("HEARD 95"),
                       "21t-i (record). the figures survive a session save and reload", backA);
                check (backB.contains ("INT -19.4") && backB.contains ("HEARD 60"),
                       "21t-i (record). ...for every channel that had a record", backB);
            }
            // A RESET CLEARS ONE LINK ONLY.
            {
                proc.resetLevelRecord (uidA);
                check (A::levelTokens (*ed, uidA) == "no signal",
                       "21t-i (record). an explicit reset clears THAT channel", A::levelTokens (*ed, uidA));
                check (A::levelTokens (*ed, uidB).contains ("INT -19.4"),
                       "21t-i (record). ...and only that channel", A::levelTokens (*ed, uidB));
            }
            A::knownState (*ed, proc);
        }

        // ---- 21t-h: THE REAL LEASED BUILD PATH, END TO END ---------------------------------------------------
        // WHY THIS LEG EXISTS. Sean's 10:15 build: "path=SESSION (borrowed host) adds=1" and, in the SAME second,
        // "EJThreshold: block not usable - slot 0 is not in this rack (0 slot(s))". The add landed six seconds later.
        // Every (6) leg so far called startCalibrationFromChain DIRECTLY, on a rack that was already built, so none
        // of them could see that the Build button starts it one message-loop turn too early. This one goes through
        // sendChainToLink - the button's own choke point - and waits the way the product waits.
        {
            std::printf ("\n== 21t-h: a build to a LEASED rack starts the loop when the build FINISHES ==\n");
            A::knownState (*ed, proc);
            const juce::String luid = "lease_h1";
            auto& bh = EchoJayBorrowTestAccess::engage (proc, luid);
            while (bh.getNumSlots() > 0) bh.removeSlot (0);
            { EedCompressorProcessor fc; juce::ignoreUnused (fc); }
            proc.pendingChannelUid = luid;
            check (bh.getNumSlots() == 0,
                   "21t-h. fixture: the leased rack starts EMPTY, which is the state the premature start read",
                   juce::String (bh.getNumSlots()) + " slot(s)");

            const juce::String chainJson =
                R"({"chain":[{"name":"EchoJay Compressor","settings_structured":{"params":{}}}],)"
                R"("calibration":{"source":"tally","heard_s":120,"measure":"short90","mode":"passive",)"
                R"("actuator":"drive","slot":1,"start_db":2.0,"sense":null,"gr_target_db":[2,3]}})";

            A::buildToLink (*ed, luid, chainJson);
            // Asynchronous on this path: the adds are queued into the borrowed host and the dial settles after
            // them. The leg pumps and asserts on the LOOP, never on a timer of its own invention.
            bool active = false; int slots = 0;
            for (int i = 0; i < 80 && ! active; ++i)
            {
                pumpMs (100);
                slots = bh.getNumSlots();
                active = proc.calibLoad (luid).active();
            }
            check (slots >= 1,
                   "21t-h. the build lands its slot in the borrowed host (asynchronously, which is the whole point)",
                   juce::String (slots) + " slot(s)");
            check (active,
                   "21t-h. ...and the loop is RUNNING afterwards  (RED as it stood: the start ran at send time, "
                   "read 0 slots and rejected the block - Sean's 10:15 log, word for word)",
                   active ? juce::String ("active") : juce::String ("never started"));
            {
                const auto loop = proc.calibLoad (luid);
                check (! active || (loop.slot == 0 && loop.mode == echojay::CalibLoop::Mode::Passive),
                       "21t-h. ...on the slot the block named, passive, with the staged drive as its opening value",
                       "slot " + juce::String (loop.slot) + ", drive " + juce::String (loop.preDb, 1) + " dB");
            }
            EchoJayBorrowTestAccess::release (proc);
            proc.pendingChannelUid.clear();
            A::knownState (*ed, proc);
        }

        // ---- 21t-f (5): SHORT90 RIDES ONE BYTE, AND THE FRAME DID NOT GROW ----------------------------------
        {
            std::printf ("\n== 21t-f (5): SHORT90 in one byte of pad - encoding, absence, and the stride ==\n");
            check (sizeof (LinkMeterFrame) == 128,
                   "21t-f (5). the frame is still 128 bytes - the size IS the mapping stride, so a growth would "
                   "move every frame after slot 0 under any instance that had not been rebuilt",
                   juce::String ((int) sizeof (LinkMeterFrame)) + " bytes");
            LinkMeterFrame f {};
            check (! frameHasShort90 (f) && frameShort90Db (f) < -99.0f,
                   "21t-f (5). a zero frame - an OLD writer's whole zero pad - reads as ABSENT, never as -60",
                   juce::String (frameShort90Db (f), 1));
            frameSetShort90 (f, -13.25f);
            check (frameHasShort90 (f) && std::abs (frameShort90Db (f) - (-13.25f)) < 0.001f,
                   "21t-f (5). a value on the quantum round-trips exactly", juce::String (frameShort90Db (f), 2));
            frameSetShort90 (f, -13.4f);
            check (std::abs (frameShort90Db (f) - (-13.5f)) <= 0.125f + 0.001f,
                   "21t-f (5). ...and a value off it lands within half a quantum (0.25 LU steps, stated)",
                   juce::String (frameShort90Db (f), 2));
            frameSetShort90 (f, -200.0f);
            check (std::abs (frameShort90Db (f) - (-60.0f)) < 0.001f,
                   "21t-f (5). silence clamps to the floor, it does not wrap", juce::String (frameShort90Db (f), 1));
            frameSetShort90 (f, 12.0f);
            check (std::abs (frameShort90Db (f) - 0.0f) < 0.001f,
                   "21t-f (5). ...and the ceiling clamps too", juce::String (frameShort90Db (f), 1));
            frameSetShort90 (f, std::numeric_limits<float>::quiet_NaN());
            check (! frameHasShort90 (f) && f.short90Code == 0 && (f.fieldsMask & kFrameHasShort90) == 0,
                   "21t-f (5). NaN publishes NOTHING and clears the promise - the writer cannot claim a field it "
                   "has no reading for");
            // The byte it lives in is the key group's pad, and the key group must still read correctly beside it.
            LinkMeterFrame k {};
            k.keyRoot = 7; k.keyIsMinor = 1; k.keyConfidence = 0.8f; k.fieldsMask = kFrameHasKey;
            frameSetShort90 (k, -18.0f);
            check (k.keyRoot == 7 && k.keyIsMinor == 1 && std::abs (k.keyConfidence - 0.8f) < 0.001f
                   && std::abs (frameShort90Db (k) - (-18.0f)) < 0.001f,
                   "21t-f (5). ...and it does not disturb the key group it shares four bytes with",
                   "root " + juce::String ((int) k.keyRoot) + " minor " + juce::String ((int) k.keyIsMinor)
                   + " short90 " + juce::String (frameShort90Db (k), 1));
        }

        // ---- 21t-g (6): THE BUILD BODY CARRIES THE TARGET CHANNEL'S OWN TALLY -------------------------------
        // [TRACK LEVELS] is what lets the server set a compressor from figures already kept instead of asking the
        // user to play the track again - which is the whole reason the loop's default mode is passive. Same tokens
        // as a [GROUP LEVELS] member line, for one channel, and NEVER beside the group block.
        {
            std::printf ("\n== 21t-g (6): the turn carries [TRACK LEVELS] for the chat's own channel ==\n");
            // KNOWN STATE IN, KNOWN STATE OUT, and that includes THE CHAT: this leg moves the chat to a channel,
            // and the first cut left it there - the (13) arm-bubble leg further down then wrote its messages into a
            // different chat and read back "<none>" for chips that were never missing.
            const auto savedId   = A::chatId (*ed);
            const auto savedMsgs = A::msgs (*ed);
            A::knownState (*ed, proc);
            int e1 = 0; const auto dir = LinkShm::resolveDir (e1);
            int fd = -1, e2 = 0;
            if (auto* reg = LinkShm::openRegistry (dir, fd, e2))
            {
                const juce::String uid = "trklv1";   // SHORT ON PURPOSE: the registry's uid field is fixed-width and
                                                    // claimSlot truncates in silence - "trk_levels_1" landed as
                                                    // "trk_levels_" and every lookup by the full name missed.
                const int slot = LinkShm::claimSlot (reg, "Nafe Lead Vocal", "nafe.wav", uid, 48000.0f, 2);
                if (slot >= 0)
                {
                    // Exactly the 21t-d fixture's shape (no placement call - that leg proved this is what
                    // refreshLinkRegistry lists), so this leg tests the BLOCK and not my fixture.
                    LinkShm::setSlotActive (reg, slot, true);
                    LinkShm::setSlotGain (reg, slot, -3.0f);
                    // 21t-i: ...and the 1 Hz RECORD FEED, because every block line is composed from the stored
                    // record now and the record is fed exactly here, from whatever frame the registry shows.
                    auto beat = [&] { LinkShm::bumpHeartbeat (reg, slot); proc.refreshLinkRegistry();
                                      proc.updateLinkAudioRecency(); };
                    beat(); beat();
                    LinkMeterFrame f;
                    f.momentary = -13.0f; f.shortTerm = -14.0f; f.integrated = -17.0f;
                    f.truePeakMax = -1.0f; f.shortTermTP = -9.0f;
                    f.shortTermMax = -12.0f; f.heardSeconds = 120.0f;
                    f.fieldsMask = kFrameHasPreTrim | kFrameHasShortMax | kFrameHasHeard;
                    frameSetShort90 (f, -14.25f);
                    LinkShm::publishMeterFrame (reg, slot, f);
                    beat();

                    // NOTHING BETWEEN THE LAST HEARTBEAT AND THE READ. selectRack used to sit here, and its
                    // registry refresh dropped this slot as not-yet-live (liveness is observed in TIME, so two
                    // beats in the same millisecond are not enough) - the block was then empty for a fixture
                    // reason, not a product one. The chat target is stated directly, the way the [CURRENT CHAIN]
                    // leg states it, and both calls below take the turn's uid explicitly anyway.
                    proc.pendingChannelUid = uid;
                    const auto tl = A::trackLevels (*ed, uid);
                    check (tl.startsWith ("[TRACK LEVELS"),
                           "21t-g (6e). the block is there, in the documented shape  (RED as it stood: there was "
                           "no single-track block at all)", tl.substring (0, 60));
                    // THE WHOLE LINE, PRINTED VERBATIM, because the server runs its parser on exactly this text.
                    std::printf ("    [TRACK LEVELS] as composed:\n    %s\n", tl.toRawUTF8());
                    // THE SERVER MATCHES THE BLOCK'S NAME AGAINST THE REQUEST'S `channel` FIELD and drops the
                    // tally on a mismatch. Both are printed side by side, and the uid is asserted in the header
                    // because the two strings can differ: `channel` is materialContextName() (channelDisplayLabel,
                    // deliberately EMPTY when the label is only a uid passthrough) while the block's name comes
                    // from the display list.
                    {
                        const auto bodyChannel = A::materialName (*ed);
                        const auto blockName   = tl.fromFirstOccurrenceOf ("\"", false, false)
                                                   .upToFirstOccurrenceOf ("\"", false, false);
                        std::printf ("    body channel field = \"%s\"   block name = \"%s\"\n",
                                     bodyChannel.toRawUTF8(), blockName.toRawUTF8());
                        // The server compares them TRIMMED AND CASE-FOLDED, and refuses the tally with a
                        // [tally-channel-mismatch] note on any difference - so the guard compares them the same way.
                        check (bodyChannel.trim().equalsIgnoreCase (blockName.trim()),
                               "21t-g (6e). the block's quoted name is the same string the body's channel field "
                               "carries, trimmed and case-folded, so the server's match cannot refuse the tally",
                               "\"" + bodyChannel + "\" vs \"" + blockName + "\"");
                        check (tl.contains ("(id " + uid + ")"),
                               "21t-g (6e). ...and the uid rides in the header too, because those two CAN differ "
                               "(an unusable label makes the channel field empty)",
                               tl.upToFirstOccurrenceOf ("]", true, false));
                    }
                    // 21t-i, asserted here 28 Sep: AS HEARD. The frame publishes SHORT90 -14.25, INT -17.0 and
                    // PEAK -1.0; this Link's trim is -3.0, and the block reports what a listener hears.
                    check (tl.contains ("SHORT90 -17.2") && tl.contains ("INT -20.0")
                           && tl.contains ("PEAK -4.0") && tl.contains ("HEARD 120"),
                           "21t-g (6e) as re-ruled. ...carrying this channel's own figures as heard, token for "
                           "token (the frame's -14.25 / -17.0 / -1.0 under a -3.0 trim)",
                           tl.fromFirstOccurrenceOf ("SHORTMAX", true, false).substring (0, 60));
                    const auto composed = A::body (*ed, "make it harder", uid);
                    check (composed.contains ("[TRACK LEVELS"),
                           "21t-g (6e). ...and the COMPOSED TURN carries it, not just the builder",
                           composed.contains ("[TRACK LEVELS") ? juce::String ("attached") : juce::String ("absent"));

                    // ...and never beside the group block: two loudness blocks on one turn are two answers.
                    const auto gid = proc.createLinkGroup ("Vox", juce::StringArray { uid });
                    A::targetGroup (*ed, gid);
                    const auto both = A::body (*ed, "level these", uid);
                    check (both.contains ("[GROUP LEVELS") && ! both.contains ("[TRACK LEVELS"),
                           "21t-g (6e). a GROUP turn carries the group block and not the track one",
                           both.contains ("[GROUP LEVELS") ? juce::String ("group only") : juce::String ("neither"));
                    A::targetGroup (*ed, {}); proc.removeLinkGroup (gid);
                    proc.pendingChannelUid.clear();
                    LinkShm::releaseSlot (reg, slot);
                }
                else check (false, "21t-g (6e). fixture: a Link slot was claimed");
            }
            else check (false, "21t-g (6e). fixture: the isolated registry opened");
            A::knownState (*ed, proc);
            A::setChatId (*ed, savedId);
            A::msgs (*ed) = savedMsgs;
        }

        // ---- 21t-d (c): SHORTMAX is the max of SHORT since the tally started, and resets with it -------------
        {
            std::printf ("\n== 21t-d (c): SHORTMAX follows the short-term window and resets with the tally ==\n");
            echojay::LevelTally tally { echojay::LevelTally::Weighting::K };
            tally.prepare (48000.0);
            std::vector<float> buf (4800, 0.0f);
            // A MOVING signal, not a constant tone: a gated loudness meter is specified against programme
            // material, and a DC-flat block is not that. 1 kHz-ish alternating samples at the asked amplitude.
            juce::Random rng (1234);
            auto pushSeconds = [&] (float amp, double seconds)
            {
                const int blocks = (int) (seconds * 10.0);   // 4800 samples at 48k = 0.1 s
                for (int i = 0; i < blocks; ++i)
                {
                    for (int k = 0; k < (int) buf.size(); ++k)
                        buf[(size_t) k] = amp * (rng.nextFloat() * 2.0f - 1.0f);
                    tally.push (buf.data(), buf.data(), (int) buf.size());
                }
            };
            pushSeconds (0.1f, 4.0);                       // ~-20 dBFS for 4 s
            const auto quiet = tally.snapshot();
            check (quiet.maxShortTermDb == quiet.maxShortTermDb,
                   "21t-d (c). a closed 3 s window gives a SHORTMAX", juce::String (quiet.maxShortTermDb, 1));
            const float afterQuiet = quiet.maxShortTermDb;
            pushSeconds (0.5f, 4.0);                       // ~14 dB louder for 4 s
            const auto loud = tally.snapshot();
            check (loud.maxShortTermDb > afterQuiet + 5.0f,
                   "21t-d (c). ...and it RISES to the loudest 3 s window",
                   juce::String (afterQuiet, 1) + " -> " + juce::String (loud.maxShortTermDb, 1));
            pushSeconds (0.1f, 4.0);                       // quiet again
            const auto back = tally.snapshot();
            check (std::abs (back.maxShortTermDb - loud.maxShortTermDb) < 0.5f,
                   "21t-d (c). ...and it HOLDS when the signal drops - it is a max, not a follower",
                   juce::String (back.maxShortTermDb, 1));
            check (back.shortTermDb < back.maxShortTermDb - 5.0f,
                   "21t-d (c). ...while SHORT itself has come back down",
                   juce::String (back.shortTermDb, 1) + " vs " + juce::String (back.maxShortTermDb, 1));
            tally.resetShortTermMax();
            pushSeconds (0.1f, 4.0);
            const auto reset = tally.snapshot();
            check (reset.maxShortTermDb < loud.maxShortTermDb - 5.0f,
                   "21t-d (c). ...and resetting the tally's max starts it again from the quiet material",
                   juce::String (reset.maxShortTermDb, 1));
            const float heardBefore = reset.heardSeconds;
            pushSeconds (0.1f, 3.0);
            const auto grown = tally.snapshot();
            check (grown.heardSeconds > heardBefore,
                   "21t-d (c). HEARD counts UP as audio arrives - it is a measurement of how much was heard, "
                   "not a clock", juce::String (heardBefore, 1) + " s -> " + juce::String (grown.heardSeconds, 1) + " s");
        }

        // ---- 21t-c (25 Sep 2026): a group turn is about the MEMBERS ------------------------------------------
        // "level these vocal channels" on a group of 7 went out with all 14 Links, none of them carrying a
        // loudness figure, while the Link tab was showing MOM/SHORT/INT for all seven.
        {
            std::printf ("\n== 21t-c: a group turn carries the members only, with their own readings ==\n");
            // 14 Links, 7 of them members. One member is given no meter frame at all - that is the "no signal"
            // case, and it must be SAID, not omitted.
            std::vector<EchoJayProcessor::LinkSlotInfo> rows;
            juce::StringArray memberUids;
            for (int i = 0; i < 14; ++i)
            {
                EchoJayProcessor::LinkSlotInfo li;
                li.uid = "grp_lnk_" + juce::String (i);
                li.name = (i < 7 ? "Main vocal " : "Other ") + juce::String (i + 1);
                li.connected = true; li.gainDb = -2.0f - (float) i * 0.1f; li.regIdx = -1;   // regIdx -1 = no live frame
                rows.push_back (li);
                if (i < 7) memberUids.add (li.uid);
            }
            const auto gid = proc.createLinkGroup ("Main vocals", memberUids);
            A::targetGroup (*ed, gid); pumpMs (40);
            // INJECTED AFTER the selection and with no pump between here and the assertions: the editor's own
            // 1 Hz tick rebuilds the registry from the real one (empty under an isolated home) and would wipe
            // the rows this leg owns.
            EchoJayAlignTestAccess::setLinks (proc, rows);

            const auto body = EchoJayAPIRequestPin::body (proc.getApi(), juce::StringArray { "user" },
                                                          juce::StringArray { "level these vocal channels" }, "sys", {});
            juce::ignoreUnused (body);
            proc.getApi().setGroupsContext (proc.linksBodyVar(), proc.groupsBodyVar());
            const auto body2 = EchoJayAPIRequestPin::body (proc.getApi(), juce::StringArray { "user" },
                                                           juce::StringArray { "level these vocal channels" }, "sys", {});
            int linkCount = 0; bool anyNonMember = false;
            // THE VAR IS HELD: a pointer into the temporary linksBodyVar() returns dangles the moment the full
            // expression ends (the 18 Sep Pro Tools crash class, in a guard this time - it read 0 of 14 and
            // looked like a product failure).
            const juce::var linksVar = proc.linksBodyVar();
            if (auto* la = linksVar.getArray())
            {
                linkCount = la->size();
                for (const auto& lv : *la)
                    if (! memberUids.contains (lv.getProperty ("instanceId", juce::var()).toString())) anyNonMember = true;
            }
            check (linkCount == 7,
                   "21t-c. the body's links list holds exactly the 7 members  (RED as it stood: all 14 went out)",
                   juce::String (linkCount) + " of 14");
            check (! anyNonMember, "21t-c. ...and no non-member appears in it");
            check (body2.contains ("\"selectedGroupId\""), "21t-c. ...and the group id rides with it");

            const auto block = A::groupLevels (*ed);
            check (block.contains ("[GROUP LEVELS"),
                   "21t-c. the turn carries a [GROUP LEVELS] block  (RED as it stood: no such block existed)",
                   block.substring (0, 70));
            juce::StringArray blockLines;
            blockLines.addLines (block);
            int memberLines = 0, noSignal = 0;
            for (const auto& l : blockLines)
                if (l.startsWith ("  ")) { ++memberLines; if (l.contains ("no signal")) ++noSignal; }
            check (memberLines == 7, "21t-c. ...with one line per member, and only the members",
                   juce::String (memberLines) + " line(s)");
            check (noSignal == 7,
                   "21t-c. ...and a member with no frame is sent as \"no signal\", never omitted and never zero",
                   juce::String (noSignal) + " of " + juce::String (memberLines));
            for (const auto& l : blockLines)
                if (l.startsWith ("  ")) { check (l.contains ("trim "), "21t-c. ...and every line carries the member's trim", l.substring (0, 60)); break; }
            // A member WITH a frame carries every field the Link publishes, and the trim does not rewrite them.
            {
                EchoJayLinkFrameTestAccess::set (proc, memberUids[0], -14.2f, -16.8f, -18.4f, -1.2f, -9.9f);
                const auto withFrame = A::groupLevels (*ed);
                juce::StringArray ls; ls.addLines (withFrame);
                juce::String first;
                for (const auto& l : ls) if (l.startsWith ("  ")) { first = l; break; }
                // THE TOKENS ARE ASSERTED LITERALLY, in the ruled order and spelling.
                check (first.contains (" (id ") && first.contains ("): trim ") && first.contains (" dB, MOM ")
                       && first.contains (", SHORT ") && first.contains (", SHORTMAX ") && first.contains (", INT ")
                       && first.contains (", PEAK ") && first.contains (", PSR ") && first.contains (", HEARD "),
                       "21t-c. a member's line carries the ruled tokens: name (id uid): trim, MOM, SHORT, "
                       "SHORTMAX, INT, PEAK, PSR, HEARD", first.substring (0, 140));
                check (withFrame.contains ("[GROUP LEVELS - \"Main vocals\"]"),
                       "21t-c. ...under the ruled header", withFrame.substring (2, 40));
                check (first.contains ("SHORTMAX no reading") && first.contains ("HEARD no reading"),
                       "21t-c. ...and the two the Link does not publish say \"no reading\", never a number",
                       first.substring (0, 140));
                check (withFrame.contains ("POST-TRIM"),
                       "21t-c. ...and the block SAYS the figures are post-trim, so the model is not told they "
                       "are input levels when the Link meters after its gain");
                check (withFrame.contains ("do not ask for a listen pass"),
                       "21t-c. ...and levelling never asks for a listen pass");
                check (first.contains ("INT -18.4") && first.contains ("PEAK -1.2") && first.contains ("PSR 6.9"),
                       "21t-c. ...and the figures are the frame's own, PSR computed from its two terms", first.substring (0, 130));
                // A TRIM CHANGE DOES NOT REWRITE THE MEMBER'S INT: the figure comes from the frame, and the frame
                // is not a function of the trim we just moved.
                auto rows2 = rows; rows2[0].gainDb = rows[0].gainDb - 6.0f;
                EchoJayAlignTestAccess::setLinks (proc, rows2);
                const auto after = A::groupLevels (*ed);
                juce::StringArray ls2; ls2.addLines (after);
                juce::String firstAfter;
                for (const auto& l : ls2) if (l.startsWith ("  ")) { firstAfter = l; break; }
                check (firstAfter.contains ("INT -18.4") && firstAfter.contains ("trim -8.0 dB"),
                       "21t-c. moving a member's trim changes the trim it reports and NOT its INT",
                       firstAfter.substring (0, 110));
                EchoJayAlignTestAccess::setLinks (proc, rows);
                EchoJayLinkFrameTestAccess::clear (proc);
            }
            A::targetGroup (*ed, {});
            proc.removeLinkGroup (gid);
        }

        // ---- 21t-c: the apply side - a level_match moves each member's own trim ------------------------------
        {
            std::printf ("\n== 21t-c: level_match adds each member's delta to its own Link trim ==\n");
            int err = 0; const auto dir = LinkShm::resolveDir (err);
            std::vector<EchoJayProcessor::LinkSlotInfo> rows;
            const char* uids[4] = { "lm_a", "lm_b", "lm_c", "lm_d" };
            const float trims[4] = { -2.0f, -3.0f, -4.0f, -5.0f };
            for (int i = 0; i < 4; ++i)
            {
                EchoJayProcessor::LinkSlotInfo li;
                li.uid = uids[i]; li.name = juce::String ("Member ") + juce::String (i + 1);
                li.connected = true; li.gainDb = trims[i];
                rows.push_back (li);
                juce::File (dir + "ctrl-cmd-" + juce::String (uids[i]) + ".json").deleteFile();
            }
            EchoJayAlignTestAccess::setLinks (proc, rows);
            juce::Array<juce::var> members;
            auto member = [] (const char* uid, const char* nm, juce::var intL, double d)
            {
                auto* o = new juce::DynamicObject();
                o->setProperty ("uid", uid); o->setProperty ("name", nm);
                o->setProperty ("int_lufs", intL); o->setProperty ("delta_db", d);
                return juce::var (o);
            };
            members.add (member ("lm_a", "Member 1", juce::var (-19.5), -1.5));
            members.add (member ("lm_b", "Member 2", juce::var (-18.0),  0.0));
            members.add (member ("lm_c", "Member 3", juce::var (-16.0),  2.0));
            members.add (member ("lm_d", "Member 4", juce::var(),        0.0));   // no signal
            const int moved = A::levelMatch (*ed, juce::var (members));
            auto sentGain = [&] (const char* uid, float& out) -> bool
            {
                const auto v = juce::JSON::parse (juce::File (dir + "ctrl-cmd-" + juce::String (uid) + ".json").loadFileAsString());
                if (! v.isObject() || ! v.getDynamicObject()->hasProperty ("gainDb")) return false;
                out = (float) (double) v.getProperty ("gainDb", juce::var (0.0));
                return true;
            };
            float ga = 0, gb = 0, gc = 0, gd = 0;
            const bool hasA = sentGain ("lm_a", ga), hasB = sentGain ("lm_b", gb), hasC = sentGain ("lm_c", gc), hasD = sentGain ("lm_d", gd);
            check (hasA && std::abs (ga - (-3.5f)) < 0.01f,
                   "21t-c. -1.5 dB lands on a trim of -2.0 as -3.5  (RED as it stood: no level_match apply existed)",
                   hasA ? juce::String (ga, 2) : juce::String ("no command written"));
            check (! hasB || std::abs (gb - (-3.0f)) < 0.01f,
                   "21t-c. a delta of 0 leaves the trim where it was", hasB ? juce::String (gb, 2) : juce::String ("no command - unchanged"));
            check (hasC && std::abs (gc - (-2.0f)) < 0.01f,
                   "21t-c. +2.0 dB lands on a trim of -4.0 as -2.0", hasC ? juce::String (gc, 2) : juce::String ("no command written"));
            check (! hasD,
                   "21t-c. the no-signal member is NOT touched - nothing was written for it",
                   hasD ? juce::String (gd, 2) : juce::String ("no command, as it must be"));
            check (moved == 2, "21t-c. ...and exactly two trims were written", juce::String (moved));
            for (int i = 0; i < 4; ++i) juce::File (dir + "ctrl-cmd-" + juce::String (uids[i]) + ".json").deleteFile();
        }

        // ---- R1 (21t-b, 25 Sep 2026): the rack you look at is not the channel you build on -------------------
        // O3: a mix-bus request built into "Aitch Lead Vocal" because that rack was on screen. The view and the
        // Working-on channel were ONE variable, so selecting a strip moved the destination silently.
        {
            std::printf ("\n== R1: a rack selection moves the view; the build targets the chat's channel ==\n");
            const auto savedId = A::chatId (*ed);
            A::setChatId (*ed, {});
            A::unpinView (*ed);                          // 21t-e: the pin is a rack SELECTION, not a fixture default
            proc.pendingChannelUid = "lnk_01";          // the chat is working on lnk_01
            check (A::workingOn (*ed) == "lnk_01" && A::viewUid (*ed) == "lnk_01",
                   "R1. fixture: with nothing selected, the view follows the chat's channel", A::viewUid (*ed));
            A::selectViewOnly (*ed, "lnk_02"); pumpMs (40);
            check (A::viewUid (*ed) == "lnk_02",
                   "R1. selecting a rack moves the view to it", A::viewUid (*ed));
            check (A::workingOn (*ed) == "lnk_01",
                   "R1. ...and the chat is STILL working on its own channel  (RED as it stood: the selection "
                   "moved the build target too, which is how a bus request built into another Link)",
                   A::workingOn (*ed));
            check (proc.pendingChannelUid == "lnk_01",
                   "R1. ...and the chat was not thrown away by the selection", proc.pendingChannelUid);
            // THE BUTTON'S OWN DECISION: Build moves the view to whatever it targeted, so where the view lands
            // after a press is what the build acted on.
            A::setActiveBuilds (*ed, 1);
            A::setBuildJson (*ed, 0, "{\"chain\":[{\"name\":\"EchoJay Gain\"}]}");
            A::buildBtn (*ed, 0).triggerClick(); pumpMs (80);
            check (A::viewUid (*ed) == "lnk_01",
                   "R1. pressing Build targets the chat's channel, and the view follows it there  (RED as it "
                   "stood: it built on the rack that happened to be on screen)", A::viewUid (*ed));
            // The own channel: a main chat builds locally, and the view returns to the local rack first.
            proc.pendingChannelUid.clear();
            A::selectViewOnly (*ed, "lnk_02"); pumpMs (40);
            check (A::viewUid (*ed) == "lnk_02" && A::workingOn (*ed).isEmpty(),
                   "R1. fixture: a main chat with another rack on screen", A::viewUid (*ed));
            A::buildBtn (*ed, 0).triggerClick(); pumpMs (80);
            check (A::viewUid (*ed).isEmpty(),
                   "R1. building on the own channel switches the view to the own rack first",
                   A::viewUid (*ed).isEmpty() ? juce::String ("(local rack)") : A::viewUid (*ed));
            A::setActiveBuilds (*ed, 0);
            A::setChatId (*ed, savedId);
            A::toChat (*ed); pumpMs (40);
        }

        // ---- R2 (21t-b, 25 Sep 2026): a lease handover never locks the user out ------------------------------
        // O2: three edits sat unacknowledged, the rack stayed held for six minutes and another rack was stuck on
        // "Connecting to rack...". The wait is 5 s, the edits are parked against that rack, the rack is released,
        // and the queue is retried the next time the rack is engaged.
        {
            std::printf ("\n== R2: unacked edits park, the rack is released, and the queue survives ==\n");
            int err = 0; const auto dir = LinkShm::resolveDir (err);
            juce::File (dir + "chain-ack-lnk_09.json").deleteFile();
            EchoJayBorrowTestAccess::engage (proc, "lnk_09");
            EchoJayBorrowTestAccess::queuePush (proc, 424242, "lnk_09-424242", "bypass");
            EchoJayBorrowTestAccess::queuePush (proc, 424243, "lnk_09-424243", "remove");
            check (EchoJayBorrowTestAccess::pending (proc) == 2, "R2. fixture: two edits are queued");
            // No Link exists under the isolated home, so nothing will ever answer - which is the case under test.
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            proc.borrowApplyAndRelease (true);
            const double waited = juce::Time::getMillisecondCounterHiRes() - t0;
            check (waited < 6500.0,
                   "R2. the wait is bounded at 5 s, measured on the wall clock  (RED as it stood: 8 s, and the "
                   "rack stayed held afterwards)", juce::String (waited, 0) + " ms");
            check (! proc.borrowActive(),
                   "R2. the rack is RELEASED even though the edits did not land - a handover never locks you out");
            check (EchoJayBorrowTestAccess::parked (proc, "lnk_09") == 2,
                   "R2. ...and the two edits are PARKED against that rack, not thrown away",
                   juce::String (EchoJayBorrowTestAccess::parked (proc, "lnk_09")));
            const auto bn = EchoJayBorrowTestAccess::banner (proc);
            check (bn.contains ("2 edit(s) have not reached") && bn.contains ("bypass") && bn.contains ("remove"),
                   "R2. ...and the banner says how many and which ones", bn.substring (0, 120));
            check (proc.borrowRearmParkedFor ("lnk_09") == 2 && EchoJayBorrowTestAccess::pending (proc) == 2,
                   "R2. engaging that rack again re-arms the queue - the retry the ruling promises");
            check (EchoJayBorrowTestAccess::parked (proc, "lnk_09") == 0,
                   "R2. ...and the parked copy is consumed, so the edits cannot be sent twice");
            EchoJayBorrowTestAccess::release (proc);
            EchoJayBorrowTestAccess::clearPending (proc);
        }

        // ---- F1 REVISED (21s-b): ONE assistant panel, mounted on every tab that has a column ------------------
        // THE COMPLAINT: the whole chat - banner, transcript, composer - is missing on Link, Visualisation,
        // Compare and Meters. One line said so: the panel was gated on "Chat or Chain", which was the rule for
        // which tabs own a chat REPLY, applied to the whole panel.
        {
            std::printf ("\n== F1 revised: the assistant panel is on every tab that has a column for it ==\n");
            ed->setSize (2000, 1100);
            A::toChat (*ed); pumpMs (40);
            const int msgs = (int) A::msgs (*ed).size();
            check (msgs > 0, "F1r. fixture: the transcript has messages", juce::String (msgs));
            struct T { A::Tb tab; const char* name; };
            const T six[] = { { A::tabChat(), "Chat" }, { A::tabChain(), "Chain" }, { A::tabLink(), "Link" },
                              { A::tabVis(), "Visualisation" }, { A::tabComp(), "Compare" }, { A::tabMeters(), "Meters" } };
            for (const auto& t : six)
            {
                A::toTab (*ed, t.tab); ed->resized(); pumpMs (40);
                const bool panel = A::panelVisibleOn (*ed);
                check (panel, juce::String ("F1r. ") + t.name + ": the panel has a column  (RED as it stood on four of these)");
                check (panel == A::replyAllowed (*ed),
                       juce::String ("F1r. ") + t.name + ": ...and the reply layer follows the panel, not a tab list");
                check ((int) A::msgs (*ed).size() == msgs,
                       juce::String ("F1r. ") + t.name + ": ...over the SAME transcript state (one instance)",
                       juce::String ((int) A::msgs (*ed).size()) + " of " + juce::String (msgs));
            }
            // Settings still has no column, and that is the layout's answer, not a second list of tabs.
            A::toTab (*ed, A::tabSettings()); ed->resized(); pumpMs (40);
            check (! A::panelVisibleOn (*ed) && ! A::replyAllowed (*ed),
                   "F1r. Settings has no column, so no panel - excluded by the layout, not by a tab list");
            A::toChat (*ed); pumpMs (40);
        }

        // ---- F1-rest (21s-b): the banner and the composer label are one answer, wherever the composer is --------
        {
            std::printf ("\n== F1-rest: the Working-on banner and the composer label follow the one selection ==\n");
            const auto gid2 = proc.createLinkGroup ("Group 2", juce::StringArray { "lnk_01", "lnk_02" });
            A::targetGroup (*ed, gid2);
            const A::Tb tabs[] = { A::tabChat(), A::tabChain(), A::tabLink() };
            for (auto t : tabs)
            {
                A::toTab (*ed, t); pumpMs (30);
                check (A::banner (*ed) == "Working on Group: Group 2 (2)" && A::targetLabel (*ed) == "Group 2",
                       "F1-rest. tab " + juce::String ((int) t) + ": banner and label both say the group",
                       A::banner (*ed) + " / " + A::targetLabel (*ed));
            }
            check (A::pillEligible (*ed), "F1-rest. ...and the composer pill is shown, because a group IS a target");
            A::targetGroup (*ed, {});
            proc.removeLinkGroup (gid2);
            A::toChat (*ed); pumpMs (30);
        }

        // ---- F4 (21s-b): ONE plugins menu, and both triggers are the same trigger ------------------------------
        {
            std::printf ("\n== F4: the plugins menu is one menu, in one place, in one order ==\n");
            const auto items = A::menuItems();
            juce::StringArray labels;
            for (const auto& it : items) labels.add (it.label);
            check (labels.joinIntoString ("|") == "View all|Scan Now|Add Folder...",
                   "F4. the menu is View all, Scan Now, Add Folder... in that order", labels.joinIntoString ("|"));
            // Both triggers are wired to the SAME function, so their item lists cannot differ: what a guard can
            // assert without opening a modal menu is that neither has an onClick of its own any more.
            check (A::scanTrigger (*ed).onClick != nullptr && A::viewAllTrigger (*ed).onClick != nullptr,
                   "F4. both the header pill and the Settings row have a trigger");
            // ...and that the list a trigger would build has no second author: scanMenuItems() is static, so the
            // only way to add an item to one trigger is to add it to both.
            const auto again = A::menuItems();
            juce::StringArray labels2;
            for (const auto& it : again) labels2.add (it.label);
            check (labels2 == labels, "F4. both triggers build from the same list - adding to one cannot miss the other");
        }

        // ---- F2 (21s-a): one selection source of truth for a group target -------------------------------------
        {
            std::printf ("\n== F2: the strip click, the menu, the banner and the composer label are one answer ==\n");
            const auto gid = proc.createLinkGroup ("Group 1", juce::StringArray { "lnk_01", "lnk_02", "lnk_03", "lnk_04", "lnk_05" });
            A::targetGroup (*ed, gid);
            check (proc.chatTargetGroupId == gid && proc.chatTargetLinkUid.isEmpty(),
                   "F2. selecting a group sets the target and clears the Link target");
            check (A::banner (*ed) == "Working on Group: Group 1 (5)",
                   "F2. the banner reads \"Working on Group: Group 1 (5)\"", A::banner (*ed));
            check (A::targetLabel (*ed) == "Group 1",
                   "F2. the composer label NAMES the group (21t-e: the pill says where a turn goes)", A::targetLabel (*ed));
            check (A::stripSelected (*ed, "grp:" + gid),
                   "F2. the group strip is selected, highlighted like a Link strip");
            check (proc.getApi().selectedGroupId() == gid,
                   "F2. ...and the same answer is what the body will carry", proc.getApi().selectedGroupId());
            A::targetGroup (*ed, {});
            check (proc.chatTargetGroupId.isEmpty() && A::targetLabel (*ed) == "This channel"
                   && ! A::stripSelected (*ed, "grp:" + gid),
                   "F2. clearing it clears the target, the label and the highlight together");
            proc.removeLinkGroup (gid);
        }

        // ---- 21r item 7 (24 Sep 2026): the channel choice is answered ONCE PER INSTANCE and survives reopen ----
        // THE COMPLAINT: reopening the project asks "which channel is this on?" again. The prompt keyed off
        // "channelType is still the default", so a user whose answer WAS Full Mix was asked on every reopen.
        {
            std::printf ("\n== 21r item 7: the channel answer round-trips with the instance's state ==\n");
            check (! proc.isChannelChosen(), "(7) a fresh instance has not answered");
            proc.setChannelType (ChannelType::FullMix);   // the answer that used to be ignored
            check (proc.isChannelChosen(), "(7) answering - even with Full Mix - counts as answered");
            juce::MemoryBlock state;
            proc.getStateInformation (state);
            check (state.getSize() > 0, "(7) the state is written", juce::String ((int) state.getSize()) + " bytes");

            auto reopened = std::make_unique<EchoJayProcessor>();
            check (! reopened->isChannelChosen(), "(7) ...a NEW instance starts unanswered");
            reopened->setStateInformation (state.getData(), (int) state.getSize());
            check (reopened->isChannelChosen(),
                   "(7) ...and the reopened instance keeps the answer  (RED as it stood: nothing carried it)");
            check (reopened->getChannelType() == ChannelType::FullMix,
                   "(7) ...including the channel itself");

            // A state written BEFORE this build carries no flag: a non-default channel is an answer already given.
            auto legacy = std::make_unique<EchoJayProcessor>();
            {
                auto older = std::make_unique<EchoJayProcessor>();
                older->setChannelType (ChannelType::LeadVocal);
                juce::MemoryBlock mb; older->getStateInformation (mb);
                auto v = juce::JSON::parse (juce::String::createStringFromData (mb.getData(), (int) mb.getSize()));
                if (auto* o = v.getDynamicObject()) o->removeProperty ("channelChosen");   // as an older build wrote it
                const auto txt = juce::JSON::toString (v);
                legacy->setStateInformation (txt.toRawUTF8(), (int) txt.getNumBytesAsUTF8());
            }
            check (legacy->isChannelChosen() && legacy->getChannelType() == ChannelType::LeadVocal,
                   "(7) a pre-21r state with a non-default channel counts as answered, not asked again");
        }

        // ---- 21r item 1 (24 Sep 2026): ONE arm bubble per build, and its Listen is a chat-reply control ----------
        // THE COMPLAINT: on 21q the arm bubble rendered TWICE and with NO Listen button. Two causes, both here:
        // the emit path wrote the message to the workspace twice, and pills are not part of that store, so every
        // reload drew bare copies.
        {
            const auto armText = loop.liveBubbleText();
            check (armText.isNotEmpty(), "(13) fixture: the build armed the loop", armText);
            int armBubbles = 0, armIdx = -1;
            for (int i = 0; i < (int) M.size(); ++i)
                if (M[(size_t) i].role == "assistant" && M[(size_t) i].content == armText) { ++armBubbles; armIdx = i; }
            check (armBubbles == 1, "(13) ONE arm bubble per build  (RED as it stood: the emit wrote it to the chat twice)",
                   juce::String (armBubbles) + " bubble(s)");
            auto chipsAt = [&] (int i) { return i >= 0 ? A::chips (*ed, M[(size_t) i]).joinIntoString ("|") : juce::String ("<none>"); };
            check (chipsAt (armIdx).contains ("Listen#2"), "(13) the arm bubble renders a visible Listen", chipsAt (armIdx));
            // A workspace reload rebuilds the bubbles from the store, where pills do not exist: this is that state.
            if (armIdx >= 0) M[(size_t) armIdx].loopPills.clear();
            check (! chipsAt (armIdx).contains ("Listen"), "(13) a reloaded bubble comes back bare - the 21q defect, reproduced",
                   chipsAt (armIdx));
            A::reattach (*ed);
            check (chipsAt (armIdx).contains ("Listen#2"),
                   "(13) ...and the LIVE loop puts Listen back  (RED as it stood: nothing reattached it)", chipsAt (armIdx));
            // Shown where Apply and Build are shown, hidden where they are hidden.
            A::toTab (*ed, A::tabSettings()); pumpMs (30);
            check (! A::replyAllowed (*ed) && chipsAt (armIdx).isEmpty(),
                   "(13) hidden on a tab that hides the chat-reply controls (Settings)", chipsAt (armIdx));
            A::setChainsMode (*ed, true); A::toChat (*ed); pumpMs (30);
            check (! A::replyAllowed (*ed) && chipsAt (armIdx).isEmpty(),
                   "(13) hidden in CHAINS mode, like Apply and Build", chipsAt (armIdx));
            A::setChainsMode (*ed, false); A::toChat (*ed); pumpMs (30);
            check (A::replyAllowed (*ed) && chipsAt (armIdx).contains ("Listen#2"),
                   "(13) and back on the Chat tab", chipsAt (armIdx));
            // A stale bubble from an earlier build must NOT get live verbs.
            A::Msg stale; stale.role = "assistant"; stale.content = armText; M.push_back (stale);
            A::reattach (*ed);
            check (A::chips (*ed, M.back()).joinIntoString ("|").contains ("Listen#2")
                   && ! chipsAt (armIdx).contains ("Listen#2"),
                   "(13) exactly ONE message holds the live verbs - the newest match, never two",
                   "newest=" + A::chips (*ed, M.back()).joinIntoString ("|") + " older=" + chipsAt (armIdx));
            M.pop_back();
            A::reattach (*ed);
        }

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
    // This leg constructs a SECOND EchoJayProcessor and destroys it. Until 21t-f (b) that was fatal - the
    // deferred state restore fired into freed memory afterwards and aborted the process, so this leg ran LAST
    // where it could truncate nothing but itself. The cause is fixed (the restore holds the processor's
    // liveness token weakly) and the leg below proves it, so the order here is no longer load-bearing.
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
    // ---- 21t-f (b): A DEFERRED RESTORE MUST NOT OUTLIVE ITS PROCESSOR ------------------------------------
    // THE ABORT THIS SUITE DIED OF, now a leg. setStateInformation posts the slot restore to the message loop;
    // the lambda captured a raw `this`, so a processor destroyed before the loop turned left
    // ChainHost::tryRestoreSlotsFromXml locking a std::mutex in freed memory - "mutex lock failed: Invalid
    // argument", an abort, and every assertion after it skipped. lldb named the frame; this leg reproduces it.
    // On the pre-fix binary this leg ABORTS (the process dies here and prints no verdict), which is the RED.
    {
        std::printf ("\n== 21t-f (b): a deferred state restore does not outlive its processor ==\n");
        juce::MemoryBlock mb;
        {
            EchoJayProcessor src; src.prepareToPlay (48000.0, 512);
            { SurgicalEqProcessor fe; juce::ignoreUnused (fe); }
            if (const auto* eqd = BuiltinDeviceRegistry::instance().findByName ("EchoJay EQ"))
                src.getChainHost().insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*eqd), 0);
            src.getStateInformation (mb);
            check (src.getChainHost().getNumSlots() == 1 && mb.getSize() > 0,
                   "21t-f (b). fixture: a session with a slot in the rack",
                   juce::String ((int) src.getChainHost().getNumSlots()) + " slot(s), "
                   + juce::String ((int) mb.getSize()) + "b of state");
        }
        // A plugin that receives its state and is removed before the message loop turns. This is a DAW case
        // (remove a plugin, or close a session, during a restore), not only a harness one.
        {
            auto doomed = std::make_unique<EchoJayProcessor>();
            doomed->prepareToPlay (48000.0, 512);
            doomed->setStateInformation (mb.getData(), (int) mb.getSize());
            doomed.reset();          // destroyed with the restore still queued
        }
        pumpMs (200);                // ...and here is where it used to abort
        check (true, "21t-f (b). the queued restore fired after the processor died, and nothing aborted",
               "the lambdas hold the processor's liveness token weakly");
        // And the ordinary path still restores when the processor IS alive - a token that always says "gone"
        // would pass the check above and break the product.
        {
            EchoJayProcessor live; live.prepareToPlay (48000.0, 512);
            live.setStateInformation (mb.getData(), (int) mb.getSize());
            pumpMs (400);
            check (live.getChainHost().getNumSlots() == 1,
                   "21t-f (b). ...while a LIVE processor still restores its rack (the token is not a mute button)",
                   juce::String ((int) live.getChainHost().getNumSlots()) + " slot(s) restored");
        }
    }

    std::printf ("== BUILD 2 (30 Sep 2026 ruling): IN and OUT on every slot card ==\n");
    {
        // "Two small readouts, IN +0.0 and OUT -6.0, dB with one decimal, bound to the same slot pre-gain and slot
        // output gain the loop and the hold write (the ones getSlotOutGainDb reads), so they show what the loop has
        // done the moment it does it. They are controls, not labels."
        EchoJayProcessor proc; proc.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> edBase (proc.createEditor());
        auto* ed = dynamic_cast<EchoJayEditor*> (edBase.get()); if (! ed) return 2;
        ed->setSize (2000, 1100); pumpMs (60);
        A::build (*ed, "{\"chain\":[{\"name\":\"EchoJay EQ\",\"role\":\"eq\",\"settings\":\"\"}]}");
        for (int k = 0; k < 40 && proc.getChainHost().getNumSlots() < 1; ++k) pumpMs (100);
        check (proc.getChainHost().getNumSlots() == 1, "Build 2. precondition: one slot in the rack",
               juce::String (proc.getChainHost().getNumSlots()) + " slot(s)");
        auto& panel = A::panel (*ed);
        check (! panel.blocks.empty(), "Build 2. precondition: the panel has a card for it",
               juce::String ((int) panel.blocks.size()) + " card(s)");
        if (proc.getChainHost().getNumSlots() == 1 && ! panel.blocks.empty())
        {
            // ---- THE HOLD'S WRITE SHOWS. -6.0 dB on the slot's OUT, exactly as the hold writes it.
            proc.getChainHost().setSlotOutGainDb (0, -6.0f);
            pumpMs (150);                                   // the readout polls at 20 Hz; no panel rebuild is owed
            auto& blk = *panel.blocks.front();
            check (blk.outReadout.readoutText() == "OUT -6.0",
                   "Build 2. after a hold that wrote -6.0 the OUT readout on that slot reads -6.0  (RED as it "
                   "stood: there was no readout on the card at all)",
                   "\"" + blk.outReadout.readoutText() + "\"");
            check (blk.inReadout.readoutText() == "IN +0.0",
                   "Build 2. ...and IN still reads +0.0, one decimal, signed",
                   "\"" + blk.inReadout.readoutText() + "\"");
            check (blk.outReadout.isMoved() && ! blk.inReadout.isMoved(),
                   "Build 2. ...and only the one that MOVED is in the accent colour");
            // ---- IT IS A CONTROL: a real drag of +3.0 dB on IN.
            const float inBefore = proc.getChainHost().getSlotPreTrimDb (0);
            {
                auto& ro = blk.inReadout;
                ro.setSize (46, 9);
                auto src = juce::Desktop::getInstance().getMainMouseSource();
                const juce::Point<int> from { 23, 8 };
                const juce::Point<int> to   { 23, 8 - 30 };       // 30 px up at 0.1 dB/px = +3.0 dB
                const juce::MouseEvent down (src, from.toFloat(), juce::ModifierKeys::leftButtonModifier,
                    0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &ro, &ro, juce::Time::getCurrentTime(),
                    from.toFloat(), juce::Time::getCurrentTime(), 1, false);
                ro.mouseDown (down);
                const juce::MouseEvent drag (src, to.toFloat(), juce::ModifierKeys::leftButtonModifier,
                    0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &ro, &ro, juce::Time::getCurrentTime(),
                    from.toFloat(), juce::Time::getCurrentTime(), 1, true);
                ro.mouseDrag (drag);
            }
            pumpMs (120);
            const float inAfter = proc.getChainHost().getSlotPreTrimDb (0);
            check (std::abs ((inAfter - inBefore) - 3.0f) < 0.15f,
                   "Build 2. dragging IN by +3.0 moves the SLOT PRE-GAIN by +3.0 - the same write the loop makes",
                   juce::String (inBefore, 1) + " -> " + juce::String (inAfter, 1) + " dB");
            check (blk.inReadout.readoutText() == "IN +3.0",
                   "Build 2. ...and the readout follows it", "\"" + blk.inReadout.readoutText() + "\"");
            check (blk.inReadout.isMoved(),
                   "Build 2. ...and it is now in the accent colour too");
            // ---- AND THE DRAG DOES NOT COLLIDE: the readout is a child, so the card never saw the gesture.
            check (blk.inReadout.getParentComponent() == &blk,
                   "Build 2. the readout is a CHILD of the card, so its drag cannot reach the card's own "
                   "click-to-select or the strip's scroll");
            check (! blk.inReadout.getBounds().intersects (blk.wetKnob.getBounds()),
                   "Build 2. ...and its bounds do not overlap the wet knob",
                   blk.inReadout.getBounds().toString() + " vs " + blk.wetKnob.getBounds().toString());
            check (! blk.inReadout.getBounds().intersects (blk.outReadout.getBounds())
                       && ! blk.outReadout.getBounds().intersects (blk.bypassBtn.getBounds()),
                   "Build 2. ...nor each other, nor the button row");
            // ---- TYPED VALUES: the same write, clamped to the trims' range.
            blk.outReadout.set (99.0f);   pumpMs (60);
            check (std::abs (proc.getChainHost().getSlotOutGainDb (0) - 12.0f) < 0.01f,
                   "Build 2. a typed value is clamped to the trims' own range (+12 dB)",
                   juce::String (proc.getChainHost().getSlotOutGainDb (0), 1) + " dB");
            blk.outReadout.set (-99.0f);  pumpMs (60);
            check (std::abs (proc.getChainHost().getSlotOutGainDb (0) - -24.0f) < 0.01f,
                   "Build 2. ...and at the bottom too (-24 dB)",
                   juce::String (proc.getChainHost().getSlotOutGainDb (0), 1) + " dB");
        }
    }

    std::printf ("\n==== ui_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
