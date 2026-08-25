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

// The friend declared in LinkProcessor.h — drives the REAL rack-lease arms
// (engage saves priors and bypasses all; release restores), so the gate
// proves the shipping code, not a re-implementation.
struct EchoJayLinkSyncTestAccess
{
    static void engage (LinkProcessor& p)  { p.rackLeaseEngage(); }
    static void release (LinkProcessor& p)
    {
        p.leaseActive_.store (false, std::memory_order_relaxed);
        p.rackLeaseRelease();
    }
    // Registration arms (26 Aug 2026): drive the REAL claim path the way
    // the 1s tick does, so the gate proves claim/wait/adopt end to end.
    static void releaseOnly (LinkProcessor& p) { p.releaseRegistrySlot(); }
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
        auto slotCount = [&ldir]
        {
            juce::MemoryBlock mb;
            juce::File(ldir + "registry_v2.bin").loadFileAsData(mb);
            const auto* d = static_cast<const uint8_t*>(mb.getData());
            int n = 0;
            for (int i = 0; i < 16
                            && mb.getSize() >= (size_t)(64 + (i+1)*128); ++i)
            { uint32_t v; std::memcpy(&v, d + 64 + i*128, 4); if (v) ++n; }
            return n;
        };
        for (int t = 0; t < 3 && slotCount() == 0; ++t)
            proc.updateShmState();
        check (slotCount() == 1,
               "a Link against an EMPTY registry registers within a poll cycle",
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
        EchoJayLinkSyncTestAccess::releaseAndReclaim (proc, 1);
        check (! EchoJayLinkSyncTestAccess::registered (proc),
               "our-uid frozen holder: first sight WAITS, not adopts");
        EchoJayLinkSyncTestAccess::releaseAndReclaim (proc, 6);
        check (EchoJayLinkSyncTestAccess::registered (proc)
                 && EchoJayLinkSyncTestAccess::uid (proc) == myUid,
               "through the threshold the ghost is adopted, uid KEPT",
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
        bool allDry = true;
        for (int i = 0; i < host.getNumSlots(); ++i)
            if (! host.getSlotInfo (i).bypassed) allDry = false;
        check (allDry, "engage bypassed every slot (dry rack)");

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
        allDry = true;
        for (int i = 0; i < host.getNumSlots(); ++i)
            if (! host.getSlotInfo (i).bypassed) allDry = false;
        check (allDry, "the lease's dry rack held through the reshape "
                       "(created slot included)");
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
    std::printf ("== §8 mute: lease-carried, output-only ==\n");
    {
        proc.prepareToPlay (48000.0, 512);
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
        feed();
        check (peak() > 0.01f, "unmuted lease passes signal",
               juce::String (peak()));
        EchoJayLinkSyncTestAccess::setMute (proc, true);
        for (int b = 0; b < 8; ++b) feed();         // ride out the 30ms ramp
        check (peak() < 0.001f, "muteOut silences the OUTPUT",
               juce::String (peak()));
        // The one restore path lifts it (Release/Expire clears the want).
        EchoJayLinkSyncTestAccess::releaseArmClearsMute (proc);
        for (int b = 0; b < 8; ++b) feed();
        check (peak() > 0.01f, "the restore path unmutes",
               juce::String (peak()));
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

    // ---- negative control -------------------------------------------------
    check (false, "NEGATIVE CONTROL - this line is SUPPOSED to fail");
    const bool caught = (failures == 1);
    failures = 0;
    check (caught, "the harness caught the planted failure");

    std::printf ("\nlinksync_test: %s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
