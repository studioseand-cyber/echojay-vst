// linksync_test — the FUNCTIONAL gate for structure-plan display parity
// (24 Aug 2026). The Link has two rack models: chainHost (audio truth, what
// the sidecar publishes, what applyStructurePlan mutates) and chainModel
// (editor-facing, keeps missing-slot memory). A dispatched plan updated only
// the first, so a freshly reopened editor still showed the old shape — and a
// SOURCE pin passed while that bug was live, because it proved a call existed
// without proving it did anything. So this gate applies a real plan through
// LinkProcessor::applyStructurePlanAndSync against the REAL Link object code
// and asserts the EDITOR-FACING model reports the new slot count.
//
// Runs sandboxed (EJ_STATE_TEST_HOME) like every suite that touches disk.

#include <CoreFoundation/CoreFoundation.h>   // before JUCE: MacTypes' Point vs juce::Point
#include <JuceHeader.h>
#include <cstddef>
#include "LinkProcessor.h"
#include "EedDeviceRegistry.h"
#include "LinkShm.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

// The sandbox mechanism, same as borrowhost_test: interpose NSHomeDirectory
// so JUCE's mac app-data resolution honours EJ_STATE_TEST_HOME. Without this
// the binary sees the REAL ~/Library and the disk guard refuses to run.
extern "C" CFStringRef NSHomeDirectory (void)
{
    static CFStringRef s = [] () -> CFStringRef
    {
        const char* home = std::getenv ("EJ_STATE_TEST_HOME");
        return CFStringCreateWithCString (kCFAllocatorDefault,
                                          home != nullptr ? home : "/nonexistent",
                                          kCFStringEncodingUTF8);
    }();
    return s;
}

// ---- a minimal builtin probe, registered TU-locally (borrowhost pattern) --
struct SyncProbe : juce::AudioProcessor
{
    SyncProbe() : juce::AudioProcessor (BusesProperties()
        .withInput ("In", juce::AudioChannelSet::stereo(), true)
        .withOutput ("Out", juce::AudioChannelSet::stereo(), true)) {}
    const juce::String getName() const override { return "EJ Sync Probe"; }
    void prepareToPlay (double, int) override {}
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    int value = 0;   // observable state: seeded vs defaults is provable
    void getStateInformation (juce::MemoryBlock& mb) override
    { mb.append (&value, sizeof (int)); }
    void setStateInformation (const void* data, int size) override
    { if (size >= (int) sizeof (int)) std::memcpy (&value, data, sizeof (int)); }
};

static BuiltinDevice makeProbe()
{
    BuiltinDevice d;
    d.name = "EJ Sync Probe"; d.category = "Utility";
    d.descriptiveName = d.name; d.summary = "linksync_test probe";
    d.identifier = "echojay:test:syncprobe"; d.uid = 0x454A5350;
    d.create = [] { return std::make_unique<SyncProbe>(); };
    return d;
}
static const BuiltinDeviceRegistrar probeReg { makeProbe() };

// ---- a probe that CHANGES THE SIGNAL, for stage 1's acceptance test ------
// SyncProbe's processBlock is empty, which is right for the structure legs - they are about plans, indices and
// bypass state - but it means no leg here could ever show whether a rack was in circuit. Sean's acceptance test
// for stage 1 is "the kick still goes through the drum bus", and that is a claim about AUDIO: a state-only
// assertion that nothing is flagged bypassed would still pass if the rack were switched off some other way.
// x2 is +6.02 dB, chosen so the measurement cannot be confused with a dry path (x1) or silence.
struct SyncGain : juce::AudioProcessor
{
    SyncGain() : juce::AudioProcessor (BusesProperties()
        .withInput ("In", juce::AudioChannelSet::stereo(), true)
        .withOutput ("Out", juce::AudioChannelSet::stereo(), true)) {}
    const juce::String getName() const override { return "EJ Sync Gain"; }
    void prepareToPlay (double, int) override {}
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    { b.applyGain (2.0f); }
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}
};

static BuiltinDevice makeGain()
{
    BuiltinDevice d;
    d.name = "EJ Sync Gain"; d.category = "Utility";
    d.descriptiveName = d.name; d.summary = "linksync_test gain probe (x2)";
    d.identifier = "echojay:test:syncgain"; d.uid = 0x454A5347;
    d.create = [] { return std::make_unique<SyncGain>(); };
    return d;
}
static const BuiltinDeviceRegistrar gainReg { makeGain() };

// The friend declared in LinkProcessor.h — drives the REAL rack-lease arms
// (engage saves priors and bypasses all; release restores), so the gate
// proves the shipping code, not a re-implementation.
struct EchoJayLinkSyncTestAccess
{
    static void engage (LinkProcessor& p)  { p.rackLeaseEngage(); }
    // Stage 1: the lease is a CONTROL lease now, so a leg has to be able to say it is still HELD while the
    // audio is untouched. Reading rackLeaseActive_ directly, because that IS the control lease's own flag.
    static bool leaseHeld (LinkProcessor& p) { return p.rackLeaseActive_; }
    // Stage 1: the ruling "a session release can never clear a user mute" is about the USER's reason, so the
    // leg reads that reason rather than inferring it from silence - silence has three possible authors.
    static bool userMuteStillOn (LinkProcessor& p) { return p.userMuteOn(); }
    static void release (LinkProcessor& p)
    {
        p.leaseActive_.store (false, std::memory_order_relaxed);
        p.rackLeaseRelease();
    }
    // Registration arms (26 Aug 2026): drive the REAL claim path the way
    // the 1s tick does, so the gate proves claim/wait/adopt end to end.
    static void releaseOnly (LinkProcessor& p) { p.releaseRegistrySlot(); }
    // 06d item 0: the model is what the editor renders AND what persists, so a leg about a persistent
    // bypass has to read it through the real resync rather than infer it from the host.
    static void resync (LinkProcessor& p) { p.resyncChainModelFromHost(); }
    static const std::vector<LinkProcessor::ChainSlotSpec>& model (LinkProcessor& p) { return p.chainModel; }
    static void releaseAndReclaim (LinkProcessor& p, int n)
    {
        p.releaseRegistrySlot();
        for (int i = 0; i < n; ++i) p.claimRegistrySlot();
    }
    static bool registered (LinkProcessor& p) { return p.regSlotIdx >= 0; }
    static juce::String uid (LinkProcessor& p) { return p.instanceUid_; }
    // §8 mute arms: drive the REAL lease-mute state the poll would set.
    static void setMute (LinkProcessor& p, bool m)
    { p.rackLeaseMuteWant_.store (m, std::memory_order_relaxed); }
    static void pollLease (LinkProcessor& p) { p.pollEditLease(); }
    static bool muteWant (LinkProcessor& p)
    { return p.rackLeaseMuteWant_.load (std::memory_order_relaxed); }
    static void releaseArmClearsMute (LinkProcessor& p)
    { p.rackLeaseMuteWant_.store (false, std::memory_order_relaxed); }
    // Mute/solo arms (27 Aug 2026): drive and observe the REAL composed
    // state, the REAL fabric scan, the REAL ctrl poll and publish.
    static void setUserMute (LinkProcessor& p, bool m)
    { p.muteUserOn_.store (m, std::memory_order_relaxed); }
    static void setSolo (LinkProcessor& p, bool s)
    { p.soloOn_.store (s, std::memory_order_relaxed); }
    static void setSoloMuteDirect (LinkProcessor& p, bool s)
    { p.soloMuteWant_.store (s, std::memory_order_relaxed); }
    static bool soloMuteWant (LinkProcessor& p)
    { return p.soloMuteWant_.load (std::memory_order_relaxed); }
    static void fabricScan (LinkProcessor& p) { p.soloFabricScan(); }
    static void pollCtrl (LinkProcessor& p) { p.pollControlCommand(); }
    static void publish (LinkProcessor& p) { p.publishRackSidecar(); }
};

