// borrow_budget_guard - round B proposal (17 Sep 2026), extended by HURDLE 1 ITEM 4, on the real processor.
// The first in-context borrow of a session refused with "false term(s): budget" because the borrow budget is
// committed ONLY at prepareToPlay or at a STOPPED block (PluginProcessor.cpp commitBorrowBudget). THE BUDGET:
// kBorrowAlignBudgetFrames = 16384 samples = 341.3 ms at 48 k, reported to the host as extra latency while ACTIVE
// (one PDC re-run on the transition; the ring alignment target 1024 = the edit cushion is independent of it).
// RULED FIX (item 4): the persisted state remembers that a capable Link was seen; a REOPENED session restores it
// before prepareToPlay, so the budget is reserved AT PREPARE - no stop needed. The very first session keeps the
// mid-play -> stopped-block path.
//   (1) budget OFF at start; (2) WANTED after the listing; (3) FIRST SESSION, ruled: wanted mid-play stays pending
//   while playing and commits at the first STOPPED block; (4) the committed budget reports +16384 latency;
//   (5) ITEM 4: a state blob from a session that saw a capable Link, restored into a NEW processor, makes the budget
//   ACTIVE at prepareToPlay with no stop and no listing (RED today: nothing is persisted, OFF);
//   (6) control: a state blob from a session that never saw one leaves it OFF.
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
    std::printf ("borrow_budget_guard: the budget (16384 smp = 341.3 ms @48k) - first session commits at a stop; a reopened session reserves it at prepareToPlay\n");
    juce::MemoryBlock seenBlob, unseenBlob;
    {
        EchoJayProcessor proc; proc.prepareToPlay (48000.0, 512);
        TestPlayHead ph; proc.setPlayHead (&ph);
        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
        auto block = [&] (int n) { for (int i = 0; i < n; ++i) { buf.clear(); proc.processBlock (buf, midi); ph.pos += 512; } };
        ph.playing = true; block (10);
        const int latOff = proc.getLatencySamples();
        check (! EchoJayAlignTestAccess::active (proc), "(1) budget OFF at start (prepareToPlay with nothing wanted)");
        proc.getStateInformation (unseenBlob);   // a session that never saw a capable Link
        proc.setBorrowBudgetWanted (true);       // the capable-Link listing flips WANTED ON mid-play (refreshLinkRegistry)
        block (50);                              // ~0.5 s of PLAYING blocks
        check (EchoJayAlignTestAccess::wanted (proc), "(2) budget WANTED after the listing");
        check (! EchoJayAlignTestAccess::active (proc), "(3) FIRST SESSION (ruled): wanted mid-play stays PENDING while playing - commits only at a stop", EchoJayAlignTestAccess::active (proc) ? "active" : "pending");
        ph.playing = false; block (1); ph.playing = true;
        check (EchoJayAlignTestAccess::active (proc), "(3) FIRST SESSION: after ONE stopped block the budget commits");
        const int latOn = proc.getLatencySamples();
        check (latOn - latOff == 16384, "(4) the committed budget reports +16384 samples of latency (341.3 ms @48k; one PDC re-run)", "before=" + juce::String (latOff) + " after=" + juce::String (latOn));
        proc.getStateInformation (seenBlob);     // this session SAW a capable Link
    }
    std::printf ("== (5) ITEM 4: a reopened session restores the state BEFORE prepareToPlay ==\n");
    {
        EchoJayProcessor proc;
        proc.setStateInformation (seenBlob.getData(), (int) seenBlob.getSize());
        proc.prepareToPlay (48000.0, 512);
        TestPlayHead ph; proc.setPlayHead (&ph); ph.playing = true;
        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
        for (int i = 0; i < 20; ++i) { buf.clear(); proc.processBlock (buf, midi); ph.pos += 512; }   // playing, never stopped, no listing yet
        check (EchoJayAlignTestAccess::active (proc), "(5) state says capable seen -> budget ACTIVE at prepareToPlay, no stop needed, before any listing", EchoJayAlignTestAccess::active (proc) ? "active" : "OFF - nothing persisted: the first borrow of this session would be dry");
        check (proc.getLatencySamples() >= 16384, "(5) and the +16384 budget is already in the reported latency", juce::String (proc.getLatencySamples()));
    }
    std::printf ("== (6) control: a session that never saw a capable Link ==\n");
    {
        EchoJayProcessor proc;
        proc.setStateInformation (unseenBlob.getData(), (int) unseenBlob.getSize());
        proc.prepareToPlay (48000.0, 512);
        check (! EchoJayAlignTestAccess::active (proc) && ! EchoJayAlignTestAccess::wanted (proc), "(6) no capable Link ever seen -> budget OFF at prepareToPlay (no added latency for a project with no Links)");
    }
    std::printf ("\n==== borrow_budget_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
