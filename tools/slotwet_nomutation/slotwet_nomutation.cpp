// slotwet_nomutation — COMMIT 3 guard (17 Sep 2026): the per-slot wet write
// path on the Link must NEVER touch the graph. 200 setSlotWet writes on a
// live slot, audio pumped between them, must give the hosted instance
// Prepare == 0, Release == 0, no reconstruct, no destruct — and it must keep
// rendering every block (a graph rebuild would also be visible as a Prepare;
// a lock-held skip would be visible as a missing Process). The value must
// read back from the ChainHost that renders it after every write.
//
// attach_order-style recording mock, same lib (ChainHost.cpp is SHARED by
// V2 and the Link; the Link's slotWet consumer calls this exact setSlotWet).
// This guard is GREEN on today's code and stays as the REGRESSION guard for
// the rule "the wet write path must not mutate the graph" — stated plainly:
// it does not go RED before COMMIT 3, it pins what COMMIT 3 must not break.
// chainRevision legitimately advances (that is how the sidecar republishes),
// so it is reported, not asserted.
// Isolation: private ECHOJAY_STATE_HOME + HOME (build_and_run.sh).
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "ChainHost.h"
#include "EedDeviceRegistry.h"
#include <cstdio>
#include <atomic>

namespace
{
std::atomic<int> g_construct { 0 }, g_prepare { 0 }, g_release { 0 }, g_process { 0 }, g_destruct { 0 };

struct RecMock final : juce::AudioProcessor
{
    RecMock() : juce::AudioProcessor (BusesProperties()
            .withInput  ("In",  juce::AudioChannelSet::stereo(), true)
            .withOutput ("Out", juce::AudioChannelSet::stereo(), true)) { ++g_construct; }
    ~RecMock() override { ++g_destruct; }
    const juce::String getName() const override { return "EJ Wet Rec Mock"; }
    void prepareToPlay (double, int) override { ++g_prepare; }
    void releaseResources() override { ++g_release; }
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override { ++g_process; }
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
    void getStateInformation (juce::MemoryBlock& d) override { int v = 0; d.setSize (sizeof (int)); d.copyFrom (&v, 0, sizeof (int)); }
    void setStateInformation (const void*, int) override {}
};
BuiltinDevice makeDevice()
{
    BuiltinDevice d;
    d.name = "EJ Wet Rec Mock"; d.category = "Utility"; d.descriptiveName = d.name;
    d.summary = "slotwet_nomutation recording mock"; d.identifier = "echojay:test:wetrecmock"; d.uid = 0x454A574D;
    d.create = [] { return std::unique_ptr<juce::AudioProcessor> (new RecMock()); };
    return d;
}
const BuiltinDeviceRegistrar reg { makeDevice() };

int failures = 0;
void check (bool ok, const juce::String& what, const juce::String& detail = {})
{
    std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", what.toRawUTF8(),
                 detail.isNotEmpty() ? ("  [" + detail + "]").toRawUTF8() : "");
    if (! ok) ++failures;
}
juce::var oneSlotVar()
{
    juce::Array<juce::var> arr; auto* o = new juce::DynamicObject();
    o->setProperty ("n", 1); o->setProperty ("plugin", "EJ Wet Rec Mock"); o->setProperty ("bypassed", false);
    arr.add (juce::var (o)); return juce::var (arr);
}
void pump (ChainHost& h, int blocks)
{
    juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
    for (int b = 0; b < blocks; ++b) { buf.clear(); h.process (buf, midi); }
}
} // namespace

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::printf ("slotwet_nomutation: 200 setSlotWet writes on a rendering slot -> Prepare==0, no graph mutation\n");

    ChainHost host (ChainHost::Mode::Primary);
    host.prepare (48000.0, 512);
    host.restoreSavedChain (oneSlotVar(), juce::var (new juce::DynamicObject()));
    pump (host, 4);
    check (host.getNumSlots() == 1, "one live mock slot", juce::String (host.getNumSlots()));
    check (g_prepare.load() >= 1 && g_process.load() >= 1, "slot is prepared and rendering before the writes",
           "prepare=" + juce::String (g_prepare.load()) + " process=" + juce::String (g_process.load()));

    const int c0 = g_construct.load(), p0 = g_prepare.load(), r0 = g_release.load(), d0 = g_destruct.load();
    const int proc0 = g_process.load();
    const int rev0 = host.getChainRevision();
    int readbackMisses = 0;
    for (int n = 0; n < 200; ++n)
    {
        const float w = (float) (n % 101) / 100.0f;
        host.setSlotWet (0, w, ChainHost::WetSource::User);
        if (std::abs (host.getSlotWet (0) - w) > 1e-6f) ++readbackMisses;
        pump (host, 1);
    }
    check (readbackMisses == 0, "every one of the 200 writes reads back from the rendering ChainHost", juce::String (readbackMisses) + " misses");
    check (g_prepare.load()   - p0 == 0, "Prepare == 0 across 200 writes",         juce::String (g_prepare.load() - p0));
    check (g_release.load()   - r0 == 0, "releaseResources == 0 across 200 writes", juce::String (g_release.load() - r0));
    check (g_construct.load() - c0 == 0, "no reconstruct (no addNode of a fresh instance)", juce::String (g_construct.load() - c0));
    check (g_destruct.load()  - d0 == 0, "no destruct (no removeNode)",             juce::String (g_destruct.load() - d0));
    check (g_process.load() - proc0 == 200, "rendered EVERY one of the 200 blocks between writes (no lock-held skip)",
           juce::String (g_process.load() - proc0));
    std::printf ("   (chainRevision %d -> %d: advances by design, the sidecar's republish trigger)\n", rev0, host.getChainRevision());

    std::printf ("\n==== slotwet_nomutation: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