static int failures = 0;
static void check (bool ok, const juce::String& what,
                   const juce::String& detail = {})
{
    std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", what.toRawUTF8(),
                 detail.isNotEmpty() ? ("  [" + detail + "]").toRawUTF8() : "");
    if (! ok) ++failures;
}

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    std::printf ("linksync_test: plan display parity, editor-facing model\n");
    juce::ScopedJuceInitialiser_GUI juceInit;

    // ---- disk guard, same as state_match_test -----------------------------
    const char* home = std::getenv ("EJ_STATE_TEST_HOME");
    const auto appData = juce::File::getSpecialLocation (
        juce::File::userApplicationDataDirectory).getFullPathName();
    if (home == nullptr || ! appData.startsWith (juce::String (home)))
    {
        std::printf ("REFUSING TO RUN: app-data resolves to %s, not under "
                     "EJ_STATE_TEST_HOME\n", appData.toRawUTF8());
        return 2;
    }
    const juce::String dir =
        juce::File (juce::String (home)).getChildFile ("plandir")
            .getFullPathName() + "/";
    juce::File (dir).createDirectory();

    // A REAL third-party-shaped plugin for the resolution gate: an Apple AU
    // (ships with macOS), enumerated live and staged into the sandbox
    // catalogue BEFORE the processor constructs — the ctor loads
    // chain_plugins.xml, so this is the Link's own list at plan time.
    juce::PluginDescription realAu;
    {
        juce::AudioUnitPluginFormat aufmt;
        for (const auto& id : aufmt.searchPathsForPlugins({}, false, false))
        {
            if (! id.containsIgnoreCase ("dely")) continue;
            juce::OwnedArray<juce::PluginDescription> found;
            aufmt.findAllTypesForFile (found, id);
            if (! found.isEmpty()) { realAu = *found[0]; break; }
        }
        check (realAu.name.isNotEmpty(),
               "a real Apple AU (AUDelay) enumerated for the catalogue",
               realAu.name);
        juce::KnownPluginList kl0;
        kl0.addType (realAu);
        if (auto xml = kl0.createXml())
            juce::File (appData).getChildFile ("EchoJay")
                .getChildFile ("chain_plugins.xml")
                .getParentDirectory().createDirectory(),
            juce::File (appData).getChildFile ("EchoJay")
                .getChildFile ("chain_plugins.xml")
                .replaceWithText (xml->toString (juce::XmlElement::TextFormat()));
    }

    LinkProcessor proc;
    auto& host = proc.getChainHost();

    // ---- THE ORDINARY PATH, end to end (26 Aug 2026 regression): every
    // UidClaimGate arm was about REFUSING; nothing proved a free registry
    // still gets claimed — and it didn't. A Link constructed against an
    // empty registry must appear as REGISTERED within a poll cycle.
    std::printf ("== registration: empty registry claims immediately ==\n");
    {
        int err0 = 0;
        const juce::String ldir = LinkShm::resolveDir(err0);
        check (ldir.isNotEmpty(), "link dir resolves in the sandbox");
        auto slotCount = [&ldir, &proc]
        {
            juce::MemoryBlock mb;
            juce::File(ldir + juce::String (LinkShm::kRegistryFilename)).loadFileAsData(mb);
            const auto* d = static_cast<const uint8_t*>(mb.getData());
            int n = 0;
            // 6 Sep 2026: the layout is kRegMaxSlots (256) and the registry on
            // this Mac is shared with live hosts, so count ONLY rows this test
            // owns (our uid, or the "Foreign"/"Ghost" rows it plants) - never
            // assume the registry is empty.
            const auto mine = proc.getInstanceUidForTest();
            for (int i = 0; i < kRegMaxSlots
                            && mb.getSize() >= (size_t)(64 + (i+1)*128); ++i)
            {
                uint32_t v; std::memcpy(&v, d + 64 + i*128, 4); if (! v) continue;
                const juce::String nm  = juce::String::fromUTF8 ((const char*) d + 64 + i*128 + 4);
                const juce::String uid = juce::String::fromUTF8 ((const char*) d + 64 + i*128 + offsetof (RegistrySlot, instanceUid));
                if (uid == mine || nm == "Foreign" || nm == "Ghost") ++n;
            }
            return n;
        };
        const int before0 = slotCount();
        for (int t = 0; t < 3 && slotCount() == before0; ++t)
            proc.updateShmState();
        check (slotCount() == 1,
               "a Link registers within a poll cycle (its own row; the registry is shared, not assumed empty)",
               juce::String (slotCount()));

        // A DIFFERENT-uid holder is not a collision at all: it must never
        // enter the probe, and registration proceeds immediately. We plant
        // a foreign slot through a second shared mapping (MAP_SHARED —
        // coherent with the processor's own), release ours, and re-claim.
        int rfd = -1, rerr = 0;
        void* rmap = LinkShm::openRegistry(ldir, rfd, rerr);
        check (rmap != nullptr, "test's second registry mapping opened");
        auto* slots = LinkShm::regSlots(rmap);
        int foreign = -1;
        for (int i = 0; i < kRegMaxSlots; ++i)
            if (LinkShm::loadAcquire(&slots[i].inUse) == 0) { foreign = i; break; }
        check (foreign >= 0, "a free slot exists for the foreign holder");
        std::strcpy(slots[foreign].displayName, "Foreign");
        std::strcpy(slots[foreign].instanceUid, "ffffffff01");
        LinkShm::storeRelease(&slots[foreign].heartbeat, 40u);   // frozen forever
        LinkShm::storeRelease(&slots[foreign].inUse, 1u);
        EchoJayLinkSyncTestAccess::releaseAndReclaim (proc, 1);
        check (slotCount() == 2,
               "a different-uid holder never enters the probe - claim is "
               "immediate", juce::String (slotCount()));

        // A FROZEN holder carrying OUR uid: the ghost of a dead launch.
        // Adoption must come only through the threshold — never on the
        // first sight — and the uid survives (no re-mint, no churn).
        const juce::String myUid = EchoJayLinkSyncTestAccess::uid (proc);
        EchoJayLinkSyncTestAccess::releaseOnly (proc);
        int ghost = -1;
        for (int i = 0; i < kRegMaxSlots; ++i)
            if (LinkShm::loadAcquire(&slots[i].inUse) == 0) { ghost = i; break; }
        std::strcpy(slots[ghost].displayName, "Ghost");
        std::strncpy(slots[ghost].instanceUid, myUid.toRawUTF8(), 11);
        LinkShm::storeRelease(&slots[ghost].heartbeat, 99u);     // frozen
        LinkShm::storeRelease(&slots[ghost].inUse, 1u);
        // C4 (6 Sep 2026): a frozen holder whose publisher is ALIVE is adopted
        // only after kUidGateFloorMs (a liveness decision made in time, not
        // counts) - six quick calls must still WAIT. A holder whose publisher
        // pid is DEAD (the unclean-kill case the gate was built for) is adopted
        // at once: plant its sidecar naming a dead pid.
        EchoJayLinkSyncTestAccess::releaseAndReclaim (proc, 1);
        check (! EchoJayLinkSyncTestAccess::registered (proc),
               "our-uid frozen holder: first sight WAITS, not adopts");
        EchoJayLinkSyncTestAccess::releaseAndReclaim (proc, 6);
        check (! EchoJayLinkSyncTestAccess::registered (proc),
               "six quick observations of a frozen holder with a LIVE publisher still WAIT (C4: time, not counts)");
        { LinkShm::RackSidecar dead; dead.valid = true; dead.uid = myUid; dead.name = "Ghost"; dead.revision = 1; dead.publisherPid = 999999; dead.hostPid = 999999; LinkShm::writeRackSidecar (ldir, dead); }
        EchoJayLinkSyncTestAccess::releaseAndReclaim (proc, 1);
        check (EchoJayLinkSyncTestAccess::registered (proc)
                 && EchoJayLinkSyncTestAccess::uid (proc) == myUid,
               "a frozen holder whose publisher pid is DEAD is adopted at once, uid KEPT (C4b)",
               EchoJayLinkSyncTestAccess::uid (proc));
        check (LinkShm::loadAcquire(&slots[ghost].inUse) == 0
                 || EchoJayLinkSyncTestAccess::uid (proc) == myUid,
               "the ghost slot was reaped, not duplicated");
        // Clean the foreign plant so later arms see a sane registry.
        LinkShm::storeRelease(&slots[foreign].inUse, 0u);
    }

    // ---- build a two-slot rack straight into chainHost --------------------
    // Deliberately BYPASSING the model writers (restoreSavedChain is the
    // session-restore path, chainHost-only): the editor-facing model must
    // stay empty, proving the two models really are two models (the control
    // this gate needs, or a one-model refactor would pass vacuously).
    {
        juce::Array<juce::var> arr;
        for (int n = 1; n <= 2; ++n)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("n", n);
            o->setProperty ("plugin", "EJ Sync Probe");
            o->setProperty ("bypassed", false);
            arr.add (juce::var (o));
        }
        host.restoreSavedChain (juce::var (arr),
                                juce::var (new juce::DynamicObject()));
    }
    check (host.getNumSlots() == 2, "host holds two",
           juce::String (host.getNumSlots()));
    check ((int) proc.getChainModel().size() == 0,
           "editor-facing model is a SECOND source (empty until synced)",
           juce::String ((int) proc.getChainModel().size()));

    // ---- the plan: base = live identity, one Create at the end ------------
    namespace SE = LinkShm::StructureEdit;
    const auto base = host.liveIdentity();
    check ((int) base.size() == 2, "live identity snapshot has two");
    SE::Plan plan;
    plan.uid = "linksync-test";
    plan.baseIdentity = base;
    SE::Op create;
    create.type = SE::OpType::Create;
    create.to   = 2;
    create.name = "EJ Sync Probe";
    create.identity = { "EJ Sync Probe",
                        juce::String (0x454A5350), {} };
    plan.ops.push_back (create);
    plan.creating = 1;

    const auto res = proc.applyStructurePlanAndSync (dir, plan);
    check (res.ok, "the plan applied",
           res.reasons.joinIntoString ("; "));
    check (host.getNumSlots() == 3, "host holds three",
           juce::String (host.getNumSlots()));
    // THE GATE: the model the editor renders from reports the new count.
    check ((int) proc.getChainModel().size() == 3,
           "the EDITOR-FACING model reports the new slot count",
           juce::String ((int) proc.getChainModel().size()));
    bool idxOk = true;
    for (int i = 0; i < (int) proc.getChainModel().size(); ++i)
        if (proc.getChainModel()[(size_t) i].hostIdx != i) idxOk = false;
    check (idxOk, "model hostIdx maps 1:1 onto the host after the sync");

    // ---- a REFUSED plan still leaves the two models agreeing --------------
    // (the sync is unconditional: refusal and rollback repaint too).
    SE::Plan bad = plan;
    bad.baseIdentity.pop_back();          // count mismatch -> refused whole
    const auto res2 = proc.applyStructurePlanAndSync (dir, bad);
    check (! res2.ok, "the mismatched plan was refused whole");
    check (host.getNumSlots() == 3
             && (int) proc.getChainModel().size() == 3,
           "after refusal the models still agree",
           juce::String (host.getNumSlots()) + "/"
             + juce::String ((int) proc.getChainModel().size()));

    // ---- lease + plan: bypass restore is plan-aware -----------------------
    // The 24 Aug defect: priors are captured per index before the plan, the
    // plan shifts indices, and every plugin came back bypassed. Engage with
    // MIXED bypass, apply a plan that removes and creates, release — every
    // surviving slot must match its pre-borrow state, the created slot must
    // match what the plan said, and mid-lease the MODEL (what the editor
    // renders and what persists) must never record the lease's dry rack.
    std::printf ("== lease + plan: bypass survives a reshape ==\n");
    {
        // Rack is 3 probes from the arms above. Pre-borrow: [off, BYP, off].
        host.setSlotBypassed (0, false);
        host.setSlotBypassed (1, true);
        host.setSlotBypassed (2, false);
        EchoJayLinkSyncTestAccess::engage (proc);
        // ---- STAGE 1 (10 Oct 2026): ENGAGE NO LONGER SWITCHES THE RACK OFF ---------------------------
        // This used to assert the opposite - "engage bypassed every slot (dry rack)" - and it was GREEN, which
        // is the point worth recording: the leg was right about the code and the code was the fault Sean could
        // hear. A dry rack in place means the channel strip, the sends and the drum bus downstream all carry an
        // UNPROCESSED signal while the processed one is summed in at the V2's position.
        // Re-aimed to the ruling, BOTH SIDES asserted: every slot the user wanted live IS live, and a slot the
        // user had bypassed STAYS bypassed. The second half matters - "nothing is bypassed" would also pass if
        // engage had started forcing slots ON, which would be the same class of fault pointing the other way.
        // The rack here is [live, BYPASSED, live] from the three lines above.
        {
            juce::String got;
            for (int i = 0; i < host.getNumSlots(); ++i)
                got << (i ? "," : "") << juce::String (i) << (host.getSlotInfo (i).bypassed ? ":byp" : ":live");
            const bool asIntended = host.getNumSlots() == 3
                                 && ! host.getSlotInfo (0).bypassed
                                 &&   host.getSlotInfo (1).bypassed
                                 && ! host.getSlotInfo (2).bypassed;
            check (asIntended,
                   "stage 1: engage leaves every slot AS THE USER INTENDED - the rack keeps processing in its "
                   "own place, so the kick still reaches its own strip and its bus", got);
        }

        // The plan, THROUGH computePlan (byp must ride from CurrentSlot):
        // remove pre-borrow slot 1 (the bypassed one), create one at the
        // end whose plan-carried bypass is TRUE.
        const auto base2 = host.liveIdentity();
        std::vector<SE::CurrentSlot> cur;
        cur.push_back ({ base2[0], 0, false, false, {}, false });
        cur.push_back ({ base2[2], 2, false, false, {}, false });
        cur.push_back ({ SE::SlotIdentity { "EJ Sync Probe",
                             juce::String (0x454A5350), {} },
                         -1, true, false, {}, /*bypassedNow*/ true });
        auto plan2 = SE::computePlan ("linksync-test", base2, cur);
        check (plan2.removing == 1 && plan2.creating == 1,
               "the plan removes one and creates one");
        // The wire carries the bypass: serialize and read back like the
        // real transport does, and apply the DESERIALIZED plan.
        SE::Plan wire; SE::PreImages preIgnored;
        check (SE::planFromVar (SE::planToVar (plan2, {}), wire, preIgnored),
               "the plan round-trips the wire");
        bool bypOnWire = false;
        for (const auto& op : wire.ops)
            if (op.type == SE::OpType::Create && op.bypassed) bypOnWire = true;
        check (bypOnWire, "the Create op's bypass survives serialization");

        const auto res3 = proc.applyStructurePlanAndSync (dir, wire);
        check (res3.ok, "the reshape applied mid-lease",
               res3.reasons.joinIntoString ("; "));
        // STAGE 1 (10 Oct 2026): there is no dry rack to hold. This asserted that the lease's bypass-all
        // survived a reshape, created slot included; the lease no longer bypasses anything. The claim that
        // SURVIVES the ruling - and it is the one the reshape could actually break - is that every slot, the
        // created one included, sits at the INTENT the plan carried. The plan above creates its new slot with
        // bypassed TRUE, and the two pre-borrow survivors were [live, live] (slot 1, the bypassed one, was the
        // one removed).
        {
            juce::String got;
            for (int i = 0; i < host.getNumSlots(); ++i)
                got << (i ? "," : "") << juce::String (i) << (host.getSlotInfo (i).bypassed ? ":byp" : ":live");
            const bool asPlanned = host.getNumSlots() == 3
                                && ! host.getSlotInfo (0).bypassed
                                && ! host.getSlotInfo (1).bypassed
                                &&   host.getSlotInfo (2).bypassed;
            check (asPlanned,
                   "stage 1: after the reshape every slot sits at the intent the plan carried - the survivors "
                   "live, the created slot bypassed because that is what the Create op said", got);
        }
        // THE GENERAL RULE, mid-lease: the model records the TRUE states,
        // not the lease's temporary bypass — because the model persists.
        const auto& m = proc.getChainModel();
        check ((int) m.size() == 3
                 && m[0].bypassed == false
                 && m[1].bypassed == false
                 && m[2].bypassed == true,
               "mid-lease the model holds the TRUE bypass states, remapped",
               juce::String (m.size() >= 3
                   ? juce::String ((int) m[0].bypassed)
                       + juce::String ((int) m[1].bypassed)
                       + juce::String ((int) m[2].bypassed)
                   : juce::String ("short")));

        EchoJayLinkSyncTestAccess::release (proc);
        // Survivors: pre-borrow slot 0 (off) and slot 2 (off), now at 0/1;
        // the created slot takes the plan's TRUE.
        check (host.getSlotInfo (0).bypassed == false
                 && host.getSlotInfo (1).bypassed == false
                 && host.getSlotInfo (2).bypassed == true,
               "after release every survivor matches its pre-borrow bypass "
               "and the created slot matches the plan",
               juce::String ((int) host.getSlotInfo (0).bypassed)
                   + juce::String ((int) host.getSlotInfo (1).bypassed)
                   + juce::String ((int) host.getSlotInfo (2).bypassed));
        const auto& m2 = proc.getChainModel();
        check ((int) m2.size() == 3
                 && m2[0].bypassed == false
                 && m2[1].bypassed == false
                 && m2[2].bypassed == true,
               "and the model agrees after release");
    }

    // ---- a cross-format Create never seeds silently -----------------------
    // The main may host a SUBSTITUTE build (preferInlineHostableDesc), so a
    // Create's blob can come from a different format than what this rack
    // stages. State is format-specific: only a matching format seeds; a
    // mismatch arrives at DEFAULTS and says so, in the withheld voice (§5c).
    std::printf ("== cross-format Create: defaults, said aloud ==\n");
    {
        host.clearStateNotes();
        const auto base3 = host.liveIdentity();
        const juce::String stagedFmt = host.getSlotInfo (0).format;
        auto blob = [] (int v)
        { juce::MemoryBlock mb; mb.append (&v, sizeof (int));
          return LinkShm::stateToB64 (mb); };
        std::vector<SE::CurrentSlot> cur3;
        for (int i = 0; i < (int) base3.size(); ++i)
            cur3.push_back ({ base3[(size_t) i], i, false, false, {}, false, {} });
        const SE::SlotIdentity probeId { "EJ Sync Probe",
                                         juce::String (0x454A5350), {} };
        // One Create whose state matches the staged format (must seed)...
        cur3.push_back ({ probeId, -1, true, false, blob (42), false,
                          stagedFmt });
        // ...and one whose state claims a format this rack does not stage
        // (must arrive at defaults, with a named note).
        cur3.push_back ({ probeId, -1, true, false, blob (99), false,
                          "VST3" });
        auto plan3 = SE::computePlan ("linksync-test", base3, cur3);
        SE::Plan wire3; SE::PreImages pre3;
        check (SE::planFromVar (SE::planToVar (plan3, {}), wire3, pre3),
               "the cross-format plan round-trips the wire");
        bool fmtOnWire = false;
        for (const auto& op : wire3.ops)
            if (op.type == SE::OpType::Create && op.stateFormat == "VST3")
                fmtOnWire = true;
        check (fmtOnWire, "the state's format survives serialization");
        const auto res4 = proc.applyStructurePlanAndSync (dir, wire3);
        check (res4.ok, "the plan applied (a format mismatch is honesty, "
                        "not failure)", res4.reasons.joinIntoString ("; "));
        const int n4 = host.getNumSlots();
        check (n4 == 5, "both created slots exist", juce::String (n4));
        auto* seeded = dynamic_cast<SyncProbe*> (host.getSlotProcessor (3));
        auto* dflt   = dynamic_cast<SyncProbe*> (host.getSlotProcessor (4));
        check (seeded != nullptr && seeded->value == 42,
               "the matching-format Create SEEDED",
               seeded ? juce::String (seeded->value) : juce::String ("null"));
        check (dflt != nullptr && dflt->value == 0,
               "the cross-format Create arrived at DEFAULTS, never seeded",
               dflt ? juce::String (dflt->value) : juce::String ("null"));
        const auto notes = host.getStateNotes().joinIntoString (" | ");
        check (notes.contains ("EJ Sync Probe")
                 && notes.contains ("running at defaults")
                 && notes.contains ("settings do not travel across formats"),
               "and it says so, in the withheld voice", notes);
    }

    // ---- a NON-BUILTIN Create resolves from the catalogue and loads -------
    // THE GATE THAT WOULD HAVE CAUGHT THIS ON DAY ONE: every prior gate
    // staged builtins, which skip the resolution arm entirely — so "no
    // format name matches no format" shipped invisible. A Create carrying
    // name + uid only must resolve to a REAL format from the Link's own
    // catalogue and instantiate; a plugin the Link doesn't have must be
    // refused BY NAME, never as a format error.
    std::printf ("== non-builtin Create: catalogue resolution ==\n");
    {
        const auto base5 = host.liveIdentity();
        const int n5 = (int) base5.size();
        std::vector<SE::CurrentSlot> cur5;
        for (int i = 0; i < n5; ++i)
            cur5.push_back ({ base5[(size_t) i], i, false, false, {}, false, {} });
        cur5.push_back ({ SE::SlotIdentity { realAu.name,
                              juce::String (ChainHost::descUid (realAu)), {} },
                          -1, true, false, {}, false, {} });
        auto plan5 = SE::computePlan ("linksync-test", base5, cur5);
        const auto res5 = proc.applyStructurePlanAndSync (dir, plan5);
        check (res5.ok, "a bare name+uid Create resolved and applied",
               res5.reasons.joinIntoString ("; ") + " failedAt=" + res5.failedAt);
        check (host.getNumSlots() == n5 + 1, "the slot exists",
               juce::String (host.getNumSlots()));
        const auto info5 = host.getSlotInfo (n5);
        check (info5.format == realAu.pluginFormatName
                 && host.getSlotProcessor (n5) != nullptr,
               "resolved to a REAL format from the catalogue and instantiated",
               info5.format);
        // Single-source manufacturer (2 Sep 2026): this slot ARRIVED via a
        // plan identity that carried NO manufacturer — the instance
        // backfill at storage must supply it, because the float-by-identity
        // placement reads it and a blank one embedded a Waves slot blank.
        check (info5.manufacturer.isNotEmpty(),
               "a name+uid-only arrival still carries the manufacturer "
               "(instance backfill at storage)",
               "mfr=\"" + info5.manufacturer + "\"");
        // BOTH projections (2 Sep): getAllSlotInfos dropped the sixth
        // field in its own five-field brace init while getSlotInfo carried
        // it — the main's panel and the sidecar publish feed from the
        // PLURAL, which is exactly why the Link floated and the main did
        // not. One builder now; this arm keeps them converged.
        check (host.getAllSlotInfos()[(size_t) n5].manufacturer
                   == info5.manufacturer,
               "getAllSlotInfos and getSlotInfo project the SAME fields",
               "plural mfr=\""
                 + host.getAllSlotInfos()[(size_t) n5].manufacturer + "\"");

        // The genuinely-missing plugin: refused by name.
        const auto base6 = host.liveIdentity();
        std::vector<SE::CurrentSlot> cur6;
        for (int i = 0; i < (int) base6.size(); ++i)
            cur6.push_back ({ base6[(size_t) i], i, false, false, {}, false, {} });
        cur6.push_back ({ SE::SlotIdentity { "EJ No Such Plugin",
                                             "999999", {} },
                          -1, true, false, {}, false, {} });
        auto plan6 = SE::computePlan ("linksync-test", base6, cur6);
        const auto res6 = proc.applyStructurePlanAndSync (dir, plan6);
        check (! res6.ok && res6.failedAt == "stage",
               "a plugin this Link lacks fails at stage, rack untouched");
        check (res6.reasons.joinIntoString ("; ")
                   .contains ("this Link doesn't have EJ No Such Plugin"),
               "and the refusal names the plugin, not a format error",
               res6.reasons.joinIntoString ("; "));
    }

    // ---- a BUILTIN Create with a NAME-ONLY identity (the session-build
    // shape) --------------------------------------------------------------
    // 27 Aug field failure: Phase A pre-resolved the builtin name into a
    // uid-ful desc and PARKED under name|uid; Phase B reattached by the op
    // identity's key, name| — "staged instance vanished", whole-plan
    // rollback. Every prior gate carried a uid, so the keys happened to
    // agree. The park key must be the plan's identity vocabulary, always.
    std::printf ("== builtin Create, name-only identity (session build) ==\n");
    {
        const auto base7 = host.liveIdentity();
        const int n7 = (int) base7.size();
        std::vector<SE::CurrentSlot> cur7;
        for (int i = 0; i < n7; ++i)
            cur7.push_back ({ base7[(size_t) i], i, false, false, {}, false, {} });
        cur7.push_back ({ SE::SlotIdentity { "EJ Sync Probe",
                                             juce::String(), {} },   // NO uid
                          -1, true, false, {}, false, {} });
        auto plan7 = SE::computePlan ("linksync-test", base7, cur7);
        const auto res7 = proc.applyStructurePlanAndSync (dir, plan7);
        check (res7.ok,
               "a name-only builtin Create stages AND reattaches (key symmetry)",
               res7.reasons.joinIntoString ("; ") + " failedAt=" + res7.failedAt);
        check (host.getNumSlots() == n7 + 1, "the built slot exists",
               juce::String (host.getNumSlots()));
    }

    // ---- §8: the lease-carried mute, functionally ------------------------
    // muteOut zeroes the Link's OUTPUT (after the ring write) while the
    // rack lease holds; lifting it restores. Ramped, so the assert allows
    // the 30ms tail and checks the settled blocks.
    // ---- WHY THESE MUTE LEGS BYPASS THE RACK EXPLICITLY (10 Oct 2026) ------
    // The two blocks below are about the MUTE COMPOSITION - three reasons, one silence - and not about the rack.
    // They measured a path that only ever carried audio because a LEASE bypassed the whole rack: this harness
    // builds its rack from TU-local probes and a real AUDelay and never pumps a message thread, so ChainHost's
    // graph is not a working one here. With the rack live the chain delivers silence, and the instrumented run
    // says so precisely - slot 0's IN reads -200.0 dBTP, so nothing reaches the rack at all, let alone leaves it.
    // Stage 1 stopped the lease bypassing anything, which removed the side effect these legs were standing on.
    // So the bypass is now asked for OUT LOUD, by the leg that needs it, instead of arriving as a side effect of
    // the thing under test. That is the honest version either way: a leg about mute should not depend on a rack.
    auto bypassWholeRack = [&] (bool on)
    {
        for (int i = 0; i < host.getNumSlots(); ++i) host.setSlotBypassed (i, on);
    };

    std::printf ("== §8 mute: lease-carried, output-only ==\n");
    {
        proc.prepareToPlay (48000.0, 512);
        bypassWholeRack (true);   // see the note above: this leg is about mute, not the rack
        juce::AudioBuffer<float> blk (2, 512);
        juce::MidiBuffer midi;
        auto feed = [&]{ for (int ch = 0; ch < 2; ++ch)
                             for (int i = 0; i < 512; ++i)
                                 blk.setSample (ch, i, 0.5f);
                         proc.processBlock (blk, midi); };
        auto peak = [&]{ float m = 0;
                         for (int ch = 0; ch < 2; ++ch)
                             for (int i = 0; i < 512; ++i)
                                 m = juce::jmax (m, std::abs (blk.getSample (ch, i)));
                         return m; };
        EchoJayLinkSyncTestAccess::engage (proc);   // rack lease active
        // STAGE 1 (10 Oct 2026): A SETTLED BLOCK, not the first one. This called feed() ONCE, which was enough
        // while a lease bypassed the whole rack - a dry chain reaches full level in a single block. The rack now
        // keeps processing, so the chain's own wet/dry smoothing ramps, and the first block is mid-ramp. This
        // block's own comment already said the rule ("Ramped, so the assert allows the 30ms tail and checks the
        // settled blocks"); the engage arm was the one place that did not follow it.
        for (int b = 0; b < 8; ++b) feed();
        check (peak() > 0.01f, "unmuted lease passes signal",
               juce::String (peak()));
        // RETIRED (STAGE 1, 10 Oct 2026): "muteOut silences the OUTPUT" and "the restore path unmutes".
        // Their subject was THE LEASE'S OWN MUTE, and it is gone. It existed because the V2 summed a processed
        // copy of this channel into its own output, so the Link had to go quiet or the two would double - the
        // mute was the audio detour's other half. With no injection, a lease that still muted would mean
        // SELECTING A LINK SILENCES THAT CHANNEL, which is worse than the kick that started this.
        // The lease still CARRIES muteOut on the wire and the Link still parses it - the "file -> poll -> want"
        // legs below are untouched and still assert that - it simply no longer composes into silence. Keeping
        // the parse is deliberate: an older V2 will still send it, and the Link must read it without obeying it.
        EchoJayLinkSyncTestAccess::setMute (proc, true);
        for (int b = 0; b < 8; ++b) feed();
        check (peak() > 0.01f,
               "stage 1: a lease's muteOut does NOT silence the channel - the V2 is not standing in for this "
               "rack any more, so a selected Link stays audible where it is",
               juce::String (peak()));
        check (EchoJayLinkSyncTestAccess::muteWant (proc),
               "stage 1: ...and the want is still PARSED and held, so an older V2's muteOut is read and "
               "disobeyed rather than ignored");
        EchoJayLinkSyncTestAccess::releaseArmClearsMute (proc);
        EchoJayLinkSyncTestAccess::release (proc);
    }

    // ---- THE MUTE CONTRACT, Link half (26 Aug doubling): the REAL lease
    // ---- file, byte-shaped as the main writes it, through the REAL poll.
    std::printf ("== §8 mute contract: file -> poll -> want ==\n");
    {
        int errL = 0;
        const juce::String ldir = LinkShm::resolveDir (errL);
        const juce::String myUid2 = EchoJayLinkSyncTestAccess::uid (proc);
        const juce::String lease =
            "{\"v\": 1, \"leaseId\": \"lease-file-test\", \"slot\": 0, "
            "\"scope\": \"rack\", \"muteOut\": true, \"tMs\": "
            + juce::String (juce::Time::currentTimeMillis()) + "}";
        juce::File (LinkShm::leasePath (ldir, myUid2)).replaceWithText (lease);
        EchoJayLinkSyncTestAccess::pollLease (proc);
        check (EchoJayLinkSyncTestAccess::muteWant (proc),
               "the Link's poll reads muteOut from the REAL file");
        // editPending (27 Aug wrong-banner): absent reads FALSE — an old
        // main never strands the pending wording.
        check (! proc.rackEditPendingHeld(),
               "a lease without editPending reads not-held");
        const juce::String lease2 =
            "{\"v\": 1, \"leaseId\": \"lease-file-test\", \"slot\": 0, "
            "\"scope\": \"rack\", \"muteOut\": true, "
            "\"editPending\": true, \"tMs\": "
            + juce::String (juce::Time::currentTimeMillis()) + "}";
        juce::File (LinkShm::leasePath (ldir, myUid2)).replaceWithText (lease2);
        EchoJayLinkSyncTestAccess::pollLease (proc);
        check (proc.rackEditPendingHeld(),
               "editPending in the FILE reaches the banner state");
        juce::File (LinkShm::leasePath (ldir, myUid2)).deleteFile();
        for (int t = 0; t < 40; ++t)                 // ride to the 3s expiry
            EchoJayLinkSyncTestAccess::pollLease (proc);
        check (! EchoJayLinkSyncTestAccess::muteWant (proc),
               "lease gone -> the want clears through the restore path");
        check (! proc.rackEditPendingHeld(),
               "lease gone -> the pending hold clears with it");
    }

    // ---- EDITOR PLACEMENT: one decision, builtins never float ------------
    // 31 Aug 2026: the Link consulted only the popout list (no builtin
    // arm), so the native-size poll — blind to a plain JUCE editor —
    // marked EchoJay's own devices popout-only and floated them forever
    // under a false "plugin limitation" caption. Both hosts now consult
    // ChainHost::editorPlacement; builtins are checked FIRST (a stale
    // on-disk mark is inert) and markPopoutOnly refuses them outright.
    std::printf ("== editor placement: builtins embed, popouts float ==\n");
    {
        using CH = ChainHost;
        check (CH::editorPlacement ("EchoJay Gain", CH::kBuiltinFormat)
                   == CH::EditorPlacement::InlineJuce,
               "a builtin places INLINE (plain JUCE editor, no native view)");
        CH::markPopoutOnly ("EchoJay Gain", CH::kBuiltinFormat);
        check (! CH::isPopoutOnly ("EchoJay Gain", CH::kBuiltinFormat),
               "markPopoutOnly REFUSES a builtin (the mis-mark cannot recur)");
        check (CH::editorPlacement ("EchoJay Gain", CH::kBuiltinFormat)
                   == CH::EditorPlacement::InlineJuce,
               "and the placement still embeds it");
        CH::markPopoutOnly ("WaveShell-AU", "AudioUnit");
        check (CH::editorPlacement ("WaveShell-AU", "AudioUnit")
                   == CH::EditorPlacement::Float,
               "a REAL out-of-process mark still floats, label truthful");
        check (CH::editorPlacement ("Some Plugin", "VST3")
                   == CH::EditorPlacement::InlineNative,
               "everything else takes the native inline path");
        // Float-by-identity (2 Sep 2026, the WaveShell blank pane): a
        // valid-frame-renders-nothing view defeats the size poll by
        // construction, so WaveShell floats by CATALOGUE identity,
        // decided before any embed attempt — through the same one
        // decision, so glyph, caption and click agree.
        check (CH::editorPlacement ("Abbey Road Chambers (s)", "AudioUnit",
                                    "Waves")
                   == CH::EditorPlacement::Float,
               "a Waves-identity plugin floats BEFORE any embed attempt");
        check (! CH::isPopoutOnly ("Abbey Road Chambers (s)", "AudioUnit"),
               "and needs NO persisted mark to do so");
        check (CH::editorPlacement ("EchoJay Gain", CH::kBuiltinFormat,
                                    "EchoJay")
                   == CH::EditorPlacement::InlineJuce,
               "builtin-first ordering survives the identity arm");
        // ONE READER (1 Sep 2026 audit): isPopoutOnly is consulted ONLY
        // inside editorPlacement (plus its own decl/impl) — a second
        // direct reader skips the builtin arm and reintroduces the bug
        // in one place while this truth table stays green. Counted per
        // file so a new call site fails loudly.
        auto countIn = [](const char* path)
        {
            const juce::String src =
                juce::File::getCurrentWorkingDirectory()
                    .getChildFile (path).loadFileAsString();
            int n = 0, at = 0;
            while ((at = src.indexOf (at, "isPopoutOnly(")) >= 0) { ++n; ++at; }
            return n;
        };
        check (countIn ("Source/ChainHost.h") == 2       // decl + the ONE call
                 && countIn ("Source/ChainHost.cpp") == 1 // the impl
                 && countIn ("Source/PluginEditor.h") == 0
                 && countIn ("Source/LinkEditor.h") == 0,
               "isPopoutOnly has ONE reader: editorPlacement (no direct "
               "call sites on either side)");
    }

    // ---- MUTE/SOLO (27 Aug 2026, MUTE_SOLO_SPEC) --------------------------
    // Composition truth table through the REAL processBlock: three reasons,
    // one silence; each keeps its own lifetime, and a session release can
    // NEVER clear a user mute — the ruling, asserted as behaviour.
    std::printf ("== mute/solo: three reasons, one silence ==\n");
    {
        using T = EchoJayLinkSyncTestAccess;
        bypassWholeRack (true);   // see the note above the §8 mute block
        juce::AudioBuffer<float> blk (2, 512);
        juce::MidiBuffer midi;
        auto feed = [&]{ for (int ch = 0; ch < 2; ++ch)
                             for (int i = 0; i < 512; ++i)
                                 blk.setSample (ch, i, 0.5f);
                         proc.processBlock (blk, midi); };
        auto peak = [&]{ float m = 0;
                         for (int ch = 0; ch < 2; ++ch)
                             for (int i = 0; i < 512; ++i)
                                 m = juce::jmax (m, std::abs (blk.getSample (ch, i)));
                         return m; };
        auto settle = [&]{ for (int b = 0; b < 8; ++b) feed(); };
        settle();
        check (peak() > 0.01f, "all reasons clear: audible",
               juce::String (peak()));
        T::setUserMute (proc, true); settle();
        check (peak() < 0.001f, "user mute alone silences", juce::String (peak()));
        // THE RULING SURVIVES STAGE 1, and it is the half worth keeping: a session release can NEVER clear a
        // user mute. The session mute itself no longer composes into silence (see the retirement note in the
        // block above), so "user + session mutes hold together" is now carried by the user's mute alone - which
        // is exactly what the ruling is about. Asserted through the real release arm, as before.
        T::engage (proc); T::setMute (proc, true); settle();
        check (peak() < 0.001f,
               "the USER mute holds while a session mute is also wanted", juce::String (peak()));
        T::releaseArmClearsMute (proc); T::release (proc); settle();
        check (peak() < 0.001f,
               "session released - the USER mute survives (the ruling)",
               juce::String (peak()));
        check (T::userMuteStillOn (proc),
               "...and it survives as the USER's own reason, not as a leftover of the session's");
        T::setUserMute (proc, false); settle();
        check (peak() > 0.01f, "user mute lifted - audible again",
               juce::String (peak()));
        T::setSoloMuteDirect (proc, true); settle();
        check (peak() < 0.001f, "solo-mute alone silences", juce::String (peak()));
        T::setSoloMuteDirect (proc, false); settle();
        check (peak() > 0.01f, "solo-mute lifted - audible again");
    }

    // The FABRIC, against the real registry mapping and real sidecar files:
    // a live foreign solo mutes; my own membership exempts; a clean exit
    // recovers immediately; a frozen heartbeat (crash) recovers within the
    // freshness window. RegLiveness::proven is sticky, so the ghost arm is
    // the one that proves death has its own check.
    std::printf ("== solo fabric: live foreign solo mutes; ghosts cannot ==\n");
    {
        using T = EchoJayLinkSyncTestAccess;
        int errF = 0;
        const juce::String fdir = LinkShm::resolveDir (errF);
        int ffd = -1, ferr = 0;
        void* fmap = LinkShm::openRegistry (fdir, ffd, ferr);
        check (fmap != nullptr, "fabric arm's registry mapping opened");
        auto* fslots = LinkShm::regSlots (fmap);
        const int fi = 9;                       // a slot nobody else claims
        const char* fuid = "fabsolo001";
        std::memset (&fslots[fi], 0, sizeof (RegistrySlot));
        std::memcpy (fslots[fi].instanceUid, fuid, 10);
        LinkShm::storeRelease (&fslots[fi].heartbeat, 1u);
        LinkShm::storeRelease (&fslots[fi].inUse, 1u);
        LinkShm::RackSidecar frc;
        frc.valid = true; frc.uid = fuid; frc.name = "Fabric Probe";
        frc.soloOn = true; frc.muteSoloCapable = true;
        LinkShm::writeRackSidecar (fdir, frc);
        T::fabricScan (proc);
        check (! T::soloMuteWant (proc),
               "one observation is NOT proof of life - no mute yet");
        LinkShm::storeRelease (&fslots[fi].heartbeat, 2u);
        T::fabricScan (proc);
        check (T::soloMuteWant (proc),
               "a PROVEN-live foreign solo mutes this Link");
        T::setSolo (proc, true); T::fabricScan (proc);
        check (! T::soloMuteWant (proc),
               "joining the solo set exempts (multi-solo membership)");
        T::setSolo (proc, false);
        LinkShm::storeRelease (&fslots[fi].heartbeat, 3u);
        T::fabricScan (proc);
        check (T::soloMuteWant (proc), "leaving the set re-mutes");
        LinkShm::storeRelease (&fslots[fi].inUse, 0u);
        T::fabricScan (proc);
        check (! T::soloMuteWant (proc),
               "clean exit: the solo dies with the row, immediately");
        // The ghost: re-plant, prove live, then freeze past the window.
        LinkShm::storeRelease (&fslots[fi].inUse, 1u);
        LinkShm::storeRelease (&fslots[fi].heartbeat, 4u);
        T::fabricScan (proc);
        LinkShm::storeRelease (&fslots[fi].heartbeat, 5u);
        T::fabricScan (proc);
        check (T::soloMuteWant (proc), "ghost arm: proven live first");
        juce::Thread::sleep (3600);             // the freshness window
        T::fabricScan (proc);
        check (! T::soloMuteWant (proc),
               "frozen heartbeat: the solo cannot outlive its owner");
        LinkShm::storeRelease (&fslots[fi].inUse, 0u);
        juce::File (LinkShm::rackSidecarPath (fdir, fuid)).deleteFile();
        T::fabricScan (proc);
    }

    // TRANSPORT: the two additive cmd fields through the REAL poll —
    // consumed, applied, answered.
    std::printf ("== mute/solo transport: ctrl-cmd, consumed + acked ==\n");
    {
        using T = EchoJayLinkSyncTestAccess;
        int errT = 0;
        const juce::String tdir = LinkShm::resolveDir (errT);
        const juce::String tuid = T::uid (proc);
        auto sendCmd = [&](const char* field, bool on)
        {
            juce::File (tdir + "ctrl-cmd-" + tuid + ".json").replaceWithText (
                "{\"v\": 1, \"seq\": " + juce::String (LinkShm::nextCtrlSeq())
                + ", \"" + field + "\": " + (on ? "true" : "false") + "}");
            T::pollCtrl (proc);
        };
        sendCmd ("muteUser", true);
        check (proc.userMuteOn(), "muteUser=true applied through the poll");
        check (! juce::File (tdir + "ctrl-cmd-" + tuid + ".json").existsAsFile(),
               "the command was consumed");
        check (juce::File (tdir + "ctrl-ack-" + tuid + ".json").existsAsFile(),
               "and answered");
        sendCmd ("soloOn", true);
        check (proc.soloIsOn(), "soloOn=true applied through the poll");
        // The published sidecar carries all three bits + the COMPOSED
        // engaged state (a user mute is confirmed silence too).
        T::publish (proc);
        const auto prc = LinkShm::readRackSidecar (tdir, tuid);
        check (prc.uid == tuid && prc.muteUser && prc.soloOn
                 && prc.muteSoloCapable,
               "the sidecar publishes muteUser, soloOn and the capability");
        check (prc.muteEngaged,
               "muteEngaged reports the COMPOSED actual (user mute counts)");
        sendCmd ("muteUser", false);
        sendCmd ("soloOn", false);
        check (! proc.userMuteOn() && ! proc.soloIsOn(),
               "both lift through the same transport");
    }

    // PERSISTENCE: muteUser rides the saved state; soloOn NEVER exists in
    // it, by construction — asserted against the real serializer, both
    // directions. (Last arm: setState schedules an async chain restore.)
    std::printf ("== persistence: muteUser rides state; solo never ==\n");
    {
        using T = EchoJayLinkSyncTestAccess;
        T::setUserMute (proc, true);
        T::setSolo (proc, true);
        juce::MemoryBlock mb;
        proc.getStateInformation (mb);
        const juce::String js = juce::String::fromUTF8 (
            static_cast<const char*> (mb.getData()), (int) mb.getSize());
        check (js.contains ("\"muteUser\": true"),
               "the saved state carries the user mute");
        check (! js.contains ("soloOn"),
               "a saved solo CANNOT exist - the key is never written");
        T::setUserMute (proc, false);
        proc.setStateInformation (mb.getData(), (int) mb.getSize());
        check (proc.userMuteOn(), "restore brings the user mute back");
        T::setUserMute (proc, false);
        T::setSolo (proc, false);
    }

    // ---- 8 Oct 2026 (06d item 0): THE PERSISTENT BYPASS AFTER A HAND-BACK -------------------------------
    // Sean 11:41: "rarely, after the V2 releases a Link rack, the Link's plugins STAY bypassed" - real, persistent,
    // with no lease held. The hole was in what the MODEL records, which is what the editor renders AND what
    // persists: `resyncChainModelFromHost` tested `i < rackLeasePrior_.size()`, and the prior list is captured AT
    // ENGAGE, so a slot ADDED DURING the lease - which is what every build does - had no prior and fell through to
    // the EFFECTIVE bypass, true for every slot under a lease. Releasing could not undo a value already saved.
    std::printf ("== 06d item 0: a hand-back restores the ORIGINAL states, including slots added under the lease ==\n");
    {
        using T = EchoJayLinkSyncTestAccess;
        // Pre-borrow intent, deliberately mixed so an all-live or all-bypassed restore cannot pass by accident.
        host.setSlotBypassed (0, false);
        host.setSlotBypassed (1, true);
        if (host.getNumSlots() > 2) host.setSlotBypassed (2, false);
        const int nBefore = host.getNumSlots();

        T::engage (proc);
        // (a) A SECOND ENGAGE MUST NOT RE-SNAPSHOT. Called straight through the real arm, as a re-engage would.
        T::engage (proc);

        // A slot ARRIVES under the lease, as a build's add does. It is attach-bypassed (effective) and its INTENT
        // is live - the case the prior list cannot describe, because it was taken before this slot existed.
        const auto* probe = BuiltinDeviceRegistry::instance().findByName ("EJ Sync Probe");
        if (probe != nullptr)
            host.insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*probe), host.getNumSlots());
        const int nUnderLease = host.getNumSlots();
        check (nUnderLease == nBefore + 1, "a slot was added while the rack was leased",
               juce::String (nBefore) + " -> " + juce::String (nUnderLease));

        // MID-LEASE the model must record INTENT for every slot, the new one included. RED as it stood: the added
        // slot recorded the lease's dry state and that is what would have persisted.
        T::resync (proc);
        const auto& model = T::model (proc);
        bool modelHonest = (int) model.size() == nUnderLease;
        for (int i = 0; i < (int) model.size() && modelHonest; ++i)
            if (model[(size_t) i].bypassed != host.getSlotInfo (i).intendedBypassed) modelHonest = false;
        juce::String got;
        for (int i = 0; i < (int) model.size(); ++i)
            got << (i ? "," : "") << juce::String (i + 1) << (model[(size_t) i].bypassed ? ":byp" : ":live");
        check (modelHonest,
               "mid-lease the model records the INTENT of every slot, including one added under the lease "
               "(RED as it stood: the added slot recorded the lease's dry rack, and the model is what persists)",
               got);

        // THE HAND-BACK.
        T::release (proc);
        bool restored = true;
        for (int i = 0; i < host.getNumSlots(); ++i)
            if (host.getSlotInfo (i).bypassed != host.getSlotInfo (i).intendedBypassed) restored = false;
        juce::String after;
        for (int i = 0; i < host.getNumSlots(); ++i)
            after << (i ? "," : "") << juce::String (i + 1)
                  << (host.getSlotInfo (i).bypassed ? ":byp" : ":live");
        check (restored, "after the hand-back every slot's EFFECTIVE state is its intent again", after);
        check (! host.getSlotInfo (0).bypassed && host.getSlotInfo (1).bypassed,
               "...and the ORIGINAL mixed states came back, not all-live and not all-bypassed", after);
        if (probe != nullptr)
            check (! host.getSlotInfo (nUnderLease - 1).bypassed,
                   "...and the slot that arrived under the lease is LIVE, as its intent always said", after);

        // Leave the rack as the later arms expect it.
        host.setSlotBypassed (1, false);
    }

    // ---- STAGE 1 ACCEPTANCE: THE KICK STILL GOES THROUGH THE DRUM BUS -----
    // Sean's own acceptance test for stage 1, and it is a claim about AUDIO. Every other lease leg in this file
    // is about plans, indices and flags; none could tell whether the rack was in circuit.
    //
    // THE SHAPE OF THE OLD FAULT, which is what the leg has to be able to see: during a lease the Link's rack
    // was bypassed in place, so the Link's OUTPUT WAS ITS INPUT. Everything downstream in the host - the
    // channel's own fader and sends, the drum bus and its chain - carried an unprocessed kick, while a processed
    // copy was summed in at the V2's position.
    //
    // ON THE SHARED `proc`, DELIBERATELY: a freshly constructed LinkProcessor outputs silence until it has been
    // through the registration and mute arms this file runs earlier, so a private one would measure my fixture
    // and not the product. (My first attempt did exactly that and read 0.0000 with and without a lease - the
    // same figure under both, which is the tell that it was measuring nothing.)
    std::printf ("\n== stage 1 acceptance: the rack processes WHILE A LEASE IS HELD ==\n");
    {
        using T = EchoJayLinkSyncTestAccess;
        const auto* gainDev = BuiltinDeviceRegistry::instance().findByName ("EJ Sync Gain");
        check (gainDev != nullptr, "acceptance precondition: the x2 gain probe is registered");
        if (gainDev != nullptr)
        {
            const int at = host.getNumSlots();
            host.insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*gainDev), at);
            juce::AudioBuffer<float> blk (2, 512); juce::MidiBuffer midi;
            auto feed = [&] { for (int ch = 0; ch < 2; ++ch)
                                  for (int i = 0; i < 512; ++i) blk.setSample (ch, i, 0.25f);
                              proc.processBlock (blk, midi);
                              float m = 0.0f;
                              for (int ch = 0; ch < 2; ++ch)
                                  for (int i = 0; i < 512; ++i) m = juce::jmax (m, std::abs (blk.getSample (ch, i)));
                              return m; };
            auto settled = [&] { float m = 0.0f; for (int b = 0; b < 10; ++b) m = feed(); return m; };

            // THE TWO-SIDED CONTROL FIRST, so the measurement is known to be able to tell the two apart at all.
            // Bypassed, this slot must not multiply; live, it must. Without this pair, "x2 arrived" could be any
            // figure at all and the acceptance assertion below would be unfalsifiable.
            host.setSlotBypassed (at, true);
            const float byp = settled();
            host.setSlotBypassed (at, false);
            const float live = settled();
            check (byp > 0.1f && byp < 0.30f,
                   "acceptance control: with the x2 slot BYPASSED the Link passes its input unmultiplied",
                   juce::String (byp, 4) + " from 0.2500");
            check (live > byp * 1.8f,
                   "acceptance control: with the x2 slot LIVE the Link's rack is in circuit (x2 arrives)",
                   juce::String (live, 4) + " vs bypassed " + juce::String (byp, 4));

            // NOW THE RULING: hold a control lease across the render and nothing about the audio may change.
            T::engage (proc);
            const float leased = settled();
            check (leased > byp * 1.8f,
                   "STAGE 1 ACCEPTANCE: a lease is HELD and the Link still processes its own rack - the kick "
                   "reaches its own strip and its bus PROCESSED (RED as it stood: the rack was bypassed in "
                   "place, so the Link's output was its input)",
                   juce::String (leased, 4) + " vs bypassed " + juce::String (byp, 4));
            // The stronger claim, and the one that says the detour is gone rather than merely quieter: the lease
            // changed the audio NOT AT ALL.
            check (std::abs (leased - live) < 0.001f,
                   "STAGE 1 ACCEPTANCE: ...and the lease changed the audio NOT AT ALL",
                   juce::String (live, 4) + " -> " + juce::String (leased, 4));
            check (T::leaseHeld (proc),
                   "STAGE 1 ACCEPTANCE: ...while the lease is still HELD as a control lease (the command "
                   "channel, the acks and the sidecar are untouched - only the audio detour is gone)");

            T::release (proc);
            host.removeSlot (at);   // leave the shared rack as it was found
            settled();
        }
    }

    // ---- STAGE 5 MIGRATION: A PROJECT SAVED MID-LEASE OPENS SENSIBLY -------
    // The plan asks for one explicit migration test: open a project saved during a lease and confirm the Link
    // has its rack, the V2 shows it, and nothing is doubled.
    //
    // WHAT STAGE 1 ALREADY SETTLED, which is most of it: an old lease file can no longer bypass or mute
    // anything, because nothing does. So the dangerous half of that migration - a rack that comes back dry, or
    // a channel that comes back silent, because a stale lease file was read as fresh - is closed by
    // construction rather than by a restore path that has to remember to undo something.
    //
    // THE OTHER HALF IS NOT CLOSED AND I AM NOT PRETENDING IT IS: the plan also says the V2's saved borrowed
    // COPY must be ignored rather than applied, or the rack doubles. That is only true once the borrowed host
    // is gone - while it is still the edit-staging path, that copy is load-bearing (the 4 Oct ruling: a chain
    // built on a Link mid-lease and saved without switching racks lived ONLY there, and dropping it is how it
    // was lost in the first place). The two must move together, and they are the deferred half of stage 5.
    std::printf ("\n== stage 5 migration: a lease file from an old session cannot dry or silence the rack ==\n");
    {
        using T = EchoJayLinkSyncTestAccess;
        // A rack at a MIXED intent, as a user's own would be.
        host.setSlotBypassed (0, false);
        host.setSlotBypassed (1, true);
        // A lease is engaged, exactly as a reopen that reads a still-fresh lease file would engage it.
        T::engage (proc);
        juce::String got;
        for (int i = 0; i < juce::jmin (2, host.getNumSlots()); ++i)
            got << (i ? "," : "") << juce::String (i) << (host.getSlotInfo (i).bypassed ? ":byp" : ":live");
        check (host.getNumSlots() >= 2 && ! host.getSlotInfo (0).bypassed && host.getSlotInfo (1).bypassed,
               "stage 5 migration: with a lease HELD the rack sits at the user's intent - a stale lease file "
               "cannot bring a rack back dry", got);
        check (! proc.linkMuteWanted(),
               "stage 5 migration: ...and it cannot bring the channel back SILENT either (the lease arm of the "
               "mute composition is gone)");
        // ...and the MODEL - what the editor renders and what persists - records the intent, so the next save
        // cannot write the lease's state as the user's.
        T::resync (proc);
        const auto& m = T::model (proc);
        bool modelHonest = (int) m.size() == host.getNumSlots();
        for (int i = 0; i < (int) m.size() && modelHonest; ++i)
            if (m[(size_t) i].bypassed != host.getSlotInfo (i).intendedBypassed) modelHonest = false;
        check (modelHonest,
               "stage 5 migration: ...and the MODEL records intent, so the next save cannot persist a lease's "
               "state as the user's");
        T::release (proc);
    }

    // ---- negative control -------------------------------------------------
    check (false, "NEGATIVE CONTROL - this line is SUPPOSED to fail");
    const bool caught = (failures == 1);
    failures = 0;
    check (caught, "the harness caught the planted failure");

    std::printf ("\nlinksync_test: %s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
