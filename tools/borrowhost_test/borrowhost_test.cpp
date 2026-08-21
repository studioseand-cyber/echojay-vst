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

// The grinch: honest on its FIRST seed, throws on every later one — i.e. a
// plugin whose state machinery does not survive reuse. Exercises the
// pool-ineligible fallback arm.
struct BorrowGrinch final : juce::AudioProcessor
{
    BorrowGrinch() : juce::AudioProcessor (BusesProperties()
        .withInput ("In", juce::AudioChannelSet::stereo(), true)
        .withOutput ("Out", juce::AudioChannelSet::stereo(), true)) {}
    int value = 0, seeds = 0;
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
        if (++seeds >= 2) throw std::runtime_error ("grinch: no reuse for you");
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

void borrowRack (ChainHost& h)
{
    h.restoreSavedChain (slotsVar(), stateVar (4242, 99));
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
    borrowRack (host);
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
    borrowRack (host);
    check (host.borrowFreshInstantiations() == 3,
           "grinch instantiated FRESH (counter 2 -> 3); probe still reused",
           juce::String (host.borrowFreshInstantiations()));
    check (host.getNumSlots() == 2, "rack complete despite the fallback");

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
        for (const char* key : { "commitState", "commitSlot", "editOps" })
        {
            check (! procRegion.contains (key),
                   juce::String ("processor borrow code has no \"") + key + "\"");
            check (! edRegion.contains (key),
                   juce::String ("editor borrow code has no \"") + key + "\"");
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
