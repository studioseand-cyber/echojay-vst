// wet_rebuild_guard — COMMIT 5 guard (17 Sep 2026): a wet write must NOT rebuild
// the chain panel (the rebuild storm that destroyed the ChainWetKnob under the
// mouse); STRUCTURE changes must still rebuild. On the REAL panel, Chain tab
// active, through the SAME callbacks the knobs use, with the 20 Hz tick's own
// call (refreshChainPanelForView(false)) between steps.
//   (1) ten slot-knob drag steps -> SlotBlock address unchanged, final ChainHost
//       wet == last step                                   (RED today at step 1)
//   (2) STRUCTURE ops still rebuild (address changes): restore(add), bypass,
//       move, remove, and a plan apply on a BorrowHost   (GREEN before AND after)
//   (3) a wet change from OUTSIDE the knob (setSlotWet on the host) reaches the
//       existing knob's drawn value within one tick, no rebuild (RED today: it
//       arrives only by rebuilding)
//   (4) ten master-knob drag steps: same rule as (1)
//   (5) the borrowed-rack variant of (1) on the BorrowHost path (rack Held)
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "ChainHost.h"
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "EedDeviceRegistry.h"
#include "LinkShm.h"
#include <cstdio>

struct EchoJayAlignTestAccess { static void hold (EchoJayProcessor& p) { p.rackLockState_ = EchoJayProcessor::RackLockState::Held; } };
struct EchoJayTabStripTestAccess
{
    static void  knob   (EchoJayEditor& e, int i, float v) { e.chainListPanel.onSlotWet (i, v); }
    static void  master (EchoJayEditor& e, float v)        { e.chainListPanel.onMasterWet (v); }
    static void  bypass (EchoJayEditor& e, int i)          { e.chainListPanel.onBypassSlot (i); }
    static void  remove (EchoJayEditor& e, int i)          { e.chainListPanel.onRemoveSlot (i); }
    static void  move   (EchoJayEditor& e, int i, int d)   { e.chainListPanel.onMoveSlot (i, d); }
    static void  refresh(EchoJayEditor& e, bool force)     { e.refreshChainPanelForView (force); }
    static juce::Component::SafePointer<juce::Component> blockPtr (EchoJayEditor& e, int i)
    { return i < (int) e.chainListPanel.blocks.size() ? juce::Component::SafePointer<juce::Component> (e.chainListPanel.blocks[(size_t) i].get()) : juce::Component::SafePointer<juce::Component>(); }
    static const void* block (EchoJayEditor& e, int i)     { return i < (int) e.chainListPanel.blocks.size() ? (const void*) e.chainListPanel.blocks[(size_t) i].get() : nullptr; }
    static float knobVal (EchoJayEditor& e, int i)         { return i < (int) e.chainListPanel.blocks.size() ? e.chainListPanel.blocks[(size_t) i]->wetKnob.getValue() : -1.0f; }
    static float masterVal (EchoJayEditor& e)              { return e.chainListPanel.masterKnob.getValue(); }
    static void  toChain (EchoJayEditor& e)                { e.switchToTab (EchoJayEditor::Tab::Chain, true); }
};

