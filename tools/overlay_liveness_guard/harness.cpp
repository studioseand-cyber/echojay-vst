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
    check (reengagedAt > 0 && proc.rackLockHeldFor (kUid), "(b2) after the auto-release the want survives / re-arms and the borrow RE-ENGAGES (Held + borrowed)", "tick " + juce::String (reengagedAt));

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

    std::printf ("\n==== overlay_liveness_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
    return 0;
}
