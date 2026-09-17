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
};
struct EchoJayTabStripTestAccess
{
    static void toChain (EchoJayEditor& e)              { e.switchToTab (EchoJayEditor::Tab::Chain, true); }
    static void selectUser (EchoJayEditor& e, const juce::String& uid) { e.pendingSelectionIsUser_ = true; e.openChannelByUid (uid); }
    static void tickEditor (EchoJayEditor& e)           { if (e.chainViewUid().isNotEmpty()) e.refreshLinkRackCache (false); e.refreshChainPanelForView (false); e.borrowSelectionTick(); }
    static void xPress (EchoJayEditor& e, int i)        { e.chainListPanel.onRemoveSlot (i); }
    static bool editBlocked (EchoJayEditor& e)          { return e.chainListPanel.editBlocked; }
    static bool overlayVisible (EchoJayEditor& e)       { return e.chainListPanel.editGate.isVisible(); }
    static bool pillOrange (EchoJayEditor& e)           { return e.chainListPanel.rackBtn.findColour (juce::TextButton::textColourOffId) == juce::Colour (0xffFFB020); }
    static juce::String readInFlight (EchoJayEditor& e) { return e.borrowReadInFlightUid_; }
    static juce::String viewUid (EchoJayEditor& e)      { return e.chainViewUid(); }
    static bool gateRefuses (EchoJayEditor& e)          { return e.chainEditGateRefuses(); }
    static void refresh (EchoJayEditor& e)              { e.refreshChainPanelForView (false); }
};

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
    const int nslots = std::getenv ("EJ_OV_SLOTS") ? atoi (std::getenv ("EJ_OV_SLOTS")) : 1;
    std::printf ("overlay_step0: select a Link rack -> Held-but-blue window -> X press -> ticks   (sidecar slots=%d)\n", nslots);
    if (! seed (nslots)) { std::printf ("SEED FAILED\n"); return 2; }

    EchoJayProcessor proc; proc.prepareToPlay (48000.0, 512);
    for (int k = 0; k < 8; ++k) { heartbeat(); proc.refreshLinkRegistry(); proc.updateLinkAudioRecency(); }
    bool connected = false; for (const auto& en : proc.getLinkDisplayList()) if (en.info.uid == juce::String (kUid)) connected = en.info.connected;
    std::printf ("  Link in display list, connected=%d\n", (int) connected);
    std::unique_ptr<juce::AudioProcessorEditor> edBase (proc.createEditor());
    auto* ed = dynamic_cast<EchoJayEditor*> (edBase.get()); if (! ed) return 2;
    ed->setSize (1280, 820); EchoJayTabStripTestAccess::toChain (*ed); pumpMs (50);
    show ("before select", proc, *ed);

    std::printf ("== SELECT (rack menu path: openChannelByUid, user) ==\n");
    EchoJayTabStripTestAccess::selectUser (*ed, kUid);
    show ("after select (sync)", proc, *ed);
    EchoJayTabStripTestAccess::tickEditor (*ed);   // one editor tick WITHOUT ingesting... (refreshLinkRackCache runs here)
    show ("after 1 editor tick", proc, *ed);
    proc.rackLockTick();
    show ("after rackLockTick", proc, *ed);
    EchoJayTabStripTestAccess::refresh (*ed);
    show ("after panel refresh", proc, *ed);

    std::printf ("== X PRESS: onRemoveSlot(0) in this window ==\n");
    EchoJayTabStripTestAccess::xPress (*ed, 0);
    show ("right after X", proc, *ed);
    pumpMs (150); EchoJayTabStripTestAccess::refresh (*ed);
    show ("150 ms after X", proc, *ed);

    std::printf ("== SUBSEQUENT TICKS (heartbeat + registry + lock tick + editor tick), 12 x 250 ms ==\n");
    for (int k = 1; k <= 12; ++k)
    {
        heartbeat(); proc.refreshLinkRegistry(); proc.rackLockTick(); EchoJayTabStripTestAccess::tickEditor (*ed); pumpMs (250);
        if (k <= 3 || k % 4 == 0 || proc.borrowActive()) show (("tick " + juce::String (k)).toRawUTF8(), proc, *ed);
        if (proc.borrowActive()) break;
    }
    std::printf ("  FINAL: borrowActive=%d lock=%s pending=%s want=%s pillOrange=%d editBlocked=%d\n", (int) proc.borrowActive(), st (proc.rackLockState()),
                 EchoJayAlignTestAccess::pending (proc).toRawUTF8(), EchoJayAlignTestAccess::want (proc).toRawUTF8(),
                 (int) EchoJayTabStripTestAccess::pillOrange (*ed), (int) EchoJayTabStripTestAccess::editBlocked (*ed));
    return 0;
}
