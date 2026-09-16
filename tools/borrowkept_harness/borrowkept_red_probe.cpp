// borrowkept_red_probe — the BEHAVIOURAL RED for COMMIT 1, written against
// the CURRENT (pre-fix) API on purpose: EchoJayProcessor keeps ONE
// BorrowKept block. Select A -> keep -> select B -> keep, then ask whether
// A's block still exists. On the pre-fix lib this FAILS (the block is B's).
// This file is a one-time RED artefact: after the fix borrowKept_ no longer
// exists and this probe is retired; borrowkept_harness is the permanent gate.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "ChainHost.h"
#include "PluginProcessor.h"
#include "EedDeviceRegistry.h"
#include <cstdio>

namespace
{
struct KeptProbe final : juce::AudioProcessor
{
    int value = 0;
    KeptProbe() : juce::AudioProcessor (BusesProperties()
            .withInput ("In", juce::AudioChannelSet::stereo(), true)
            .withOutput ("Out", juce::AudioChannelSet::stereo(), true)) {}
    const juce::String getName() const override { return "EJ Kept Probe"; }
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
BuiltinDevice makeDevice()
{
    BuiltinDevice d;
    d.name = "EJ Kept Probe"; d.category = "Utility"; d.descriptiveName = d.name;
    d.summary = "borrowkept red probe"; d.identifier = "echojay:test:keptprobe"; d.uid = 0x454A4B50;
    d.create = [] { return std::unique_ptr<juce::AudioProcessor> (new KeptProbe()); };
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
juce::String chunkFor (int v) { return juce::Base64::toBase64 (&v, sizeof (int)); }
juce::var slotVar()
{
    juce::Array<juce::var> a; auto* o = new juce::DynamicObject();
    o->setProperty ("n", 1); o->setProperty ("plugin", "EJ Kept Probe"); o->setProperty ("bypassed", false);
    a.add (juce::var (o)); return juce::var (a);
}
juce::var stateVar (int v) { auto* o = new juce::DynamicObject(); o->setProperty ("1", chunkFor (v)); return juce::var (o); }
void select (EchoJayProcessor& p, const juce::String& uid, const juce::String& text, int v)
{
    p.borrowEngageBegin (uid, "lease-" + uid, true, true);
    if (auto* bh = p.borrowHost()) { bh->restoreSavedChain (slotVar(), stateVar (v)); if (bh->getNumSlots() > 0) bh->setSlotSettings (0, text); }
}
} // namespace

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::printf ("borrowkept_red_probe (pre-fix API): does A's kept block survive keeping B?\n");
    EchoJayProcessor p;
    select (p, "uid-A", "A-suggest", 111); p.borrowRelease (true);
    check (p.borrowKept_.uid == "uid-A" && p.borrowKept_.settings.size() == 1 && p.borrowKept_.settings[0] == "A-suggest",
           "after keeping A: the kept block is A's", p.borrowKept_.uid);
    select (p, "uid-B", "B-suggest", 222); p.borrowRelease (true);
    check (p.borrowKept_.uid == "uid-A",
           "after keeping B: A's kept block STILL EXISTS (it does not - single slot, now B's)", "kept uid = " + p.borrowKept_.uid);
    std::printf ("\n==== borrowkept_red_probe: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
