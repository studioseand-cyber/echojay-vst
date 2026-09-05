/*  latency_impulse_test (round 52, 5 Sep 2026, DEFECT_PRESS_PLAY_PHASING):
    does the plugin ACTUALLY delay by what it REPORTS, in every state the
    alignment budget can be in? An impulse through the top-level processor
    (EchoJay V2, empty chain), the output peak located, compared with
    getLatencySamples(). HOME sandboxed like borrowhost_test.
*/
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <cstdio>
#include <cstdlib>

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
        {
            const float v = std::fabs (b.getSample (0, s));
            if (v > peak) { peak = v; at = i * blk + s; }
        }
    }
    return at;
}

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI juceInit;
    // Not sandboxed: JUCE resolves app-data from the account, not $HOME. This
    // test constructs a processor and pushes audio, nothing else; the build
    // script snapshots ~/Library/Application Support/EchoJay before and after
    // and prints any file that changed.
    std::printf ("== IMPULSE vs REPORTED LATENCY, EchoJay V2 top level, empty chain, 48 kHz / 1024 ==\n");
    const int blk = 1024, maxBlocks = 40;   // 40960 samples: past the 16384 budget
    int fails = 0;
    auto leg = [&] (const char* name, EchoJayProcessor& p)
    {
        float peak = 0.0f;
        const int at = measure (p, blk, maxBlocks, peak);
        const int rep = p.getLatencySamples();
        std::printf ("  %-44s reported %6d   impulse out at %6d (peak %.3f)   %s\n", name, rep, at, peak,
                     at == rep ? "MATCH" : "MISMATCH");
        if (at != rep) ++fails;
    };
    {
        EchoJayProcessor p; p.prepareToPlay (48000.0, blk);
        leg ("budget OFF (fresh, no capable Link)", p);
        p.setBorrowBudgetActive (true);          // what the registry pass did at 19:17:39 in Sean's session
        leg ("budget ON (capable Link seen), same instance", p);
        p.setBorrowBudgetActive (false);
        leg ("budget OFF again", p);
    }
    {
        EchoJayProcessor p; p.prepareToPlay (48000.0, blk);
        p.setBorrowBudgetActive (true);
        p.prepareToPlay (48000.0, blk);          // a re-prepare with the budget already on
        leg ("budget ON before prepare (re-prepared)", p);
    }
    std::printf ("%s (%d mismatch%s)\n", fails == 0 ? "ALL MATCH" : "MISMATCH", fails, fails == 1 ? "" : "es");
    return fails == 0 ? 0 : 1;
}
