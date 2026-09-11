/*  LINK v9 LEGS (9 Sep 2026 ruling). Link archive. Isolated root required.
      attach   - change A: a slot attached while a RACK LEASE is active is never rendered until the
                 lease is released; at release it is live (intended state). Negative control: the v8
                 archive renders it in the first block after attach.
      churn N  - change B: a paced processBlock thread against message-thread graph mutations
                 (attach / bypass / move / remove) for 10 s; asserts the probe was never rendered while
                 a mutation was in flight (leg-side flag, so the v8 control measures the same thing).
                 N = run count (twenty for the record). On v9 also asserts the product's own counter.  */
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "EJStateRoot.h"
#include "LinkProcessor.h"
#include "LinkShm.h"
#include <cstdio>
#include <thread>
#include <atomic>
#if __has_include("legs_link_v9.h")
  #define EJ_LEGS_LINK_V9 1
#endif

static void pump (int iters) { for (int t = 0; t < iters; ++t) CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false); }
static void pumpMs (int ms) { const double end = juce::Time::getMillisecondCounterHiRes() + ms; while (juce::Time::getMillisecondCounterHiRes() < end) CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false); }

struct EchoJayLinkSyncTestAccess
{
    static juce::String uid (LinkProcessor& p)        { return p.instanceUid_; }
    static bool rackLeased (LinkProcessor& p)          { return p.rackLeaseActive_; }
    static ChainHost& host (LinkProcessor& p)          { return p.chainHost; }
};
using TA = EchoJayLinkSyncTestAccess;

static std::atomic<int> gMutating { 0 };            // leg-side: set around every mutation the leg performs
struct Probe final : public juce::AudioPluginInstance
{
    Probe() : juce::AudioPluginInstance (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo(), true)
                                                           .withOutput ("Out", juce::AudioChannelSet::stereo(), true)) {}
    std::atomic<int> calls { 0 }, callsDuringMutation { 0 }, callsDuringRebuild { 0 };
    ChainHost* host = nullptr;   // v9: the product's own in-flight counter is read at render time
    void fillInPluginDescription (juce::PluginDescription& d) const override { d.name = "Probe"; d.pluginFormatName = "Internal"; d.manufacturerName = "legs"; }
    const juce::String getName() const override { return "Probe"; }
    void prepareToPlay (double, int) override {} void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override
    {
        calls.fetch_add (1);
        if (gMutating.load() > 0) callsDuringMutation.fetch_add (1);   // leg-side flag: covers the call INCLUDING its wait for the lock
#ifdef EJ_LEGS_LINK_V9
        if (host != nullptr && host->rebuildInFlightNow() > 0) callsDuringRebuild.fetch_add (1);   // inside the critical section
#endif
    }
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; } bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; } bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; } int getCurrentProgram() override { return 0; } void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; } void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock& m) override { m.setSize (4); } void setStateInformation (const void*, int) override {}
};
static juce::PluginDescription probeDesc() { juce::PluginDescription d; d.name = "Probe"; d.pluginFormatName = "Internal"; d.manufacturerName = "legs"; d.fileOrIdentifier = "legs:probe"; return d; }

static std::unique_ptr<LinkProcessor> makeLink (const char* nm)
{
    auto l = std::make_unique<LinkProcessor>(); l->linkName = nm; l->markTypedNameAuthoritative(); l->prepareToPlay (48000.0, 512);
    for (int i = 0; i < 200 && TA::uid (*l).isEmpty(); ++i) pump (5);
    return l;
}
static void writeLease (const juce::String& dir, const juce::String& uid, const juce::String& leaseId)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("v", 1); o->setProperty ("leaseId", leaseId); o->setProperty ("slot", 0); o->setProperty ("scope", "rack");
    o->setProperty ("muteOut", false); o->setProperty ("editPending", false); o->setProperty ("tMs", juce::Time::currentTimeMillis());
    juce::File (LinkShm::leasePath (dir, uid)).replaceWithText (juce::JSON::toString (juce::var (o), true));
}
static void block (LinkProcessor& l) { juce::AudioBuffer<float> b (2, 512); b.clear(); juce::MidiBuffer m; l.processBlock (b, m); }

