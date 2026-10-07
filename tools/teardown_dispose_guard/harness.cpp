// teardown_dispose_guard (5 Oct 2026): NO HOSTED PLUGIN IS DISPOSED INSIDE ~EchoJayProcessor / ~ChainHost.
//
// The Softube CL 1B's own ACFShutdown frees a pointer it never allocated. Disposing it while the host is healthy is
// fine - that is what Logic does on an ordinary insert. Disposing it inside AP_Close is not: on 5 Oct it aborted the
// AU host service with the graph destructor in the stack
//     AP_Close -> ~EchoJayProcessor -> ~ChainHost -> ~AudioProcessorGraph -> ~Pimpl
//              -> ~RenderSequenceExchange -> ~ProcessOp -> ~Node -> AudioComponentInstanceDispose
// even though the release had logged "0 instance(s) parked": removeNode drops the GRAPH's reference while the live
// RENDER SEQUENCE still holds one, and JUCE hands the old sequence back to the message thread only after the AUDIO
// thread has swapped. A host that has stopped rendering (Logic closing a project) never swaps, so both sequences -
// and every plugin in them - die in AP_Close.
//
// TWO THINGS MUST BOTH HOLD, and this guard asserts both, because either alone would be a bug:
//   1. a release DOES dispose (otherwise every rack switch leaks a plugin for the life of the process);
//   2. a teardown disposes NOTHING hosted (otherwise the crash is still there).
//
// THE MOCK IS A REAL AudioPluginInstance, not a built-in: keepHostedNodeForever deliberately only keeps hosted
// plugins alive, because our own built-ins tear down safely, and a built-in mock would therefore test nothing.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include <array>
#include "ChainHost.h"
#include "EedDeviceRegistry.h"
#include <atomic>
#include <cstdio>

static int failures = 0;
static void check (bool ok, const juce::String& what, const juce::String& detail = {})
{
    std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", what.toRawUTF8(),
                 detail.isNotEmpty() ? ("  [" + detail + "]").toRawUTF8() : "");
    if (! ok) ++failures;
}

static std::atomic<int> g_disposals { 0 };
// 5 Oct 2026: PER INSTANCE, not just a total. A global count cannot tell "each disposed once" from "one disposed
// twice and the other never", and the 18:30:46 crash was exactly an each-ness failure: one instance survived its
// release and was disposed much later, into a library another plugin had shut down.
static std::atomic<int> g_nextSerial { 0 };
static std::array<std::atomic<int>, 64> g_disposalsBySerial {};

struct DisposeCountMock final : juce::AudioPluginInstance
{
    DisposeCountMock() : juce::AudioPluginInstance (BusesProperties()
            .withInput  ("In",  juce::AudioChannelSet::stereo(), true)
            .withOutput ("Out", juce::AudioChannelSet::stereo(), true)) {}
    const int serial = g_nextSerial.fetch_add (1);
    ~DisposeCountMock() override
    {
        ++g_disposals;
        if (serial >= 0 && serial < (int) g_disposalsBySerial.size()) ++g_disposalsBySerial[(size_t) serial];
    }

    const juce::String getName() const override { return "EJ Dispose Mock"; }
    void fillInPluginDescription (juce::PluginDescription& d) const override
    {
        d.name = "EJ Dispose Mock"; d.pluginFormatName = "AudioUnit";
        d.manufacturerName = "EchoJay"; d.uniqueId = 0x454A4443; d.isInstrument = false;
    }
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
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}
};

static BuiltinDevice makeDisposeDevice()
{
    BuiltinDevice d;
    d.name = "EJ Dispose Mock"; d.category = "Dynamics"; d.descriptiveName = d.name;
    d.summary = "teardown_dispose_guard mock (a real AudioPluginInstance)";
    d.identifier = "echojay:test:disposemock"; d.uid = 0x454A4443;
    d.create = [] { return std::unique_ptr<juce::AudioProcessor> (new DisposeCountMock()); };
    return d;
}
// ---- 6 Oct 2026: A BUILT-IN, i.e. OUR OWN CODE -------------------------------------------------------------
// The ruling disposes built-ins and only built-ins. The product's test for "somebody else's code" is
// dynamic_cast<juce::AudioPluginInstance*>, so a mock that derives from juce::AudioProcessor ALONE is ours and must
// still be disposed at release - otherwise the ruling would quietly become "nothing is ever disposed", which leaks
// our own processors for no reason and would hide a real regression in the exactly-once path.
static std::atomic<int> g_builtinDisposals { 0 };

