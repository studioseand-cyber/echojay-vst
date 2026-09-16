// attach_order — the gate for the Link rack-switch AU crash (16 Sep 2026).
//
// BUILD-PATH-IS-THE-SPEC. completeLoad gives an AU one clean sequence:
// construct, one addNode under GraphMutation, ONE prepare by the graph, then
// render. The switch path (planStageOne -> tryReattachParked) must give the AU
// the IDENTICAL call set. This harness drives both with a recording mock and
// asserts, per instance on the switch path:
//   Prepare == 1, Release == 0, no Prepare while suspended, seed BEFORE the
//   (single) Prepare, never rendered before Prepare, and the slot arrives in
//   the lease's target state (bypassed == attachBypassed_) — never rendered
//   live under the lease.
// and that the BUILD path's own sequence is unchanged (it is the spec).
//
// addNode / suspendProcessing are not virtual on AudioProcessor, so the mock
// cannot hook them directly. They are covered by what IS observable: the graph
// can only prepare a node after addNode, so "seed < the single Prepare" proves
// the seed preceded the graph's involvement; and isSuspended() sampled AT each
// prepare proves no prepare happened on a suspended (parked) node.
//
// RED on the pre-fix lib (LIB=<backup>): switch path Prepare==2, one of them
// while suspended, seed between them, slot arrives live (bypassed=false).
// GREEN after. Isolation: private ECHOJAY_STATE_HOME + HOME (build_and_run.sh).
#include <CoreFoundation/CoreFoundation.h>   // before JUCE: MacTypes' Point
#include <JuceHeader.h>
#include "ChainHost.h"
#include "EedDeviceRegistry.h"
#include "LinkShm.h"
#include <cstdio>
#include <vector>

namespace
{
enum class Ev { Construct, Prepare, PrepareSusp, Release, SetState, Process, Destruct };
struct LogEntry { int serial; Ev ev; };
std::vector<LogEntry> g_log;
int g_nextSerial = 1;

const char* evName (Ev e)
{
    switch (e) { case Ev::Construct: return "Construct"; case Ev::Prepare: return "Prepare";
        case Ev::PrepareSusp: return "PREPARE-WHILE-SUSPENDED"; case Ev::Release: return "Release";
        case Ev::SetState: return "SetState"; case Ev::Process: return "Process"; case Ev::Destruct: return "Destruct"; }
    return "?";
}

struct RecMock final : juce::AudioProcessor
{
    int serial; int value = 0;
    RecMock() : juce::AudioProcessor (BusesProperties()
            .withInput  ("In",  juce::AudioChannelSet::stereo(), true)
            .withOutput ("Out", juce::AudioChannelSet::stereo(), true)),
        serial (g_nextSerial++) { g_log.push_back ({ serial, Ev::Construct }); }
    ~RecMock() override { g_log.push_back ({ serial, Ev::Destruct }); }
    const juce::String getName() const override { return "EJ Rec Mock"; }
    void prepareToPlay (double, int) override
    { g_log.push_back ({ serial, isSuspended() ? Ev::PrepareSusp : Ev::Prepare }); }
    void releaseResources() override { g_log.push_back ({ serial, Ev::Release }); }
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override
    { g_log.push_back ({ serial, Ev::Process }); }
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
    { if (size >= (int) sizeof (int)) std::memcpy (&value, data, sizeof (int));
      g_log.push_back ({ serial, Ev::SetState }); }
};

BuiltinDevice makeDevice (const char* name, const char* ident, int uid)
{
    BuiltinDevice d;
    d.name = name; d.category = "Utility"; d.descriptiveName = name;
    d.summary = "attach_order recording mock"; d.identifier = ident; d.uid = uid;
    d.create = [] { return std::unique_ptr<juce::AudioProcessor> (new RecMock()); };
    return d;
}
const BuiltinDeviceRegistrar recReg { makeDevice ("EJ Rec Mock", "echojay:test:recmock", 0x454A524D) };

int failures = 0;
void check (bool ok, const juce::String& what, const juce::String& detail = {})
{
    std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", what.toRawUTF8(),
                 detail.isNotEmpty() ? ("  [" + detail + "]").toRawUTF8() : "");
    if (! ok) ++failures;
}