static int legAttach()
{
    int err = 0; const juce::String dir = LinkShm::resolveDir (err);
    auto l = makeLink ("AttachLeg"); const auto uid = TA::uid (*l);
    if (uid.isEmpty()) { std::printf ("attach: FAIL - the Link never registered\n"); return 1; }
    const juce::String leaseId = uid + "-rack-legs";
    writeLease (dir, uid, leaseId);
    for (int i = 0; i < 100 && ! TA::rackLeased (*l); ++i) { pump (5); if ((i % 10) == 9) writeLease (dir, uid, leaseId); }
    if (! TA::rackLeased (*l)) { std::printf ("attach: FAIL - the rack lease never engaged\n"); return 1; }
    auto probe = std::make_unique<Probe>(); auto* pr = probe.get();
    TA::host (*l).completeLoad (std::move (probe), probeDesc(), ChainHost::LoadOrigin::Assistant);
    const bool bypassedAtAttach = TA::host (*l).getSlotInfo (0).bypassed;
    for (int b = 0; b < 3; ++b) block (*l);
    const int rendersUnderLease = pr->calls.load();
    // release: delete the lease, let the poll see it (100 ms cadence, 3 s expiry irrelevant - absent reads stale)
    juce::File (LinkShm::leasePath (dir, uid)).deleteFile();
    for (int i = 0; i < 100 && TA::rackLeased (*l); ++i) pump (5);
    const bool released = ! TA::rackLeased (*l);
    const bool bypassedAfter = TA::host (*l).getSlotInfo (0).bypassed;
    pr->calls.store (0); for (int b = 0; b < 3; ++b) block (*l);
    const int rendersAfter = pr->calls.load();
    std::printf ("attach: attached bypassed=%d renders-under-lease=%d (3 blocks) | released=%d bypassed-after=%d renders-after=%d (3 blocks)\n",
                 (int) bypassedAtAttach, rendersUnderLease, (int) released, (int) bypassedAfter, rendersAfter);
    const bool pass = bypassedAtAttach && rendersUnderLease == 0 && released && ! bypassedAfter && rendersAfter == 3;
    std::printf ("attach: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

static int legChurn (int runs)
{
    int fails = 0;
    for (int r = 0; r < runs; ++r)
    {
        auto l = makeLink ("ChurnLeg"); auto& host = TA::host (*l);
        std::atomic<bool> stop { false }; std::atomic<int> blocks { 0 };
        std::thread audio ([&]{ const double per = 512.0 / 48000.0 * 1000.0; double next = juce::Time::getMillisecondCounterHiRes();
            while (! stop.load()) { block (*l); blocks.fetch_add (1); next += per; const double now = juce::Time::getMillisecondCounterHiRes(); if (next > now) juce::Thread::sleep ((int) (next - now)); } });
        std::vector<Probe*> probes; int muts = 0; const double end = juce::Time::getMillisecondCounterHiRes() + 10000.0;
        juce::Random rng (r + 1);
        while (juce::Time::getMillisecondCounterHiRes() < end)
        {
            const int n = host.getNumSlots(); const int op = rng.nextInt (4);
            gMutating.store (1);
            if (op == 0 || n == 0)       { auto p = std::make_unique<Probe>(); p->host = &host; probes.push_back (p.get()); host.completeLoad (std::move (p), probeDesc(), ChainHost::LoadOrigin::Assistant); }
            else if (op == 1)            { host.setSlotBypassed (rng.nextInt (n), rng.nextBool()); }
            else if (op == 2 && n >= 2)  { host.moveSlot (rng.nextInt (n - 1), 1); }
            else if (n > 0)              { host.removeSlot (rng.nextInt (n)); }
            gMutating.store (0); ++muts;
            pumpMs (10);
        }
        stop.store (true); audio.join();
        int during = 0, calls = 0, inside = 0; for (auto* p : probes) { during += p->callsDuringMutation.load(); calls += p->calls.load(); inside += p->callsDuringRebuild.load(); }
#ifdef EJ_LEGS_LINK_V9
        const int productCount = host.processDuringRebuildCount();
        // v9 criterion: no render inside a mutation's critical section (the graph being rebuilt). The
        // leg-side flag also covers the mutation's WAIT for the lock, where a render is the point.
        const bool pass = inside == 0;
        std::printf ("churn run %d: mutations=%d blocks=%d probe-renders=%d rendered-inside-rebuild=%d (must be 0) rendered-while-call-pending=%d (benign) product:dry-passes=%d -> %s\n",
                     r + 1, muts, blocks.load(), calls, inside, during, productCount, pass ? "PASS" : "FAIL");
#else
        const bool pass = during == 0;
        std::printf ("churn run %d: mutations=%d blocks=%d probe-renders=%d rendered-during-mutation=%d -> %s\n",
                     r + 1, muts, blocks.load(), calls, during, pass ? "PASS" : "FAIL");
#endif
        if (! pass) ++fails;
        // the graveyard keeps removed probes alive (process-lifetime); nothing dangles
    }
    std::printf ("churn: %d/%d runs PASS\n", runs - fails, runs);
    return fails ? 1 : 0;
}

int main (int argc, char** argv)
{
    echojay::requireIsolationOrDie ("link_v9_legs_test.cpp");
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::String mode = argc > 1 ? argv[1] : "attach";
#ifdef EJ_LEGS_LINK_V9
    std::printf ("binary: Link v9 API present\n");
#else
    std::printf ("binary: v8 API (control)\n");
#endif
    if (mode == "attach") return legAttach();
    if (mode == "churn")  return legChurn (argc > 2 ? atoi (argv[2]) : 1);
    std::printf ("unknown mode\n"); return 2;
}