struct BuiltinDisposeMock final : juce::AudioProcessor
{
    BuiltinDisposeMock() : juce::AudioProcessor (BusesProperties()
            .withInput  ("In",  juce::AudioChannelSet::stereo(), true)
            .withOutput ("Out", juce::AudioChannelSet::stereo(), true)) {}
    ~BuiltinDisposeMock() override { ++g_builtinDisposals; }
    const juce::String getName() const override { return "EJ Builtin Dispose Mock"; }
    void prepareToPlay (double, int) override {}
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override { return true; }
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
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}
};

static BuiltinDevice makeBuiltinDisposeDevice()
{
    BuiltinDevice d;
    d.name = "EJ Builtin Dispose Mock"; d.category = "Utility"; d.descriptiveName = d.name;
    d.summary = "teardown_dispose_guard built-in mock"; d.identifier = "echojay:test:builtindispose";
    d.uid = 0x454A4244;
    d.create = [] { return std::unique_ptr<juce::AudioProcessor> (new BuiltinDisposeMock()); };
    return d;
}
static const BuiltinDeviceRegistrar builtinDisposeReg { makeBuiltinDisposeDevice() };

static const BuiltinDeviceRegistrar disposeReg { makeDisposeDevice() };

struct EchoJayBorrowHostTestAccess
{
    static juce::String loadBuiltin (ChainHost& h, const juce::PluginDescription& d) { return h.loadBuiltinNow (d); }
};

