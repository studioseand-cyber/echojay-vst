// sidechain_guard (4 Oct 2026, Kathy's finding): A SIDECHAIN BUS MUST NEVER BE FED SILENCE.
//
// Several Waves compressors (C1, RComp, SSLComp, VComp, dbx-160) declare a sidechain input bus and key off it.
// EchoJay connected only channels 0/1 of a hosted plugin, and AudioProcessorGraph ZEROES every unconnected input
// channel - so the key input received digital silence and the plugin compressed nothing at all. Kathy measured it
// out of process: C1 0 dB of gain reduction against 10.5 dB with the sidechain disconnected; RComp 0 against 54.
// Nothing reported an error; the user's compressor simply did not work.
//
// Logic leaves an unassigned sidechain DISCONNECTED, so the plugin falls back to its own input. We cannot reproduce
// that from a JUCE host: prepareToPlay installs a render callback for every DECLARED input bus (no skip for a
// disabled one), so a disabled bus still has a live callback handing it a 0-channel buffer - a third state. The
// ruled fix is to feed the main input into the first non-main input bus, which is what internal keying means.
//
// THE MOCK IS THE TEST: a compressor that ONLY acts when its key is non-silent. On the old wiring it reports 0 dB
// forever; on the fixed wiring it reports the gain reduction it would have applied. The assertion is on what the
// plugin SAW, not on our intentions.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "ChainHost.h"
#include "EedDeviceRegistry.h"
#include <cstdio>

static int failures = 0;
static void check (bool ok, const juce::String& what, const juce::String& detail = {})
{
    std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", what.toRawUTF8(),
                 detail.isNotEmpty() ? ("  [" + detail + "]").toRawUTF8() : "");
    if (! ok) ++failures;
}

// A stereo main bus plus a stereo KEY bus. It measures the key's level and "compresses" only when the key carries
// signal - exactly the behaviour that made Kathy's Waves plugins read 0 dB.
struct KeyedCompMock final : juce::AudioProcessor
{
    std::atomic<float> keyPeak { 0.0f };      // the loudest key sample seen
    std::atomic<float> mainPeak { 0.0f };
    std::atomic<int>   blocks { 0 };
    std::atomic<float> grDb { 0.0f };         // what it would be reducing by

    KeyedCompMock() : juce::AudioProcessor (BusesProperties()
            .withInput  ("In",        juce::AudioChannelSet::stereo(), true)
            .withInput  ("Sidechain", juce::AudioChannelSet::stereo(), true)
            .withOutput ("Out",       juce::AudioChannelSet::stereo(), true)) {}
    const juce::String getName() const override { return "EJ Keyed Comp Mock"; }
    void prepareToPlay (double, int) override {}
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override { return true; }

    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        ++blocks;
        const auto main = getBusBuffer (b, true, 0);
        float mp = 0.0f;
        for (int c = 0; c < main.getNumChannels(); ++c)
            mp = juce::jmax (mp, main.getMagnitude (c, 0, main.getNumSamples()));
        mainPeak = juce::jmax (mainPeak.load(), mp);

        float kp = 0.0f;
        if (getBusCount (true) > 1)
        {
            const auto key = getBusBuffer (b, true, 1);
            for (int c = 0; c < key.getNumChannels(); ++c)
                kp = juce::jmax (kp, key.getMagnitude (c, 0, key.getNumSamples()));
        }
        keyPeak = juce::jmax (keyPeak.load(), kp);
        // THE WHOLE POINT: no key, no compression. A real unit keyed off silence behaves exactly like this.
        grDb = kp > 1.0e-5f ? 6.0f : 0.0f;
    }
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

static BuiltinDevice makeKeyedDevice()
{
    BuiltinDevice d;
    d.name = "EJ Keyed Comp Mock"; d.category = "Dynamics"; d.descriptiveName = d.name;
    d.summary = "sidechain_guard keyed mock"; d.identifier = "echojay:test:keyedcomp"; d.uid = 0x454A4B43;
    d.create = [] { return std::unique_ptr<juce::AudioProcessor> (new KeyedCompMock()); };
    return d;
}
static const BuiltinDeviceRegistrar keyedReg { makeKeyedDevice() };

