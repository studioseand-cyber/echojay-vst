// linksync_test — the FUNCTIONAL gate for structure-plan display parity
// (24 Aug 2026). The Link has two rack models: chainHost (audio truth, what
// the sidecar publishes, what applyStructurePlan mutates) and chainModel
// (editor-facing, keeps missing-slot memory). A dispatched plan updated only
// the first, so a freshly reopened editor still showed the old shape — and a
// SOURCE pin passed while that bug was live, because it proved a call existed
// without proving it did anything. So this gate applies a real plan through
// LinkProcessor::applyStructurePlanAndSync against the REAL Link object code
// and asserts the EDITOR-FACING model reports the new slot count.
//
// Runs sandboxed (EJ_STATE_TEST_HOME) like every suite that touches disk.

#include <CoreFoundation/CoreFoundation.h>   // before JUCE: MacTypes' Point vs juce::Point
#include <JuceHeader.h>
#include "LinkProcessor.h"
#include "EedDeviceRegistry.h"
#include "LinkShm.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

// The sandbox mechanism, same as borrowhost_test: interpose NSHomeDirectory
// so JUCE's mac app-data resolution honours EJ_STATE_TEST_HOME. Without this
// the binary sees the REAL ~/Library and the disk guard refuses to run.
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

// ---- a minimal builtin probe, registered TU-locally (borrowhost pattern) --
struct SyncProbe : juce::AudioProcessor
{
    SyncProbe() : juce::AudioProcessor (BusesProperties()
        .withInput ("In", juce::AudioChannelSet::stereo(), true)
        .withOutput ("Out", juce::AudioChannelSet::stereo(), true)) {}
    const juce::String getName() const override { return "EJ Sync Probe"; }
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
    void getStateInformation (juce::MemoryBlock& mb) override
    { int v = 7; mb.append (&v, sizeof (int)); }
    void setStateInformation (const void*, int) override {}
};

static BuiltinDevice makeProbe()
{
    BuiltinDevice d;
    d.name = "EJ Sync Probe"; d.category = "Utility";
    d.descriptiveName = d.name; d.summary = "linksync_test probe";
    d.identifier = "echojay:test:syncprobe"; d.uid = 0x454A5350;
    d.create = [] { return std::make_unique<SyncProbe>(); };
    return d;
}
static const BuiltinDeviceRegistrar probeReg { makeProbe() };

static int failures = 0;
static void check (bool ok, const juce::String& what,
                   const juce::String& detail = {})
{
    std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", what.toRawUTF8(),
                 detail.isNotEmpty() ? ("  [" + detail + "]").toRawUTF8() : "");
    if (! ok) ++failures;
}

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    std::printf ("linksync_test: plan display parity, editor-facing model\n");
    juce::ScopedJuceInitialiser_GUI juceInit;

    // ---- disk guard, same as state_match_test -----------------------------
    const char* home = std::getenv ("EJ_STATE_TEST_HOME");
    const auto appData = juce::File::getSpecialLocation (
        juce::File::userApplicationDataDirectory).getFullPathName();
    if (home == nullptr || ! appData.startsWith (juce::String (home)))
    {
        std::printf ("REFUSING TO RUN: app-data resolves to %s, not under "
                     "EJ_STATE_TEST_HOME\n", appData.toRawUTF8());
        return 2;
    }
    const juce::String dir =
        juce::File (juce::String (home)).getChildFile ("plandir")
            .getFullPathName() + "/";
    juce::File (dir).createDirectory();

    LinkProcessor proc;
    auto& host = proc.getChainHost();

    // ---- build a two-slot rack straight into chainHost --------------------
    // Deliberately BYPASSING the model writers (restoreSavedChain is the
    // session-restore path, chainHost-only): the editor-facing model must
    // stay empty, proving the two models really are two models (the control
    // this gate needs, or a one-model refactor would pass vacuously).
    {
        juce::Array<juce::var> arr;
        for (int n = 1; n <= 2; ++n)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("n", n);
            o->setProperty ("plugin", "EJ Sync Probe");
            o->setProperty ("bypassed", false);
            arr.add (juce::var (o));
        }
        host.restoreSavedChain (juce::var (arr),
                                juce::var (new juce::DynamicObject()));
    }
    check (host.getNumSlots() == 2, "host holds two",
           juce::String (host.getNumSlots()));
    check ((int) proc.getChainModel().size() == 0,
           "editor-facing model is a SECOND source (empty until synced)",
           juce::String ((int) proc.getChainModel().size()));

    // ---- the plan: base = live identity, one Create at the end ------------
    namespace SE = LinkShm::StructureEdit;
    const auto base = host.liveIdentity();
    check ((int) base.size() == 2, "live identity snapshot has two");
    SE::Plan plan;
    plan.uid = "linksync-test";
    plan.baseIdentity = base;
    SE::Op create;
    create.type = SE::OpType::Create;
    create.to   = 2;
    create.name = "EJ Sync Probe";
    create.identity = { "EJ Sync Probe",
                        juce::String (0x454A5350), {} };
    plan.ops.push_back (create);
    plan.creating = 1;

    const auto res = proc.applyStructurePlanAndSync (dir, plan);
    check (res.ok, "the plan applied",
           res.reasons.joinIntoString ("; "));
    check (host.getNumSlots() == 3, "host holds three",
           juce::String (host.getNumSlots()));
    // THE GATE: the model the editor renders from reports the new count.
    check ((int) proc.getChainModel().size() == 3,
           "the EDITOR-FACING model reports the new slot count",
           juce::String ((int) proc.getChainModel().size()));
    bool idxOk = true;
    for (int i = 0; i < (int) proc.getChainModel().size(); ++i)
        if (proc.getChainModel()[(size_t) i].hostIdx != i) idxOk = false;
    check (idxOk, "model hostIdx maps 1:1 onto the host after the sync");

    // ---- a REFUSED plan still leaves the two models agreeing --------------
    // (the sync is unconditional: refusal and rollback repaint too).
    SE::Plan bad = plan;
    bad.baseIdentity.pop_back();          // count mismatch -> refused whole
    const auto res2 = proc.applyStructurePlanAndSync (dir, bad);
    check (! res2.ok, "the mismatched plan was refused whole");
    check (host.getNumSlots() == 3
             && (int) proc.getChainModel().size() == 3,
           "after refusal the models still agree",
           juce::String (host.getNumSlots()) + "/"
             + juce::String ((int) proc.getChainModel().size()));

    // ---- negative control -------------------------------------------------
    check (false, "NEGATIVE CONTROL - this line is SUPPOSED to fail");
    const bool caught = (failures == 1);
    failures = 0;
    check (caught, "the harness caught the planted failure");

    std::printf ("\nlinksync_test: %s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
