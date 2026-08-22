/*  borrowhost_test — RACK_BORROW_IMPLEMENTATION_SPEC §6, step 1's gate.

    Proves, by content:
      1. Borrowed mode writes NOTHING to any shared file: every file under the
         sandboxed app-support dir is byte-identical after a full
         build/borrow/release cycle — including direct calls to every guarded
         writer, so the guarantee is enforcement, not luck.
      2. A second borrow of the same rack instantiates ZERO new plugins, and
         a REUSED instance seeded with a state blob is parameter-identical to
         a FRESH instance seeded with the same blob.
      3. The fallback: a plugin failing reuse verification goes
         pool-ineligible and later borrows instantiate fresh (counter moves).
      4. The latency hard block: a Borrowed host refuses the latency mirror
         (-1); a Primary host answers normally.

    DISK: sandboxed exactly like state_match_test — refuses to run unless
    JUCE's app-data dir resolves under EJ_STATE_TEST_HOME.
*/

#include <CoreFoundation/CoreFoundation.h>   // before JUCE: MacTypes' Point vs juce::Point
#include <JuceHeader.h>
#include "ChainHost.h"
#include "EedDeviceRegistry.h"
#include "LinkShm.h"   // BorrowRing — the stale-ring decision (finding #3)

#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <map>
#include <fstream>
#include <sstream>

/** The friend declared in ChainHost.h — reaches the PRIVATE guarded writers
    so the read-only enforcement is proven by calling them, not by hoping no
    path does. */
struct EchoJayBorrowHostTestAccess
{
    static void saveParamMaps (ChainHost& h)   { h.saveParamMapsToDisk(); }
    static void recordOversize (ChainHost& h)
    { h.recordStateOversize ("/tmp/fake.vst3", 123, "Fake", "borrowhost_test"); }
    static void refresh (ChainHost& h)         { h.doRefresh(); }
};

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

namespace
{
constexpr int kProbeValueDefault = 0;

// The well-behaved probe: 4-byte int state, honest round-trip.
struct BorrowProbe final : juce::AudioProcessor
{
    BorrowProbe() : juce::AudioProcessor (BusesProperties()
        .withInput ("In", juce::AudioChannelSet::stereo(), true)
        .withOutput ("Out", juce::AudioChannelSet::stereo(), true)) {}
    int value = kProbeValueDefault;
    const juce::String getName() const override { return "EJ Borrow Probe"; }
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
    void getStateInformation (juce::MemoryBlock& d) override
    { d.setSize (sizeof (int)); d.copyFrom (&value, 0, sizeof (int)); }
    void setStateInformation (const void* data, int size) override
    { if (size >= (int) sizeof (int)) std::memcpy (&value, data, sizeof (int)); }
};

// The grinch: a SEED-refuser — accepts any state except the poison value,
// so pool resets succeed but a poisoned pull seed throws. Exercises the
// failed-reuse-seed -> pool-ineligible arm.
struct BorrowGrinch final : juce::AudioProcessor
{
    BorrowGrinch() : juce::AudioProcessor (BusesProperties()
        .withInput ("In", juce::AudioChannelSet::stereo(), true)
        .withOutput ("Out", juce::AudioChannelSet::stereo(), true)) {}
    static constexpr int kPoison = 666;
    int value = 0;
    const juce::String getName() const override { return "EJ Borrow Grinch"; }
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
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void setCurrentProgram (int) override {}
    void getStateInformation (juce::MemoryBlock& d) override
    { d.setSize (sizeof (int)); d.copyFrom (&value, 0, sizeof (int)); }
    void setStateInformation (const void* data, int size) override
    {
        int v = 0;
        if (size >= (int) sizeof (int)) std::memcpy (&v, data, sizeof (int));
        if (v == kPoison) throw std::runtime_error ("grinch: poison refused");
        value = v;
    }
};

// The sulk: a RESET-refuser — throws on every setState after its first, so
// its pool reset fails and it must be retired + ineligible + fresh, never
// reused dirty.
struct BorrowSulk final : juce::AudioProcessor
{
    BorrowSulk() : juce::AudioProcessor (BusesProperties()
        .withInput ("In", juce::AudioChannelSet::stereo(), true)
        .withOutput ("Out", juce::AudioChannelSet::stereo(), true)) {}
    int value = 0, sets = 0;
    const juce::String getName() const override { return "EJ Borrow Sulk"; }
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
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void setCurrentProgram (int) override {}
    void getStateInformation (juce::MemoryBlock& d) override
    { d.setSize (sizeof (int)); d.copyFrom (&value, 0, sizeof (int)); }
    void setStateInformation (const void* data, int size) override
    {
        if (++sets >= 2) throw std::runtime_error ("sulk: no resets");
        if (size >= (int) sizeof (int)) std::memcpy (&value, data, sizeof (int));
    }
};

BuiltinDevice makeDevice (const char* name, const char* ident, int uid,
                          std::function<std::unique_ptr<juce::AudioProcessor>()> create)
{
    BuiltinDevice d;
    d.name = name; d.category = "Utility"; d.descriptiveName = name;
    d.summary = "borrowhost_test probe"; d.identifier = ident; d.uid = uid;
    d.create = std::move (create);
    return d;
}
const BuiltinDeviceRegistrar probeReg {
    makeDevice ("EJ Borrow Probe", "echojay:test:borrowprobe", 0x454A4250,
                [] { return std::make_unique<BorrowProbe>(); }) };
const BuiltinDeviceRegistrar grinchReg {
    makeDevice ("EJ Borrow Grinch", "echojay:test:borrowgrinch", 0x454A4247,
                [] { return std::make_unique<BorrowGrinch>(); }) };
const BuiltinDeviceRegistrar sulkReg {
    makeDevice ("EJ Borrow Sulk", "echojay:test:borrowsulk", 0x454A4253,
                [] { return std::make_unique<BorrowSulk>(); }) };

int failures = 0;
void check (bool ok, const juce::String& what, const juce::String& detail = {})
{
    std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", what.toRawUTF8(),
                 detail.isNotEmpty() ? ("  [" + detail + "]").toRawUTF8() : "");
    if (! ok) ++failures;
}

juce::String chunkFor (int v) { return juce::Base64::toBase64 (&v, sizeof (int)); }