// ---- 6 Oct 2026 (Sean's ruling): TWO non-main input buses -------------------------------------------------
// rebuildGraph fed bus 1 only, so a plugin declaring a second sidechain bus had it left UNCONNECTED, which
// AudioProcessorGraph zeroes. Kathy's split makes that the failing case: C1 comp-sc, Zip and the VBC FG units stop
// processing when the sidechain is DISCONNECTED, so a silent-and-unconnected second key is exactly what breaks them.
struct TwoKeyCompMock final : juce::AudioProcessor
{
    std::atomic<float> key1Peak { 0.0f }, key2Peak { 0.0f }, mainPeak { 0.0f };
    std::atomic<int>   blocks { 0 };

    TwoKeyCompMock() : juce::AudioProcessor (BusesProperties()
            .withInput  ("In",         juce::AudioChannelSet::stereo(), true)
            .withInput  ("Sidechain",  juce::AudioChannelSet::stereo(), true)
            .withInput  ("Sidechain2", juce::AudioChannelSet::stereo(), true)
            .withOutput ("Out",        juce::AudioChannelSet::stereo(), true)) {}
    const juce::String getName() const override { return "EJ Two-Key Comp Mock"; }
    void prepareToPlay (double, int) override {}
    void releaseResources() override {}
    // IT WILL NOT GIVE ITS SIDECHAINS UP, which is the point. The single-key leg above passes vacuously because the
    // graph adopts a built-in at 2-in/2-out, disabling its key bus - so "every enabled bus is fed" becomes 0 of 0
    // and holds whatever the wiring does. A real AU arrives with its sidechain ENABLED and keeps it. Refusing any
    // layout that drops a sidechain is how a mock reproduces that, and it is what makes this leg discriminate.
    bool isBusesLayoutSupported (const BusesLayout& l) const override
    {
        if (l.inputBuses.size() < 3 || l.outputBuses.size() < 1) return false;
        return l.inputBuses[0] == juce::AudioChannelSet::stereo()
            && l.inputBuses[1] == juce::AudioChannelSet::stereo()
            && l.inputBuses[2] == juce::AudioChannelSet::stereo()
            && l.outputBuses[0] == juce::AudioChannelSet::stereo();
    }
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        ++blocks;
        auto peakOf = [&b, this] (int bus) -> float
        {
            if (getBusCount (true) <= bus) return 0.0f;
            const auto buf = getBusBuffer (b, true, bus);
            float p = 0.0f;
            for (int c = 0; c < buf.getNumChannels(); ++c)
                p = juce::jmax (p, buf.getMagnitude (c, 0, buf.getNumSamples()));
            return p;
        };
        mainPeak = juce::jmax (mainPeak.load(), peakOf (0));
        key1Peak = juce::jmax (key1Peak.load(), peakOf (1));
        key2Peak = juce::jmax (key2Peak.load(), peakOf (2));
    }
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

static BuiltinDevice makeTwoKeyDevice()
{
    BuiltinDevice d;
    d.name = "EJ Two-Key Comp Mock"; d.category = "Dynamics"; d.descriptiveName = d.name;
    d.summary = "sidechain_guard two-sidechain mock"; d.identifier = "echojay:test:twokeycomp"; d.uid = 0x454A4B32;
    d.create = [] { return std::unique_ptr<juce::AudioProcessor> (new TwoKeyCompMock()); };
    return d;
}
static const BuiltinDeviceRegistrar twoKeyReg { makeTwoKeyDevice() };

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

// One pass of programme through a host, so the mock sees real blocks.
static void feed (ChainHost& h, double seconds)
{
    const int blockSize = 512; const double sr = 48000.0;
    juce::AudioBuffer<float> buf (2, blockSize);
    const int blocks = (int) (seconds * sr / blockSize);
    double phase = 0.0;
    for (int i = 0; i < blocks; ++i)
    {
        for (int n = 0; n < blockSize; ++n)
        {
            const float v = 0.35f * (float) std::sin (phase);
            phase += 2.0 * juce::MathConstants<double>::pi * 220.0 / sr;
            buf.setSample (0, n, v); buf.setSample (1, n, v);
        }
        juce::MidiBuffer midi;
        h.process (buf, midi);
    }
}

