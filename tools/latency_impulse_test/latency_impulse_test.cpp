/*  latency_impulse_test (rounds 52-53, DEFECT_PRESS_PLAY_PHASING §9-§10):
    does the plugin ACTUALLY delay by what it REPORTS, in every state the
    borrow alignment budget can be in - including PENDING states, where the
    ruling's point is that nothing half-engages? An impulse through the
    top-level EchoJay V2 processor (empty chain), the output peak located,
    compared with getLatencySamples(). The transport is a fake play head the
    test drives; latency notifications are counted through the processor's
    listener (the same signal the host gets). Also the C4 scoping decision
    and process liveness, and the quiet session (L7).
*/
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <cstdio>
#include <cstdlib>
#include <signal.h>
#include <unistd.h>

static int g_fail = 0;
static void check (bool ok, const juce::String& what)
{ std::printf ("  [%s] %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8()); if (! ok) ++g_fail; }

struct FakeHead : juce::AudioPlayHead
{
    bool playing = false, known = true; double t = 0.0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        if (! known) return {};
        PositionInfo p; p.setIsPlaying (playing); p.setTimeInSeconds (t); return p;
    }
};
struct LatencyCounter : juce::AudioProcessorListener
{
    int changes = 0;
    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails& d) override { if (d.latencyChanged) ++changes; }
};

static void runSilent (EchoJayProcessor& p, int blk, int blocks)
{ juce::AudioBuffer<float> b (2, blk); juce::MidiBuffer m; for (int i = 0; i < blocks; ++i) { b.clear(); p.processBlock (b, m); } }

