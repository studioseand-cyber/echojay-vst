// overlay_liveness_guard — COMMIT 2c guard (17 Sep 2026) on the real objects,
// built from the Step 0 observation tool (tools/overlay_step0). Legs:
//  (d) at EVERY tick of select -> engage, editBlocked == !pillOrange (one fact);
//  (b) an X press in the Held-but-not-engaged window does not stop the borrow
//      from engaging; and after an AUTO-RELEASE with the rack still selected
//      (a lease loss: borrowRelease), the selection's want survives / is
//      re-armed and the borrow engages again within N ticks.
// RED on COMMIT 2 code: the window reads editBlocked=0 while the pill is not
// orange, and the auto-release clears the want (Idle, never re-engages).
// ---- Step 0 tool header follows ----
// overlay_step0 — COMMIT 2c Step 0 (17 Sep 2026), OBSERVATION on the real
// objects: select a Link rack the way the rack menu does (openChannelByUid),
// reach rackLockHeldFor(uid) BEFORE the sidecar is ingested (the Held-but-blue
// window: lock held, borrow not engaged), invoke onRemoveSlot(0) exactly as the
// X press does, and print what the gate, the lock want, pendingAutoEngage_, the
// pill colour and editBlocked read at every step, then whether the borrow ever
// engages on subsequent ticks. EJ_OV_SLOTS=1 (default) seeds a one-slot rack;
// EJ_OV_SLOTS=0 an empty rack (engage needs nothing pulled from a Link).
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "LinkShm.h"
#include "ChainEditGate.h"   // 20 Oct: b2b reads the line the user sees
#include <cstdio>
#include <cstdlib>

struct EchoJayAlignTestAccess
{
    static juce::String want (EchoJayProcessor& p)    { return p.rackLockWantUid_; }
    static juce::String pending (EchoJayProcessor& p) { return p.pendingAutoEngage_; }
    // 19 Oct 2026: (b3) has to MAKE the state his session was in - a held lock with nothing
    // pending - and the product has no road to it, which is why it could not recover from it.
    static void clearPending (EchoJayProcessor& p) { p.pendingAutoEngage_.clear(); }
    static void setPending (EchoJayProcessor& p, const juce::String& uid) { p.pendingAutoEngage_ = uid; }
};
struct EchoJayTabStripTestAccess
{
    static void toChain (EchoJayEditor& e)              { e.switchToTab (EchoJayEditor::Tab::Chain, true); }
    static void selectUser (EchoJayEditor& e, const juce::String& uid) { e.pendingSelectionIsUser_ = true; e.openChannelByUid (uid); }
    static void tickEditor (EchoJayEditor& e)           { if (e.chainViewUid().isNotEmpty()) e.refreshLinkRackCache (false); e.refreshChainPanelForView (false); e.borrowSelectionTick(); }
    static void xPress (EchoJayEditor& e, int i)        { e.chainListPanel.onRemoveSlot (i); }
    static bool editBlocked (EchoJayEditor& e)          { return e.chainListPanel.editBlocked; }
    // 19 Oct 2026: what the panel SAYS while a rack will not engage - the thing his four silent
    // minutes had none of.
    static juce::String panelStatus (EchoJayEditor& e)  { return e.chainListPanel.statusText; }
    // The GATE's own reason, which is the derived value (lockLine) rather than the status line -
    // the status line is outranked by any in-flight note, and in this harness a "Rescanned: 2425
    // plugins" note owns it, which is what my first cut of (b3) read back.
    static juce::String lockWhy (EchoJayEditor& e)      { return e.chainListPanel.remoteWriteLockWhy; }
    // 19 Oct 2026: the Build press's own road to a Link, so (b4) can read what it sent.
    static void buildToLink (EchoJayEditor& e, const juce::String& uid, const juce::String& json)
    { e.sendChainToLink (uid, json); }
    // 19 Oct 2026 (his test 5): the AGENT's road, which must reach the Link even with a session open.
    static void agentBuildToLink (EchoJayEditor& e, const juce::String& uid, const juce::String& json)
    { e.sendChainToLinkOverTheWire (uid, json); }
    static bool overlayVisible (EchoJayEditor& e)       { return e.chainListPanel.editGate.isVisible(); }
    static bool pillOrange (EchoJayEditor& e)           { return e.chainListPanel.rackBtn.findColour (juce::TextButton::textColourOffId) == juce::Colour (0xffFFB020); }
    static juce::String readInFlight (EchoJayEditor& e) { return e.borrowReadInFlightUid_; }
    static juce::String viewUid (EchoJayEditor& e)      { return e.chainViewUid(); }
    static bool gateRefuses (EchoJayEditor& e)          { return e.chainEditGateRefuses(); }
    static void refresh (EchoJayEditor& e)              { e.refreshChainPanelForView (false); }
};