static void runOn (ChainHost::Mode mode, const char* label)
{
    std::printf ("\n== %s host ==\n", label);
    ChainHost host (mode);
    host.prepare (48000.0, 512);
    const auto* dev = BuiltinDeviceRegistry::instance().findByName ("EJ Keyed Comp Mock");
    check (dev != nullptr, juce::String (label) + ": the keyed mock is registered");
    if (dev == nullptr) return;
    EchoJayBorrowHostTestAccess::loadBuiltin (host, BuiltinDeviceRegistry::descriptionFor (*dev));
    pumpMs (250);
    check (host.getNumSlots() == 1, juce::String (label) + ": one slot",
           juce::String (host.getNumSlots()));
    auto* m = dynamic_cast<KeyedCompMock*> (host.getSlotProcessor (0));
    check (m != nullptr, juce::String (label) + ": the slot holds the keyed mock");
    if (m == nullptr) return;
    check (m->getBusCount (true) == 2,
           juce::String (label) + ": it declares a main bus AND a sidechain bus",
           juce::String (m->getBusCount (true)));
    // ---- WARNING, 5 Oct 2026: THIS LEG ASSERTS THE BUILT-IN CASE, WHICH IS NOT THE CASE THAT MATTERS ---------
    //
    // A real AU arrives with its sidechain ENABLED and, before the fix, had those channels zeroed - confirmed in
    // Sean's Logic log ("Tube-Tech CL 1B sidechain fed from main (2 ch of 2) from plugin channel 2") and by SSLComp
    // compressing on 04e where it did nothing before. The assertions below describe a BUILT-IN, which the graph
    // adopts at 2-in/2-out and whose sidechain is therefore disabled - so they hold the wrong thing fixed, and
    // reading them as "EchoJay never fed silence" was my error.
    //
    // OWED: re-aim this at a mock whose sidechain survives adoption, or register it through the path a real AU
    // takes, so the leg covers the behaviour the fix exists for rather than the fixture's accident.
    // ---------------------------------------------------------------------------------------------------------
    // AudioProcessorGraph forces every node to the GRAPH's main-bus channel counts when it adopts it, and this
    // graph is 2-in/2-out - so a declared sidechain bus arrives DISABLED, with zero channels. There are therefore
    // no extra input channels for the graph to zero, and EchoJay never fed a key bus silence.
    //
    // Kathy's probe measured C1 at 0 dB against 10.5 disconnected because HER probe enabled every input bus and
    // fed the spare one silence. That is a property of that probe, not of this host. Asserted here so the
    // difference is held fixed: if a future change starts enabling these buses, this leg fails and says why.
    auto* keyBus = m->getBus (true, 1);
    check (keyBus != nullptr && ! keyBus->isEnabled(),
           juce::String (label) + ": the sidechain bus is DISABLED by the graph (2-in/2-out), so it is never fed "
           "silence - Kathy's 0 dB came from a probe that ENABLED it and fed it zeros",
           keyBus == nullptr ? juce::String ("no bus 1")
                             : (keyBus->isEnabled() ? "ENABLED " : "disabled ")
                               + juce::String (keyBus->getNumberOfChannels()) + "ch");
    check (m->getTotalNumInputChannels() == 2,
           juce::String (label) + ": ...so the plugin has exactly two input channels, both carrying programme",
           juce::String (m->getTotalNumInputChannels()));

    feed (host, 1.0);
    pumpMs (100);

    check (m->blocks.load() > 0, juce::String (label) + ": it processed audio",
           juce::String (m->blocks.load()));
    check (m->mainPeak.load() > 0.01f, juce::String (label) + ": its MAIN input carried the programme",
           juce::String (m->mainPeak.load(), 4));
    // WHAT THE PLUGIN ACTUALLY SEES, in its own terms: no key bus, not a silent one. A disabled bus yields a
    // zero-channel buffer, so a plugin that reads its key finds nothing to read rather than a stream of zeros.
    check (m->keyPeak.load() == 0.0f,
           juce::String (label) + ": the plugin finds NO key channels (a disabled bus is 0 channels, not a silent "
           "stream) - which is why this host does not reproduce Kathy's silent-key failure",
           "key peak " + juce::String (m->keyPeak.load(), 4));
    // OPEN QUESTION, DELIBERATELY NOT ASSERTED HERE: a real AU is a different matter. JUCE installs a render
    // callback for every DECLARED input bus whether or not it is enabled (prepareToPlay loops
    // `i < getBusCount(isInput)` with no skip), so a real Waves AU hosted here gets a live callback on a
    // zero-channel element. Whether it reads that as "disconnected" (and keys internally, as under Logic) or as
    // silence cannot be settled with a mock, and cannot be settled on this machine at all: an unsigned harness
    // cannot load a PACE-wrapped plugin, and loading one here raises iLok prompts that have taken the host down.
    // That measurement needs a signed probe. Recorded for Sean rather than guessed at.
}