// ---- per-serial queries over a slice [lo, hi) of the log -----------------
int countIn (int serial, Ev ev, int lo, int hi)
{ int n = 0; for (int i = lo; i < hi; ++i) if (g_log[(size_t) i].serial == serial && g_log[(size_t) i].ev == ev) ++n; return n; }
int firstIdx (int serial, Ev ev, int lo, int hi)
{ for (int i = lo; i < hi; ++i) if (g_log[(size_t) i].serial == serial && g_log[(size_t) i].ev == ev) return i; return -1; }
int lastIdx (int serial, Ev ev, int lo, int hi)
{ int r = -1; for (int i = lo; i < hi; ++i) if (g_log[(size_t) i].serial == serial && g_log[(size_t) i].ev == ev) r = i; return r; }
int anyPrepare (int serial, int lo, int hi)   // Prepare or PrepareSusp
{ return countIn (serial, Ev::Prepare, lo, hi) + countIn (serial, Ev::PrepareSusp, lo, hi); }
int firstAnyPrepare (int serial, int lo, int hi)
{ const int a = firstIdx (serial, Ev::Prepare, lo, hi), b = firstIdx (serial, Ev::PrepareSusp, lo, hi);
  if (a < 0) return b; if (b < 0) return a; return juce::jmin (a, b); }
juce::String timeline (int serial, int lo, int hi)
{ juce::String s; for (int i = lo; i < hi; ++i) if (g_log[(size_t) i].serial == serial) s << evName (g_log[(size_t) i].ev) << " "; return s.trim(); }

