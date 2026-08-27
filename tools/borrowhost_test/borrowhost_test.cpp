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
#include "PluginProcessor.h"   // §5a-R functional editor-open gate
#include "PluginEditor.h"      // M/S permanence gate drives the REAL tab path

// §8 alignment gate: bind/unbind the REAL ring consumer on the processor.
struct EchoJayAlignTestAccess
{
    static void connect (EchoJayProcessor& p, int i, const juce::String& key,
                         const juce::String& nm, float sr, const juce::String& uid)
    { p.connectLinkAudioSlot (i, key, nm, sr, uid); }
    static void disconnect (EchoJayProcessor& p, int i)
    { p.disconnectLinkAudioSlot (i); }
};

// The friend already declared in PluginEditor.h — the M/S permanence arm
// drives the REAL switchToTab (the single writer of panel visibility).
struct EchoJayTabStripTestAccess
{
    static void toChain (EchoJayEditor& e)
    { e.switchToTab (EchoJayEditor::Tab::Chain, true); }
    static juce::Component& panel (EchoJayEditor& e)
    { return e.chainListPanel; }
    static void toLink (EchoJayEditor& e)
    { e.switchToTab (EchoJayEditor::Tab::Link, true); }
    static void measure (EchoJayEditor& e) { e.measureLinkStrips(); }
    static juce::Component& mixer (EchoJayEditor& e)
    { return e.linkMixerView_; }
    static const EchoJayEditor::StripGeom* geomFor (EchoJayEditor& e,
                                                    const juce::String& addr)
    {
        for (const auto& sg : e.linkStripGeom_)
            if (sg.addr == addr) return &sg;
        return nullptr;
    }
    // New-chat arms (30 Aug 2026): drive the REAL button and read what the
    // binding actually drives.
    static void toChat (EchoJayEditor& e)
    { e.switchToTab (EchoJayEditor::Tab::Chat, true); }
    static int  tab (EchoJayEditor& e) { return (int) e.currentTab; }
    static void seedMsg (EchoJayEditor& e)
    {
        EchoJayEditor::ChatMsg m;
        m.role = "user"; m.content = "seed";
        e.chatMessages.push_back (m);
    }
    static juce::TextButton& newChatBtn (EchoJayEditor& e)
    { return e.headerNewChatBtn; }
    static juce::String activeChatUid (EchoJayEditor& e)
    {
        if (auto* c = e.workspace.findChatById (e.currentChatId))
            return c->linkUid;
        return "<no chat>";
    }
    static juce::String viewUid (EchoJayEditor& e) { return e.chainViewUid(); }
    static void clearActiveChat (EchoJayEditor& e) { e.currentChatId.clear(); }
    // The rack row's STORED rects (31 Aug centring): clicks and pixel
    // asserts read the same rects paint does.
    static juce::Rectangle<int> rackMRect (EchoJayEditor& e)
    { return e.chainListPanel.msLamps.mR; }
};
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
    juce::AudioProcessorEditor* createEditor() override
    { return new juce::GenericAudioProcessorEditor (*this); }
    bool hasEditor() const override { return true; }
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
        check (psrc3.contains ("if (borrowThrough)\n        applyBorrowSoloMixOn(buffer,"),
               "through-main site present (before chainHost.process)");
        check (psrc3.contains ("if (!borrowThrough)\n        applyBorrowSoloMixOn(buffer, fallbackSolo);"),
               "replace-after site present (after chainHost.process)");
        // The banner SAYS the mode, both wordings live in the editor.
        std::ifstream fe3 ("Source/PluginEditor.cpp");
        std::stringstream se3; se3 << fe3.rdbuf();
        const juce::String esrc3 (se3.str());
        // 27 Aug 2026: the route override RETIRED with LISTEN
        // (MUTE_SOLO_SPEC §6.4) — the route auto-detects, no user control,
        // no mode wording. Deleted, not dormant.
        check (! esrc3.contains ("HEARD: THROUGH THIS CHAIN")
                 && ! esrc3.contains ("HEARD: REPLACES OUTPUT"),
               "the route-mode wordings are gone");
        check (! esrc3.contains ("toggleBorrowRoute"),
               "the visible override is gone");
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
        // §5a-R: NO Apply confirm exists any more — the asks are gone from
        // the codebase, and the honesty (withheld never written) rides the
        // commit filter above plus the engage banner, not a confirm.
        check (! ed3.contains ("presentStructureApplyAsk")
                 && ! ed3.contains ("presentBorrowApplyAsk")
                 && ! ed3.contains ("apply_confirm"),
               "no Apply confirm exists anywhere (§5a-R)");
        check (ed3.contains ("running at defaults; never")
                 && ed3.contains ("written back"),
               "the withheld banner still says never-written, confirm or no");
        check (! ed3.contains ("revert_rack"),
               "no revert chips exist (revert removed by ruling)");
        // onCreateEditor ORDER (bug, 22 Aug 2026): the borrowed-host branch
        // must come BEFORE the remote guard, or it is dead code behind the
        // guard's early nullptr — exactly what a source pin catches and a
        // functional test might not. Pin: within the handler, the borrow
        // check's offset precedes the remote guard's.
        {
            // The decision moved to the processor and is FUNCTIONALLY gated
            // below (twice a source pin here proved a branch existed while
            // it was unreachable). This pin only holds the delegation.
            const int handler = ed3.indexOf ("chainListPanel.onCreateEditor");
            const auto body = handler >= 0 ? ed3.substring (handler, handler + 900)
                                           : juce::String();
            check (body.contains ("createSlotEditorForView(chainViewUid(), i)"),
                   "onCreateEditor delegates to the one gated author");
        }
        // The verdict reads the RECORDED FACT, never a recomputed policy,
        // the converter guards the seed seam, and every classify logs.
        const auto pp3 = slurp2 ("Source/PluginProcessor.cpp");
        check (pp3.contains ("bh->borrowSlotSeededWithState(i)"),
               "Apply's verdict reads the seed fact (by source)");
        check (ed3.contains ("LinkShm::sidecarUidToStateUid(s.uid)"),
               "the seed converts the sidecar's hex uid (by source)");
        check (pp3.contains ("EJApply: slot "),
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

        // GATE 2, by source (§5a-R): the orchestrator lives on the
        // PROCESSOR. Ruling 3: a failed DESELECT keeps the session (no
        // release without borrowApplyReleaseOnFail_); ruling 4: a failed
        // CLOSE releases anyway — no lock without a visible owner — with
        // the edits kept and the note recorded.
        {
            std::ifstream f ("Source/PluginProcessor.cpp");
            std::stringstream ss; ss << f.rdbuf();
            const juce::String src (ss.str());
            const int fn  = src.indexOf ("void EchoJayProcessor::borrowApplyFinish");
            const int end = fn >= 0 ? src.indexOf (fn, "\nvoid EchoJayProcessor::borrowEditorClosed") : -1;
            const auto body = (fn >= 0 && end > fn) ? src.substring (fn, end)
                                                    : juce::String();
            check (body.isNotEmpty(), "borrowApplyFinish located");
            check (body.contains ("borrowRelease(false)")
                     && body.contains ("if (borrowApplyReleaseOnFail_)")
                     && body.contains ("borrowRelease(true)"),
                   "success releases; failure releases ONLY under the close "
                   "policy (deselect keeps the session, ruling 3)");
            check (body.contains ("Your session is still live"),
                   "the deselect-failure banner says the session is live");
            check (body.contains ("unwrittenEditNote_[uid]")
                     && body.contains ("close-apply FAILED"),
                   "the close-failure records the note and LOGS regardless "
                   "of any window (ruling 4)");
            const auto ed2 = [&]{ std::ifstream f2 ("Source/PluginEditor.cpp");
                std::stringstream s2; s2 << f2.rdbuf(); return juce::String (s2.str()); }();
            check (ed2.contains ("if (p.borrowStructureCapable_)")
                     && ed2.contains ("p.borrowApplyAndRelease(/*releaseLockOnFail=*/false)"),
                   "the capability gate precedes the send (deselect fork)");
        }
    }

    std::printf ("== phase 3: affordances and words, pinned by source ==\n");
    {
        auto slurp3 = [] (const char* p)
        { std::ifstream f (p); std::stringstream s; s << f.rdbuf(); return juce::String (s.str()); };
        const auto ed = slurp3 ("Source/PluginEditor.cpp");
        // CAPABILITY gates every affordance, and the old wording survives in
        // the incapable arm — an older Link keeps settings-only behaviour.
        int capForks = 0, oldWording = 0;
        for (int p = 0; (p = ed.indexOf (p, "borrowStructureCapable_")) >= 0; ++p)
            ++capForks;
        for (int p = 0; (p = ed.indexOf (p, "An edited rack keeps its shape")) >= 0; ++p)
            ++oldWording;
        // 4 = the add, remove and move forks plus the deselect routing;
        // the SNAPSHOT itself moved into borrowEngageBegin (26 Aug: it must
        // ride the session from its first instant, functionally gated).
        check (capForks >= 4,
               "capability forks the affordances and the deselect send",
               juce::String (capForks));
        // THREE sites — remove, add AND move. Move's refusal was silent
        // before phase 3; silent-does-nothing is the selector-bug failure
        // mode, so an incapable Link's move now says why, same words.
        check (oldWording >= 3,
               "the settings-only wording survives for older Links "
               "(remove, add, move — never silent)",
               juce::String (oldWording));
        // EVERY op appears in the confirm, by name, with the withheld-removal
        // line the spec demands.
        auto bodyOf = [&ed] (const char* sig)
        {
            const int at = ed.indexOf (sig);
            if (at < 0) return juce::String();
            const int end = ed.indexOf (at, "\n}");
            return ed.substring (at, end > at ? end : at + 6000);
        };
        const auto pp = slurp3 ("Source/PluginProcessor.cpp");
        // §5a-R: no confirm — the atomicity promise now rides the applied
        // sticky banner, and the removed-withheld honesty was recorded at
        // removal time (the memory pins below still hold).
        check (pp.contains ("the whole plan, or "),
               "the applied banner states the atomicity promise");
        // The commit vehicle routes by capability at the DESELECT fork.
        check (ed.contains ("p.borrowApplyAndRelease(/*releaseLockOnFail=*/false)")
                 && ed.contains ("runBorrowApply();   // legacy"),
               "deselect routes: capable -> plan, older -> per-slot commits");
        // The Link overlay gains the restructuring line while a journal is
        // active.
        check (slurp3 ("Source/LinkEditor.h").contains ("is restructuring this rack"),
               "the Link overlay states the restructure while a plan runs");
        check (slurp3 ("Source/LinkProcessor.h").contains ("structPlanJournalPresent"),
               "driven by journal presence, not a guess");

        // ---- The lost-add class (23 Aug 2026): one computation, loud on
        // ---- disagreement, and a record for every slot.
        // ONE COMPUTATION: buildStructurePlan has exactly ONE call site (the
        // ask, in toggleBorrow); Apply sends the CONFIRMED pendingStructPlan_
        // and never recomputes. Two computations at two times, with session
        // state mutating between, is how the ask said adds=1 and the send
        // computed empty.
        // §5a-R keeps ONE COMPUTATION by construction: the orchestrator
        // computes the plan at the moment of deselect/close and sends that
        // very object — there is no ask-to-send gap for session state to
        // mutate across.
        int planCalls = 0;
        for (int p = 0; (p = pp.indexOf (p, "= buildStructurePlan()")) >= 0; ++p)
            ++planCalls;
        check (planCalls == 1,
               "the plan is computed ONCE, at the deselect/close moment",
               juce::String (planCalls));
        // EMPTY PLAN, DIRTY SHAPE = a said-aloud defect that keeps the
        // session live (deselect) or records the note (close policy).
        check (pp.contains ("EJStruct: DEFECT empty plan")
                 && pp.contains ("borrowSessionShapeDirty()"),
               "an empty plan against a dirty shape refuses loudly");
        // A CREATED SLOT HAS A RECORD: exactly two record-push sites —
        // engage (pulled slots) and the borrowed add (created slots).
        int recPush = 0;
        for (int p = 0; (p = ed.indexOf (p, "borrowSlotRecords_.push_back")) >= 0; ++p)
            ++recPush;
        // 3 = engage (pulled slots), the picker add, and the chain-build
        // divert (26 Aug) — every path that creates a session slot records.
        check (recPush == 3,
               "every slot gets a record: engage, the add, and the build",
               juce::String (recPush));
        // And the verdict diagnostic covers created slots — a log that
        // skips slots is how the contradiction hid.
        check (pp.contains ("CREATED here - no pulled "),
               "the per-slot diagnostic logs created slots too");

        // ---- The silent-Apply class (24 Aug 2026): three answer=apply
        // ---- attempts with no log line and no UI change. Every path
        // ---- between the answer and the send now speaks in BOTH channels.
        auto bodyIn = [] (const juce::String& src, const char* sig)
        {
            const int at = src.indexOf (sig);
            if (at < 0) return juce::String();
            const int end = src.indexOf (at, "\n}");
            return src.substring (at, end > at ? end : at + 8000);
        };
        const auto rsa2 = bodyIn (pp, "void EchoJayProcessor::borrowApplyAndRelease");
        // The send logs BEFORE it writes — the line that separates "never
        // sent" from "sent and waiting".
        const int sendLog  = rsa2.indexOf ("EJStruct: send uid=");
        const int cmdWrite = rsa2.indexOf ("replaceWithText");
        check (sendLog >= 0 && cmdWrite > sendLog,
               "the send logs unconditionally BEFORE writing the command");
        // The write's result is checked and a failure is said aloud.
        check (rsa2.contains ("cmd write FAILED"),
               "a failed command write speaks instead of timing out mutely");
        // Every early return names itself in the log as well as the banner.
        check (rsa2.contains ("folder unavailable")
                 && rsa2.contains ("EJStruct: DEFECT empty plan"),
               "every pre-send return logs its reason");
        // Every terminal poll outcome speaks: applied, failed, timeout,
        // abandoned editor — and a stale ack is named while polling.
        check (rsa2.contains ("EJStruct: ack applied")
                 && rsa2.contains ("EJStruct: ack FAILED")
                 && rsa2.contains ("EJStruct: NO ACK")
                 && rsa2.contains ("ack poll abandoned")
                 && rsa2.contains ("EJStruct: stale ack"),
               "every ack-poll outcome logs, timeout included");
        // The Link says which side dropped a plan: receipt logged BEFORE
        // the parse, and both silent drop arms in the dispatcher speak.
        const auto lp = slurp3 ("Source/LinkProcessor.cpp");
        check (lp.contains ("] link: plan received"),
               "the Link logs plan receipt before parsing or applying");
        check (lp.contains ("REFUSED ctrl-cmd")
                 && lp.contains ("UNPARSEABLE ctrl-cmd"),
               "the ctrl dispatcher's drop arms speak, never silent");

        // ---- Consume and answer, always (24 Aug 2026 ruling): a refused
        // ---- ctrl-cmd is DELETED and ACKED with its reason — silence is
        // ---- never a valid response, and no refused file wedges the
        // ---- channel at 10Hz.
        check (lp.contains ("consumed and answered")
                 && lp.contains ("r->setProperty(\"refused\", why);")
                 && ! lp.contains ("file left on disk"),
               "every refusal consumes the file and answers with a reason");
        check (lp.contains ("duplicate seq, already applied")
                 && lp.contains ("seq 0 invalid")
                 && lp.contains ("unsupported"),
               "every refusal class is named: dup seq, zero seq, bad version");
        check (rsa2.contains ("EJStruct: ack REFUSED"),
               "the main names a transport refusal to the user and the log");
        // ---- Display parity (24 Aug 2026): the dispatcher and the journal
        // ---- restore route through the ONE four-step structural writer.
        // ---- These pins prove only the CALL-SITES — the effect (the
        // ---- editor-facing model actually moves) is proven functionally
        // ---- in linksync_test, because a call-exists pin passed while the
        // ---- two-models bug was live.
        {
            const int rcv = lp.indexOf ("] link: plan received");
            const auto planArm = rcv >= 0 ? lp.substring (rcv, rcv + 1400)
                                          : juce::String();
            check (planArm.contains ("applyStructurePlanAndSync"),
                   "the dispatcher applies through the syncing writer");
            const int jr = lp.indexOf ("planJournalRestoreIfPresent");
            const auto jrArm = jr >= 0 ? lp.substring (jr, jr + 700)
                                       : juce::String();
            check (jrArm.contains ("syncModelAfterStructuralChange"),
                   "the launch journal restore syncs the model too");
        }
        // ---- The picked plugin, not the substitute (24 Aug 2026: Apply
        // ---- failed at stage, four runs — the main swapped to an
        // ---- inline-hostable variant for its own hosting, liveIdentity()
        // ---- read the swap, and the Link was asked for a build it may
        // ---- not have). The Create identity comes from `picked`, period.
        {
            const int addAt = ed.indexOf ("CREATE class (spec §3)");
            const auto addArm = addAt >= 0 ? ed.substring (addAt, addAt + 1800)
                                           : juce::String();
            check (addArm.contains ("picked.name")
                     && addArm.contains ("juce::String(ChainHost::descUid(picked))")
                     && ! addArm.contains ("->liveIdentity()"),
                   "the Create identity is the PICKED plugin, never the "
                   "substitute - through the ONE uid idiom");
        }
        // ---- The stage-lookup diagnostic (25 Aug 2026: failedAt=stage
        // ---- names the plugin but not the LOOKUP): one line per non-
        // ---- builtin stage attempt — searched name, BOTH uid encodings
        // ---- plus deprecatedUid, the descriptor's (possibly EMPTY)
        // ---- format at createPluginInstance, and catalogue hit counts.
        {
            const auto ch = slurp3 ("Source/ChainHost.cpp");
            const int lk = ch.indexOf ("EJPlan: stage lookup");
            const auto lkArm = lk >= 0 ? ch.substring (lk, lk + 1400)
                                       : juce::String();
            check (lk >= 0
                     && lkArm.contains ("uid.dec=")
                     && lkArm.contains ("uid.hex=")
                     && lkArm.contains ("depUid.dec=")
                     && lkArm.contains ("nameMatches=")
                     && lkArm.contains ("NOT IN CATALOGUE")
                     && lkArm.contains ("resolved"),
                   "the stage lookup logs both uid encodings, the catalogue "
                   "hits and the resolution outcome");
        }
        // ---- Session-state-first banners (24 Aug 2026: the status line
        // ---- truncated before "Your session is still live", a working
        // ---- refusal read as a hang, Apply was pressed four times).
        check (pp.contains ("Your session is still live. The write to"),
               "the failure banner LEADS with the session state");
        {
            int tailStates = 0;   // any Apply banner still ENDING with it
            for (int p = 0; (p = rsa2.indexOf (p, "Your session is still live.\"")) >= 0; ++p)
                ++tailStates;
            check (tailStates == 0,
                   "no Apply banner buries the session state at the end",
                   juce::String (tailStates));
        }
        // SEQ IS COLLISION-PROOF, one author: no ctrl sender stamps a
        // seconds-resolution seq any more (the two remaining
        // currentTimeMillis()/1000 sites are the what's-new dismissal
        // timestamps, not seqs), and no hand-rolled baseSeq offsets remain.
        int secStamps = 0, seqCalls = 0;
        for (int p = 0; (p = ed.indexOf (p, "currentTimeMillis() / 1000")) >= 0; ++p)
            ++secStamps;
        for (int p = 0; (p = ed.indexOf (p, "LinkShm::nextCtrlSeq()")) >= 0; ++p)
            ++seqCalls;
        check (secStamps == 2 && seqCalls >= 12 && ! ed.contains ("baseSeq"),
               "one seq author: every sender uses nextCtrlSeq",
               juce::String (secStamps) + "/" + juce::String (seqCalls));
    }

    std::printf ("== §5a-R: selection is the session, pinned by source ==\n");
    {
        auto slurpS = [] (const char* p)
        { std::ifstream f (p); std::stringstream s; s << f.rdbuf(); return juce::String (s.str()); };
        const auto edS = slurpS ("Source/PluginEditor.cpp");
        const auto ppS = slurpS ("Source/PluginProcessor.cpp");
        // BOTH selection writers route the session transition — deselect
        // applies, select engages. A third writer that skips the hook is
        // the next silent-does-nothing.
        {
            const int a = edS.indexOf ("void EchoJayEditor::resetToMainContext");
            const int b = edS.indexOf ("void EchoJayEditor::openChannelByUid");
            check (a >= 0 && edS.substring (a, a + 700)
                       .contains ("handleBorrowSelectionChange({}, pendingSelectionIsUser_)"),
                   "resetToMainContext routes the deselect");
            check (b >= 0 && edS.substring (b, b + 700)
                       .contains ("handleBorrowSelectionChange(uid, pendingSelectionIsUser_)"),
                   "openChannelByUid routes the select");
        }
        // Ruling 4: the editor's destructor commits like a deselect, on the
        // PROCESSOR (it must outlive the window).
        {
            const int d = edS.indexOf ("EchoJayEditor::~EchoJayEditor");
            check (d >= 0 && edS.substring (d, d + 600)
                       .contains ("borrowEditorClosed()"),
                   "the destructor applies-then-releases via the processor");
        }
        // LISTEN is DELETED (27 Aug 2026, MUTE_SOLO_SPEC §1/§6.4): solo
        // subsumes it. Deleted, not dormant — pinned as absences of the
        // code forms (comments explaining the deletion are welcome).
        {
            check (! edS.contains ("borrowAudioOn()")
                     && ! edS.contains ("borrowAudioIsOn()"),
                   "the editor holds no LISTEN control calls");
            check (! edS.contains ("\"LISTENING\""),
                   "the LISTENING button state is gone");
            check (edS.contains ("sendLinkMuteSoloForView"),
                   "the under-rack M/S controls replaced it");
        }
        // Honesty surfaces are NOT losable by navigating away: the sticky
        // banner and the unwritten-edit note render from PROCESSOR state.
        check (edS.contains ("processorRef.borrowStickyBanner_.isNotEmpty()")
                 && edS.contains ("processorRef.unwrittenEditNote_.find(chainViewUid())"),
               "sticky banner and unwritten note render from processor state");
        // Revert is NOT part of the model (26 Aug ruling): deleted, not
        // dormant — the affordance, the chips and the machinery. Pinned as
        // ABSENCE across every source file the machinery lived in.
        {
            auto lpS = slurpS ("Source/LinkProcessor.cpp");
            auto chS = slurpS ("Source/ChainHost.h");
            check (! edS.contains ("revertLastApply")
                     && ! ppS.contains ("revertOfferUid_")
                     && ! lpS.contains ("revertLastApply")
                     && ! chS.contains ("revertLastApply"),
                   "revert is deleted everywhere, not dormant");
        }
        // A new session supersedes the previous session's surfaces.
        check (edS.contains ("p2.unwrittenEditNote_.erase(st->uid);"),
               "a new engage supersedes the old note, never navigation");
    }

    std::printf ("== §3f pin: selection contention, pure and by source ==\n");
    {
        using D = EchoJayProcessor::SelDecision;
        // All four arms of the pure decision.
        check (EchoJayProcessor::decideSelection (true,  true,  true)  == D::Nothing
                 && EchoJayProcessor::decideSelection (true,  true,  false) == D::Nothing,
               "same-uid selection is a no-op, whoever made it");
        check (EchoJayProcessor::decideSelection (true,  false, false) == D::ViewOnly,
               "a programmatic grab moves the VIEW only - never applies");
        check (EchoJayProcessor::decideSelection (true,  false, true)  == D::ApplyAndPend,
               "only a USER deselect commits");
        check (EchoJayProcessor::decideSelection (false, false, false) == D::ViewOnly
                 && EchoJayProcessor::decideSelection (false, false, true) == D::PendEngage,
               "without a session: user selections engage, programmatic never");
        // THE PING-PONG, directly: chat grabs and user snap-backs
        // alternating over a live session on A produce ZERO commits and
        // ZERO engages of B - the session never moves.
        int commits = 0, engagesOfB = 0;
        for (int i = 0; i < 10; ++i)
        {
            // chat activation snaps to B (programmatic)
            const auto d1 = EchoJayProcessor::decideSelection (true, false, false);
            if (d1 == D::ApplyAndPend) ++commits;
            if (d1 != D::ViewOnly && d1 != D::Nothing) ++engagesOfB;
            // the user snaps back to A (user, same uid as the session)
            const auto d2 = EchoJayProcessor::decideSelection (true, true, true);
            if (d2 == D::ApplyAndPend) ++commits;
        }
        check (commits == 0 && engagesOfB == 0,
               "10 rounds of selection ping-pong: zero commits, zero engages",
               juce::String (commits) + "/" + juce::String (engagesOfB));
        // By source: five user click sites arm the flag; every other
        // writer inherits the programmatic default; the choke-point build
        // divert exists; the borrowed view's signature reads the borrowed
        // host's OWN revision.
        auto slurpF = [] (const char* f)
        { std::ifstream ff (f); std::stringstream ss; ss << ff.rdbuf();
          return juce::String (ss.str()); };
        const auto edF = slurpF ("Source/PluginEditor.cpp");
        int userSites = 0;
        for (int q = 0; (q = edF.indexOf (q, "pendingSelectionIsUser_ = true;")) >= 0; ++q)
            ++userSites;
        check (userSites == 5,
               "exactly the five user click sites arm the user flag",
               juce::String (userSites));
        check (edF.contains ("handleBorrowSelectionChange({}, pendingSelectionIsUser_)")
                 && edF.contains ("handleBorrowSelectionChange(uid, pendingSelectionIsUser_)"),
               "both selection writers consume the flag (default programmatic)");
        check (edF.contains ("if (auto* bhB = processorRef.borrowHostIfActiveFor(linkUid))"),
               "chain builds divert into the SESSION at the one choke point");
        check (edF.contains ("v.revision = bh->getChainRevision();"),
               "the borrowed view's signature reads the borrowed host");
    }

    std::printf ("== build-into-session, functionally (27 Aug) ==\n");
    {
        // "Nothing to build" shipped because the first divert parsed the
        // wrong schema and only a source pin guarded it. This gate does
        // what the order says: session live, a proposed CHAIN ARRAY (the
        // real payload shape), and the borrowed host's slot count asserted
        // afterwards — through the ONE shared conversion the divert uses.
        EchoJayProcessor bp;
        bp.borrowEngageBegin ("uid-build", "lease-build", true, true);
        auto* bhb = bp.borrowHost();
        check (bhb != nullptr, "session engaged for the build gate");
        borrowRack (*bhb);                       // 2 originals
        check (bhb->getNumSlots() == 2, "two originals present");
        juce::Array<juce::var> chainArr;
        for (const char* nm : { "EJ Borrow Probe", "EJ Borrow Grinch",
                                "EJ Borrow Probe" })
        {
            auto* eo = new juce::DynamicObject();
            eo->setProperty ("name", nm);
            eo->setProperty ("settings", "gate settings text");
            chainArr.add (juce::var (eo));
        }
        auto ops = ChainHost::chainArrayToReplaceOps (chainArr,
                                                      bhb->getNumSlots());
        check ((int) ops.size() == 5,
               "the conversion yields removes + adds (2 + 3)",
               juce::String ((int) ops.size()));
        juce::StringArray baseB { "EJ Borrow Probe", "EJ Borrow Grinch" };
        bool done = false; int appliedN = 0;
        bhb->applyChainEdits (std::move (ops), -1, baseB,
            [&done, &appliedN] (const juce::StringArray&, int applied, bool)
            { appliedN = applied; done = true; });
        // The sequencer defers between ops (message-thread async), so the
        // headless gate pumps the run loop + pending timers until done.
        for (int t = 0; t < 200 && ! done; ++t)
        {
            juce::Timer::callPendingTimersSynchronously();
            CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false);
        }
        check (done, "the build sequencer completed");
        check (bhb->getNumSlots() == 3,
               "the proposed chain BUILT into the borrowed host (3 slots)",
               juce::String (bhb->getNumSlots()));
        check (bhb->getSlotInfo (0).name == "EJ Borrow Probe"
                 && bhb->getSlotInfo (1).name == "EJ Borrow Grinch"
                 && bhb->getSlotInfo (2).name == "EJ Borrow Probe",
               "in the proposed ORDER (append semantics held)");
        check (bhb->getSlotInfo (0).settings == "gate settings text",
               "settings text attached to the built slots");
        bp.borrowRelease (false);
    }

    std::printf ("== nextCtrlSeq: distinct under same-millisecond bursts ==\n");
    {
        // The ruling's gate: two commands issued in the same millisecond get
        // distinct seqs. A tight 1000-call burst spans well under a second,
        // so it contains hundreds of same-millisecond pairs; every value
        // must be strictly increasing (distinct AND monotonic).
        const juce::int64 t0 = juce::Time::currentTimeMillis();
        int prev = LinkShm::nextCtrlSeq();
        bool strictly = true;
        for (int i = 0; i < 999; ++i)
        {
            const int s = LinkShm::nextCtrlSeq();
            if (s <= prev) { strictly = false; break; }
            prev = s;
        }
        const juce::int64 spanMs = juce::Time::currentTimeMillis() - t0;
        check (strictly,
               "1000-call burst: every seq strictly greater than the last");
        check (spanMs < 1000,
               "the burst really was sub-second (same-ms pairs guaranteed)",
               juce::String (spanMs) + "ms");
        // And the seed keeps restart monotonicity: values sit at or above
        // wall seconds, so a relaunched main cannot reissue a seq a Link
        // already applied from the previous run.
        check (prev >= (int) (t0 / 1000),
               "seqs never fall below wall seconds (restart monotonicity)");
    }

    std::printf ("== the hosting substitute CAN differ from the picked plugin ==\n");
    {
        // FUNCTIONAL half of the picked-vs-substitute gate: prove the swap
        // is real — a popout-only AU with a catalogue VST3 build comes back
        // as a DIFFERENT descriptor (name, uid, format). This is the
        // identity a plan must never carry; the source pin above holds the
        // add path to `picked`.
        juce::File shared2 = juce::File (appData).getChildFile ("EchoJay");
        juce::KnownPluginList kl;
        juce::PluginDescription v3;
        v3.name = "EJ Popout Probe Stereo";
        v3.pluginFormatName = "VST3";
        v3.fileOrIdentifier = "/nonexistent/EJPopoutProbe.vst3";
        v3.uniqueId = 0x50505631;
        kl.addType (v3);
        if (auto xml = kl.createXml())
            shared2.getChildFile ("chain_plugins.xml")
                   .replaceWithText (xml->toString (juce::XmlElement::TextFormat()));
        ChainHost::markPopoutOnly ("EJ Popout Probe (s)", "AudioUnit");
        ChainHost differHost;                     // fresh ctor loads the catalogue
        juce::PluginDescription au;
        au.name = "EJ Popout Probe (s)";
        au.pluginFormatName = "AudioUnit";
        au.uniqueId = 0x50505541;
        const auto sub = differHost.preferInlineHostableDesc (au);
        check (sub.pluginFormatName == "VST3" && sub.name != au.name,
               "a popout-only AU swaps to its VST3 build for local hosting",
               sub.name);
        check (sub.uniqueId != au.uniqueId,
               "and the substitute's uid differs - the wrong identity to send");
    }

    std::printf ("== selector liveness + dead-uid file reaper ==\n");
    {
        // LIVENESS BEFORE LISTING, the pure decision: a frozen heartbeat is
        // never proven; a climb proves; proof persists across later ties.
        LinkShm::RegLiveness rl;
        check (! rl.observe (7), "first sight proves nothing");
        check (! rl.observe (7), "a frozen heartbeat never proves");
        check (rl.observe (8),   "a climb proves liveness");
        check (rl.observe (8),   "proof persists across a tied read");
        LinkShm::RegLiveness rl2;
        check (! rl2.observe (0) && ! rl2.observe (0),
               "a ghost stuck at zero is never listed");

        // THE REAPER, functionally, in the sandbox — NARROWED to transient
        // classes (25 Aug 2026 ruling: uids RETURN, the dead-forever
        // premise was false): protocol transients and unreferenced rings
        // die; SIDECARS are spared however dead (a returning uid's rack
        // description); structplan spared entirely (someone's rollback);
        // rings reap by the registry's referenced FILENAMES.
        juce::File rdir = juce::File (appData).getChildFile ("EJReapTest");
        rdir.createDirectory();
        auto mk = [&rdir] (const char* n) { rdir.getChildFile (n)
                                                .replaceWithText ("x"); };
        mk ("rack-deaddead01.json");       // sidecar, dead, old -> SPARED now
        mk ("structplan-deaddead01.json"); // journal, dead, old -> SPARED
        mk ("ctrl-cmd-deaddead01.json");   // transient, dead, old -> reaped
        mk ("lease-deaddead01.json");      // transient, dead, old -> reaped
        mk ("lease-livelive01.json");      // transient, LIVE uid -> spared
        mk ("ctrl-ack-freshfresh1.json");  // transient, dead, fresh -> spared
        mk ("audio_untitled_dead.bin");    // unreferenced ring  -> reaped
        mk ("audio_drums.bin");            // referenced ring    -> spared
        const juce::int64 now = juce::Time::currentTimeMillis();
        const juce::int64 old = now - (60 * 60 * 1000);
        for (const char* n : { "rack-deaddead01.json",
                               "structplan-deaddead01.json",
                               "ctrl-cmd-deaddead01.json",
                               "lease-deaddead01.json",
                               "lease-livelive01.json",
                               "audio_untitled_dead.bin", "audio_drums.bin" })
            rdir.getChildFile (n).setLastModificationTime (juce::Time (old));
        const int reaped = LinkShm::reapDeadUidFiles (
            rdir.getFullPathName() + "/", { "livelive01" },
            { "audio_drums.bin" }, now, 10 * 60 * 1000);
        check (reaped == 3, "exactly the three transient dead files reaped",
               juce::String (reaped));
        check (! rdir.getChildFile ("ctrl-cmd-deaddead01.json").exists()
                 && ! rdir.getChildFile ("lease-deaddead01.json").exists()
                 && ! rdir.getChildFile ("audio_untitled_dead.bin").exists(),
               "dead transients and the orphan ring are gone");
        check (rdir.getChildFile ("rack-deaddead01.json").exists(),
               "a SIDECAR is spared however dead - uids return");
        check (rdir.getChildFile ("lease-livelive01.json").exists(),
               "a live uid's transients survive, however old");
        check (rdir.getChildFile ("ctrl-ack-freshfresh1.json").exists(),
               "a fresh file survives the grace window");
        check (rdir.getChildFile ("structplan-deaddead01.json").exists(),
               "structplan is SPARED entirely - a journal is a rollback");
        check (rdir.getChildFile ("audio_drums.bin").exists(),
               "a registry-referenced ring survives");

        // THE UID CLAIM GATE, all three arms, functionally: a climbing
        // holder is a duplicate (re-mint), a frozen holder is a ghost
        // (adopt) — but never before the threshold (an unproven holder is
        // never adopted).
        using UCG = LinkShm::UidClaimGate;
        UCG dup;
        check (dup.observe (10) == UCG::Decision::Wait
                 && dup.observe (11) == UCG::Decision::Remint,
               "a proven-live holder forces a re-mint (real duplicate)");
        UCG ghost;
        bool earlyAdopt = false;
        UCG::Decision d = UCG::Decision::Wait;
        for (int t = 0; t < 5; ++t)
        {
            d = ghost.observe (7);
            if (t < 4 && d != UCG::Decision::Wait) earlyAdopt = true;
        }
        check (! earlyAdopt, "an unproven holder is NEVER adopted early");
        check (d == UCG::Decision::AdoptGhost,
               "a holder frozen through the threshold is a ghost - adopted");
        UCG lateLife;
        lateLife.observe (3); lateLife.observe (3); lateLife.observe (3);
        check (lateLife.observe (4) == UCG::Decision::Remint,
               "a holder that wakes mid-probe is live - re-mint, not adopt");

        // Pins: the listing gates on the pure helper; the Link's destructor
        // cleans its own uid files but never structplan; the reaper's uid
        // pattern list cannot quietly grow a structplan entry.
        auto slurpR = [] (const char* p)
        { std::ifstream f (p); std::stringstream s; s << f.rdbuf();
          return juce::String (s.str()); };
        check (slurpR ("Source/PluginProcessor.cpp")
                   .contains ("if (! ps.live.observe(snap.heartbeat))"),
               "the selector lists only heartbeat-proven slots");
        const auto lpR = slurpR ("Source/LinkProcessor.cpp");
        const int dtor = lpR.indexOf ("LinkProcessor::~LinkProcessor");
        const auto dtorBody = dtor >= 0 ? lpR.substring (dtor, dtor + 1800)
                                        : juce::String();
        check (dtorBody.contains ("rack-\" + instanceUid_")
                 && ! dtorBody.contains ("structplan-\" + instanceUid_"),
               "the Link deletes its own uid files on clean exit, journal excepted");
        const auto sh = slurpR ("Source/LinkShm.h");
        const int up = sh.indexOf ("uidPatterns[]");
        const auto upArm = up >= 0 ? sh.substring (up, up + 400) : juce::String();
        check (up >= 0 && ! upArm.contains ("structplan")
                 && ! upArm.contains ("\"rack-"),
               "the reaper's pattern list carries neither structplan nor sidecars");
        // The claim guard decides by heartbeat through the gate, and the
        // strip renders the gone state distinctly from silence.
        check (lpR.contains ("uidGate_.observe(holderHb)")
                 && lpR.contains ("uid adopted"),
               "the claim guard routes through UidClaimGate, ghost-adopting");
        const auto edR = slurpR ("Source/PluginEditor.cpp");
        check (edR.contains ("heartbeatFresh")
                 && edR.contains ("\"not responding\""),
               "the strip renders heartbeat-stale as GONE, not as silence");
    }

    std::printf ("== §5a-R functional: slot editors open while editing ==\n");
    {
        // TWICE this exact capability regressed behind passing source pins
        // (22 Aug guard order; 26 Aug an engage that never completed). So
        // this gate CONSTRUCTS the states and CALLS the one-author decision
        // (EchoJayProcessor::createSlotEditorForView) against real object
        // code — including the async-engage window, which is exactly the
        // state built here: engage begun, rack settling, nothing finished.
        EchoJayProcessor mainProc;
        check (! mainProc.borrowActive(), "fresh processor: no session");
        check (mainProc.createSlotEditorForView ("some-remote", 0) == nullptr,
               "a remote view without a session refuses (Link-owned instance)");
        mainProc.borrowEngageBegin ("uid-edit", "lease-ed1",
                                    /*structureCapable=*/true);
        check (mainProc.borrowActive(), "session engaged (begin half)");
        // THE CAPABILITY RIDES THE SESSION FROM ITS FIRST INSTANT — this
        // assert runs BEFORE any load settles, which is exactly the async
        // window where adds were refused on a live session (26 Aug 2026:
        // the flag used to be set only at load settlement).
        check (mainProc.borrowStructureCapable_,
               "the capability snapshot is set the instant the session is");
        auto* bh = mainProc.borrowHost();
        check (bh != nullptr && bh->isBorrowed(), "borrowed host exists");
        borrowRack (*bh);
        auto* ed0 = mainProc.createSlotEditorForView ("uid-edit", 0);
        check (ed0 != nullptr,
               "a slot editor OPENS while the rack is being edited");
        delete ed0;
        check (mainProc.createSlotEditorForView ("other-uid", 0) == nullptr,
               "a DIFFERENT remote view still refuses");
        check (mainProc.createSlotEditorForView ("uid-edit", 99) == nullptr,
               "an out-of-range slot refuses, never crashes");
        // A STRUCTURE EDIT IS PERMITTED: with the session vectors present
        // (engage-time, not settlement-time), an added slot computes into
        // a Create — the plan-level proof the picker's fork guards.
        mainProc.borrowBaseIdentity_ = bh->liveIdentity();
        mainProc.borrowSlotOrigin_ = { 0, 1 };
        mainProc.borrowCreatedIdentity_.assign (2, {});
        mainProc.borrowSlotRecords_.clear();
        for (int i = 0; i < 2; ++i)
        {
            EchoJayProcessor::BorrowSlotRecord r0;
            r0.name = bh->getSlotInfo (i).name;
            mainProc.borrowSlotRecords_.push_back (std::move (r0));
        }
        borrowRack (*bh);   // no-op shape refresh keeps 2 slots
        {
            // simulate the picker's add: a third slot appended, origin -1
            juce::Array<juce::var> arr1; int n1 = 1;
            for (const char* nm : { "EJ Borrow Probe", "EJ Borrow Grinch",
                                    "EJ Borrow Probe" })
            {
                auto* o = new juce::DynamicObject();
                o->setProperty ("n", n1++);
                o->setProperty ("plugin", nm);
                o->setProperty ("bypassed", false);
                arr1.add (juce::var (o));
            }
            bh->restoreSavedChain (juce::var (arr1),
                                   juce::var (new juce::DynamicObject()));
            mainProc.borrowSlotOrigin_.push_back (-1);
            mainProc.borrowCreatedIdentity_.push_back (
                { "EJ Borrow Probe", "1161904976", {} });
            EchoJayProcessor::BorrowSlotRecord cr0;
            cr0.name = "EJ Borrow Probe";
            mainProc.borrowSlotRecords_.push_back (std::move (cr0));
        }
        const auto permitPlan = mainProc.buildStructurePlan();
        check (permitPlan.creating == 1,
               "a structure edit is PERMITTED: the added slot computes into "
               "a Create", juce::String (permitPlan.creating));
        mainProc.borrowRelease (false);
        check (mainProc.createSlotEditorForView ("uid-edit", 0) == nullptr,
               "after release the view is remote again - refuses");

        // ---- §8.3 (amended): the report NEVER moves --------------------
        // The whole amendment in three asserts: the budget is reported
        // from prepare, and engage/release/mode changes leave it alone —
        // ordinary browsing never re-runs PDC.
        mainProc.prepareToPlay (48000.0, 512);
        // §8.3 refinement: NO capable Link -> NO budget -> no added latency.
        const int latBare = mainProc.getLatencySamples();
        check (latBare < EchoJayProcessor::kBorrowAlignBudgetFrames,
               "no capable Link: no alignment budget is carried",
               juce::String (latBare));
        // The 0->1 capable-Link transition is THE one PDC event.
        mainProc.setBorrowBudgetActive (true);
        const int lat0 = mainProc.getLatencySamples();
        check (lat0 == latBare + EchoJayProcessor::kBorrowAlignBudgetFrames,
               "a capable Link present: the budget is carried",
               juce::String (lat0));
        mainProc.borrowEngageBegin ("uid-ctx", "lease-ctx", true, true);
        check (mainProc.getLatencySamples() == lat0,
               "ENGAGE does not touch the report");
        check (mainProc.borrowInContextOk_.load(),
               "in-context OK at engage (capable, fits)");
        mainProc.borrowRelease (false);
        check (mainProc.getLatencySamples() == lat0,
               "RELEASE does not touch the report");
        mainProc.setBorrowBudgetActive (false);
        check (mainProc.getLatencySamples() == latBare,
               "the last capable Link leaving withdraws the budget");

        // ---- §8 STABILITY, with a REAL signal (26 Aug 2026 URGENT: the
        // ---- in-context path injected an UNINITIALISED buffer — peak 746,
        // ---- pinned meters. A DSP path with no level assertion is how it
        // ---- reached a user; every §8 mode now proves bounded output).
        mainProc.setBorrowBudgetActive (true);
        mainProc.prepareToPlay (48000.0, 512);
        mainProc.borrowEngageBegin ("uid-lvl", "lease-lvl", true, true);
        // The rig plays the Link's confirmation (the injection ramps in
        // only after the sidecar confirms the mute - 30 Aug gate).
        mainProc.borrowMuteConfirmedOnce_.store (true, std::memory_order_relaxed);
        check (mainProc.borrowInContextOk_.load(), "level arm: ctx engaged");
        {
            juce::AudioBuffer<float> blk (2, 512);
            juce::MidiBuffer midi;
            float maxPeak = 0.0f;
            auto runBlocks = [&](int nBlocks)
            {
                for (int b = 0; b < nBlocks; ++b)
                {
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < 512; ++i)
                            blk.setSample (ch, i,
                                0.5f * std::sin (0.05f * (float) (b * 512 + i)));
                    mainProc.processBlock (blk, midi);
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < 512; ++i)
                            maxPeak = juce::jmax (maxPeak,
                                std::abs (blk.getSample (ch, i)));
                }
            };
            runBlocks (60);                       // in-context, no ring bound
            check (maxPeak < 2.0f,
                   "IN-CONTEXT output stays bounded over 60 real blocks",
                   juce::String (maxPeak, 3));
            // The AUTOMATIC fallback solo (LISTEN deleted): in-context
            // refused -> the borrow ring replaces the output, no user flag.
            mainProc.borrowInContextOk_.store (false, std::memory_order_relaxed);
            maxPeak = 0.0f; runBlocks (60);
            check (maxPeak < 2.0f,
                   "FALLBACK SOLO output stays bounded over 60 real blocks",
                   juce::String (maxPeak, 3));
            mainProc.borrowInContextOk_.store (true, std::memory_order_relaxed);
            // THE HONORARY STRIP (MUTE_SOLO_SPEC §6.1), as BEHAVIOUR: the
            // injection gain follows the solo fabric — a foreign solo
            // ramps it to silence, the edited rack joining the set (or the
            // set emptying) ramps it back. Suppression must never touch
            // the pad (no history clear), so the return is a ramp, not a
            // re-prime.
            runBlocks (30);   // settle in-context: gain reaches 1
            check (mainProc.borrowCtxMixNow() > 0.99f,
                   "in-context settled: injection gain 1",
                   juce::String (mainProc.borrowCtxMixNow(), 3));
            mainProc.borrowSoloSuppressInj_.store (true, std::memory_order_relaxed);
            runBlocks (30);
            check (mainProc.borrowCtxMixNow() < 0.01f,
                   "a foreign solo ramps the injection OUT (honorary strip)",
                   juce::String (mainProc.borrowCtxMixNow(), 3));
            mainProc.borrowSoloSuppressInj_.store (false, std::memory_order_relaxed);
            runBlocks (30);
            check (mainProc.borrowCtxMixNow() > 0.99f,
                   "solo lifted (or joined): the injection ramps back",
                   juce::String (mainProc.borrowCtxMixNow(), 3));
            maxPeak = 0.0f; runBlocks (30);       // mode switch ramps settle
            check (maxPeak < 2.0f,
                   "the mode switch stays bounded through the ramps",
                   juce::String (maxPeak, 3));
            // RACK-SWITCH CONTINUITY (26 Aug crackle): with a steady sine
            // playing, a chain-latency jump (what a rack switch does) must
            // produce no sample-to-sample discontinuity in the MIX beyond
            // the signal's own slope — the passthrough delay is constant
            // by design; only the ramped injection may change.
            float maxDelta = 0.0f; float prevLast = 0.0f; bool first = true;
            long phase = 0;   // CONTINUOUS across runs - the input must not
                              // be the discontinuity the assert then blames
                              // on the device under test
            auto runDelta = [&](int nBlocks)
            {
                for (int b = 0; b < nBlocks; ++b)
                {
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < 512; ++i)
                            blk.setSample (ch, i,
                                0.4f * std::sin (0.05f * (float) (phase + i)));
                    phase += 512;
                    mainProc.processBlock (blk, midi);
                    for (int i = 0; i < 512; ++i)
                    {
                        const float v = blk.getSample (0, i);
                        if (! first || i > 0)
                            maxDelta = juce::jmax (maxDelta,
                                std::abs (v - (i == 0 ? prevLast
                                                      : blk.getSample (0, i - 1))));
                        first = false;
                    }
                    prevLast = blk.getSample (0, 511);
                }
            };
            runDelta (40);        // prime: flush the 32-block delay line of
            maxDelta = 0.0f;      // the previous arm's unrelated signal
            runDelta (20);
            mainProc.borrowChainLat_.store (500);   // the rack switch
            runDelta (20);
            mainProc.borrowChainLat_.store (4000);  // and another
            runDelta (20);
            check (maxDelta < 0.2f,
                   "a chain-latency jump produces NO mix discontinuity "
                   "beyond the ramp", juce::String (maxDelta, 3));
        }
        // ---- THE MUTE CONTRACT, main half (26 Aug: continuous doubling —
        // ---- the Link never muted; the gate had proven the mute only
        // ---- from the want-flag inward, never the FILE that carries it).
        {
            int errL = 0;
            const juce::String ldir2 = LinkShm::resolveDir(errL);
            const juce::File lf (LinkShm::leasePath (ldir2, "uid-lvl"));
            check (lf.existsAsFile(), "engage wrote the lease file");
            auto lv = juce::JSON::parse (lf.loadFileAsString());
            auto* lo = lv.getDynamicObject();
            check (lo != nullptr && lo->hasProperty ("muteOut")
                     && (bool) lo->getProperty ("muteOut"),
                   "the lease on DISK carries muteOut:true for in-context",
                   lf.loadFileAsString().substring (0, 200));
            // editPending (27 Aug wrong-banner): a normal session renews
            // FALSE; the failure-held session renews TRUE — asserted on
            // the FILE, the only channel the Link's banner can read.
            check (lo != nullptr && ! (bool) lo->getProperty ("editPending"),
                   "a live selection renews editPending:false");
            mainProc.borrowEditPendingHeld_ = true;   // ruling-3 keep arm
            mainProc.renewBorrowLease();
            auto lv2 = juce::JSON::parse (lf.loadFileAsString());
            auto* lo2 = lv2.getDynamicObject();
            check (lo2 != nullptr && (bool) lo2->getProperty ("editPending"),
                   "the failure-held session renews editPending:true on DISK",
                   lf.loadFileAsString().substring (0, 200));
        }
        mainProc.borrowRelease (false);
        check (! mainProc.borrowEditPendingHeld_,
               "every ending clears the pending hold with the session");

        // ---- M/S PERMANENCE + ONE AUTHOR (27/28 Aug hands-on) ----------
        // Permanence: the rack lamps survive the borrowed-view flip and a
        // status write, visible AND hittable (getComponentAt respects
        // z-order — covered/hidden/mislaid fails where a pin passes).
        // One author: the SAME click on the rack lamps and on the mixer
        // strip must publish the SAME state, and BOTH surfaces must show
        // it — pixel-asserted, because two renderers is how the rack
        // drifted (its TextButton pair froze behind the panel signature:
        // first render before the snap ingested left it disabled — dead
        // clicks; only forced rebuilds momentarily adopted fresh state).
        {
            // A REAL registered Link row + published sidecar: the snaps
            // must arrive through the shipping ingest, not a test poke.
            int errR = 0;
            const juce::String rdir = LinkShm::resolveDir (errR);
            int rfd = -1, rerr = 0;
            void* rmap = LinkShm::openRegistry (rdir, rfd, rerr);
            check (rmap != nullptr, "one-author arm: registry mapping");
            auto* rslots = LinkShm::regSlots (rmap);
            const int ri = 11;
            const char* ruid = "uid-ms";
            std::memset (&rslots[ri], 0, sizeof (RegistrySlot));
            std::strncpy (rslots[ri].displayName, "MS Test", 39);
            std::memcpy (rslots[ri].instanceUid, ruid, 6);
            rslots[ri].sampleRate = 48000.0f;
            rslots[ri].numChannels = 2;
            LinkShm::storeRelease (&rslots[ri].heartbeat, 1u);
            LinkShm::storeRelease (&rslots[ri].inUse, 1u);
            LinkShm::RackSidecar rc;
            rc.valid = true; rc.uid = ruid; rc.name = "MS Test";
            rc.muteSoloCapable = true;
            // inContextCapable too: the registry pass re-derives the §8
            // budget whenever the listed-uid set changes, and a sidecar
            // without the bit would WITHDRAW it mid-arm (the axis arm's
            // first draft lost the budget to exactly this when its foreign
            // row appeared — the gain froze at 1.0 with the whole §8 block
            // skipped, not suppressed).
            rc.inContextCapable = true;
            LinkShm::writeRackSidecar (rdir, rc);
            for (int t = 0; t < 4; ++t)
            {
                LinkShm::storeRelease (&rslots[ri].heartbeat, (uint32_t)(2 + t));
                mainProc.refreshLinkRegistry();
            }
            check (mainProc.muteSoloSnaps_.count (ruid) == 1
                     && mainProc.muteSoloSnaps_[ruid].capable,
                   "the sidecar bits arrived through the REAL ingest");

            mainProc.pendingChannelUid = ruid;      // view the Link rack
            std::unique_ptr<juce::AudioProcessorEditor> ed (mainProc.createEditor());
            check (ed != nullptr, "the REAL editor constructed");
            ed->setSize (1280, 820);
            auto* eje = dynamic_cast<EchoJayEditor*> (ed.get());
            check (eje != nullptr, "and is the real EchoJayEditor");
            EchoJayTabStripTestAccess::toChain (*eje);
            // The sandboxed editor sits on the LOGIN screen, whose layout
            // pass skips main-screen geometry — the panel is sized here
            // directly; visibility, refresh and z-order are the shipping
            // path.
            EchoJayTabStripTestAccess::panel (*eje)
                .setBounds (0, 96, 900, 640);
            auto pump = [&]{ for (int i = 0; i < 12; ++i)
                             { juce::Timer::callPendingTimersSynchronously();
                               CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false); } };
            auto findByName = [&](juce::Component* root, const juce::String& nm,
                                  auto&& self) -> juce::Component*
            {
                if (root->getName() == nm) return root;
                for (auto* c : root->getChildren())
                    if (auto* r = self (c, nm, self)) return r;
                return nullptr;
            };
            auto lampsHittable = [&](const juce::String& when)
            {
                auto* lamps = findByName (ed.get(), "msLamps", findByName);
                check (lamps != nullptr && lamps->isVisible(),
                       "rack lamps present and visible " + when,
                       lamps == nullptr ? "NOT FOUND"
                                        : (lamps->isVisible() ? "ok" : "INVISIBLE"));
                if (lamps == nullptr || ! lamps->isVisible()) return;
                auto* parent = lamps->getParentComponent();
                const auto centre = lamps->getBounds().getCentre();
                check (parent != nullptr
                         && parent->getComponentAt (centre) == lamps,
                       "rack lamps HITTABLE " + when);
            };
            pump();
            lampsHittable ("on the remote view");
            mainProc.borrowEngageBegin (ruid, "lease-ms", true, true);
            pump();
            lampsHittable ("after the engage view flip");
            mainProc.borrowStickyBanner_ = "a status line takes the panel";
            pump();
            lampsHittable ("after a status write");
            mainProc.borrowRelease (false);
            pump();
            lampsHittable ("after release");

            // ---- ONE AUTHOR, the click half: a REAL mouse event on the
            // rack lamps, then on the mixer strip, must publish the SAME
            // field and value (both toggling from the same snap).
            auto sendClick = [&](juce::Component& target, juce::Point<int> pos)
            {
                auto src = juce::Desktop::getInstance().getMainMouseSource();
                const juce::MouseEvent down (src, pos.toFloat(),
                    juce::ModifierKeys::leftButtonModifier,
                    0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                    &target, &target, juce::Time::getCurrentTime(),
                    pos.toFloat(), juce::Time::getCurrentTime(), 1, false);
                target.mouseDown (down);
            };
            auto readCmd = [&]() -> juce::var
            {
                juce::File f (rdir + "ctrl-cmd-" + juce::String (ruid) + ".json");
                auto v = juce::JSON::parse (f.loadFileAsString());
                f.deleteFile();
                return v;
            };
            auto* lamps = findByName (ed.get(), "msLamps", findByName);
            check (lamps != nullptr, "lamps found for the click half");
            sendClick (*lamps,
                EchoJayTabStripTestAccess::rackMRect (*eje).getCentre());
            auto rackCmd = readCmd();
            check (rackCmd.getDynamicObject() != nullptr
                     && rackCmd.hasProperty ("muteUser")
                     && (bool) rackCmd["muteUser"],
                   "rack M click published muteUser:true",
                   juce::JSON::toString (rackCmd, true));

            EchoJayTabStripTestAccess::toLink (*eje);
            EchoJayTabStripTestAccess::measure (*eje);
            // The login-screen layout never sizes the scrolled mixer view;
            // size it here so clicks land and snapshots have pixels. The
            // geometry itself comes from the real measure pass above.
            EchoJayTabStripTestAccess::mixer (*eje).setSize (2000, 800);
            const auto* sgMs = EchoJayTabStripTestAccess::geomFor (*eje, ruid);
            check (sgMs != nullptr, "the mixer laid out a strip for uid-ms");
            if (sgMs != nullptr)
            {
                sendClick (EchoJayTabStripTestAccess::mixer (*eje),
                           sgMs->mute.getCentre());
                auto mixCmd = readCmd();
                check (mixCmd.getDynamicObject() != nullptr
                         && mixCmd.hasProperty ("muteUser")
                         && (bool) mixCmd["muteUser"],
                       "mixer M click published muteUser:true",
                       juce::JSON::toString (mixCmd, true));
                check (rackCmd.hasProperty ("muteUser")
                         == mixCmd.hasProperty ("muteUser")
                       && (bool) rackCmd["muteUser"] == (bool) mixCmd["muteUser"]
                       && ! rackCmd.hasProperty ("soloOn")
                       && ! mixCmd.hasProperty ("soloOn"),
                       "SAME published state from both surfaces (one author)");
            }

            // ---- and BOTH SURFACES SHOW IT: the Link answers (sidecar
            // muteUser:true), the real ingest re-reads, and both lamps
            // render LIT — asserted on PIXELS, the rendering itself.
            juce::Thread::sleep (12);              // mtime resolution
            rc.muteUser = true;
            LinkShm::writeRackSidecar (rdir, rc);
            LinkShm::storeRelease (&rslots[ri].heartbeat, 9u);
            mainProc.refreshLinkRegistry();
            check (mainProc.muteSoloSnaps_[ruid].muteUser,
                   "the answer arrived through the real ingest");
            auto hasAmber = [](const juce::Image& img)
            {
                for (int y = 0; y < img.getHeight(); ++y)
                    for (int x = 0; x < img.getWidth(); ++x)
                    {
                        const auto c = img.getPixelAt (x, y);
                        if (c.getRed() > 220 && c.getGreen() > 140
                            && c.getGreen() < 210 && c.getBlue() < 90)
                            return true;
                    }
                return false;
            };
            if (lamps != nullptr)
            {
                const auto img = lamps->createComponentSnapshot (
                    EchoJayTabStripTestAccess::rackMRect (*eje));
                check (hasAmber (img), "the RACK lamp renders LIT (pixels)");
            }
            if (sgMs != nullptr)
            {
                const auto img = EchoJayTabStripTestAccess::mixer (*eje)
                    .createComponentSnapshot (sgMs->mute);
                check (hasAmber (img), "the MIXER lamp renders LIT (pixels)");
            }
            // Narrow S click through the STORED rect (30 Aug centring: the
            // narrow strip is where the rects are tightest, so it is where
            // a paint/hit mismatch would show).
            if (sgMs != nullptr)
            {
                sendClick (EchoJayTabStripTestAccess::mixer (*eje),
                           sgMs->solo.getCentre());
                auto sCmd = readCmd();
                check (sCmd.getDynamicObject() != nullptr
                         && sCmd.hasProperty ("soloOn")
                         && (bool) sCmd["soloOn"],
                       "narrow S click lands through its stored rect",
                       juce::JSON::toString (sCmd, true));
            }
            // Screenshot proof rig (EJ_SHOTS=1): one narrow and one wide
            // strip PNG for visual verification of the centred group.
            if (std::getenv ("EJ_SHOTS") != nullptr && sgMs != nullptr)
            {
                auto savePng = [&](const juce::Image& img,
                                   const juce::String& path)
                {
                    juce::File f (path);
                    f.deleteFile();
                    juce::FileOutputStream os (f);
                    juce::PNGImageFormat png; png.writeImageToStream (img, os);
                };
                savePng (EchoJayTabStripTestAccess::mixer (*eje)
                             .createComponentSnapshot (sgMs->full, false, 3.0f),
                         juce::String (std::getenv ("EJ_SHOTS"))
                             + "/strip_narrow.png");
                mainProc.linkMixerWide = true;
                EchoJayTabStripTestAccess::measure (*eje);
                EchoJayTabStripTestAccess::mixer (*eje).setSize (2000, 800);
                if (const auto* sgW = EchoJayTabStripTestAccess::geomFor (*eje, ruid))
                    savePng (EchoJayTabStripTestAccess::mixer (*eje)
                                 .createComponentSnapshot (sgW->full, false, 3.0f),
                             juce::String (std::getenv ("EJ_SHOTS"))
                                 + "/strip_wide.png");
                mainProc.linkMixerWide = false;
                EchoJayTabStripTestAccess::measure (*eje);
                // The rack row (31 Aug): pill + centred tick/M/S group.
                auto& pnl = EchoJayTabStripTestAccess::panel (*eje);
                savePng (pnl.createComponentSnapshot (
                             { 0, pnl.getHeight() - 76, 176, 76 },
                             false, 3.0f),
                         juce::String (std::getenv ("EJ_SHOTS"))
                             + "/rack_row.png");
            }
            // ---- NEW CHAT (30 Aug 2026): no tab jump; the binding is the
            // parameter of the ONE creator, inherited from the viewed
            // context — proven by what the binding DRIVES (chainViewUid,
            // which routes the rack panel and the rack-lock want), not by
            // a header label.
            {
                using A = EchoJayTabStripTestAccess;
                // From CHAT, main context: stays on CHAT, stays main.
                A::toChat (*eje);
                mainProc.pendingChannelUid.clear();
                mainProc.activeChatId.clear();
                A::seedMsg (*eje);                    // an empty chat would
                                                      // short-circuit (by
                                                      // design: empty IS new)
                A::newChatBtn (*eje).onClick();
                check (A::tab (*eje) == 3,            // Tab::Chat
                       "new chat from CHAT: the tab does not move",
                       juce::String (A::tab (*eje)));
                check (A::activeChatUid (*eje).isEmpty(),
                       "and a main-context chat stays MAIN",
                       A::activeChatUid (*eje));
                // From LINK, with a LINK-BOUND chat active: stays on LINK,
                // inherits the binding from the chat being viewed. Pending
                // applies only while NO chat is active (the lazy-creation
                // contract), so the rig deactivates first — the same state
                // a user tapping a channel is in.
                A::clearActiveChat (*eje);
                mainProc.pendingChannelUid = ruid;    // view the Link channel
                A::seedMsg (*eje);
                A::newChatBtn (*eje).onClick();       // binds via pending
                check (A::activeChatUid (*eje) == juce::String (ruid),
                       "a link-context chat binds to the Link",
                       A::activeChatUid (*eje));
                A::toLink (*eje);
                A::seedMsg (*eje);                    // now the ACTIVE chat
                                                      // is link-bound: the
                                                      // "on a Link chat" case
                A::newChatBtn (*eje).onClick();
                check (A::tab (*eje) == 5,            // Tab::Link
                       "new chat from LINK: the tab does not move "
                       "(the old handler jumped to CHAT here)",
                       juce::String (A::tab (*eje)));
                check (A::activeChatUid (*eje) == juce::String (ruid),
                       "and the new chat is BOUND to the viewed chat's Link",
                       A::activeChatUid (*eje));
                check (A::viewUid (*eje) == juce::String (ruid),
                       "the binding DRIVES the view: chainViewUid follows it",
                       A::viewUid (*eje));
                A::toChain (*eje);                    // restore for later arms
            }

            // ---- THE MISSING AXIS (28 Aug hands-on): the injection obeys
            // EVERY silence reason. Sean's repro: with a session live the
            // Link is session-muted and the audible source IS the
            // injection — a user mute on the edited rack silenced
            // something already silent. Gated as BEHAVIOUR on the
            // injection gain, through the REAL ingest and the REAL
            // suppression computation (no atomic pokes):
            {
                auto sidecarMute = [&](bool m, bool s)
                {
                    juce::Thread::sleep (12);        // mtime resolution
                    rc.muteUser = m; rc.soloOn = s;
                    LinkShm::writeRackSidecar (rdir, rc);
                    static uint32_t hb = 20;
                    LinkShm::storeRelease (&rslots[ri].heartbeat, ++hb);
                    mainProc.refreshLinkRegistry();
                };
                juce::AudioBuffer<float> blk (2, 512);
                juce::MidiBuffer midi;
                auto runBlocks = [&](int n)
                {
                    for (int b = 0; b < n; ++b)
                    {
                        for (int ch = 0; ch < 2; ++ch)
                            for (int i = 0; i < 512; ++i)
                                blk.setSample (ch, i, 0.5f);
                        mainProc.processBlock (blk, midi);
                    }
                };
                mainProc.setBorrowBudgetActive (true);
                sidecarMute (false, false);          // clean slate
                mainProc.borrowEngageBegin (ruid, "lease-ms2", true, true);
                mainProc.borrowMuteConfirmedOnce_.store (true, std::memory_order_relaxed);
                mainProc.refreshLinkRegistry();      // suppress recomputed
                runBlocks (30);
                check (mainProc.borrowCtxMixNow() > 0.99f,
                       "axis arm: in-context settled at 1",
                       juce::String (mainProc.borrowCtxMixNow(), 3));
                sidecarMute (true, false);           // user-mute the EDITED rack
                runBlocks (30);
                check (mainProc.borrowCtxMixNow() < 0.01f,
                       "a user mute on the edited rack ramps the injection "
                       "OUT (the missing axis)",
                       juce::String (mainProc.borrowCtxMixNow(), 3));
                sidecarMute (false, false);          // un-mute
                runBlocks (30);
                check (mainProc.borrowCtxMixNow() > 0.99f,
                       "un-mute: the injection ramps back",
                       juce::String (mainProc.borrowCtxMixNow(), 3));
                // Third axis: a FOREIGN mute never touches the injection.
                const int fi2 = 12;
                const char* fuid2 = "uid-fm";
                std::memset (&rslots[fi2], 0, sizeof (RegistrySlot));
                std::strncpy (rslots[fi2].displayName, "FM Test", 39);
                std::memcpy (rslots[fi2].instanceUid, fuid2, 6);
                rslots[fi2].sampleRate = 48000.0f;
                rslots[fi2].numChannels = 2;
                LinkShm::RackSidecar frc2;
                frc2.valid = true; frc2.uid = fuid2; frc2.name = "FM Test";
                frc2.muteSoloCapable = true; frc2.muteUser = true;
                frc2.inContextCapable = true;
                LinkShm::writeRackSidecar (rdir, frc2);
                for (uint32_t t = 1; t <= 3; ++t)
                {
                    LinkShm::storeRelease (&rslots[fi2].heartbeat, t);
                    LinkShm::storeRelease (&rslots[fi2].inUse, 1u);
                    mainProc.refreshLinkRegistry();
                }
                runBlocks (10);
                check (mainProc.borrowCtxMixNow() > 0.99f,
                       "a FOREIGN mute is that strip's own business - "
                       "the injection plays on",
                       juce::String (mainProc.borrowCtxMixNow(), 3));
                // And the OR: edited rack muted AND soloed -> mute wins.
                sidecarMute (true, true);
                runBlocks (30);
                check (mainProc.borrowCtxMixNow() < 0.01f,
                       "muted AND soloed: mute wins the strip OR",
                       juce::String (mainProc.borrowCtxMixNow(), 3));
                mainProc.borrowRelease (false);
                mainProc.setBorrowBudgetActive (false);
                LinkShm::storeRelease (&rslots[fi2].inUse, 0u);
                juce::File (LinkShm::rackSidecarPath (rdir, fuid2)).deleteFile();
            }

            LinkShm::storeRelease (&rslots[ri].inUse, 0u);
            juce::File (LinkShm::rackSidecarPath (rdir, ruid)).deleteFile();
            mainProc.refreshLinkRegistry();
            mainProc.pendingChannelUid.clear();
        }

        // ---- §8 ALIGNMENT, MEASURED (28 Aug hands-on: the edited channel
        // "slightly out of time"). Everything about in-context was gated
        // for level and continuity; NOTHING ever asserted it is IN TIME —
        // which is why this shipped. An impulse fed to both paths must
        // land within 2 samples at the sum point, at more than one buffer
        // size. The ring impulse is 0.5 and the passthrough's 1.0 so the
        // two peaks are identifiable (coincidence sums to ~1.5). The
        // producer runs LINK-FIRST per cycle — a channel strip renders
        // before the output strip it feeds — and the ring is pre-filled
        // past the reseek trip so the drain's first consume seeks to the
        // cushion exactly as a field engage does.
        std::printf ("== §8 alignment: impulse through both paths ==\n");
        // The producer STAMPS every block (ring-index <-> host-position,
        // as the shipping Link does) and the main gets a test playhead, so
        // the EJAlign instrument runs in-harness and its sign is verified
        // AGAINST THE IMPULSE — content truth, not a comment's claim.
        // modes:
        //  0 prefilled, ideal phase (cushion established)   -> delta 0
        //  1 main-first cycles                              -> prints
        //  2 producer one block ahead at the seek           -> prints
        //  3 THE FIELD SEQUENCE: ring connected and discard-drained
        //    BEFORE engage (backlog ~0). Pre-fix this read -1024/-1024
        //    (the trim-only seek could not build the cushion); the
        //    stamp-based seek steps BACK into held content -> 0/0.
        //  4 prefilled + 1024 EXTRA frames after engage (a real +1024
        //    content shift): the first stamped drain seeks it away -> 0/0.
        //  5 RELOCATE mid-session (both clocks jump +20000, the -20903
        //    excursion's shape): the discontinuity detector re-seeks and
        //    the impulse after the jump lands aligned      -> 0/0.
        // GATED: modes 0, 3, 4 and 5 all aligned AND instrument-agreed —
        // the age is a measured fact and the pad is computed from it.
        struct TestPlayHead : juce::AudioPlayHead
        {
            juce::int64 pos = 0;
            bool playing = true;   // false = parked: position frozen too
            juce::Optional<juce::AudioPlayHead::PositionInfo> getPosition() const override
            {
                juce::AudioPlayHead::PositionInfo p;
                p.setTimeInSamples (pos);
                p.setIsPlaying (playing);
                return p;
            }
        };
        struct AlignRead { int delta; juce::int64 skew; };
        TestPlayHead alignPH;
        auto alignDelta = [&](int bs, int mode) -> AlignRead
        {
            int errA = 0;
            const juce::String adir = LinkShm::resolveDir (errA);
            mainProc.setBorrowBudgetActive (true);
            mainProc.prepareToPlay (48000.0, bs);
            mainProc.setPlayHead (&alignPH);
            alignPH.pos = 0;
            juce::int64 prodPos = 0;
            int pfd = -1, perr = 0;
            void* pm = LinkShm::openRingProducer (adir, "audio_align.bin",
                                                  48000.0f, 2, pfd, perr);
            if (pm == nullptr) return { 99999, 99999 };
            std::vector<float> zL ((size_t) bs, 0.0f), zR ((size_t) bs, 0.0f);
            auto produce = [&](float imp)
            {
                zL[0] = imp; zR[0] = imp;
                const float* p[2] = { zL.data(), zR.data() };
                LinkShm::ringStampPublish (pm,
                    LinkShm::loadRelaxed (&LinkShm::ringHeader (pm)->writeIdx),
                    prodPos);
                LinkShm::ringProduce (pm, p, 2, bs);
                prodPos += bs;
                zL[0] = zR[0] = 0.0f;
            };
            juce::AudioBuffer<float> blk (2, bs);
            juce::MidiBuffer midi;
            auto process = [&](float mainImp, float* sink)
            {
                blk.clear();
                blk.setSample (0, 0, mainImp);
                blk.setSample (1, 0, mainImp);
                mainProc.processBlock (blk, midi);
                alignPH.pos += bs;
                if (sink != nullptr)
                    for (int i2 = 0; i2 < bs; ++i2)
                        sink[(size_t) i2] = blk.getSample (0, i2);
            };
            if (mode == 3)
            {
                // FIELD: connected first; the discard arm keeps backlog ~0
                // for a while before the engage. Clocks stay in step.
                EchoJayAlignTestAccess::connect (mainProc, 2, "audio_align.bin",
                                                 "Align Probe", 48000.0f, "uid-al");
                for (int i2 = 0; i2 < 40; ++i2) { produce (0); process (0, nullptr); }
            }
            else
            {
                for (int i2 = 0; i2 < 12000 / bs + 2; ++i2)
                { produce (0.0f); alignPH.pos += bs; }   // clocks stay in step
                prodPos = alignPH.pos;                   // (re-sync exactly)
                if (mode == 2) { produce (0.0f); prodPos -= bs; }
                EchoJayAlignTestAccess::connect (mainProc, 2, "audio_align.bin",
                                                 "Align Probe", 48000.0f, "uid-al");
            }
            mainProc.borrowEngageBegin ("uid-al", "lease-al", true, true);
            mainProc.borrowMuteConfirmedOnce_.store (true, std::memory_order_relaxed);
            if (mode == 4)
                for (int i2 = 0; i2 < 1024 / bs; ++i2)
                { produce (0.0f); prodPos -= bs; }       // KNOWN +1024 delay:
                                                         // extra content, the
                                                         // producer clock says
                                                         // it never happened
            auto cycle = [&](float ringImp, float mainImp, float* sink)
            {
                if (mode != 1) produce (ringImp);
                process (mainImp, sink);
                if (mode == 1) produce (ringImp);
            };
            for (int i2 = 0; i2 < (16384 * 2) / bs; ++i2) cycle (0, 0, nullptr);
            if (mode == 5)
            {
                // The relocate: both clocks jump together, as a transport
                // reposition moves every plugin's playhead at once.
                alignPH.pos += 20000;
                prodPos     += 20000;
            }
            for (int i2 = 0; i2 < (16384 * 1) / bs; ++i2) cycle (0, 0, nullptr);
            const int capN = ((16384 * 3) / bs) * bs;
            std::vector<float> out ((size_t) capN, 0.0f);
            int w = 0;
            cycle (0.5f, 1.0f, out.data()); w = bs;
            while (w + bs <= capN) { cycle (0, 0, out.data() + w); w += bs; }
            int iPass = -1, iInj = -1;
            for (int i2 = 0; i2 < capN; ++i2)
            {
                const float a = std::abs (out[(size_t) i2]);
                if (a > 0.75f && iPass < 0) iPass = i2;
                else if (a > 0.25f && a < 0.75f && iInj < 0) iInj = i2;
            }
            if (iPass >= 0 && iInj < 0
                && std::abs (out[(size_t) iPass]) > 1.25f)
                iInj = iPass;
            const juce::int64 skew = mainProc.borrowAlignSkew_.load();
            mainProc.borrowRelease (false);
            EchoJayAlignTestAccess::disconnect (mainProc, 2);
            mainProc.setBorrowBudgetActive (false);
            mainProc.setPlayHead (nullptr);
            juce::File (adir + "audio_align.bin").deleteFile();
            return { (iPass >= 0 && iInj >= 0) ? iInj - iPass : 99999, skew };
        };
        for (const int bs : { 512, 128, 1024 })
        {
            const auto a0 = alignDelta (bs, 0);
            std::printf ("  [align bs=%d ideal] delta=%+d skew=%+lld\n",
                         bs, a0.delta, (long long) a0.skew);
            check (std::abs (a0.delta) <= 2,
                   "bs=" + juce::String (bs)
                     + ": the injection lands WITH the passthrough",
                   "delta=" + juce::String (a0.delta) + " (positive = LATE)");
            check (std::abs (a0.skew) <= 2,
                   "bs=" + juce::String (bs)
                     + ": the instrument agrees (skew ~ 0)",
                   "skew=" + juce::String ((juce::int64) a0.skew));
            const auto a1 = alignDelta (bs, 1);
            const auto a2 = alignDelta (bs, 2);
            std::printf ("  [align bs=%d main-first] delta=%+d skew=%+lld\n",
                         bs, a1.delta, (long long) a1.skew);
            std::printf ("  [align bs=%d producer-ahead] delta=%+d skew=%+lld\n",
                         bs, a2.delta, (long long) a2.skew);
            const auto a3 = alignDelta (bs, 3);
            std::printf ("  [align bs=%d FIELD-SEQUENCE] delta=%+d skew=%+lld"
                         "  (pre-fix this read -1024/-1024)\n",
                         bs, a3.delta, (long long) a3.skew);
            check (std::abs (a3.delta) <= 2 && std::abs (a3.skew) <= 2,
                   "bs=" + juce::String (bs)
                     + ": THE FIELD SEQUENCE is aligned — the seek builds "
                       "the cushion the trim could not",
                   "delta=" + juce::String (a3.delta)
                     + " skew=" + juce::String ((juce::int64) a3.skew));
            check (a3.skew == (juce::int64) a3.delta,
                   "bs=" + juce::String (bs)
                     + ": and instrument == impulse holds at 0/0");
            const auto a4 = alignDelta (bs, 4);
            std::printf ("  [align bs=%d known +1024] delta=%+d skew=%+lld\n",
                         bs, a4.delta, (long long) a4.skew);
            check (std::abs (a4.delta) <= 2 && std::abs (a4.skew) <= 2,
                   "bs=" + juce::String (bs)
                     + ": a real +1024 content shift is SEEN and ABSORBED "
                       "by the measured-age pad",
                   "delta=" + juce::String (a4.delta)
                     + " skew=" + juce::String ((juce::int64) a4.skew));
            const auto a5 = alignDelta (bs, 5);
            std::printf ("  [align bs=%d relocate +20000] delta=%+d skew=%+lld\n",
                         bs, a5.delta, (long long) a5.skew);
            check (std::abs (a5.delta) <= 2 && std::abs (a5.skew) <= 2,
                   "bs=" + juce::String (bs)
                     + ": a mid-session relocate re-seeks and heals "
                       "(the -20903 excursion's shape)",
                   "delta=" + juce::String (a5.delta)
                     + " skew=" + juce::String ((juce::int64) a5.skew));
        }

        // ---- 30 Aug REGRESSIONS from the stamp seek, gated as LEVEL ----
        // The second runaway on this path; the first (uninitialised
        // buffer) arrived because there was no level assertion. There is
        // one now — extended to STOPPED TRANSPORT and ENGAGE, as ordered.
        std::printf ("== §8 stopped-transport hold + engage transient ==\n");
        {
            const int bs = 512;
            int errA = 0;
            const juce::String adir = LinkShm::resolveDir (errA);
            mainProc.setBorrowBudgetActive (true);
            mainProc.prepareToPlay (48000.0, bs);
            mainProc.setPlayHead (&alignPH);
            alignPH.pos = 0; alignPH.playing = true;
            juce::int64 prodPos = 0;
            int pfd = -1, perr = 0;
            void* pm = LinkShm::openRingProducer (adir, "audio_align.bin",
                                                  48000.0f, 2, pfd, perr);
            check (pm != nullptr, "regression rig: ring mapped");
            long phase = 0;
            std::vector<float> sL ((size_t) bs), sR ((size_t) bs);
            auto produceSine = [&](juce::int64 stampPos)
            {
                for (int i = 0; i < bs; ++i)
                    sL[(size_t) i] = sR[(size_t) i]
                        = 0.5f * std::sin (0.03f * (float) (phase + i));
                const float* p[2] = { sL.data(), sR.data() };
                LinkShm::ringStampPublish (pm,
                    LinkShm::loadRelaxed (&LinkShm::ringHeader (pm)->writeIdx),
                    stampPos);
                LinkShm::ringProduce (pm, p, 2, bs);
            };
            juce::AudioBuffer<float> blk (2, bs);
            juce::MidiBuffer midi;
            float peak = 0.0f;
            auto processSine = [&]
            {
                for (int i = 0; i < bs; ++i)
                {
                    const float v = 0.5f * std::sin (0.03f * (float) (phase + i));
                    blk.setSample (0, i, v);
                    blk.setSample (1, i, v);
                }
                phase += bs;
                mainProc.processBlock (blk, midi);
                if (alignPH.playing) alignPH.pos += bs;
                for (int i = 0; i < bs; ++i)
                    peak = juce::jmax (peak,
                                       std::abs (blk.getSample (0, i)));
            };
            EchoJayAlignTestAccess::connect (mainProc, 2, "audio_align.bin",
                                             "Align Probe", 48000.0f, "uid-al");
            mainProc.borrowEngageBegin ("uid-al", "lease-rg", true, true);
            // ENGAGE TRANSIENT: no confirmation yet — the injection must
            // hold at SILENCE (the +6dB double lived in this window).
            peak = 0.0f;
            for (int c = 0; c < 60; ++c)
            { produceSine (prodPos); prodPos += bs; processSine(); }
            check (mainProc.borrowCtxMixNow() < 0.001f,
                   "before the mute confirms, the injection holds at silence",
                   juce::String (mainProc.borrowCtxMixNow(), 3));
            check (peak <= 0.55f,
                   "and the output is the passthrough alone",
                   juce::String (peak, 3));
            mainProc.borrowMuteConfirmedOnce_.store (true, std::memory_order_relaxed);
            peak = 0.0f;
            for (int c = 0; c < 120; ++c)
            { produceSine (prodPos); prodPos += bs; processSine(); }
            check (mainProc.borrowCtxMixNow() > 0.99f,
                   "after confirmation it ramps to unity, once",
                   juce::String (mainProc.borrowCtxMixNow(), 3));
            check (peak <= 1.03f,
                   "and never exceeds unity through the ramp",
                   juce::String (peak, 3));
            // STOPPED TRANSPORT: playhead parked, producer keeps writing
            // with a parked stamp — the field loop's exact shape. The
            // seek must HOLD: bounded output, NO growth in the measured
            // age over 300 blocks.
            const int ageAtStop = mainProc.borrowRingAgeMeasured_.load();
            alignPH.playing = false;
            peak = 0.0f;
            // The field shape: stopped, the Link KEEPS WRITING but STOPS
            // STAMPING ("no host position this block = no stamp") — the
            // stale mapping is what made the age shrink every block and
            // the detector chase a frozen target: the loop.
            auto produceUnstamped = [&]
            {
                for (int i = 0; i < bs; ++i)
                    sL[(size_t) i] = sR[(size_t) i]
                        = 0.5f * std::sin (0.03f * (float) (phase + i));
                const float* p[2] = { sL.data(), sR.data() };
                LinkShm::ringProduce (pm, p, 2, bs);
            };
            for (int c = 0; c < 300; ++c)
            { produceUnstamped(); processSine(); }
            check (peak <= 1.03f,
                   "STOPPED: output stays bounded over 300 blocks",
                   juce::String (peak, 3));
            check (mainProc.borrowRingAgeMeasured_.load() == ageAtStop,
                   "STOPPED: the measured age HOLDS - no growth, no chase",
                   juce::String (mainProc.borrowRingAgeMeasured_.load())
                     + " vs " + juce::String (ageAtStop));
            // RESUME: the first fresh stamp re-seeks once and re-aligns.
            alignPH.playing = true;
            peak = 0.0f;
            for (int c = 0; c < 120; ++c)
            { produceSine (prodPos); prodPos += bs; processSine(); }
            check (std::abs (mainProc.borrowRingAgeMeasured_.load() - 1024) <= 2,
                   "RESUME: one re-seek lands the age back on target",
                   juce::String (mainProc.borrowRingAgeMeasured_.load()));
            check (peak <= 1.03f, "and the resume itself stays bounded",
                   juce::String (peak, 3));
            mainProc.borrowRelease (false);
            EchoJayAlignTestAccess::disconnect (mainProc, 2);
            mainProc.setBorrowBudgetActive (false);
            mainProc.setPlayHead (nullptr);
            juce::File (adir + "audio_align.bin").deleteFile();
        }
        mainProc.setBorrowBudgetActive (false);
        // The level arm alone can false-pass on a fresh process (OS pages
        // arrive zeroed; Sean's garbage came from recycled heap), so the
        // two causes are ALSO pinned: the drain runs for every live
        // consumer, and the injection source can never hold garbage.
        {
            std::ifstream f8 ("Source/PluginProcessor.cpp");
            std::stringstream s8; s8 << f8.rdbuf();
            const juce::String pp9 (s8.str());
            // 27 Aug: LISTEN deleted — the drain gates on the session
            // alone (its consumers, injection and automatic fallback,
            // cover every session state between them).
            check (pp9.contains ("if (li == borrowSession_.ringSlot.load(std::memory_order_relaxed))"),
                   "the ring drain runs for EVERY session state");
            // 26 Aug: in-context must not depend on the MAIN's channel
            // type — the route fork belongs to SOLO alone.
            {
                const int at = pp9.indexOf ("const bool ctxNow");
                const int semi = at >= 0 ? pp9.indexOf (at, ";") : -1;
                const auto decl = (at >= 0 && semi > at)
                                      ? pp9.substring (at, semi)
                                      : juce::String();
                check (at >= 0 && ! decl.contains ("borrowThrough"),
                       "ctxNow is independent of the main's channel type");
            }
            check (pp9.contains ("applyBorrowSoloMixOn(buffer, fallbackSolo);\n"),
                   "the through-solo site keys on the automatic fallback");
            check (! pp9.contains ("audioOn.load")
                     || juce::String (pp9).indexOf ("borrowSession_.audioOn") < 0,
                   "no LISTEN flag survives in the borrow session");
            // Crackle redesign pins: the passthrough delay is CONSTANT;
            // the pad lives on the injection's own line.
            check (pp9.contains ("alignPost_.process(buffer, kBorrowAlignBudgetFrames);"),
                   "the passthrough delay is constant - the mix cannot click");
            check (pp9.contains ("A pad change interrupts ONLY the injection"),
                   "pad changes land on the injection alone");
            // Closed loop pins: live state published, watchdog drops+names.
            check (pp9.contains ("mute UNCONFIRMED"),
                   "the main's watchdog drops an unconfirmed mute, named");
            {
                std::ifstream lf9 ("Source/LinkProcessor.cpp");
                std::stringstream ls9; ls9 << lf9.rdbuf();
                const juce::String lp10 (ls9.str());
                check (lp10.contains ("rc.muteEngaged = linkMuteWanted();"),
                       "the Link publishes its LIVE mute state (closed "
                       "loop) - now the COMPOSED actual (spec §2)");
            }
            check (pp9.contains ("borrowBuf_.clear();   // setSize leaves contents UNDEFINED"),
                   "the injection source is cleared at allocation, always");
            // Mute/solo (27 Aug 2026) pins: ONE writer for the suppression
            // (the registry pass), computed from LIVE rows with the edited
            // rack exempt; both transition banners exist; the contextual
            // honest-limit line is conditional on an ACTIVE solo, and the
            // old-binary count rides it as a warning.
            check (pp9.contains ("const bool suppress = borrowActive()\n            && (editedMuted || (anySolo && ! editedIn));"),
                   "suppression = EVERY silence reason (edited mute OR "
                   "foreign solo), mute winning the OR");
            check (pp9.contains ("is soloed - your edit is muted with the other")
                     && pp9.contains ("is muted - your edit is muted with its strip")
                     && pp9.contains ("Your edit is audible again."),
                   "all three honorary-strip transition banners exist");
            {
                std::ifstream fe9 ("Source/PluginEditor.cpp");
                std::stringstream se9; se9 << fe9.rdbuf();
                const juce::String ed9 (se9.str());
                check (ed9.contains ("if (processorRef.soloSetActive_)")
                         && ed9.contains ("Solo mutes Link channels only"),
                       "the honest-limit line is CONTEXTUAL (amendment)");
                check (ed9.contains ("Link(s) predate solo and keep playing"),
                       "the old-binary warning rides the same line");
                check (ed9.contains ("predates mute/solo - reinstall"),
                       "an incapable target is refused WITH the reason");
            }
        }
        // The pad arithmetic, pure: fits, exact fit, refuse-over-budget.
        const int head = EchoJayProcessor::kBorrowAlignBudgetFrames - 1024;
        check (EchoJayProcessor::alignPad (0)
                   == EchoJayProcessor::kBorrowAlignBudgetFrames - 1024,
               "pad math: empty chain uses the full headroom");
        check (EchoJayProcessor::alignPad (head) == 0,
               "pad math: an exact fit pads zero");
        check (EchoJayProcessor::alignPad (head + 1) == -1,
               "pad math: one frame over the headroom REFUSES");

        // ---- §8 pins: the mute rides the lease, mode-gated; the Link
        // ---- mutes AFTER the ring write; the restore arm clears it.
        auto slurp8 = [] (const char* f)
        { std::ifstream ff (f); std::stringstream ss; ss << ff.rdbuf();
          return juce::String (ss.str()); };
        const auto pp8 = slurp8 ("Source/PluginProcessor.cpp");
        check (pp8.contains ("o->setProperty(\"muteOut\","),
               "muteOut rides the lease renew");
        const auto lp8 = slurp8 ("Source/LinkProcessor.cpp");
        const int prodAt = lp8.indexOf ("LinkShm::ringProduce(shmMap");
        const int muteAt = lp8.indexOf ("IN-CONTEXT MUTE, strictly AFTER");
        check (prodAt >= 0 && muteAt > prodAt,
               "the Link mutes strictly AFTER the ring write");
        check (lp8.contains ("rackLeaseMuteWant_.store(false"),
               "the one restore path clears the mute want");
        check (lp8.contains ("rc.inContextCapable     = true;"),
               "the Link announces in-context capability");
        check (slurp8 ("Source/PluginEditor.cpp")
                   .contains ("can't hand the mix over"),
               "the incapable Link gets the named solo fallback line");
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