static int measure (EchoJayProcessor& p, int blk, int maxBlocks, float& peak)
{
    juce::AudioBuffer<float> b (2, blk); juce::MidiBuffer m;
    int at = -1; peak = 0.0f;
    for (int i = 0; i < maxBlocks; ++i)
    {
        b.clear();
        if (i == 0) { b.setSample (0, 0, 1.0f); b.setSample (1, 0, 1.0f); }
        p.processBlock (b, m);
        for (int s = 0; s < blk; ++s)
        { const float v = std::fabs (b.getSample (0, s)); if (v > peak) { peak = v; at = i * blk + s; } }
    }
    return at;
}

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI juceInit;
    const int blk = 1024, maxBlocks = 40;   // 40960 samples: past the 16384 budget
    auto leg = [&] (const char* name, EchoJayProcessor& p, int expectReported)
    {
        float peak = 0.0f;
        const int at = measure (p, blk, maxBlocks, peak);
        const int rep = p.getLatencySamples();
        std::printf ("    %-40s reported %6d   impulse out at %6d (peak %.3f)   %s\n", name, rep, at, peak,
                     (at == rep && rep == expectReported) ? "MATCH" : "MISMATCH");
        check (at == rep && rep == expectReported, juce::String (name) + ": reported == actual == " + juce::String (expectReported));
    };

    std::printf ("== L3: REPORT AND DELAY MOVE TOGETHER, INCLUDING WHILE PENDING (five states + C2) ==\n");
    {
        EchoJayProcessor p; FakeHead head; p.setPlayHead (&head);
        LatencyCounter cnt; p.addListener (&cnt);
        p.prepareToPlay (48000.0, blk);
        const int afterPrepare = cnt.changes;
        head.playing = false;
        leg ("(a) budget off, fresh, stopped", p, 0);
        head.playing = true; p.setBorrowBudgetWanted (true);
        leg ("(b) wanted ON during PLAYBACK: pending", p, 0);
        head.known = false;   // C2: unknown transport counts as playing
        leg ("(b') wanted ON, transport UNKNOWN: pending", p, 0);
        head.known = true;
        check (cnt.changes == afterPrepare, "no latency notification while pending (" + juce::String (cnt.changes - afterPrepare) + ")");
        head.playing = false; runSilent (p, blk, 1);   // one STOPPED block commits
        leg ("(c) committed at STOP", p, 16384);
        check (cnt.changes == afterPrepare + 1, "exactly ONE latency notification at the stop (" + juce::String (cnt.changes - afterPrepare) + ")");
        head.playing = true; runSilent (p, blk, 4);
        check (cnt.changes == afterPrepare + 1, "play again: zero further notifications");
        p.setBorrowBudgetWanted (false);
        leg ("(d) wanted OFF during PLAYBACK: pending-down", p, 16384);
        check (cnt.changes == afterPrepare + 1, "no notification while pending-down (C3: both directions queue)");
        head.playing = false; runSilent (p, blk, 1);
        leg ("(e) committed-down at STOP", p, 0);
        check (cnt.changes == afterPrepare + 2, "exactly one notification at the second stop");
        p.removeListener (&cnt);
    }

    std::printf ("== prepare is the guaranteed re-decision point ==\n");
    {
        EchoJayProcessor p; FakeHead head; head.playing = true; p.setPlayHead (&head);
        p.setBorrowBudgetWanted (true);
        p.prepareToPlay (48000.0, blk);   // commits at prepare even though the head says playing
        leg ("wanted ON before prepare -> committed at prepare", p, 16384);
    }

    std::printf ("== L7: QUIET SESSION - no Link anywhere: reported stays at the chain total, zero notifications after prepare ==\n");
    {
        EchoJayProcessor p; FakeHead head; p.setPlayHead (&head);
        LatencyCounter cnt; p.addListener (&cnt);
        p.prepareToPlay (48000.0, blk);
        const int afterPrepare = cnt.changes;
        for (int i = 0; i < 6; ++i) { head.playing = (i % 2 == 0); runSilent (p, blk, 200); }   // 1200 blocks, play/stop alternating
        check (p.getLatencySamples() == 0 && cnt.changes == afterPrepare,
               "1200 blocks of play/stop: reported " + juce::String (p.getLatencySamples()) + " (chain total 0), notifications after prepare " + juce::String (cnt.changes - afterPrepare));
        p.removeListener (&cnt);
    }

    std::printf ("== L4: SCOPING - the decision, and process liveness ==\n");
    {
        ChainHost::HostIdentity me; me.pid = (int) ::getpid(); me.startSec = 1000; me.startUsec = 500;
        EchoJayProcessor::BudgetRow row; row.inContextCapable = true; row.publisherPid = (int) ::getpid();
        row.hostPid = me.pid; row.hostStartSec = me.startSec; row.hostStartUsec = me.startUsec;
        check (EchoJayProcessor::budgetRowCounts (row, me, true), "POSITIVE CONTROL: an in-process, capable, live row COUNTS");
        auto foreign = row; foreign.hostPid = me.pid + 1;
        check (! EchoJayProcessor::budgetRowCounts (foreign, me, true), "a row from a DIFFERENT host process does not count");
        auto recycled = row; recycled.hostStartSec = me.startSec + 1;
        check (! EchoJayProcessor::budgetRowCounts (recycled, me, true), "same pid, different process start (a recycled pid) does not count");
        check (! EchoJayProcessor::budgetRowCounts (row, me, false), "a row whose publisher is DEAD does not count");
        auto old = row; old.publisherPid = 0; old.hostPid = 0;
        check (! EchoJayProcessor::budgetRowCounts (old, me, true), "an old sidecar with no publisher/host fields never counts (fail closed)");
        auto incapable = row; incapable.inContextCapable = false;
        check (! EchoJayProcessor::budgetRowCounts (incapable, me, true), "a live in-process row that is not in-context capable does not count");
        // the liveness probe the pass uses
        check (::kill ((pid_t) ::getpid(), 0) == 0, "liveness probe: our own pid is alive");
        check (::kill ((pid_t) 999999, 0) != 0, "liveness probe: pid 999999 is dead");
    }

    std::printf ("\n%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILURES", g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
