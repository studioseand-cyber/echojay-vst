// slotwet_knobpath — COMMIT 3b guard (17 Sep 2026): while a Link rack is HELD
// the object Sean HEARS is V2's BorrowHost (PluginProcessor.cpp:1103
// borrowHost_->process on the dry ring; the Link's own slots are lease-
// bypassed, LinkProcessor.cpp:2004-2017). So the V2 mix knob on a Held
// Link-rack slot must write BOTH: the BorrowHost's slot wet (heard now) AND
// the slotWet verb to the Link (matches on release; the sidecar publishes
// it). This drives the REAL knob path — chainListPanel.onSlotWet, exactly
// the lambda the knob calls — through the existing editor friend, with the
// rack lock Held and the borrowed rack in view, and asserts on both ends:
// the BorrowHost object and the ctrl-cmd file the Link consumes.
// Sibling of tools/slotwet_readback (that one links the LINK lib and cannot
// host V2's editor; this one links the V2 lib). Isolation: HOME/EJ_STATE_TEST_HOME
// by the runner, ECHOJAY_STATE_HOME here.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "ChainHost.h"
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "EedDeviceRegistry.h"
#include "LinkShm.h"
#include <cstdio>

struct EchoJayAlignTestAccess          // friend of EchoJayProcessor (PluginProcessor.h)
{
    static void hold (EchoJayProcessor& p) { p.rackLockState_ = EchoJayProcessor::RackLockState::Held; }
};
struct EchoJayTabStripTestAccess       // friend of EchoJayEditor (PluginEditor.h)
{
    static void knob (EchoJayEditor& e, int i, float v) { e.chainListPanel.onSlotWet (i, v); }
    static juce::String viewUid (EchoJayEditor& e) { return e.chainViewUid(); }
};

namespace
{
struct KnobProbe final : juce::AudioProcessor
{
    KnobProbe() : juce::AudioProcessor (BusesProperties()
            .withInput ("In", juce::AudioChannelSet::stereo(), true)
            .withOutput ("Out", juce::AudioChannelSet::stereo(), true)) {}
    const juce::String getName() const override { return "EJ Knob Probe"; }
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
    void getStateInformation (juce::MemoryBlock& d) override { int v = 1; d.setSize (sizeof (int)); d.copyFrom (&v, 0, sizeof (int)); }
    void setStateInformation (const void*, int) override {}
};
constexpr int kProbeUid = 0x454A4B50;   // 'EJKP'
BuiltinDevice makeDevice()
{
    BuiltinDevice d; d.name = "EJ Knob Probe"; d.category = "Utility"; d.descriptiveName = d.name;
    d.summary = "slotwet_knobpath probe"; d.identifier = "echojay:test:knobprobe"; d.uid = kProbeUid;
    d.create = [] { return std::unique_ptr<juce::AudioProcessor> (new KnobProbe()); };
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
juce::var slotVar()
{
    juce::Array<juce::var> a; auto* o = new juce::DynamicObject();
    o->setProperty ("n", 1); o->setProperty ("plugin", "EJ Knob Probe"); o->setProperty ("bypassed", false);
    a.add (juce::var (o)); return juce::var (a);
}
}

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("ej_knobpath_" + juce::String (juce::Time::getMillisecondCounter()));
    tmp.createDirectory();
    setenv ("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);
    std::printf ("slotwet_knobpath: the V2 knob on a HELD Link-rack slot writes the BorrowHost (heard) AND the Link verb\n");

    const juce::String uid = "uid-knob";
    EchoJayProcessor proc;
    proc.prepareToPlay (48000.0, 512);
    proc.borrowEngageBegin (uid, "lease-knob", true, true);
    auto* bh = proc.borrowHost();
    check (bh != nullptr, "borrow session engaged (the BorrowHost exists)");
    bh->restoreSavedChain (slotVar(), juce::var (new juce::DynamicObject()));
    check (bh->getNumSlots() == 1, "BorrowHost holds the probe slot", juce::String (bh->getNumSlots()));
    check (proc.borrowHostIfActiveFor (uid) == bh, "borrowHostIfActiveFor(uid) is that object");
    proc.pendingChannelUid = uid;                 // the Link rack is the view
    EchoJayAlignTestAccess::hold (proc);          // the rack lock is HELD (orange)

    std::unique_ptr<juce::AudioProcessorEditor> edBase (proc.createEditor());
    auto* ed = dynamic_cast<EchoJayEditor*> (edBase.get());
    check (ed != nullptr, "the real editor constructed");
    if (ed == nullptr) return 2;
    ed->setSize (1280, 820);
    check (EchoJayTabStripTestAccess::viewUid (*ed) == uid, "chainViewUid() is the held rack", EchoJayTabStripTestAccess::viewUid (*ed));

    int err = 0;
    const juce::String dir = LinkShm::resolveDir (err);
    juce::File cmd (dir + "ctrl-cmd-" + uid + ".json");
    cmd.deleteFile();

    std::printf ("== the REAL knob path: chainListPanel.onSlotWet(0, 0.25) ==\n");
    EchoJayTabStripTestAccess::knob (*ed, 0, 0.25f);
    check (std::abs (bh->getSlotWet (0) - 0.25f) < 1e-6f,
           "BORROWHOST (the object being heard while Held): slot 0 wet == 0.25", juce::String (bh->getSlotWet (0), 3));
    check (cmd.existsAsFile(), "LINK half: ctrl-cmd-" + uid + ".json written");
    auto v = juce::JSON::parse (cmd.loadFileAsString());
    auto* o = v.getDynamicObject();
    auto* sw = (o != nullptr) ? o->getProperty ("slotWet").getDynamicObject() : nullptr;
    check (sw != nullptr, "the verb is slotWet");
    if (sw != nullptr)
    {
        check ((int) sw->getProperty ("idx") == 0, "slotWet.idx == 0", sw->getProperty ("idx").toString());
        check (sw->getProperty ("pluginId").toString() == bh->slotIdentityHex (0),
               "slotWet.pluginId == the borrowed slot's identity hex", sw->getProperty ("pluginId").toString());
        check (std::abs ((float)(double) sw->getProperty ("wet") - 0.25f) < 1e-6f, "slotWet.wet == 0.25", sw->getProperty ("wet").toString());
    }
    std::printf ("== a second drag supersedes (0.0 = dry) ==\n");
    EchoJayTabStripTestAccess::knob (*ed, 0, 0.0f);
    check (std::abs (bh->getSlotWet (0)) < 1e-6f, "BorrowHost slot 0 wet == 0.0");
    auto v2 = juce::JSON::parse (cmd.loadFileAsString());
    auto* sw2 = v2.getDynamicObject() ? v2.getDynamicObject()->getProperty ("slotWet").getDynamicObject() : nullptr;
    check (sw2 != nullptr && std::abs ((float)(double) sw2->getProperty ("wet")) < 1e-6f, "the ctrl-cmd file now carries wet 0.0 (latest wins)");

    std::printf ("\n==== slotwet_knobpath: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