juce::var slotsVar()
{
    juce::Array<juce::var> arr;
    int n = 1;
    for (const char* nm : { "EJ Borrow Probe", "EJ Borrow Grinch" })
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("n", n++);
        o->setProperty ("plugin", nm);
        o->setProperty ("bypassed", false);
        arr.add (juce::var (o));
    }
    return juce::var (arr);
}
juce::var stateVar (int probeV, int grinchV)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("1", chunkFor (probeV));
    o->setProperty ("2", chunkFor (grinchV));
    return juce::var (o);
}

std::map<juce::String, juce::MemoryBlock> snapshotDir (const juce::File& root)
{
    std::map<juce::String, juce::MemoryBlock> out;
    for (const auto& f : root.findChildFiles (juce::File::findFiles, true))
    {
        juce::MemoryBlock mb;
        f.loadFileAsData (mb);
        out[f.getRelativePathFrom (root)] = std::move (mb);
    }
    return out;
}

void borrowRack (ChainHost& h, int grinchV = 99)
{
    h.restoreSavedChain (slotsVar(), stateVar (4242, grinchV));
}

BorrowProbe*  probeIn  (ChainHost& h, int i) { return dynamic_cast<BorrowProbe*>  (h.getSlotProcessor (i)); }
} // namespace

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    std::printf ("borrowed-mode ChainHost: pool, read-only, latency block\n");
    juce::ScopedJuceInitialiser_GUI juceInit;

    // ---- disk guard, same as state_match_test -----------------------------
    const char* home = std::getenv ("EJ_STATE_TEST_HOME");
    const auto appData = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                             .getFullPathName();
    if (home == nullptr || ! appData.startsWith (juce::String (home)))
    {
        std::printf ("REFUSING TO RUN: app-data resolves to %s, not under "
                     "EJ_STATE_TEST_HOME (%s). Run via build_and_run.sh.\n",
                     appData.toRawUTF8(), home ? home : "(unset)");
        return 2;
    }
    std::printf ("  disk sandboxed under %s\n", appData.toRawUTF8());
    const juce::File shared = juce::File (appData).getChildFile ("EchoJay");
    shared.createDirectory();

    // ---- primary first (its ctor may touch disk; that is ITS right), then
    //      decoy shared files with known bytes, then the snapshot ----------
    ChainHost primary;   // Mode::Primary by default — proves latency arm too
    for (const char* nm : { "chain_plugins.xml", "chain_entries.xml",
                            "param_maps.json", "chain_blacklist.txt",
                            "session_project.json", "popout_only.txt",
                            "chain_state_oversize.txt" })
        shared.getChildFile (nm).replaceWithText (juce::String ("DECOY:") + nm);
    const auto before = snapshotDir (shared);
    check (before.size() >= 7, "decoy shared files staged",
           juce::String ((int) before.size()));

    // =======================================================================
    std::printf ("== borrow #1: fresh build ==\n");
    ChainHost host (ChainHost::Mode::Borrowed);
    host.prepare (48000.0, 512);
    check (host.isBorrowed(), "mode flag set");
    borrowRack (host);
    check (host.getNumSlots() == 2, "two slots built", juce::String (host.getNumSlots()));
    check (host.borrowFreshInstantiations() == 2, "borrow #1 instantiated 2 fresh",
           juce::String (host.borrowFreshInstantiations()));
    check (probeIn (host, 0) != nullptr && probeIn (host, 0)->value == 4242,
           "probe seeded with the blob");

    // Direct calls to every guarded writer: enforcement, not luck.
    std::printf ("== guarded writers, called directly ==\n");
    host.saveToDisk();
    host.addToBlacklist ("/tmp/fake.vst3", "borrowhost_test probe");
    host.archiveCurrentRack ("borrowhost_test probe");
    EchoJayBorrowHostTestAccess::saveParamMaps (host);
    EchoJayBorrowHostTestAccess::recordOversize (host);
    EchoJayBorrowHostTestAccess::refresh (host);

    // =======================================================================
    std::printf ("== release -> pool ==\n");
    host.releaseBorrowToPool();
    check (host.getNumSlots() == 0, "rack cleared");
    check (host.borrowPoolCount() == 2, "2 instances parked",
           juce::String (host.borrowPoolCount()));

    std::printf ("== borrow #2: reuse, zero new ==\n");
    borrowRack (host, BorrowGrinch::kPoison);
    check (host.getNumSlots() == 2, "two slots rebuilt");
    check (host.borrowFreshInstantiations() == 2,
           "borrow #2 instantiated ZERO new (counter still 2)",
           juce::String (host.borrowFreshInstantiations()));
    // CONTENT-PROVEN REUSE (spec §1): the reused, seeded probe must be
    // parameter-identical to a fresh instance seeded with the same blob.
    {
        auto* reused = probeIn (host, 0);
        check (reused != nullptr && reused->value == 4242,
               "reused probe re-seeded to the blob's value");
        BorrowProbe fresh;
        const int v = 4242;
        fresh.setStateInformation (&v, sizeof (int));
        juce::MemoryBlock a, b;
        if (reused != nullptr) reused->getStateInformation (a);
        fresh.getStateInformation (b);
        check (reused != nullptr && reused->value == fresh.value && a == b,
               "reused+seeded is parameter- and state-identical to fresh+seeded");
    }
    // The grinch threw on its reuse seed -> fallback arm engaged.
    {
        juce::PluginDescription gd;
        gd.pluginFormatName = kEchoJayBuiltinFormat;
        gd.fileOrIdentifier = "echojay:test:borrowgrinch";
        gd.uniqueId         = 0x454A4247;
        check (host.isBorrowPoolIneligible (gd),
               "grinch marked pool-INELIGIBLE after failing its reuse seed");
        bool noted = false;
        for (auto& n : host.getStateNotes())
            if (n.contains ("Grinch") && n.contains ("rejected")) noted = true;
        check (noted, "the failure is a per-slot note, not silence");
    }

    std::printf ("== borrow #3: ineligible instantiates fresh ==\n");
    host.releaseBorrowToPool();
    host.clearStateNotes();
    borrowRack (host, BorrowGrinch::kPoison);
    check (host.borrowFreshInstantiations() == 3,
           "grinch instantiated FRESH (counter 2 -> 3); probe still reused",
           juce::String (host.borrowFreshInstantiations()));
    check (host.getNumSlots() == 2, "rack complete despite the fallback");

    // =======================================================================
    std::printf ("== BorrowRing: stale binding -> sound or a stated release ==\n");
    {
        using BR = LinkShm::BorrowRing;
        check (BR::poll (2, 2, 0) == BR::Verdict::Bound,
               "ring found where bound: stay");
        check (BR::poll (2, 5, 0) == BR::Verdict::Rebind,
               "ring found elsewhere: REBIND (sound continues), never trust engage's index");
        check (BR::poll (2, -1, 0) == BR::Verdict::Lost,
               "ring gone, first tick: tolerated (Link may be re-registering)");
        check (BR::poll (2, -1, LinkShm::kBorrowRingMaxLostTicks - 1) == BR::Verdict::Release,
               "ring gone past tolerance: RELEASE — the third state, silence, does not exist");
        // The release SPEAKS: the processor's Release arm must set the
        // auto-release reason the editor shows, pinned by source.
        std::ifstream fp2 ("Source/PluginProcessor.cpp");
        std::stringstream sp2; sp2 << fp2.rdbuf();
        const juce::String psrc (sp2.str());
        const int tick = psrc.indexOf ("void EchoJayProcessor::borrowTick");
        const int tickEnd = tick >= 0 ? psrc.indexOf (tick, "\n}\n") : -1;
        const auto tickBody = (tick >= 0 && tickEnd > tick)
                                ? psrc.substring (tick, tickEnd) : juce::String();
        check (tickBody.isNotEmpty(), "borrowTick located");
        check (tickBody.contains ("BorrowRing::poll"),
               "the tick decides through the pinned pure gate");
        check (tickBody.contains ("borrowAutoReleaseReason_ ="),
               "the self-release sets the spoken reason (never silent)");
        // And the release affordance survives a view snap: the one-author
        // affordance includes borrowActive(), pinned by source.
        std::ifstream fe2 ("Source/PluginEditor.cpp");
        std::stringstream se2; se2 << fe2.rdbuf();
        const juce::String esrc (se2.str());
        check (esrc.contains ("|| processorRef.borrowActive()"),
               "the RELEASE button is reachable from every chain view");
        check (esrc.contains ("takeBorrowAutoReleaseReason"),
               "the editor consumes and shows the self-release reason");
    }

    // =======================================================================
    std::printf ("== BorrowRoute: where the solo lands, both routes gated ==\n");
    {
        using RT = LinkShm::BorrowRoute;
        check ( RT::throughMainChain (true,  false), "Mix/Master Bus default: THROUGH the main's chain");
        check (!RT::throughMainChain (false, false), "every other channel default: REPLACES the output");
        check (!RT::throughMainChain (true,  true),  "override on a bus: replaces");
        check ( RT::throughMainChain (false, true),  "override elsewhere: through");
        // BOTH routes exist in processBlock, complementary on one read —
        // pinned by source so neither site can vanish or double-fire.
        std::ifstream fp3 ("Source/PluginProcessor.cpp");
        std::stringstream sp3; sp3 << fp3.rdbuf();
        const juce::String psrc3 (sp3.str());
        check (psrc3.contains ("const bool borrowThrough = borrowRouteThroughMain();"),
               "the route is read ONCE per block");
        check (psrc3.contains ("if (borrowThrough)\n        applyBorrowSoloMix(buffer);"),
               "through-main site present (before chainHost.process)");
        check (psrc3.contains ("if (!borrowThrough)\n        applyBorrowSoloMix(buffer);"),
               "replace-after site present (after chainHost.process)");
        // The banner SAYS the mode, both wordings live in the editor.
        std::ifstream fe3 ("Source/PluginEditor.cpp");
        std::stringstream se3; se3 << fe3.rdbuf();
        const juce::String esrc3 (se3.str());
        check (esrc3.contains ("heard through this channel's own chain"),
               "banner wording: through-main present");
        check (esrc3.contains ("replacing this channel's output"),
               "banner wording: replace-after present");
        check (esrc3.contains ("toggleBorrowRoute"),
               "the visible override is wired");
    }

    // =======================================================================
    std::printf ("== desktop-window watchdog: both surfaces parented ==\n");
    {
        // A desktop-parented CallOutBox/PopupMenu inside the AU hosting XPC
        // process is dismissed ~200ms in by JUCE's foreground watchdog
        // (juce_CallOutBox.cpp CallOutBoxCallback::timerCallback). The two
        // fixed surfaces must stay EMBEDDED, pinned by source.
        auto slurp = [] (const char* p)
        { std::ifstream f (p); std::stringstream s; s << f.rdbuf(); return juce::String (s.str()); };
        const auto picker = slurp ("Source/ChainPluginPicker.h");
        check (picker.contains ("&parent);")
                 && ! picker.contains ("target.getScreenBounds(), nullptr)"),
               "the plugin picker's CallOutBox is parented, never desktop");
        const auto ed = slurp ("Source/PluginEditor.cpp");
        check (ed.contains ("ChainPluginPicker::show(chainListPanel.addBlock, *this"),
               "the main editor passes itself as the picker's parent");
        check (slurp ("Source/LinkEditor.cpp")
                   .contains ("ChainPluginPicker::show(chainPanel.addBlock, *this"),
               "the Link editor passes itself as the picker's parent");
        const int rackMenu = ed.indexOf ("withTargetComponent(chainListPanel.rackBtn)");
        check (rackMenu >= 0
                 && ed.substring (rackMenu, rackMenu + 200).contains ("withParentComponent(this)"),
               "the rack selector menu is parented to the editor");

        // THE FULL SWEEP (22 Aug 2026): every showMenuAsync in the plugin's
        // UI must be parented — the watchdog doesn't care which menu it is.
        // `showMenuAsync(opts` is allowed because the shared opts variable is
        // itself pinned parented below. Same for CallOutBoxes: no desktop
        // (nullptr-parent) launch anywhere.
        int unparented = 0;
        for (const char* path : { "Source/PluginEditor.cpp", "Source/LinkEditor.cpp",
                                  "Source/PluginEditor.h", "Source/LinkEditor.h",
                                  "Source/ChainPluginPicker.h" })
        {
            const auto s = slurp (path);
            for (int p = 0; (p = s.indexOf (p, "showMenuAsync")) >= 0; ++p)
            {
                const auto seg = s.substring (p, p + 320);
                if (! seg.contains ("withParentComponent")
                    && ! seg.startsWith ("showMenuAsync(opts"))
                    ++unparented;
            }
            for (int p = 0; (p = s.indexOf (p, "launchAsynchronously")) >= 0; ++p)
                if (s.substring (p, p + 320).upToFirstOccurrenceOf (");", false, false)
                      .contains ("nullptr"))
                    ++unparented;
        }
        check (unparented == 0,
               "EVERY menu and CallOutBox is parented (full sweep)",
               juce::String (unparented) + " unparented");
        check (ed.contains ("withTargetComponent(&intakeMoreBtn).withParentComponent(this)"),
               "the shared opts variable is parented at its definition");
    }

    // =======================================================================
    std::printf ("== step 3: Apply's per-slot filter — the Link untouched ==\n");
    {
        using BC = LinkShm::BorrowCommit;
        check (BC::classify (false, true)  == BC::Action::Commit,
               "edited + clean: COMMIT");
        check (BC::classify (true,  true)  == BC::Action::LeaveWithheld,
               "edited + WITHHELD: never written (the hard rule beats the edit)");
        check (BC::classify (false, false) == BC::Action::LeaveUnedited,
               "unedited + clean: untouched");
        check (BC::classify (true,  false) == BC::Action::LeaveWithheld,
               "unedited + withheld: untouched");
        // The 4-combo rack: exactly ONE commit, and it is the edited+clean
        // slot — the writer-level proof that untouched slots stay untouched
        // (the Link's state changes only through a commit payload).
        const auto plan = BC::plan ({ {false, true}, {true, true},
                                      {false, false}, {true, false} });
        int commits = 0;
        for (auto a : plan.actions) if (a == BC::Action::Commit) ++commits;
        check (commits == 1 && plan.actions[0] == BC::Action::Commit,
               "4-combo rack: exactly one payload, the edited+clean slot");
        check (plan.changed == 2 && plan.committing == 1
                 && plan.withheldEdited == 1 && plan.untouched == 2,
               "the asymmetry counts: changed 2, committing 1, "
               "withheld-edited 1, untouched 2");

        // THE ZERO-COMMITS ROUND'S GATE GAP, closed (22 Aug 2026): the
        // synthetic rack used DECIMAL uid strings; the real sidecar
        // publishes HEX (getSlotIdentity's toHexString), and feeding hex
        // into stateFitsPlugin's decimal compare withheld every known-uid
        // slot — seed and Apply alike. The converter is the one seam.
        {
            const juce::String hexUid = juce::String::toHexString (0x454A4250);
            check (LinkShm::sidecarUidToStateUid (hexUid)
                       == juce::String (0x454A4250),
                   "sidecar HEX uid converts to the state policy's decimal");
            check (LinkShm::sidecarUidToStateUid ({}).isEmpty(),
                   "empty uid stays empty (no opinion, not zero)");
            // The REAL-shaped seed: identity as the sidecar publishes it,
            // through the converter — must seed. The raw hex form must
            // withhold, documenting exactly why the converter exists.
            // Distinct sentinel per run: a pooled instance REMEMBERS its
            // last value, so reusing one sentinel cannot tell "seeded now"
            // from "remembered from the pool".
            auto seedWith = [&] (const juce::String& uidField, int sentinel) {
                host.releaseBorrowToPool();
                juce::Array<juce::var> arr;
                auto* o = new juce::DynamicObject();
                o->setProperty ("n", 1);
                o->setProperty ("plugin", "EJ Borrow Probe");
                o->setProperty ("bypassed", false);
                o->setProperty ("format", kEchoJayBuiltinFormat);
                o->setProperty ("uid", uidField);
                arr.add (juce::var (o));
                auto* so = new juce::DynamicObject();
                so->setProperty ("1", chunkFor (sentinel));
                host.restoreSavedChain (juce::var (arr), juce::var (so));
                auto* pp = probeIn (host, 0);
                return pp != nullptr && pp->value == sentinel;
            };
            check (seedWith (LinkShm::sidecarUidToStateUid (hexUid), 777),
                   "REAL-shaped identity (converted hex uid) SEEDS the state");
            check (host.borrowSlotSeededWithState (0),
                   "and the seed FACT is recorded on the slot");
            check (! seedWith (hexUid, 888),
                   "raw hex uid withholds (the old bug, pinned as the reason)");
            check (! host.borrowSlotSeededWithState (0),
                   "an unseeded slot's FACT reads false — the verdict Apply reads");
            // RESET-BEFORE-SEED (22 Aug 2026): the reused instance carried
            // 777 from the previous borrow; with the seed withheld it must
            // hold DEFAULTS — the previous rack's settings wearing this
            // rack's name is worse than defaults, because it sounds like a
            // working chain.
            {
                auto* pp = probeIn (host, 0);
                check (pp != nullptr && pp->value == kProbeValueDefault,
                       "a reused-then-unseeded slot holds DEFAULTS, never the "
                       "prior borrow's state",
                       pp != nullptr ? juce::String (pp->value) : juce::String ("null"));
            }
        }

        // THE RESET-REFUSER ARM: a plugin whose state machinery breaks on
        // reuse fails the pool RESET, is retired + marked ineligible, and
        // every later borrow instantiates fresh — never reused dirty.
        {
            auto sulkRack = [&] (int v) {
                host.releaseBorrowToPool();
                juce::Array<juce::var> arr;
                auto* o = new juce::DynamicObject();
                o->setProperty ("n", 1);
                o->setProperty ("plugin", "EJ Borrow Sulk");
                o->setProperty ("bypassed", false);
                arr.add (juce::var (o));
                auto* so = new juce::DynamicObject();
                so->setProperty ("1", chunkFor (v));
                host.restoreSavedChain (juce::var (arr), juce::var (so));
            };
            const int freshBefore = host.borrowFreshInstantiations();
            sulkRack (11);                       // fresh, first set: seeds fine
            sulkRack (22);                       // reuse -> RESET throws -> retire+fresh
            check (host.borrowFreshInstantiations() == freshBefore + 2,
                   "reset-refuser: retired and instantiated FRESH on re-borrow",
                   juce::String (host.borrowFreshInstantiations() - freshBefore));
            juce::PluginDescription sd;
            sd.pluginFormatName = kEchoJayBuiltinFormat;
            sd.fileOrIdentifier = "echojay:test:borrowsulk";
            sd.uniqueId         = 0x454A4253;
            check (host.isBorrowPoolIneligible (sd),
                   "reset-refuser marked ineligible (no retire dance every cycle)");
            auto* sp = dynamic_cast<BorrowSulk*> (host.getSlotProcessor (0));
            check (sp != nullptr && sp->value == 22,
                   "the fresh instance seeded correctly — never the dirty reuse");
        }

        // The Apply-time withheld verdict is the SAME policy, QUIET: no
        // state note from a verdict read.
        host.clearStateNotes();
        const int notesBefore = host.getStateNotes().size();
        check (host.borrowSlotWithheld (0, "VST3", "9.9", "12345"),
               "mismatching triplet reads withheld (same stateFitsPlugin policy)");
        check (! host.borrowSlotWithheld (0, {}, {}, {}),
               "absent triplet is no opinion, not withheld");
        check (host.getStateNotes().size() == notesBefore,
               "verdict reads write NO user-facing notes");

        // Source pins: the committer sends ONLY Commit-classified slots; the
        // ask states the asymmetry; discard clears the kept edits; every
        // keep-release captures them; apply_ rides the replace supersede
        // class (never an unscoped prefix).
        auto slurp2 = [] (const char* p)
        { std::ifstream f (p); std::stringstream s; s << f.rdbuf(); return juce::String (s.str()); };
        const auto ed3 = slurp2 ("Source/PluginEditor.cpp");
        check (ed3.contains ("!= LinkShm::BorrowCommit::Action::Commit)\n            continue;"),
               "runBorrowApply sends ONLY Commit-classified slots (by source)");
        check (ed3.contains ("will NEVER be written over them"),
               "the ask names the withheld-edited slots as never-written");
        check (ed3.contains ("\"You changed \"") && ed3.contains ("stay")
                 && ed3.contains ("untouched.\""),
               "the ask states changed / committing / untouched in words");
        check (ed3.contains ("{ \"recall_\", \"build_\", \"apply_\" }"),
               "apply_ is in the replace-ask supersede class");
        // onCreateEditor ORDER (bug, 22 Aug 2026): the borrowed-host branch
        // must come BEFORE the remote guard, or it is dead code behind the
        // guard's early nullptr — exactly what a source pin catches and a
        // functional test might not. Pin: within the handler, the borrow
        // check's offset precedes the remote guard's.
        {
            const int handler = ed3.indexOf ("chainListPanel.onCreateEditor");
            const auto body = handler >= 0 ? ed3.substring (handler, handler + 1400)
                                           : juce::String();
            const int borrowArm = body.indexOf ("borrowHostIfActiveFor(chainViewUid())");
            const int remoteArm = body.indexOf ("chainViewUid().isNotEmpty()");
            check (handler >= 0 && borrowArm >= 0 && remoteArm >= 0
                     && borrowArm < remoteArm,
                   "onCreateEditor reaches the borrowed host BEFORE the remote guard");
        }
        // The verdict reads the RECORDED FACT, never a recomputed policy,
        // the converter guards the seed seam, and every classify logs.
        check (ed3.contains ("bh->borrowSlotSeededWithState(i)")
                 && ! ed3.contains ("borrowSlotWithheld(\n            i,"),
               "Apply's verdict reads the seed fact (by source)");
        check (ed3.contains ("LinkShm::sidecarUidToStateUid(s.uid)"),
               "the seed converts the sidecar's hex uid (by source)");
        check (ed3.contains ("EJApply: slot "),
               "one diagnostic line per slot at classify time (by source)");
        const auto pr3 = slurp2 ("Source/PluginProcessor.cpp");
        check (pr3.contains ("if (keepEdits) captureBorrowKept();"),
               "keep-releases capture the edits (continuous keep)");
        check (pr3.contains ("borrowRelease(true);   // a lease death is never an implicit discard"),
               "lease death keeps, never discards");
    }

    // =======================================================================
    std::printf ("== rack mix on the borrowed host ==\n");
    {
        // Functional half of finding #1's gate: the borrowed host reports
        // the adopted master wet, keeps it across a release/re-borrow, and
        // its own pre-gain stays neutral (the stream carries the Link's).
        host.setMasterWet (0.7f);
        check (std::abs (host.getMasterWet() - 0.7f) < 1.0e-6f,
               "borrowed host reports the adopted master wet",
               juce::String (host.getMasterWet(), 3));
        check (std::abs (host.getPreGainDb()) < 1.0e-6f,
               "borrowed host pre-gain stays neutral (never adopted)",
               juce::String (host.getPreGainDb(), 3));
        host.releaseBorrowToPool();
        borrowRack (host);
        check (std::abs (host.getMasterWet() - 0.7f) < 1.0e-6f,
               "master wet survives release/re-borrow until re-adopted");
    }

    // =======================================================================
    std::printf ("== latency hard block ==\n");
    check (host.hostReportableLatencySamples() == -1,
           "Borrowed host REFUSES the latency mirror (-1)");
    check (primary.hostReportableLatencySamples() >= 0,
           "Primary host answers normally",
           juce::String (primary.hostReportableLatencySamples()));
    // NO NEGATIVE LATENCY CAN REACH setLatencySamples: the value domain is
    // exactly {-1} ∪ [0, ∞) — asserted above for both modes — and the ONE
    // mirror site is gated on >= 0, asserted here BY SOURCE (the mapfps
    // precedent) so a future edit cannot drop the gate silently.
    {
        std::ifstream fp ("Source/PluginProcessor.cpp");
        std::stringstream sp; sp << fp.rdbuf();
        const juce::String src (sp.str());
        const int site = src.indexOf ("hostReportableLatencySamples(); lat >= 0)");
        check (site >= 0,
               "the setLatencySamples mirror site is gated on lat >= 0 (by source)");
        check (! src.contains ("setLatencySamples(chainHost.getTotalLatencySamples())"),
               "no ungated getTotalLatencySamples mirror remains (by source)");
    }

    // =======================================================================
    std::printf ("== step 2: no write-back to the Link, pinned by source ==\n");
    {
        // The borrow paths must contain NO commit vocabulary: step 3 owns
        // Apply, and step 2's contract is that nothing writes state back.
        // Region-located first — a check that greps an absent region passes
        // on nothing, which is the silent-no-op failure mode.
        auto sourceOf = [] (const char* path)
        {
            std::ifstream f (path); std::stringstream s; s << f.rdbuf();
            return juce::String (s.str());
        };
        auto region = [] (const juce::String& src, const char* fromMark,
                          const char* toMark) -> juce::String
        {
            const int a = src.indexOf (fromMark);
            const int b = a >= 0 ? src.indexOf (a, toMark) : -1;
            return (a >= 0 && b > a) ? src.substring (a, b) : juce::String();
        };
        const auto procRegion = region (sourceOf ("Source/PluginProcessor.cpp"),
            "Whole-rack borrow, step 2", "Rack lock \xe2\x80\x94 the main side");
        const auto edRegion = region (sourceOf ("Source/PluginEditor.cpp"),
            "Whole-rack borrow, step 2", "sendBlockEdit");
        check (procRegion.isNotEmpty(), "processor borrow region located");
        check (edRegion.isNotEmpty(),   "editor borrow region located");
        // Step 3 upgraded this contract: the PROCESSOR borrow code still
        // never commits, and the EDITOR's commit vocabulary is confined to
        // runBorrowApply — the one filtered committer. Outside it, the
        // borrow region stays commit-free, so engage/release/pull can never
        // grow a write-back path.
        const int rba    = edRegion.indexOf ("void EchoJayEditor::runBorrowApply");
        const int rbaEnd = rba >= 0 ? edRegion.indexOf (rba, "\n}\n") : -1;
        check (rba >= 0 && rbaEnd > rba, "runBorrowApply located inside the borrow region");
        const auto edOutside = edRegion.substring (0, juce::jmax (0, rba))
                             + edRegion.substring (juce::jmax (0, rbaEnd));
        for (const char* key : { "commitState", "commitSlot", "editOps" })
        {
            check (! procRegion.contains (key),
                   juce::String ("processor borrow code has no \"") + key + "\"");
            check (! edOutside.contains (key),
                   juce::String ("editor borrow code outside runBorrowApply has no \"") + key + "\"");
        }
        // THE RACK'S MIX CARRIES (hands-on finding #1): the borrow adopts the
        // sidecar's masterWet (inaudible on a bypassed rack's stream, so it
        // must be applied here) and deliberately does NOT adopt preGainDb —
        // the ring is written after the Link's ChainHost::process, so the
        // dry stream already carries pre-gain and adopting it would double
        // it. Both pinned by source so neither can silently flip.
        check (edRegion.contains ("setMasterWet(st->masterWet)"),
               "editor borrow ADOPTS the rack's master wet");
        check (! edRegion.contains ("setPreGainDb"),
               "editor borrow does NOT adopt pre-gain (the stream carries it)");
        check (edRegion.contains ("setSlotSettings"),
               "editor borrow carries the per-slot guidance text");
        // And the only Link-bound writes are the lease + the pull command.
        check (edRegion.contains ("pullSlotState")
                 && ! edRegion.contains ("replaceWithText(juce::JSON::toString(juce::var(cmd), true));\n    juce::File(dir + \"ctrl-cmd"),
               "editor borrow writes only the pull command to ctrl-cmd");
        check (procRegion.contains ("leasePath"),
               "processor borrow writes only the lease file");
    }

    // =======================================================================
    std::printf ("== parked-node cost: prepareToPlay, empty pool vs 10 parked ==\n");
    {
        // A fresh borrowed host so the counters above stay untouched.
        ChainHost timing (ChainHost::Mode::Borrowed);
        timing.prepare (48000.0, 512);

        auto prepMs = [&timing] (double sr)
        {
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            timing.prepare (sr, 512);
            return juce::Time::getMillisecondCounterHiRes() - t0;
        };
        const double emptyMs = prepMs (44100.0);

        // Park ten: a ten-slot probe rack, borrowed fresh then released.
        {
            juce::Array<juce::var> arr;
            for (int n = 1; n <= 10; ++n)
            {
                auto* o = new juce::DynamicObject();
                o->setProperty ("n", n);
                o->setProperty ("plugin", "EJ Borrow Probe");
                o->setProperty ("bypassed", false);
                arr.add (juce::var (o));
            }
            timing.restoreSavedChain (juce::var (arr), juce::var());
            timing.releaseBorrowToPool();
        }
        check (timing.borrowPoolCount() == 10, "ten instances parked",
               juce::String (timing.borrowPoolCount()));

        const double tenSr1 = prepMs (48000.0);   // sample-rate change, full pool
        const double tenSr2 = prepMs (44100.0);   // and back
        std::printf ("  prepareToPlay: empty pool %.3f ms | 10 parked %.3f ms / %.3f ms\n",
                     emptyMs, tenSr1, tenSr2);
        // The render sequence includes every node — suspension is checked at
        // RENDER time (juce_AudioProcessorGraph.cpp:887) — so rebuild cost
        // grows with pool size by construction. The gate is the stall guard:
        // a sample-rate change with a full pool must stay far from audible.
        check (tenSr1 < 250.0 && tenSr2 < 250.0,
               "sample-rate change with a full pool does not stall (< 250 ms)",
               juce::String (juce::jmax (tenSr1, tenSr2), 3) + " ms");
    }

    // =======================================================================
    std::printf ("== shared files byte-identical after the whole cycle ==\n");
    {
        const auto after = snapshotDir (shared);
        bool identical = before.size() == after.size();
        juce::String diff;
        for (const auto& [rel, bytes] : before)
        {
            auto it = after.find (rel);
            if (it == after.end()) { identical = false; diff = rel + " deleted"; break; }
            if (it->second != bytes) { identical = false; diff = rel + " changed"; break; }
        }
        if (identical)
            for (const auto& [rel, bytes] : after)
                if (before.find (rel) == before.end())
                { identical = false; diff = rel + " created"; break; }
        check (identical, "every shared file byte-identical (borrowed wrote nothing)", diff);
    }

    std::printf ("== structure plan applier, phase 2 (the five held gates) ==\n");
    {
        using namespace LinkShm::StructureEdit;
        const juce::String planDir = juce::File (appData).getParentDirectory()
                                         .getChildFile ("plansandbox")
                                         .getFullPathName() + "/";
        juce::File (planDir).createDirectory();

        // A Primary rack: [probe(seeded 41), grinch(seeded 42)] — grinch's
        // poison is 666, these are fine.
        auto buildRack = [&] (ChainHost& h) {
            for (int i = h.getNumSlots() - 1; i >= 0; --i) h.removeSlot (i);
            juce::Array<juce::var> arr;
            int n = 1;
            for (const char* nm : { "EJ Borrow Probe", "EJ Borrow Grinch" })
            {
                auto* o = new juce::DynamicObject();
                o->setProperty ("n", n++);
                o->setProperty ("plugin", nm);
                o->setProperty ("bypassed", false);
                arr.add (juce::var (o));
            }
            auto* so = new juce::DynamicObject();
            so->setProperty ("1", chunkFor (41));
            so->setProperty ("2", chunkFor (42));
            h.restoreSavedChain (juce::var (arr), juce::var (so));
        };
        buildRack (primary);
        const auto snapId = [&] (ChainHost& h) {
            juce::String s;
            for (const auto& li : h.liveIdentity()) s << li.name << "|" << li.uid << ";";
            for (int i = 0; i < h.getNumSlots(); ++i)
            {
                juce::MemoryBlock mb;
                if (auto* p = h.getSlotProcessor (i))
                    try { p->getStateInformation (mb); } catch (...) {}
                s << juce::String ((juce::int64) mb.getSize()) << ","
                  << juce::String (mb.getSize() ? (int) *(const int*) mb.getData() : -1) << ";";
            }
            return s;
        };
        const auto base = primary.liveIdentity();

        // GATE 1: Phase A stages first; an abort leaves the rack
        // byte-identical with per-slot named reasons.
        {
            const auto before = snapId (primary);
            std::vector<CurrentSlot> cur = {
                { base[0], 0, false, false, {} },   // survivor
                                                    // slot 1 removed
                { { "No Such Plugin 9000", {}, {} }, -1, false, false, "seed" },
            };
            auto plan = computePlan ("uid-p2", base, cur);
            const auto r = primary.applyStructurePlan (planDir, plan);
            check (! r.ok && r.failedAt == "stage",
                   "Phase A abort: failed at stage, before any mutation");
            check (r.reasons.size() == 1 && r.reasons[0].contains ("No Such Plugin 9000"),
                   "the reason names the plugin", r.reasons.joinIntoString ("; "));
            check (snapId (primary) == before,
                   "the rack is BYTE-IDENTICAL after the abort (zero mutations)");
            check (! juce::File (journalPath (planDir, "uid-p2")).existsAsFile(),
                   "no journal was even written — the abort precedes it");
        }

        // GATE 4 (+ rollback): a removal parks RE-ATTACHABLY — rollback
        // un-removes the SAME instance, proven by pointer identity.
        {
            auto* probeBefore = primary.getSlotProcessor (0);
            std::vector<CurrentSlot> cur = {
                { base[1], 1, /*edited*/ true, false, "%%%not-base64%%%" },
            };  // removes probe(0), then commits garbage to grinch -> mid-B fail
            auto plan = computePlan ("uid-p2", base, cur);
            const auto r = primary.applyStructurePlan (planDir, plan);
            check (! r.ok && r.restored,
                   "mid-mutate failure ROLLED BACK", r.failedAt);
            check (primary.getNumSlots() == 2
                     && primary.getSlotProcessor (0) == probeBefore,
                   "the removed slot came back as the SAME instance (pointer-"
                   "identical): parked re-attachably, zero new");
            check (! juce::File (journalPath (planDir, "uid-p2")).existsAsFile(),
                   "the journal was consumed by the rollback");
            auto* pp = probeIn (primary, 0);
            check (pp != nullptr && pp->value == 41,
                   "and its state survived the round trip");
        }

        // GATE 5: a retried Apply's Phase A instantiates ZERO new — the
        // staging park keys by identity.
        {
            std::vector<CurrentSlot> cur = {
                { base[0], 0, false, false, {} },
                { base[1], 1, false, false, {} },
                { { "EJ Borrow Sulk", juce::String (0x454A4253), {} },
                  -1, false, false, chunkFor (7) },
                { { "No Such Plugin 9000", {}, {} }, -1, false, false, {} },
            };
            auto plan = computePlan ("uid-p2", base, cur);
            const int fresh0 = primary.planFreshInstantiations();
            (void) primary.applyStructurePlan (planDir, plan);   // aborts at stage
            const int fresh1 = primary.planFreshInstantiations();
            (void) primary.applyStructurePlan (planDir, plan);   // retry
            const int fresh2 = primary.planFreshInstantiations();
            check (fresh1 == fresh0 + 1 && fresh2 == fresh1,
                   "retry instantiated ZERO new (sulk staged once, reused)",
                   juce::String (fresh1 - fresh0) + " then "
                     + juce::String (fresh2 - fresh1));
        }

        // Success path: remove + create + commit applies whole; journal gone.
        {
            std::vector<CurrentSlot> cur = {
                { base[1], 1, true, false, chunkFor (52) },     // grinch edited
                { { "EJ Borrow Sulk", juce::String (0x454A4253), {} },
                  -1, false, false, chunkFor (7) },             // staged already
            };
            auto plan = computePlan ("uid-p2", base, cur);
            const auto r = primary.applyStructurePlan (planDir, plan);
            check (r.ok, "a full plan (remove+create+commit) applies whole",
                   r.failedAt + " " + r.reasons.joinIntoString ("; "));
            check (primary.getNumSlots() == 2
                     && primary.getSlotInfo (1).name == "EJ Borrow Sulk",
                   "the shape is the plan's shape");
            check (! juce::File (journalPath (planDir, "uid-p2")).existsAsFile(),
                   "the journal is deleted on completion");
        }

        // GATE 3: journal restore — after-session ordering is the caller's
        // contract (LinkProcessor's first quiet tick); at-most-once and
        // divergence-wins-with-a-note are proven here.
        {
            const auto preShape  = primary.liveIdentity();
            auto pre = primary.planCapturePreImages();
            Plan jp; jp.uid = "uid-p2"; jp.baseIdentity = preShape;
            writeJournal (planDir, jp, pre);
            // Mangle the rack (the divergent "session restore" outcome).
            buildRack (primary);
            primary.clearStateNotes();
            check (primary.planJournalRestoreIfPresent (planDir, "uid-p2"),
                   "an uncompleted journal restores on the post-settle check");
            check (verifyBaseIdentity (primary.liveIdentity(), preShape)
                       == BaseCheck::Match,
                   "THE JOURNAL WON: the rack is the pre-image shape, not the "
                   "session's");
            bool noted = false;
            for (auto& nte : primary.getStateNotes())
                if (nte.contains ("different shape")) noted = true;
            check (noted, "the divergence note names both truths");
            check (! primary.planJournalRestoreIfPresent (planDir, "uid-p2"),
                   "a second check is a no-op: restore runs AT MOST ONCE");
        }

        // GATE 2, by source: no rollback/refusal path releases the borrow.
        {
            std::ifstream f ("Source/PluginEditor.cpp");
            std::stringstream ss; ss << f.rdbuf();
            const juce::String src (ss.str());
            const int fn  = src.indexOf ("void EchoJayEditor::runStructureApply");
            const int end = fn >= 0 ? src.indexOf (fn, "\nvoid EchoJayEditor::sendBlockEdit") : -1;
            const auto body = (fn >= 0 && end > fn) ? src.substring (fn, end)
                                                    : juce::String();
            check (body.isNotEmpty(), "runStructureApply located");
            const int successRelease = body.indexOf ("borrowRelease(false)");
            check (successRelease >= 0
                     && body.indexOf (successRelease + 1, "borrowRelease") < 0,
                   "borrowRelease appears ONCE, on the success path only — "
                   "rollback and refusal keep the session live");
            check (body.contains ("Your session is still live"),
                   "every failure banner says the session is live");
            check (body.contains ("structureEditCapable"),
                   "the capability gate precedes the send");
        }
    }

    std::printf ("== DEV forceWithholdSlot: cannot exist in a non-DEV build ==\n");
    {
        // This suite compiles and links against build/ (DEV OFF) — the
        // right vantage to prove the hook's absence. A debug affordance
        // that can reach a release build is a worse bug than the one it
        // tests.
#if defined (ECHOJAY_DEV_TRANSPORT) && ECHOJAY_DEV_TRANSPORT
        check (false, "this suite must build with ECHOJAY_DEV_TRANSPORT OFF "
                      "for the cannot-exist proof to mean anything");
#else
        check (true, "suite built with DEV transport OFF (the proof vantage)");
#endif
        // BINARY: the OFF-built SharedCode lib carries no trace of the hook.
        {
            std::ifstream lib ("build/EchoJay_artefacts/Release/libEchoJay V2_SharedCode.a",
                               std::ios::binary);
            check (lib.good(), "OFF-built SharedCode lib found");
            const std::string needle = "forceWithholdSlot";
            std::vector<char> buf (1 << 20);
            std::string carry;
            bool found = false;
            while (lib.good() && ! found)
            {
                lib.read (buf.data(), (std::streamsize) buf.size());
                const auto got = (size_t) lib.gcount();
                if (got == 0) break;
                std::string chunk = carry + std::string (buf.data(), got);
                if (chunk.find (needle) != std::string::npos) found = true;
                carry = chunk.size() >= needle.size()
                          ? chunk.substr (chunk.size() - needle.size() + 1) : chunk;
            }
            check (! found, "the OFF binary contains ZERO trace of the hook");
        }
        // SOURCE: outside #if ECHOJAY_DEV_TRANSPORT regions, zero mentions.
        {
            std::ifstream f ("Source/ChainHost.cpp");
            std::stringstream s; s << f.rdbuf();
            juce::String src (s.str());
            juce::String outside;
            int pos = 0;
            for (;;)
            {
                const int a = src.indexOf (pos, "#if ECHOJAY_DEV_TRANSPORT");
                if (a < 0) { outside += src.substring (pos); break; }
                outside += src.substring (pos, a);
                const int b = src.indexOf (a, "#endif");
                if (b < 0) break;
                pos = b + 6;
            }
            check (! outside.contains ("forceWithholdSlot"),
                   "every source mention sits inside a DEV guard");
        }
        // FUNCTIONAL: a dev.json naming a slot is INERT here — the seed
        // applies and the fact records, because the hook does not exist.
        {
            const juce::File devDir = juce::File (appData).getParentDirectory()
                                          .getParentDirectory().getChildFile (".echojay");
            devDir.createDirectory();
            devDir.getChildFile ("dev.json")
                  .replaceWithText ("{\"forceWithholdSlot\": 1}");
            host.releaseBorrowToPool();
            juce::Array<juce::var> arr;
            auto* o = new juce::DynamicObject();
            o->setProperty ("n", 1);
            o->setProperty ("plugin", "EJ Borrow Probe");
            o->setProperty ("bypassed", false);
            arr.add (juce::var (o));
            auto* so = new juce::DynamicObject();
            so->setProperty ("1", chunkFor (555));
            host.restoreSavedChain (juce::var (arr), juce::var (so));
            auto* pp = probeIn (host, 0);
            check (pp != nullptr && pp->value == 555,
                   "with forceWithholdSlot=1 in dev.json, the OFF build seeds anyway");
            check (host.borrowSlotSeededWithState (0),
                   "and records the fact — the hook is genuinely absent, not dormant");
            devDir.getChildFile ("dev.json").deleteFile();
        }
    }

    std::printf ("== negative control ==\n");
    {
        const int beforeN = failures;
        check (false, "NEGATIVE CONTROL - this line is SUPPOSED to fail");
        check (failures == beforeN + 1, "the harness caught the planted failure");
        --failures;
    }

    std::printf ("\nborrowhost_test: %s\n", failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}