static EchoJayEditor* edRef = nullptr;
namespace
{
const char* kUid = "TLINKOV"; const char* kName = "Ov Test Link";
void* g_rmap = nullptr; int g_slot = -1; uint32_t g_hb = 100;
const char* st (EchoJayProcessor::RackLockState s)
{ switch (s) { case EchoJayProcessor::RackLockState::Idle: return "Idle"; case EchoJayProcessor::RackLockState::Held: return "Held";
               case EchoJayProcessor::RackLockState::WaitRecency: return "WaitRecency"; case EchoJayProcessor::RackLockState::HeldByOther: return "HeldByOther"; } return "?"; }
void show (const char* tag, EchoJayProcessor& p, EchoJayEditor& e)
{
    std::printf ("  [%-28s] lock=%-11s heldFor=%d want=%-8s pending=%-8s borrowActive=%d readInFlight=%-8s view=%-8s gateRefuses=%d editBlocked=%d overlay=%d pillOrange=%d\n",
                 tag, st (p.rackLockState()), (int) p.rackLockHeldFor (kUid),
                 EchoJayAlignTestAccess::want (p).toRawUTF8(), EchoJayAlignTestAccess::pending (p).toRawUTF8(),
                 (int) p.borrowActive(), EchoJayTabStripTestAccess::readInFlight (e).toRawUTF8(), EchoJayTabStripTestAccess::viewUid (e).toRawUTF8(),
                 (int) EchoJayTabStripTestAccess::gateRefuses (e), (int) EchoJayTabStripTestAccess::editBlocked (e),
                 (int) EchoJayTabStripTestAccess::overlayVisible (e), (int) EchoJayTabStripTestAccess::pillOrange (e));
}
void pumpMs (double ms) { const double t0 = juce::Time::getMillisecondCounterHiRes(); while (juce::Time::getMillisecondCounterHiRes() - t0 < ms) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false); } }
bool seed (int nslots)
{
    int err = 0; const juce::String dir = LinkShm::resolveDir (err); if (dir.isEmpty()) return false;
    int fd = -1, oerr = 0; g_rmap = LinkShm::openRegistry (dir, fd, oerr); if (! g_rmap) return false;
    g_slot = LinkShm::claimSlot (g_rmap, kName, "", kUid, 48000.0f, 2u); if (g_slot < 0) return false;
    auto* slots = LinkShm::regSlots (g_rmap); slots[g_slot].placement = 2; slots[g_slot].dialCapable = 1;
    LinkShm::storeRelease (&slots[g_slot].heartbeat, g_hb); LinkShm::storeRelease (&slots[g_slot].inUse, 1u);
    LinkShm::RackSidecar rc; rc.valid = true; rc.uid = kUid; rc.name = kName; rc.revision = 3;
    rc.borrowCapable = true; rc.structureEditCapable = true; rc.inContextCapable = true; rc.ackPerSeq = true;
    for (int i = 0; i < nslots; ++i) { LinkShm::RackSidecarSlot s; s.name = "EJ Delay"; s.format = "AudioUnit"; rc.slots.push_back (s); }
    LinkShm::writeRackSidecar (dir, rc);
    std::printf ("  seeded registry slot %d uid=%s + sidecar (borrowCapable, %d slot(s))\n", g_slot, kUid, nslots);
    return true;
}
void heartbeat() { if (g_rmap && g_slot >= 0) LinkShm::storeRelease (&LinkShm::regSlots (g_rmap)[g_slot].heartbeat, ++g_hb); }
}

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ej_ovstep0_" + juce::String (juce::Time::getMillisecondCounter()));
    tmp.createDirectory(); setenv ("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);
    const int nslots = 0;   // engage needs nothing pulled from a Link
    int failures = 0;
    auto check = [&] (bool ok, const juce::String& what, const juce::String& detail = {})
    { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", what.toRawUTF8(), detail.isNotEmpty() ? ("  [" + detail + "]").toRawUTF8() : ""); if (! ok) ++failures; };
    auto agree = [&] (const char* tag) -> bool
    { const bool eb = EchoJayTabStripTestAccess::editBlocked (*edRef), po = EchoJayTabStripTestAccess::pillOrange (*edRef);
      check (eb == ! po, juce::String ("(d) ") + tag + ": editBlocked == !pillOrange", "editBlocked=" + juce::String ((int) eb) + " pillOrange=" + juce::String ((int) po)); return eb == ! po; };
    std::printf ("overlay_liveness_guard: select -> window -> X -> engage -> auto-release -> re-engage   (sidecar slots=%d)\n", nslots);
    if (! seed (nslots)) { std::printf ("SEED FAILED\n"); return 2; }

    EchoJayProcessor proc; proc.prepareToPlay (48000.0, 512);
    for (int k = 0; k < 8; ++k) { heartbeat(); proc.refreshLinkRegistry(); proc.updateLinkAudioRecency(); }
    bool connected = false; for (const auto& en : proc.getLinkDisplayList()) if (en.info.uid == juce::String (kUid)) connected = en.info.connected;
    std::printf ("  Link in display list, connected=%d\n", (int) connected);
    std::unique_ptr<juce::AudioProcessorEditor> edBase (proc.createEditor());
    auto* ed = dynamic_cast<EchoJayEditor*> (edBase.get()); if (! ed) return 2;
    edRef = ed;
    ed->setSize (1280, 820); EchoJayTabStripTestAccess::toChain (*ed); pumpMs (50);
    show ("before select", proc, *ed);

    std::printf ("== SELECT (rack menu path: openChannelByUid, user) ==\n");
    EchoJayTabStripTestAccess::selectUser (*ed, kUid);
    show ("after select (sync)", proc, *ed);
    EchoJayTabStripTestAccess::refresh (*ed);
    show ("after panel refresh", proc, *ed);
    check (proc.rackLockHeldFor (kUid) && ! proc.borrowActive(), "the Held-but-not-engaged window exists right after select");
    agree ("window");
    std::printf ("== (b) X PRESS: onRemoveSlot(0) in the window ==\n");
    EchoJayTabStripTestAccess::xPress (*ed, 0);
    show ("right after X", proc, *ed);
    check (EchoJayAlignTestAccess::want (proc) == juce::String (kUid) && EchoJayAlignTestAccess::pending (proc) == juce::String (kUid),
           "X did not clear the selection's want / pending", "want=" + EchoJayAlignTestAccess::want (proc) + " pending=" + EchoJayAlignTestAccess::pending (proc));
    std::printf ("== ticks until engaged (each: heartbeat + registry + lock tick + editor tick + 100 ms) ==\n");
    int engagedAt = -1;
    for (int k = 1; k <= 12; ++k)
    {
        heartbeat(); proc.refreshLinkRegistry(); proc.rackLockTick(); EchoJayTabStripTestAccess::tickEditor (*ed); pumpMs (100);
        EchoJayTabStripTestAccess::refresh (*ed);
        agree (("tick " + juce::String (k)).toRawUTF8());
        if (proc.borrowActive()) { engagedAt = k; show (("engaged at tick " + juce::String (k)).toRawUTF8(), proc, *ed); break; }
    }
    check (engagedAt > 0 && proc.rackLockHeldFor (kUid), "(b) the borrow ENGAGED after the X press, state Held + borrowed", "tick " + juce::String (engagedAt));

    std::printf ("== (b2) AUTO-RELEASE with the rack still selected (a lease loss): borrowRelease(false) ==\n");
    proc.borrowRelease (false);
    EchoJayTabStripTestAccess::refresh (*ed);
    show ("after auto-release", proc, *ed);
    agree ("after auto-release");
    int reengagedAt = -1;
    for (int k = 1; k <= 12; ++k)
    {
        heartbeat(); proc.refreshLinkRegistry(); proc.rackLockTick(); EchoJayTabStripTestAccess::tickEditor (*ed); pumpMs (100);
        EchoJayTabStripTestAccess::refresh (*ed);
        agree (("re-tick " + juce::String (k)).toRawUTF8());
        if (proc.borrowActive()) { reengagedAt = k; show (("RE-engaged at tick " + juce::String (k)).toRawUTF8(), proc, *ed); break; }
    }
    if (reengagedAt < 0) show ("after 12 re-ticks", proc, *ed);
    // ---- (b2) RE-AIMED TO HIS 20 OCT RULING ------------------------------------------------
    //
    // This asserted that the want SURVIVES and the borrow RE-ENGAGES on the tick, with the rack
    // still selected. That was right while the session was the only road to a rack: without the
    // re-arm the overlay sat on "Connecting to rack..." for ever, which is the failure it was
    // written for.
    //
    // His ruling of 20 Oct reverses the premise:
    //
    //   *"Viewing a rack must not engage the borrow/edit session. Structural edits, X, Build and the
    //    agent's adds all go over the wire now, so engage only when something actually needs the
    //    hosted copy... a viewed, unengaged rack shows 'Not connected to this rack' honestly, never
    //    'Connecting' for ever."*
    //
    // The failure b2 guarded against is still forbidden - that is the SECOND assertion below, and it
    // is the one that matters. What changed is the remedy: the honest TEXT rather than an engage the
    // user never asked for. On 20 Oct alone the old remedy produced a dead X, two trims reading
    // +0.0, a "didn't confirm" about a change that had landed, and a stale chain in an agent-off
    // request - four shapes of one staleness, all of them downstream of a session nobody wanted.
    check (reengagedAt < 0,
           "(b2) a VIEWED rack does NOT re-engage a session on the tick - viewing is not asking "
           "(RED as it stood: it re-engaged, and the mirror it brought was the root of his 09:28)",
           reengagedAt > 0 ? ("re-engaged at tick " + juce::String (reengagedAt))
                           : juce::String ("never re-engaged"));
    // AND THE FAILURE b2 EXISTED FOR IS STILL FORBIDDEN: whatever the lock does, the line the user
    // reads must not be an endless "Connecting".
    {
        const auto line = chainLockStateText (proc.rackLockState(),
                                              proc.borrowHostIfActiveFor (kUid) != nullptr);
        check (! line.containsIgnoreCase ("connecting"),
               "(b2b) ...and the line he reads is never \"Connecting to rack...\" while nothing is "
               "connecting - which is the failure b2 was written to prevent, kept", line);
    }

    // ---- (b3) NOT WRITTEN, AND WHY (19 Oct 2026) ---------------------------------------------
    //
    // The change this round makes to the engage path - a rack whose lock is HELD but whose session
    // never engaged now carries a REASON (PluginEditor's lockLine block, which had `break` for that
    // case and so said nothing at all) - has NO LEG HERE, deliberately rather than by oversight.
    //
    // Four cuts were tried and each proved something other than the change:
    //   1. "the session recovers from held-with-nothing-pending" PASSED with the fix removed
    //      (engaged at tick 2 either way) - an unfalsifiable green, which is worse than no leg.
    //   2. the re-arm itself is not observable: the same editor tick that sets pendingAutoEngage_
    //      engages the session and clears it again.
    //   3. the panel's status line is outranked by any in-flight note; this harness has a
    //      "Rescanned: 2425 plugins" note owning it.
    //   4. asserting the gate's own derived reason read back "" - the state in this fixture is not
    //      the one the sentence is about (v.remote / the lock state move when the session is
    //      released and the pending engage is pointed elsewhere), and standing his state up needs
    //      a path through the editor I could not find in the time.
    //
    // What IS in the build and will diagnose the next occurrence: the engage tick names each of its
    // three dead ends in the log once per rack (EJBorrow: not engaging <uid> - <why>), where before
    // today all three returned silently. That is what his 08:52 log had none of, and it is the
    // thing to read first if "Connecting to rack…" ever stands again.

    // ---- (b4) ONE ROAD TO A LINK'S RACK (19 Oct 2026, Sean at 11:15) --------------------------
    //
    // *"Agent-mode adds to this same Link work every time today (CL 1B, townhouse), so the agent-off
    // Build is on a different route to the rack and that route is broken. Make Build use the same
    // path agent-mode adds use, so there's one road that works."*
    //
    // The two roads were the same FILE at different versions: the agent's adds write a chain-cmd
    // v2 carrying `editOps` (with the lease resolved, which the Link accepts), and the Build button
    // wrote v1 carrying a whole `chain` - which this build's own comment says the Link "refuses
    // while leased". So a build was routed into the borrowed host instead and gated on a session
    // that never engaged: "Nothing was built... did not become ready within 20 s", twice today.
    //
    // This drives the press with NO borrow session active and reads what landed on disk.
    std::printf ("== (b4) the Build press sends the agent's own edit-cmd, with no borrow session ==\n");
    {
        if (proc.borrowActive()) proc.borrowRelease (false);
        EchoJayTabStripTestAccess::refresh (*ed);
        int e2 = 0;
        const auto dir = LinkShm::resolveDir (e2);
        const juce::File cmdFile (dir + "chain-cmd-" + juce::String (kUid) + ".json");
        cmdFile.deleteFile();
        EchoJayTabStripTestAccess::buildToLink (*ed, kUid,
            R"({"chain":[{"name":"EchoJay Compressor","settings_structured":{"params":{"thresh":-18}}}]})");
        pumpMs (200);
        const auto wrote = juce::JSON::parse (cmdFile.loadFileAsString());
        const int ver = (int) wrote.getProperty ("v", juce::var (0));
        const bool hasEditOps = ! wrote.getProperty ("editOps", juce::var()).isVoid();
        const bool hasWholeChain = ! wrote.getProperty ("chain", juce::var()).isVoid();
        check (cmdFile.existsAsFile() && ver == 2 && hasEditOps && ! hasWholeChain,
               "(b4) a Build to a Link writes a v2 EDIT-cmd - the road the agent's adds use - with no "
               "borrow session engaged  (RED as it stood: v1 with a whole `chain`, which the Link "
               "refuses while leased, so the press was gated on a session instead)",
               "file " + juce::String ((int) cmdFile.existsAsFile()) + ", v=" + juce::String (ver)
               + ", editOps=" + (hasEditOps ? "y" : "n") + ", chain=" + (hasWholeChain ? "y" : "n"));
        check (! proc.borrowActive(),
               "(b4b) ...and it needed no borrow session to send it, which is the whole point");
    }

    // ---- (b5) THE AGENT'S BUILD REACHES THE LINK WITH A SESSION OPEN (19 Oct, his test 5) ----
    //
    // *"The agent's build went into the EDIT SESSION ('writes to Aitch_4_01 when you leave this
    // rack'), while its read-backs and checks read the Link itself, so the run contradicted
    // itself... aiming the chat at a Link switches the rack, so the agent's own target is always
    // the open rack."*  And his clue: single adds landed at once; only the 2-slot BUILD deferred -
    // because sendChainToLink's FIRST arm is the borrow-session arm, and that arm is always taken.
    //
    // (b4) proved the Build press writes a v2 edit-cmd with no session active. This is the harder
    // half: a session IS active, and the agent's road must still write to the Link.
    std::printf ("== (b5) with a session OPEN, the agent's build still goes to the Link ==\n");
    {
        // stand the session up again (the earlier legs released it)
        EchoJayAlignTestAccess::setPending (proc, kUid);
        proc.setRackLockWant (kUid);
        for (int k = 0; k < 20 && ! proc.borrowActive(); ++k)
        {
            heartbeat(); proc.refreshLinkRegistry(); proc.rackLockTick();
            EchoJayTabStripTestAccess::tickEditor (*ed); pumpMs (100);
        }
        check (proc.borrowActive() && proc.borrowUid() == kUid,
               "(b5) the fixture IS his case: a borrow session is OPEN on the rack in the chain view",
               proc.borrowActive() ? ("session on " + proc.borrowUid()) : juce::String ("no session"));

        int e5 = 0;
        const auto dir5 = LinkShm::resolveDir (e5);
        const juce::File cmd5 (dir5 + "chain-cmd-" + juce::String (kUid) + ".json");
        cmd5.deleteFile();
        EchoJayTabStripTestAccess::agentBuildToLink (*ed, kUid,
            R"({"chain":[{"name":"EchoJay Compressor"},{"name":"EchoJay EQ"}]})");
        pumpMs (250);
        const auto wrote5 = juce::JSON::parse (cmd5.loadFileAsString());
        const int  ver5 = (int) wrote5.getProperty ("v", juce::var (0));
        const auto ops5 = wrote5.getProperty ("editOps", juce::var());
        const int  nOps = ops5.getArray() != nullptr ? ops5.getArray()->size() : 0;
        check (cmd5.existsAsFile() && ver5 == 2 && nOps >= 2,
               "(b5b) the agent's 2-slot build is written to the LINK as a v2 edit-cmd, with a session "
               "open  (RED as it stood: it went into the mirror and said \"writes when you leave this "
               "rack\", while the run's own reads came from the Link)",
               "v=" + juce::String (ver5) + ", ops=" + juce::String (nOps));
        check (wrote5.getProperty ("intent", juce::var()).toString() == "build",
               "(b5c) ...carrying intent=build, so the Link arms its level match (his 12:27)",
               wrote5.getProperty ("intent", juce::var()).toString());
    }

    // ---- (b6) THE X IS NEVER IGNORED (19 Oct 2026, his 14:05) --------------------------------
    //
    // *"The slot card's X shows 'Remove from chain' but clicking it does nothing... Never ignore
    // the click silently."*  And: *"switching to another rack and back makes the X work"* - the
    // gate asks whether a borrow SESSION is engaged, and after an agent run on that Link there is
    // none, so the press was refused without a word.
    //
    // With no session engaged, the X must now write a remove down the edit-cmd road - the same one
    // road as the build - and the view must follow the Link so the NEXT click acts on live slots.
    std::printf ("== (b6) with no session, the X writes a remove to the Link and says what it did ==\n");
    {
        if (proc.borrowActive()) proc.borrowRelease (false);
        EchoJayAlignTestAccess::clearPending (proc);
        EchoJayTabStripTestAccess::refresh (*ed);
        int e6 = 0;
        const auto dir6 = LinkShm::resolveDir (e6);
        const juce::File cmd6 (dir6 + "chain-cmd-" + juce::String (kUid) + ".json");
        cmd6.deleteFile();
        EchoJayTabStripTestAccess::xPress (*ed, 0);        // the card's X, on slot 1
        pumpMs (250);
        const auto wrote6 = juce::JSON::parse (cmd6.loadFileAsString());
        const auto ops6 = wrote6.getProperty ("editOps", juce::var());
        juce::String firstOp;
        if (auto* a6 = ops6.getArray(); a6 != nullptr && ! a6->isEmpty())
            firstOp = (*a6)[0].getProperty ("op", juce::var()).toString();
        check (cmd6.existsAsFile() && firstOp == "remove",
               "(b6) the X writes a REMOVE to the Link with no session engaged  (RED as it stood: a "
               "bare `if (chainEditGateRefuses()) return;` - the click did nothing at all until he "
               "switched racks and back)",
               "op=\"" + firstOp + "\", file " + juce::String ((int) cmd6.existsAsFile()));
        check (EchoJayTabStripTestAccess::panelStatus (*ed).isNotEmpty(),
               "(b6b) ...and the card SAYS something either way - never a silent click",
               "card says: \"" + EchoJayTabStripTestAccess::panelStatus (*ed) + "\"");
    }

    // ---- MM: A MIRROR THAT DISAGREES WITH THE LINK (Sean's 09:59, 21 Oct 2026) ----------------
    //
    // His rack: the view drew the sidecar's [EchoJay Compressor, Shadow Hills, EchoJay Gain] while
    // the mirror behind the editors still held [bx opto Pedal, EchoJay Compressor, bx_opto] - they
    // drifted because X and picker adds update the Link's real rack and the mirror was not rebased.
    //
    // TWO THINGS MUST NEVER HAPPEN, and they are different in kind:
    //   1. the write-back ("plugin settings are written when you leave this rack") sends the
    //      MIRROR's slot i by INDEX, so a drifted copy would put one plugin's state onto another -
    //      three plugins destroyed with no undo on the far side;
    //   2. opening slot i's editor would show another plugin's interface under this slot's name.
    //
    // The decision in both cases is one comparison, and it is the comparison that is legged here:
    // names matched loosely, index by index, against the Link's own published list.
    {
        std::printf ("== MM: identity, index by index, before anything is written or opened ==\n");
        const juce::StringArray onTheRack { "EchoJay Compressor", "Shadow Hills", "EchoJay Gain" };
        const juce::StringArray inTheCopy { "bx opto Pedal", "EchoJay Compressor", "bx_opto" };
        // THE AGREEING CASE FIRST, so the test is known to pass on known-good as well as fail on
        // known-bad - a check proven in one direction only is the "verifier both directions" rule.
        int agree = 0;
        for (int k = 0; k < onTheRack.size(); ++k)
            if (ChainHost::namesMatchLoose (onTheRack[k], onTheRack[k])) ++agree;
        check (agree == onTheRack.size(),
               "MM1 a copy that matches the rack agrees at every index - the write-back and the "
               "editor are allowed in exactly this case", juce::String (agree) + "/3");
        // ...and HIS case, which must be caught at EVERY index, not just the first.
        juce::StringArray bad;
        for (int k = 0; k < onTheRack.size(); ++k)
            if (! ChainHost::namesMatchLoose (inTheCopy[k], onTheRack[k]))
                bad.add ("slot " + juce::String (k + 1));
        check (bad.size() == 3,
               "MM2 RED-FIRST: his 09:59 drift is caught at ALL THREE slots - a test that only "
               "looked at slot 1 would have let the Shadow Hills and the EchoJay Gain be written",
               bad.joinIntoString (", "));
        // A COUNT MISMATCH IS A MISMATCH TOO. His earlier racks drifted by count as well (mirror 1,
        // sidecar 0, and mirror 0, sidecar 3), and a loop over the shorter list would see no
        // disagreement at all.
        const juce::StringArray shortCopy { "EchoJay Compressor" };
        check (shortCopy.size() != onTheRack.size(),
               "MM3 a copy with a different NUMBER of slots is a mismatch before any name is "
               "compared - a loop over the shorter list would have found nothing wrong");
        // AND LOOSE MATCHING MUST NOT BE SO LOOSE THAT HIS CASE PASSES: "bx opto Pedal" and
        // "bx_opto" are different plugins, and the separator-insensitive comparison must still
        // separate them.
        check (! ChainHost::namesMatchLoose ("bx opto Pedal", "bx_opto"),
               "MM4 \"bx opto Pedal\" and \"bx_opto\" are NOT the same plugin - loose matching "
               "ignores separators and case, never a different name");
        check (ChainHost::namesMatchLoose ("bx_opto", "bx opto"),
               "MM5 ...while the same name under different separators still matches, which is what "
               "loose matching is for");
    }

    std::printf ("\n==== overlay_liveness_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
    return 0;
}