// ---- 6 Oct 2026 (Sean's ruling): EVERY ENABLED NON-MAIN INPUT BUS IS FED, not just the first ---------------
static void runTwoKey (ChainHost::Mode mode, const char* label)
{
    std::printf ("\n== %s host, TWO sidechain buses ==\n", label);
    ChainHost host (mode);
    host.prepare (48000.0, 512);
    const auto* dev = BuiltinDeviceRegistry::instance().findByName ("EJ Two-Key Comp Mock");
    check (dev != nullptr, juce::String (label) + ": the two-key mock is registered");
    if (dev == nullptr) return;
    EchoJayBorrowHostTestAccess::loadBuiltin (host, BuiltinDeviceRegistry::descriptionFor (*dev));
    pumpMs (250);
    auto* m = dynamic_cast<TwoKeyCompMock*> (host.getSlotProcessor (0));
    check (m != nullptr, juce::String (label) + ": the slot holds the two-key mock");
    if (m == nullptr) return;
    const int buses = m->getBusCount (true);
    check (buses >= 2, juce::String (label) + ": it declares more than one input bus", juce::String (buses));
    // Drive real signal through the chain, the same way the single-key leg does.
    feed (host, 1.0);
    // WHAT IS ASSERTED, AND WHAT IS NOT. As the single-key leg above already records at length, the graph adopts a
    // BUILT-IN at 2-in/2-out, so the extra buses are reported disabled here and there is nothing to feed - which is
    // precisely why this leg asserts the RULE rather than a channel count: whatever buses the host reports ENABLED,
    // the number fed must equal the number enabled. On a real AU (buses enabled) that is "all of them"; on a
    // built-in (buses disabled) it is "none", and both satisfy the rule. A build that fed only bus 1 would fail the
    // first case and pass the second, which is the hole the single-key leg fell into.
    int enabledNonMain = 0;
    for (int b = 1; b < buses; ++b)
        if (auto* bus = m->getBus (true, b); bus != nullptr && bus->isEnabled()) ++enabledNonMain;
    const bool key1Fed = m->key1Peak.load() > 1.0e-5f;
    const bool key2Fed = m->key2Peak.load() > 1.0e-5f;
    const int fed = (key1Fed ? 1 : 0) + (key2Fed ? 1 : 0);
    check (fed == enabledNonMain,
           juce::String (label) + ": every ENABLED non-main input bus receives the slot's input - the count fed "
           "equals the count enabled  (RED as it stood for a real AU: rebuildGraph fed bus 1 only, so a second "
           "enabled sidechain was left unconnected and the graph zeroed it)",
           juce::String (fed) + " fed of " + juce::String (enabledNonMain) + " enabled (key1 "
           + juce::String (m->key1Peak.load(), 4) + ", key2 " + juce::String (m->key2Peak.load(), 4) + ")");
    check (m->mainPeak.load() > 1.0e-5f,
           juce::String (label) + ": ...and the MAIN input still carries the programme",
           juce::String (m->mainPeak.load(), 4));
}

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("==== sidechain_guard: a key bus must never be fed silence ====\n");
    // BOTH HOSTS, because rebuildGraph is shared and the bug was in the shared wiring: the own chain and every
    // borrowed host wire their slots through the same code.
    runOn (ChainHost::Mode::Primary,  "own-chain");
    runOn (ChainHost::Mode::Borrowed, "borrowed");
    runTwoKey (ChainHost::Mode::Primary,  "own-chain");
    runTwoKey (ChainHost::Mode::Borrowed, "borrowed");
    std::printf ("\n==== sidechain_guard: %s (%d assertion(s) failed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