namespace
{
struct Probe final : juce::AudioProcessor
{
    Probe() : juce::AudioProcessor (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo(), true).withOutput ("Out", juce::AudioChannelSet::stereo(), true)) {}
    const juce::String getName() const override { return "EJ Wet Guard Probe"; }
    void prepareToPlay (double, int) override {} void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; } bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; } bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; } int getCurrentProgram() override { return 0; } void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; } void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock& d) override { int v = 1; d.setSize (4); d.copyFrom (&v, 0, 4); }
    void setStateInformation (const void*, int) override {}
};
BuiltinDevice makeDevice()
{
    BuiltinDevice d; d.name = "EJ Wet Guard Probe"; d.category = "Utility"; d.descriptiveName = d.name;
    d.summary = "wet_rebuild_guard"; d.identifier = "echojay:test:wetguardprobe"; d.uid = 0x454A5747;
    d.create = [] { return std::unique_ptr<juce::AudioProcessor> (new Probe()); }; return d;
}
const BuiltinDeviceRegistrar reg { makeDevice() };
int failures = 0;
void check (bool ok, const juce::String& what, const juce::String& detail = {})
{ std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", what.toRawUTF8(), detail.isNotEmpty() ? ("  [" + detail + "]").toRawUTF8() : ""); if (! ok) ++failures; }
juce::var slotsVar (int n)
{
    juce::Array<juce::var> a;
    for (int k = 1; k <= n; ++k) { auto* o = new juce::DynamicObject(); o->setProperty ("n", k); o->setProperty ("plugin", "EJ Wet Guard Probe"); o->setProperty ("bypassed", false); a.add (juce::var (o)); }
    return juce::var (a);
}
void pumpMs (double ms) { const double t0 = juce::Time::getMillisecondCounterHiRes(); while (juce::Time::getMillisecondCounterHiRes() - t0 < ms) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false); } }
using A = EchoJayTabStripTestAccess;

