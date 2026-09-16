// borrowkept_harness — COMMIT 1 guard (17 Sep 2026): the AI suggestions kept
// on a Link-rack release survive a switch to ANOTHER rack and back.
//
// Before: EchoJayProcessor kept ONE BorrowKept block (borrowKept_), replaced
// on every release, so select A -> keep -> select B -> keep -> select A found
// uid B and dropped A's block (Sean's "suggested settings vanish on switch").
// After: a per-uid map, session-scoped, bounded to 16 (least-recently-kept
// evicted), cleared for a uid when its chain is rebuilt / its Link vanishes /
// its kept block is consumed on re-borrow.
//
// Asserts on the object that processes audio: the borrowed ChainHost's slot
// carries A's text after the restore, not a sibling record.
// Isolation: private ECHOJAY_STATE_HOME + HOME (build_and_run.sh).
#include <CoreFoundation/CoreFoundation.h>   // before JUCE: MacTypes' Point
#include <JuceHeader.h>
#include "ChainHost.h"
#include "PluginProcessor.h"
#include "EedDeviceRegistry.h"
#include "LinkShm.h"
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
    d.summary = "borrowkept_harness probe"; d.identifier = "echojay:test:keptprobe"; d.uid = 0x454A4B50;
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

// Select rack <uid>: engage, build one probe slot, give it <text> as its
// suggested-settings prose. Returns the borrowed host (the audio object).
ChainHost* select (EchoJayProcessor& p, const juce::String& uid, const juce::String& text, int stateVal)
{
    p.borrowEngageBegin (uid, "lease-" + uid, true, true);
    auto* bh = p.borrowHost();
    if (bh == nullptr) return nullptr;
    bh->restoreSavedChain (slotVar(), stateVar (stateVal));
    if (bh->getNumSlots() > 0) bh->setSlotSettings (0, text);
    return bh;
}
} // namespace

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::printf ("borrowkept_harness: per-uid kept suggestions survive a rack switch\n");
    EchoJayProcessor p;

    // ---- select A, keep; select B, keep --------------------------------------
    auto* a1 = select (p, "uid-A", "A-suggest", 111);
    check (a1 != nullptr && a1->getSlotInfo (0).settings == "A-suggest", "A built with its suggestion text");
    p.borrowRelease (true);                                   // keep A
    auto* b1 = select (p, "uid-B", "B-suggest", 222);
    check (b1 != nullptr && b1->getSlotInfo (0).settings == "B-suggest", "B built with its suggestion text");
    p.borrowRelease (true);                                   // keep B

    // ---- the kept blocks: A must still be present (RED before), B untouched ----
    const auto* ka = p.borrowKeptFor ("uid-A");
    const auto* kb = p.borrowKeptFor ("uid-B");
    check (ka != nullptr, "A's kept block PRESENT after switching to B and releasing");
    check (ka != nullptr && ka->settings.size() == 1 && ka->settings[0] == "A-suggest", "A's kept text is A's, unchanged");
    check (kb != nullptr && kb->settings.size() == 1 && kb->settings[0] == "B-suggest", "B's kept block present and untouched");
    check (p.borrowKeptCount() == 2, "two racks kept", juce::String (p.borrowKeptCount()));

    // ---- select A again: the restore lands A's text on the AUDIO object ------
    auto* a2 = select (p, "uid-A", "", 0);                    // sidecar text empty: only the keep can supply it
    check (a2 != nullptr && a2->getSlotInfo (0).settings.isEmpty(), "re-engaged A starts with no text (sidecar has none)");
    const int want = a2 ? a2->getNumSlots() : 0;
    if (a2 != nullptr) p.applyBorrowKeptSettings ("uid-A", *a2, want);
    check (a2 != nullptr && a2->getSlotInfo (0).settings == "A-suggest",
           "after re-select: the borrowed host's slot carries A's suggestion (the audio object, not a record)");
    if (a2 != nullptr) p.applyBorrowKeptStates ("uid-A", *a2, want);   // consume-on-restore, as before
    check (p.borrowKeptFor ("uid-A") == nullptr, "A's kept block CONSUMED by the restore (existing semantics kept)");
    check (p.borrowKeptFor ("uid-B") != nullptr, "B's kept block still present (untouched by A's restore)");
    p.borrowRelease (false);

    // ---- bound: 17 racks kept -> 16 remain, the OLDEST evicted only ----------
    p.clearBorrowKept ("uid-B");
    check (p.borrowKeptCount() == 0, "clearBorrowKept(uid) empties the map", juce::String (p.borrowKeptCount()));
    for (int i = 0; i < 17; ++i)
    {
        select (p, "uid-" + juce::String (i), "text-" + juce::String (i), i);
        p.borrowRelease (true);
    }
    check (p.borrowKeptCount() == 16, "bounded to 16 after 17 keeps", juce::String (p.borrowKeptCount()));
    check (p.borrowKeptFor ("uid-0") == nullptr, "17th keep evicted the OLDEST (uid-0) only");
    bool others = true;
    for (int i = 1; i < 17; ++i) if (p.borrowKeptFor ("uid-" + juce::String (i)) == nullptr) others = false;
    check (others, "uid-1..uid-16 all still present");

    std::printf ("\n==== borrowkept_harness: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