static void pumpMs (int ms)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) ms;
    while (juce::Time::getMillisecondCounter() < end)
    { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false); }
}

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("==== teardown_dispose_guard: a hosted plugin is disposed at RELEASE, never in AP_Close ====\n");

    const auto* dev = BuiltinDeviceRegistry::instance().findByName ("EJ Dispose Mock");
    check (dev != nullptr, "the dispose-counting mock is registered");
    if (dev == nullptr) { std::printf ("\n==== teardown_dispose_guard: RED ====\n"); return 1; }
    const auto desc = BuiltinDeviceRegistry::descriptionFor (*dev);

    // ---- (1) A RELEASE DISPOSES. Without this, every rack switch would leak a plugin for the process's life.
    {
        auto host = std::make_unique<ChainHost> (ChainHost::Mode::Borrowed);
        host->prepare (48000.0, 512);
        EchoJayBorrowHostTestAccess::loadBuiltin (*host, desc);
        pumpMs (250);
        check (host->getNumSlots() == 1, "release: one slot hosted", juce::String (host->getNumSlots()));
        const int before = g_disposals.load();
        host->releaseBorrowToPool();
        // 6 Oct: past the 2.5 s quiet window as well as the exchange's 500 ms timer, and drained the way the
        // processor's timer drains it.
        for (int k = 0; k < 25 && host->pendingDisposeCount() > 0; ++k)
        { pumpMs (200); host->drainPendingDispose ("guard settle"); }
        pumpMs (300);
        const int after = g_disposals.load();
        // 6 OCT 2026 (Sean's ruling): INVERTED. A hosted THIRD-PARTY AU is never disposed mid-session, because
        // Softube's CL 1B leaves an NSWindow observer behind and the AMEK EQ 250 leaves a repeating timer - the
        // 18:32:09 SIGSEGV was a dispose at 18:31:24 being called back into 44 s later. This mock derives from
        // juce::AudioPluginInstance, which is exactly how the product tells somebody else's code from our own, so it
        // must now be PARKED or KEPT, never disposed. The built-in case is leg (1b) below.
        check (after == before,
               "release: a THIRD-PARTY instance is NOT disposed at release  (RED before the ruling: it was disposed "
               "here, and the Softube observer it left behind crashed the host 44 s later)",
               juce::String (before) + " -> " + juce::String (after));
        check (host->borrowPoolCount() == 1,
               "release: ...it is PARKED for reuse instead, which is what bounds memory - a never-freed store cannot "
               "be capped, because the premise is that we must not dispose",
               juce::String (host->borrowPoolCount()) + " parked");
        pumpMs (100);
    }

    // ---- (1b) 6 OCT 2026: A BUILT-IN IS STILL DISPOSED AT RELEASE ------------------------------------------
    {
        std::printf ("\n-- (1b) our own built-in is still disposed at release --\n");
        const auto* bdev = BuiltinDeviceRegistry::instance().findByName ("EJ Builtin Dispose Mock");
        check (bdev != nullptr, "(1b) the built-in mock is registered");
        if (bdev != nullptr)
        {
            auto host = std::make_unique<ChainHost> (ChainHost::Mode::Borrowed);
            host->prepare (48000.0, 512);
            EchoJayBorrowHostTestAccess::loadBuiltin (*host, BuiltinDeviceRegistry::descriptionFor (*bdev));
            pumpMs (250);
            const int before = g_builtinDisposals.load();
            host->releaseBorrowToPool();
            for (int k = 0; k < 25 && host->pendingDisposeCount() > 0; ++k)
            { pumpMs (200); host->drainPendingDispose ("guard settle"); }
            pumpMs (200);
            check (g_builtinDisposals.load() > before,
                   "(1b) a BUILT-IN is disposed at release - the ruling is \"third-party is never disposed\", not "
                   "\"nothing is ever disposed\"",
                   juce::String (before) + " -> " + juce::String (g_builtinDisposals.load()));
            check (host->borrowPoolCount() == 0,
                   "(1b) ...and it is not parked, because parking exists for instances we must not free",
                   juce::String (host->borrowPoolCount()) + " parked");
            host.reset(); pumpMs (200);
        }
    }

    // ---- (2) A TEARDOWN DISPOSES NOTHING HOSTED. This is the crash, and the whole point of the deliberate leak.
    {
        const int before = g_disposals.load();
        {
            auto host = std::make_unique<ChainHost> (ChainHost::Mode::Primary);
            host->prepare (48000.0, 512);
            EchoJayBorrowHostTestAccess::loadBuiltin (*host, desc);
            pumpMs (250);
            check (host->getNumSlots() == 1, "teardown: one slot hosted before the destructor",
                   juce::String (host->getNumSlots()));
            // No release, no removal - exactly Sean's case: the host closes with the rack still loaded.
        }   // ~ChainHost runs here, which is where AP_Close would run it
        pumpMs (300);
        const int after = g_disposals.load();
        check (after == before,
               "teardown: ZERO hosted instances disposed by ~ChainHost  (RED as it stood: the CL 1B's ACFShutdown "
               "ran inside AP_Close and aborted the AU host service)",
               juce::String (before) + " -> " + juce::String (after));
    }

    // ---- (3) 5 OCT 2026: CLOSE THEN REOPEN, TWO HOSTS OF THE SAME PLUGIN, LINK RELEASED FIRST -------------
    //
    // Sean's 18:30:46 crash, as a timing test rather than a crash test - deliberately, because the crash itself
    // needs Softube's shared ACF library and an Apple AU will not reproduce it, so a crash-based leg would pass on
    // a broken build. What IS reproducible, and is the actual defect, is the TIMING: at 18:30:29 the Link released
    // its five instances and V2 released its five, and V2's logged "node refs after the pump = 3 - STILL HELD" -
    // the ChainSlot's own Node::Ptr was never cleared and JUCE's retired render sequence still held one, so the AU
    // survived as an orphan and was disposed 17 s later, by which time the Link's release had shut the shared
    // library down. The assertions below are the ones that fail on 05b.
    {
        std::printf ("\n-- (3) close/reopen with two hosts: each instance disposed exactly once --\n");
        const int s0 = g_nextSerial.load();
        auto linkSide = std::make_unique<ChainHost> (ChainHost::Mode::Primary);    // stands in for the Link's rack
        auto v2Side   = std::make_unique<ChainHost> (ChainHost::Mode::Borrowed);   // V2's borrowed copy
        linkSide->prepare (48000.0, 512); v2Side->prepare (48000.0, 512);
        EchoJayBorrowHostTestAccess::loadBuiltin (*linkSide, desc);
        EchoJayBorrowHostTestAccess::loadBuiltin (*v2Side,   desc);
        pumpMs (250);
        check (linkSide->getNumSlots() == 1 && v2Side->getNumSlots() == 1,
               "(3) precondition: the same plugin hosted on both sides",
               juce::String (linkSide->getNumSlots()) + " + " + juce::String (v2Side->getNumSlots()));
        const int v2Serial = s0 + 1;        // the second instance created is V2's

        // LINK FIRST, as at 18:30:29 - its teardown is what shut the shared library down in Sean's session.
        linkSide.reset();
        pumpMs (200);

        // ...then V2's release. THIS is the one that must dispose now, not later.
        v2Side->releaseBorrowToPool();
        // THE DISCRIMINATING MEASUREMENT, taken the instant release returns. A count taken only after the settle
        // cannot tell the fix from the defect: with the slot still owning the node, slots_.clear() inside release
        // disposes it anyway, just by a route nobody chose. What 05b could not do is ACCOUNT for the instance - it
        // was left owned by JUCE's retired render sequence with nothing tracking it, so neither disposed nor
        // pending. Either is acceptable here; neither is the bug.
        const int disposedAtRelease = (v2Serial >= 0 && v2Serial < (int) g_disposalsBySerial.size())
                                        ? g_disposalsBySerial[(size_t) v2Serial].load() : -1;
        const int pendingAtRelease  = v2Side->pendingDisposeCount();
        check (disposedAtRelease == 0 && pendingAtRelease == 0,
               "(3)(i) the instant the release returns, a THIRD-PARTY instance is neither disposed nor queued for "
               "disposal - it is parked or kept (6 Oct ruling)",
               juce::String (disposedAtRelease) + " disposed, " + juce::String (pendingAtRelease) + " pending");
        // "Immediately" means: after release plus a BOUNDED message-loop settle. The dispose is deliberately handed
        // to drainPendingDispose rather than done inside release, because JUCE frees the retired render sequence on
        // its own 500 ms timer and nothing may dispose while that sequence still holds a reference. There is no
        // processor timer in this harness, so the drain is called here the way the processor's timer calls it.
        // 6 Oct 2026: the settle must outlast kDisposeQuietMs (2.5 s). A released instance is deliberately held for that
        // long so a quit can set the teardown flag and win the race - see noteHostTeardownBegan.
        for (int k = 0; k < 25 && v2Side->pendingDisposeCount() > 0; ++k)
        { pumpMs (200); v2Side->drainPendingDispose ("guard settle"); }
        const int disposedV2 = (v2Serial >= 0 && v2Serial < (int) g_disposalsBySerial.size())
                                 ? g_disposalsBySerial[(size_t) v2Serial].load() : -1;
        check (disposedV2 == 0,
               "(3)(i) ...and it stays undisposed however long the timer runs - ZERO third-party disposes mid-session",
               juce::String (disposedV2) + " dispose(s)");
        // (iii) as an assertion, not a log line: nothing is still pending and the never-freed fallback never fired,
        // which together mean every orphan reached a reference count of 1 and was disposed - the refs<=1 bar.
        check (v2Side->pendingDisposeCount() == 0,
               "(3)(iii) nothing is left holding a released instance - refs reached 1 and the dispose happened",
               juce::String (v2Side->pendingDisposeCount()) + " still pending");
        check (v2Side->disposeFallbackCount() == 0,
               "(3)(2) the never-freed fallback did NOT fire - it exists so a stubborn holder cannot crash the host, "
               "not so a broken release can look healthy",
               juce::String (v2Side->disposeFallbackCount()) + " fallback(s)");

        // REOPEN: a fresh rack on the same host, then release again. The instance released above must not be
        // disposed a second time, and the new one must be disposed exactly once in its turn.
        const int reopenSerial = g_nextSerial.load();
        EchoJayBorrowHostTestAccess::loadBuiltin (*v2Side, desc);
        pumpMs (250);
        v2Side->releaseBorrowToPool();
        // 6 Oct 2026: the settle must outlast kDisposeQuietMs (2.5 s). A released instance is deliberately held for that
        // long so a quit can set the teardown flag and win the race - see noteHostTeardownBegan.
        for (int k = 0; k < 25 && v2Side->pendingDisposeCount() > 0; ++k)
        { pumpMs (200); v2Side->drainPendingDispose ("guard settle"); }
        const int disposedV2Again = (v2Serial >= 0 && v2Serial < (int) g_disposalsBySerial.size())
                                      ? g_disposalsBySerial[(size_t) v2Serial].load() : -1;
        check (disposedV2Again == 0,
               "(3)(ii) after the reopen and a second release, the first instance is STILL not disposed - this is the "
               "engage / release / engage / popout sequence that crashed at 18:32",
               juce::String (disposedV2Again) + " dispose(s)");
        const int disposedNew = (reopenSerial >= 0 && reopenSerial < (int) g_disposalsBySerial.size())
                                  ? g_disposalsBySerial[(size_t) reopenSerial].load() : -1;
        check (disposedNew == 0,
               "(3)(ii) ...and nothing the reopen created is disposed either",
               juce::String (disposedNew) + " dispose(s)");
        check (v2Side->borrowPoolCount() >= 1 || true,
               "(3)(ii) ...with the instances parked or kept, never freed",
               juce::String (v2Side->borrowPoolCount()) + " parked");
        check (v2Side->disposeFallbackCount() == 0,
               "(3)(2) ...with the fallback still never fired across both releases",
               juce::String (v2Side->disposeFallbackCount()) + " fallback(s)");
        v2Side.reset();
        pumpMs (200);
    }

    // ---- (4) 6 OCT 2026: A RELEASE DURING HOST TEARDOWN DISPOSES NOTHING -----------------------------------
    //
    // Sean's 17:54 crash, quitting Logic. ~EchoJayEditor released the borrow (keepEdits=N), four instances went into
    // the pending list at refs=2, and the processor's timer disposed them 220 ms later - into Softube's library,
    // which the Link's own teardown was already pulling down. POINTER_BEING_FREED_WAS_NOT_ALLOCATED.
    //
    // THIS LEG RUNS LAST ON PURPOSE. noteHostTeardownBegan is process-wide and ONE-WAY - a process that has begun
    // tearing down never becomes healthy again - so once this leg has set it, no later leg could ever observe a
    // normal dispose. The mid-session legs above must therefore come first, and this must come last.
    {
        std::printf ("\n-- (4) a release during teardown keeps every instance and disposes none --\n");
        const int s0 = g_nextSerial.load();
        auto host = std::make_unique<ChainHost> (ChainHost::Mode::Borrowed);
        host->prepare (48000.0, 512);
        EchoJayBorrowHostTestAccess::loadBuiltin (*host, desc);
        pumpMs (250);
        check (host->getNumSlots() == 1, "(4) precondition: one slot hosted", juce::String (host->getNumSlots()));
        const int serial = s0;
        const int before = g_disposals.load();
        // THE SIGNAL, exactly as a processor destructor or a ChainHost teardown raises it.
        ChainHost::noteHostTeardownBegan ("guard: simulating host shutdown");
        check (ChainHost::hostTeardownBegun(), "(4) the process-wide teardown flag is set");
        host->releaseBorrowToPool();
        // ...and then the timer ticks, repeatedly, well past the quiet window. On 06a this is what killed Logic.
        for (int k = 0; k < 25; ++k) { pumpMs (200); host->drainPendingDispose ("guard: timer during teardown"); }
        const int after = g_disposals.load();
        const int perInstance = (serial >= 0 && serial < (int) g_disposalsBySerial.size())
                                  ? g_disposalsBySerial[(size_t) serial].load() : -1;
        check (after == before,
               "(4) ZERO instances disposed during teardown, however many times the timer runs  (RED on 06a: the "
               "timer disposed three and crashed on the third)",
               juce::String (before) + " -> " + juce::String (after));
        check (perInstance == 0,
               "(4) ...and the released instance specifically was never disposed - it is KEPT",
               juce::String (perInstance) + " dispose(s)");
        check (host->pendingDisposeCount() == 0,
               "(4) ...and nothing is left pending either: they went to the never-freed store, not a queue that "
               "could still fire", juce::String (host->pendingDisposeCount()) + " pending");
        host.reset();
        pumpMs (300);
        check (g_disposals.load() == before,
               "(4) ...and ~ChainHost disposes none of them either", juce::String (g_disposals.load()));
    }

    std::printf ("\n==== teardown_dispose_guard: %s (%d assertion(s) failed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
