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
        pumpMs (600);        // the exchange timer runs at 500 ms; the pump has already forced the swap
        const int after = g_disposals.load();
        check (after > before,
               "release: the hosted instance IS disposed at release, on the message thread  (RED as it stood: "
               "removeNode dropped the graph's reference while the render sequence still held one, so the dispose "
               "was deferred into AP_Close)",
               juce::String (before) + " -> " + juce::String (after));
        pumpMs (100);
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
        check (disposedAtRelease == 1 || pendingAtRelease >= 1,
               "(3)(i) the instant the release returns, the instance is either disposed or OWNED BY US awaiting "
               "disposal  (RED on 05b: neither - it was left owned by JUCE's retired render sequence, untracked, and "
               "disposed 17 s later into a library the Link's own release had shut down)",
               juce::String (disposedAtRelease) + " disposed, " + juce::String (pendingAtRelease) + " pending");
        // "Immediately" means: after release plus a BOUNDED message-loop settle. The dispose is deliberately handed
        // to drainPendingDispose rather than done inside release, because JUCE frees the retired render sequence on
        // its own 500 ms timer and nothing may dispose while that sequence still holds a reference. There is no
        // processor timer in this harness, so the drain is called here the way the processor's timer calls it.
        for (int k = 0; k < 10 && v2Side->pendingDisposeCount() > 0; ++k)
        { pumpMs (200); v2Side->drainPendingDispose ("guard settle"); }
        const int disposedV2 = (v2Serial >= 0 && v2Serial < (int) g_disposalsBySerial.size())
                                 ? g_disposalsBySerial[(size_t) v2Serial].load() : -1;
        check (disposedV2 == 1,
               "(3)(i) after the release, V2's instance has been disposed EXACTLY ONCE  (RED on 05b: 0 - the slot's "
               "Node::Ptr was never cleared and the retired render sequence still held one, so it survived its own "
               "release)",
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
        for (int k = 0; k < 10 && v2Side->pendingDisposeCount() > 0; ++k)
        { pumpMs (200); v2Side->drainPendingDispose ("guard settle"); }
        const int disposedV2Again = (v2Serial >= 0 && v2Serial < (int) g_disposalsBySerial.size())
                                      ? g_disposalsBySerial[(size_t) v2Serial].load() : -1;
        check (disposedV2Again == 1,
               "(3)(ii) after the reopen and a second release, the FIRST instance is still disposed exactly once - "
               "never twice",
               juce::String (disposedV2Again) + " dispose(s)");
        const int disposedNew = (reopenSerial >= 0 && reopenSerial < (int) g_disposalsBySerial.size())
                                  ? g_disposalsBySerial[(size_t) reopenSerial].load() : -1;
        check (disposedNew == 1,
               "(3)(ii) ...and the instance the reopen created is disposed exactly once too",
               juce::String (disposedNew) + " dispose(s)");
        check (v2Side->disposeFallbackCount() == 0,
               "(3)(2) ...with the fallback still never fired across both releases",
               juce::String (v2Side->disposeFallbackCount()) + " fallback(s)");
        v2Side.reset();
        pumpMs (200);
    }

    std::printf ("\n==== teardown_dispose_guard: %s (%d assertion(s) failed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