juce::String chunkFor (int v) { return juce::Base64::toBase64 (&v, sizeof (int)); }
juce::var oneSlotVar()
{
    juce::Array<juce::var> arr; auto* o = new juce::DynamicObject();
    o->setProperty ("n", 1); o->setProperty ("plugin", "EJ Rec Mock"); o->setProperty ("bypassed", false);
    arr.add (juce::var (o)); return juce::var (arr);
}
juce::var oneStateVar (int v) { auto* o = new juce::DynamicObject(); o->setProperty ("1", chunkFor (v)); return juce::var (o); }

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
    std::printf ("attach_order: BUILD-PATH-IS-THE-SPEC gate (build vs switch call sequences)\n");

    // ================= PATH A: BUILD path (the spec) + borrow reuse =========
    std::printf ("== PATH A: BUILD path via loadBuiltinNow (THE SPEC), then park + reuse ==\n");
    {
        ChainHost host (ChainHost::Mode::Borrowed);
        host.prepare (48000.0, 512);
        const int t0 = (int) g_log.size();
        host.restoreSavedChain (oneSlotVar(), oneStateVar (4242));           // BUILD
        const int t1 = (int) g_log.size();
        auto* m = dynamic_cast<RecMock*> (host.getSlotProcessor (0));
        check (m != nullptr, "build produced a Rec Mock slot");
        const int s = m ? m->serial : -1;
        std::printf ("   BUILD timeline (serial %d): %s\n", s, timeline (s, t0, t1).toRawUTF8());
        check (anyPrepare (s, t0, t1) == 1,            "SPEC build: exactly ONE prepare", juce::String (anyPrepare (s, t0, t1)));
        check (countIn (s, Ev::PrepareSusp, t0, t1) == 0, "SPEC build: never prepared while suspended");
        check (countIn (s, Ev::Release, t0, t1) == 0,  "SPEC build: zero releaseResources");
        check (firstAnyPrepare (s, t0, t1) >= 0 && lastIdx (s, Ev::SetState, t0, t1) > firstAnyPrepare (s, t0, t1),
               "SPEC build: restore seed lands AFTER the single prepare (unchanged order)");

        host.releaseBorrowToPool();                                          // PARK
        const int t2 = (int) g_log.size();
        check (countIn (s, Ev::Release, t1, t2) == 1, "park: released ONCE (uninitialised while parked)");

        host.restoreSavedChain (oneSlotVar(), oneStateVar (4242));           // REUSE
        const int t3 = (int) g_log.size();
        auto* m2 = dynamic_cast<RecMock*> (host.getSlotProcessor (0));
        check (m2 != nullptr && m2->serial == s, "reuse: same instance came back", juce::String (s));
        std::printf ("   REUSE timeline (serial %d): %s\n", s, timeline (s, t2, t3).toRawUTF8());
        check (anyPrepare (s, t2, t3) == 1, "reuse: exactly ONE prepare at re-add (by the graph)", juce::String (anyPrepare (s, t2, t3)));
        check (countIn (s, Ev::PrepareSusp, t2, t3) == 0, "reuse: never prepared while suspended");
        check (firstIdx (s, Ev::SetState, t2, t3) >= 0 && firstIdx (s, Ev::SetState, t2, t3) < firstAnyPrepare (s, t2, t3),
               "reuse: pristine reset seeds BEFORE the re-add prepare (detached)");
    }

    // ================= PATH B: SWITCH path (plan Create) under a lease =====
    std::printf ("== PATH B: SWITCH path via applyStructurePlan Create, under a rack lease ==\n");
    {
        using namespace LinkShm::StructureEdit;
        ChainHost host (ChainHost::Mode::Primary);
        host.prepare (48000.0, 512);
        host.setAttachBypassed (true);                                       // the lease: arrive dry
        std::vector<SlotIdentity> base;
        std::vector<CurrentSlot> current;
        CurrentSlot c; c.identity.name = "EJ Rec Mock"; c.originIndex = -1; c.stateB64 = chunkFor (777);
        current.push_back (c);
        Plan plan = computePlan ("test-uid", base, current);
        auto scratch = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("attach_order_journal");
        scratch.createDirectory();
        const int t0 = (int) g_log.size();
        auto res = host.applyStructurePlan (scratch.getFullPathName(), plan);
        const int t1 = (int) g_log.size();
        check (res.ok, "applyStructurePlan applied", res.ok ? "" : res.failedAt);
        auto* mc = dynamic_cast<RecMock*> (host.getSlotProcessor (0));
        check (mc != nullptr, "plan created a Rec Mock slot");
        const int s = mc ? mc->serial : -1;
        check (mc != nullptr && mc->value == 777, "created slot carries the seeded state");
        std::printf ("   SWITCH timeline (serial %d): %s\n", s, timeline (s, t0, t1).toRawUTF8());

        // THE SPEC, per instance on the switch path:
        check (anyPrepare (s, t0, t1) == 1,               "switch: exactly ONE prepare (as completeLoad)", juce::String (anyPrepare (s, t0, t1)));
        check (countIn (s, Ev::Release, t0, t1) == 0,     "switch: zero releaseResources");
        check (countIn (s, Ev::PrepareSusp, t0, t1) == 0, "switch: NEVER prepared while suspended/parked");
        const int seedI = firstIdx (s, Ev::SetState, t0, t1), prepI = firstAnyPrepare (s, t0, t1);
        check (seedI >= 0 && prepI >= 0 && seedI < prepI, "switch: seed lands BEFORE the single prepare (=> before addNode)");
        check (countIn (s, Ev::Process, t0, t1) == 0,     "switch: never rendered before/during attach");
        check (host.getSlotInfo (0).bypassed == true,     "switch: slot arrives in the lease's target state (bypassed == attachBypassed_)");

        pump (host, 8);                                                      // lease still held
        const int t2 = (int) g_log.size();
        check (countIn (s, Ev::Process, t1, t2) == 0,     "switch: NOT rendered while the lease holds it dry (no live window)");

        host.setAttachBypassed (false);                                      // lease released
        host.setSlotBypassed (0, false);
        pump (host, 8);
        const int t3 = (int) g_log.size();
        check (countIn (s, Ev::Process, t2, t3) >= 1,     "switch: renders once the lease releases (the AU is live and prepared)");
        check (anyPrepare (s, t0, t3) == 1,               "switch: STILL exactly one prepare across the whole lease cycle", juce::String (anyPrepare (s, t0, t3)));
    }

    std::printf ("\n==== attach_order: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
