/*  latency_impulse_test (rounds 52-53, DEFECT_PRESS_PLAY_PHASING §9-§10):
    does the plugin ACTUALLY delay by what it REPORTS? An impulse through the
    top-level EchoJay V2 processor (empty chain), the output peak located,
    compared with getLatencySamples(). The transport is a fake play head the
    test drives; latency notifications are counted through the processor's
    listener (the same signal the host gets).

    RE-AIMED 11 October 2026, when its subject was deleted. It was built around
    "every state the borrow alignment budget can be in - including PENDING
    states, where the ruling's point is that nothing half-engages". There is no
    budget: it existed to hold this plugin's passthrough at a constant delay so
    a V2-hosted copy of a Link's rack could be summed into the V2's own output,
    and that injection is the audio detour plan section 6 removed. So the
    question this file asks is now a simpler and better one - does the report
    stay put? - and the three budget legs are inverted rather than deleted,
    because the measurement rig they sit in (impulse in, peak located, compared
    with the report) is the part worth keeping.

    THE GATED VERSION OF THIS CLAIM IS borrow_budget_guard, which was re-aimed
    the same day. This file is the by-hand impulse rig: it measures the DELAY,
    not just the reported figure, which is the one thing a state-only guard
    cannot do.
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

    std::printf ("== L3 (RE-AIMED 11 Oct 2026): THE REPORT AND THE DELAY ARE THE CHAIN'S, AND THEY DO NOT MOVE ==\n");
    {
        EchoJayProcessor p; FakeHead head; p.setPlayHead (&head);
        LatencyCounter cnt; p.addListener (&cnt);
        p.prepareToPlay (48000.0, blk);
        const int afterPrepare = cnt.changes;
        head.playing = false;
        leg ("(a) fresh, stopped: the delay IS the report", p, 0);
        // What (b)-(e) used to do: flip the budget WANTED on mid-play, prove nothing half-engaged, commit at a
        // STOPPED block, measure +16384, and queue the way back down. All five states were about one reported
        // figure that followed capable-LINK PRESENCE. A capable Link is still noticed - the flag is still
        // persisted with the state, because other things read it - but it buys no latency, so there is nothing
        // to half-engage and no moment to choose for committing it.
        head.playing = true; p.markCapableLinkSeen();
        leg ("(b) a capable Link seen during PLAYBACK: the delay does not move", p, 0);
        head.known = false;   // C2's case survives: unknown transport is still a state this rig visits
        leg ("(b') ...with the transport UNKNOWN either", p, 0);
        head.known = true;
        head.playing = false; runSilent (p, blk, 1);   // the moment the budget used to commit
        leg ("(c) a STOPPED block - where the budget used to commit - moves nothing", p, 0);
        head.playing = true; runSilent (p, blk, 4);
        leg ("(d) playing again: still nothing", p, 0);
        check (cnt.changes == afterPrepare,
               "ZERO latency notifications across the whole sequence - a project is no longer re-PDC'd for "
               "having a Link in it (" + juce::String (cnt.changes - afterPrepare) + ")");
        p.removeListener (&cnt);
    }

    std::printf ("== prepare is still the guaranteed re-decision point, and it decides the same thing ==\n");
    {
        EchoJayProcessor p; FakeHead head; head.playing = true; p.setPlayHead (&head);
        p.markCapableLinkSeen();
        p.prepareToPlay (48000.0, blk);
        leg ("a capable Link seen BEFORE prepare -> the report is still the chain's", p, 0);
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
