// slotwet_readback — COMMIT 3 guard (17 Sep 2026): a V2 mix knob on a HELD
// Link-rack slot reaches the Link's audio object.
//
// Transport: ctrl-cmd verb  slotWet { idx, pluginId, wet }  in
// ctrl-cmd-<uid>.json (the existing consume-and-answer file). The Link
// applies it ONLY if slots[idx] carries that pluginId (hex of the slot's
// plugin uniqueId, the same string the sidecar publishes), else logs
// "EJCtrl: slotWet REJECTED idx=… want=… have=…" and ignores it.
//
// Asserts on the object that processes audio: LinkProcessor::chainHost's
// slot wet, read back after the Link's own consumer runs — never a sibling
// record. Reached through the friend EchoJayLinkSyncTestAccess that
// LinkProcessor.h already declares (linksync_test's precedent), so this
// compiles on today's code: RED because the verb is unknown (wet stays 1.0),
// GREEN once the Link consumes it.
// Isolation: private ECHOJAY_STATE_HOME + HOME (build_and_run.sh); the Link
// resolves the same private dir the harness writes into.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "LinkProcessor.h"
#include "ChainHost.h"
#include "EedDeviceRegistry.h"
#include "LinkShm.h"
#include <cstdio>

struct EchoJayLinkSyncTestAccess
{
    static void       pollCtrl (LinkProcessor& p) { p.pollControlCommand(); }
    static ChainHost& host     (LinkProcessor& p) { return p.chainHost; }
};

namespace
{
struct WetProbe final : juce::AudioProcessor
{
    int value = 0;
    WetProbe() : juce::AudioProcessor (BusesProperties()
            .withInput ("In", juce::AudioChannelSet::stereo(), true)
            .withOutput ("Out", juce::AudioChannelSet::stereo(), true)) {}
    const juce::String getName() const override { return "EJ Wet Probe"; }
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
    void getStateInformation (juce::MemoryBlock& d) override { d.setSize (sizeof (int)); d.copyFrom (&value, 0, sizeof (int)); }
    void setStateInformation (const void* data, int size) override { if (size >= (int) sizeof (int)) std::memcpy (&value, data, sizeof (int)); }
};
constexpr int kProbeUid = 0x454A5357;   // 'EJSW'
BuiltinDevice makeDevice()
{
    BuiltinDevice d;
    d.name = "EJ Wet Probe"; d.category = "Utility"; d.descriptiveName = d.name;
    d.summary = "slotwet_readback probe"; d.identifier = "echojay:test:wetprobe"; d.uid = kProbeUid;
    d.create = [] { return std::unique_ptr<juce::AudioProcessor> (new WetProbe()); };
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
    o->setProperty ("n", 1); o->setProperty ("plugin", "EJ Wet Probe"); o->setProperty ("bypassed", false);
    a.add (juce::var (o)); return juce::var (a);
}
juce::var stateVar (int v) { auto* o = new juce::DynamicObject(); o->setProperty ("1", chunkFor (v)); return juce::var (o); }

// Write one slotWet ctrl-cmd for the Link and let its consumer run.
void sendSlotWet (LinkProcessor& p, const juce::String& dir, const juce::String& uid,
                  int seq, int idx, const juce::String& pluginId, float wet)
{
    auto* sw = new juce::DynamicObject();
    sw->setProperty ("idx", idx); sw->setProperty ("pluginId", pluginId); sw->setProperty ("wet", (double) wet);
    auto* cmd = new juce::DynamicObject();
    cmd->setProperty ("v", 1); cmd->setProperty ("seq", seq); cmd->setProperty ("slotWet", juce::var (sw));
    juce::File (dir + "ctrl-ack-" + uid + ".json").deleteFile();
    juce::File (dir + "ctrl-cmd-" + uid + ".json").replaceWithText (juce::JSON::toString (juce::var (cmd), true));
    EchoJayLinkSyncTestAccess::pollCtrl (p);
}
} // namespace

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::printf ("slotwet_readback: ctrl-cmd slotWet -> the Link's ChainHost slot wet (the audio object)\n");

    LinkProcessor proc;
    proc.prepareToPlay (48000.0, 512);
    int err = 0;
    const juce::String dir = LinkShm::resolveDir (err);
    const juce::String uid = proc.getInstanceUidForTest();
    check (dir.isNotEmpty() && uid.isNotEmpty(), "Link resolved its command dir + instance uid", "dir=" + dir + " uid=" + uid);

    auto& host = EchoJayLinkSyncTestAccess::host (proc);
    host.restoreSavedChain (slotVar(), stateVar (7));
    check (host.getNumSlots() == 1, "Link rack has one probe slot", juce::String (host.getNumSlots()));
    const juce::String id0 = juce::String::toHexString (kProbeUid);   // the identity the sidecar publishes (hex uniqueId)
    check (std::abs (host.getSlotWet (0) - 1.0f) < 1e-6f, "slot wet starts at 1.0 (fully wet)", juce::String (host.getSlotWet (0), 3));

    std::printf ("== APPLY: right idx + right pluginId ==\n");
    sendSlotWet (proc, dir, uid, 101, 0, id0, 0.25f);
    check (! juce::File (dir + "ctrl-cmd-" + uid + ".json").existsAsFile(), "ctrl-cmd consumed (file gone)");
    check (std::abs (host.getSlotWet (0) - 0.25f) < 1e-6f,
           "READBACK: the Link's ChainHost slot 0 wet == 0.25 (the object that processes audio)", juce::String (host.getSlotWet (0), 3));

    std::printf ("== REJECTED: stale index (idx=7 on a 1-slot rack) ==\n");
    sendSlotWet (proc, dir, uid, 102, 7, id0, 0.9f);
    check (std::abs (host.getSlotWet (0) - 0.25f) < 1e-6f, "wet UNCHANGED after a stale-index write (ignored)", juce::String (host.getSlotWet (0), 3));

    std::printf ("== REJECTED: right idx, WRONG pluginId (the rack changed under the knob) ==\n");
    sendSlotWet (proc, dir, uid, 103, 0, "deadbeef", 0.9f);
    check (std::abs (host.getSlotWet (0) - 0.25f) < 1e-6f, "wet UNCHANGED after a wrong-pluginId write (ignored)", juce::String (host.getSlotWet (0), 3));

    std::printf ("== APPLY again: a later, valid write lands (0.0 = dry) ==\n");
    sendSlotWet (proc, dir, uid, 104, 0, id0, 0.0f);
    check (std::abs (host.getSlotWet (0) - 0.0f) < 1e-6f, "READBACK: wet == 0.0 after the valid write", juce::String (host.getSlotWet (0), 3));

    std::printf ("\n==== slotwet_readback: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
