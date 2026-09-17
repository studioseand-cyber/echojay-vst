// borrow_budget_guard — round B guard PROPOSAL (17 Sep 2026), on the real processor.
// The first in-context borrow of a session refused with "false term(s): budget"
// because the borrow budget is committed ONLY at prepareToPlay or at a STOPPED
// block (PluginProcessor.cpp:595-633, commitBorrowBudget :2763). While the
// transport keeps playing after the budget is WANTED, nothing commits it, the
// engage is refused, and the user hears the Link's leased dry stream until a
// stop/start. This leg models exactly that: budget wanted while PLAYING, N
// playing blocks, then the budget must be ACTIVE (RED today).
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <cstdio>
struct EchoJayAlignTestAccess { static bool active (EchoJayProcessor& p) { return p.borrowBudgetActive_.load(); } static bool wanted (EchoJayProcessor& p) { return p.borrowBudgetWanted_.load(); } };
namespace {
struct TestPlayHead : juce::AudioPlayHead { juce::int64 pos = 0; bool playing = true;
    juce::Optional<juce::AudioPlayHead::PositionInfo> getPosition() const override { juce::AudioPlayHead::PositionInfo p; p.setTimeInSamples (pos); p.setIsPlaying (playing); return p; } };
int failures = 0; void check (bool ok, const juce::String& what, const juce::String& d = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", what.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
}
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ej_budget_" + juce::String (juce::Time::getMillisecondCounter()));
    tmp.createDirectory(); setenv ("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);
    std::printf ("borrow_budget_guard: the budget wanted mid-play must become ACTIVE without a stop\n");
    EchoJayProcessor proc; proc.prepareToPlay (48000.0, 512);
    TestPlayHead ph; proc.setPlayHead (&ph);
    juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
    auto block = [&] (int n) { for (int i = 0; i < n; ++i) { buf.clear(); proc.processBlock (buf, midi); ph.pos += 512; } };
    ph.playing = true; block (10);
    check (! EchoJayAlignTestAccess::active (proc), "budget OFF at start (prepareToPlay with nothing wanted)");
    proc.setBorrowBudgetWanted (true);   // the capable-Link listing flips WANTED ON mid-play (PluginProcessor.cpp:5125)
    block (50);                          // ~0.5 s of PLAYING blocks
    check (EchoJayAlignTestAccess::wanted (proc), "budget WANTED after the listing");
    check (EchoJayAlignTestAccess::active (proc), "budget ACTIVE within 50 playing blocks (no stop needed) - the first-borrow dry audio defect", EchoJayAlignTestAccess::active (proc) ? "active" : "still pending: commits only at prepareToPlay or a STOPPED block");
    ph.playing = false; block (1); ph.playing = true;
    check (EchoJayAlignTestAccess::active (proc), "control: after ONE stopped block the budget commits (today's only path)");
    std::printf ("\n==== borrow_budget_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