// ten drag steps with the tick's own refresh between: returns recreations
// DESTROYED counts: a SafePointer to the block under the "mouse" goes null
// when the panel rebuilds (malloc may hand a new block the same address, so
// an address compare is not an observation).
int dragSteps (EchoJayEditor& e, ChainHost& host, bool masterKnob, float& lastWritten)
{
    int destroyed = 0; auto sp = A::blockPtr (e, 0);
    for (int k = 1; k <= 10; ++k)
    {
        const float v = 0.05f * (float) k;
        if (masterKnob) A::master (e, v); else A::knob (e, 0, v);
        lastWritten = v;
        A::refresh (e, false);
        if (sp == nullptr) { ++destroyed; sp = A::blockPtr (e, 0); }
    }
    (void) host;
    return destroyed;
}
bool rebuilt (EchoJayEditor& e, std::function<void()> op, double settleMs)
{
    auto sp = A::blockPtr (e, 0); op(); pumpMs (settleMs); A::refresh (e, false); pumpMs (30);
    return sp == nullptr;
}
}

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ej_wetguard_" + juce::String (juce::Time::getMillisecondCounter()));
    tmp.createDirectory(); setenv ("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);
    std::printf ("wet_rebuild_guard: a wet write never rebuilds the panel; structure always does\n");

    EchoJayProcessor proc; proc.prepareToPlay (48000.0, 512);
    auto& host = proc.getChainHost();
    host.restoreSavedChain (slotsVar (2), juce::var (new juce::DynamicObject()));
    std::unique_ptr<juce::AudioProcessorEditor> edBase (proc.createEditor());
    auto* ed = dynamic_cast<EchoJayEditor*> (edBase.get()); if (! ed) return 2;
    ed->setSize (1280, 820); A::toChain (*ed); A::refresh (*ed, true); pumpMs (100);
    check (A::block (*ed, 0) != nullptr && host.getNumSlots() == 2, "own rack: 2 probe slots rendered, Chain tab active");

    std::printf ("== (1) ten slot-knob drag steps, refresh(false) between each ==\n");
    { float last = 0; const int rec = dragSteps (*ed, host, false, last);
      check (rec == 0, "SlotBlock NOT destroyed across 10 drag steps (no rebuild)", "destroyed " + juce::String (rec) + " time(s)");
      check (std::abs (host.getSlotWet (0) - last) < 1e-6f, "final ChainHost slot 0 wet == last step", juce::String (host.getSlotWet (0), 3) + " vs " + juce::String (last, 3)); }

    std::printf ("== (3) a wet change from OUTSIDE the knob reaches the existing knob within one tick, no rebuild ==\n");
    { auto sp = A::blockPtr (*ed, 0);
      host.setSlotWet (0, 0.33f, ChainHost::WetSource::Restore);   // restore / sidecar / chat path (not the knob)
      A::refresh (*ed, false); pumpMs (60);
      check (sp != nullptr, "no rebuild for an external wet write");
      check (std::abs (A::knobVal (*ed, 0) - 0.33f) < 1e-3f, "the existing knob's drawn value follows (0.33)", juce::String (A::knobVal (*ed, 0), 3)); }

    std::printf ("== (4) ten MASTER-knob drag steps, refresh(false) between each ==\n");
    { float last = 0; const int rec = dragSteps (*ed, host, true, last);
      check (rec == 0, "SlotBlock NOT destroyed across 10 master drag steps", "destroyed " + juce::String (rec) + " time(s)");
      check (std::abs (host.getMasterWet() - last) < 1e-6f, "ChainHost master wet == last step", juce::String (host.getMasterWet(), 3)); }

    std::printf ("== (2) STRUCTURE ops still rebuild (address changes) - GREEN before AND after ==\n");
    { const int n0 = host.getNumSlots();
      check (rebuilt (*ed, [&]{ A::bypass (*ed, 1); }, 30) && host.getSlotInfo (1).bypassed, "bypass -> rebuilt");
      check (rebuilt (*ed, [&]{ A::move (*ed, 0, +1); }, 30), "move -> rebuilt");
      check (rebuilt (*ed, [&]{ host.restoreSavedChain (slotsVar (2), juce::var (new juce::DynamicObject())); }, 150) && host.getNumSlots() == n0 + 2,
             "restore (adds 2 slots) -> rebuilt", juce::String (host.getNumSlots()));
      check (rebuilt (*ed, [&]{ A::remove (*ed, host.getNumSlots() - 1); }, 250) && host.getNumSlots() == n0 + 1,
             "remove -> rebuilt", juce::String (host.getNumSlots())); }

    std::printf ("== (5) the BORROWED-rack variant of (1): rack Held, knob on the BorrowHost path ==\n");
    { const juce::String uid = "uid-wetguard";
      proc.borrowEngageBegin (uid, "lease-wetguard", true, true);
      auto* bh = proc.borrowHost(); bh->restoreSavedChain (slotsVar (2), juce::var (new juce::DynamicObject()));
      proc.pendingChannelUid = uid; EchoJayAlignTestAccess::hold (proc);
      A::refresh (*ed, true); pumpMs (100);
      check (A::block (*ed, 0) != nullptr, "borrowed view rendered");
      int destroyed = 0; auto sp = A::blockPtr (*ed, 0); float last = 0;
      for (int k = 1; k <= 10; ++k) { last = 0.05f * (float) k; A::knob (*ed, 0, last); A::refresh (*ed, false); if (sp == nullptr) { ++destroyed; sp = A::blockPtr (*ed, 0); } }
      check (destroyed == 0, "borrowed: SlotBlock NOT destroyed across 10 drag steps", "destroyed " + juce::String (destroyed) + " time(s)");
      check (std::abs (bh->getSlotWet (0) - last) < 1e-6f, "borrowed: BorrowHost slot 0 wet == last step", juce::String (bh->getSlotWet (0), 3));
      // (2) on the BorrowHost: a plan apply still rebuilds
      auto spPlan = A::blockPtr (*ed, 0);
      using namespace LinkShm::StructureEdit;
      std::vector<SlotIdentity> base = bh->liveIdentity(); std::vector<CurrentSlot> cur;
      for (int i = 0; i < (int) base.size(); ++i) cur.push_back ({ base[(size_t) i], i, false, false, {}, false, {} });
      CurrentSlot c; c.identity.name = "EJ Wet Guard Probe"; c.originIndex = -1; cur.push_back (c);
      auto scratch = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("wetguard_journal"); scratch.createDirectory();
      auto res = bh->applyStructurePlan (scratch.getFullPathName(), computePlan (uid, base, cur));
      A::refresh (*ed, false); pumpMs (60);
      check (res.ok && spPlan == nullptr && bh->getNumSlots() == 3, "borrowed: plan apply (Create) -> rebuilt", res.ok ? juce::String (bh->getNumSlots()) : res.failedAt); }

    // ---- (6) A WET-ONLY CHANGE SURVIVES DESELECT (2 Oct 2026 ruling) --------------------------------------
    // Sean's 14:20 residual. Create and Commit already carried wet, so a created or edited slot was fine - but a
    // slot whose ONLY change was the wet knob classified as LeaveUnedited, emitted NO op, and the change died at
    // deselect. The 28 "slotWet REJECTED" lines in his log were the live write failing too, so the value had no
    // route to the Link at all: not live (the Link's rack is parked while we hold the borrow) and not at deselect.
    {
        std::printf ("\n-- (6) deselect carries the wet of a slot nothing else writes --\n");
        using namespace LinkShm::StructureEdit;
        const juce::String uid = "uid-wetguard";            // the same borrow this guard already holds
        auto* bh = proc.borrowHostIfActiveFor (uid);
        check (bh != nullptr && bh->getNumSlots() >= 2, "(6) precondition: a borrowed host with slots",
               bh != nullptr ? juce::String (bh->getNumSlots()) + " slot(s)" : juce::String ("(no borrow host)"));
        if (bh != nullptr && bh->getNumSlots() >= 2)
        {
            const int last = bh->getNumSlots() - 1;          // "slot 4" in Sean's rack: the one he moved
            const float want = 0.42f;
            bh->setSlotWet (last, 1.0f, ChainHost::WetSource::Restore);   // the value deselect must overwrite
            std::vector<SlotIdentity> base = bh->liveIdentity();
            std::vector<CurrentSlot> cur;
            for (int i = 0; i < (int) base.size(); ++i)
                cur.push_back ({ base[(size_t) i], i, /*edited*/ false, /*withheld*/ false, {}, false, {} });
            cur[(size_t) last].wet = want;                   // the ONLY difference, and the slot is NOT edited
            const auto plan = computePlan (uid, base, cur);
            int values = 0, commits = 0;
            for (const auto& op : plan.ops)
            {
                if (op.type == OpType::Values) ++values;
                if (op.type == OpType::Commit) ++commits;
            }
            check (values == (int) base.size() && commits == 0,
                   "(6) the plan carries VALUES for every surviving slot and commits nothing  (RED as it stood: "
                   "an unedited slot emitted no op at all, so its wet never left V2)",
                   juce::String (values) + " values op(s), " + juce::String (commits) + " commit(s)");
            auto scratch = juce::File::getSpecialLocation (juce::File::tempDirectory)
                               .getChildFile ("wetguard_journal6"); scratch.createDirectory();
            // A DESELECT IS NOT A USER GESTURE ON THIS SIDE (2 Oct 2026 ruling). The knob was turned in V2, so
            // the undo entry belongs there; applying the values here must add none, or every deselect buries the
            // Link's history under one wet step per slot for something the user never did on this rack.
            const int undoBefore = bh->undoDepth();
            const auto res = bh->applyStructurePlan (scratch.getFullPathName(), plan);
            check (res.ok, "(6) ...and the plan applies without a rollback - a wet knob must never be able to "
                           "abort a deselect", res.ok ? juce::String ("ok") : res.failedAt);
            check (std::abs (bh->getSlotWet (last) - want) < 1e-6f,
                   "(6) ...and the slot's wet is the value the user left, after deselect",
                   juce::String (bh->getSlotWet (last), 3) + " vs " + juce::String (want, 3));
            check (bh->undoDepth() == undoBefore,
                   "(6) ...and it added NO undo entry - a deselect applies what the user already did in V2, so the "
                   "wet writes are a Restore, not a User gesture on this rack",
                   juce::String (bh->undoDepth()) + " vs " + juce::String (undoBefore) + " before");
        }
    }

    std::printf ("\n==== wet_rebuild_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
