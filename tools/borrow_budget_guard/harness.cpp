// borrow_budget_guard - RE-AIMED 11 October 2026, when the thing it guarded was deleted.
//
// WHAT IT GUARDED, and it was a real defect: the first in-context borrow of a session refused with
// "false term(s): budget", because the borrow budget - kBorrowAlignBudgetFrames, 16384 samples, 341.3 ms
// at 48 k - was reported to the host as extra latency and could therefore only be committed at a moment
// the user could not hear it (prepareToPlay, or a block observed with the transport STOPPED). Round 53
// ruled that; hurdle 1 item 4 then persisted "a capable Link was seen" with the state so a REOPENED
// session reserved the budget at prepare instead of waiting to flip WANTED mid-play. Six legs pinned it.
//
// WHY IT IS GONE. The budget existed for exactly one reason: to hold this plugin's passthrough at a
// CONSTANT delay so a V2-hosted copy of a Link's rack could be summed into the V2's own output,
// perfectly aligned whatever that rack's own latency. That injection is the audio detour, and the detour
// is what Sean's complaint was about - his kick left its own channel strip, its sends and its drum bus
// UNPROCESSED while the processed copy arrived at the V2's position. Plan section 6 took it out.
//
// SO THIS GUARD NOW PINS THE OPPOSITE INVARIANT, which is the user-visible half of the removal and is
// worth a gated test in its own right: ADDRESSING A LINK COSTS THE HOST NO LATENCY. It did, and it paid
// it whether or not any rack was ever addressed - merely HAVING a capable Link in the session was enough,
// because the budget followed capable-Link PRESENCE. A project with twelve Links on twelve tracks carried
// 341 ms of reported latency for them.
//
// The legs are the old ones inverted, deliberately, so the two directions are in one file: the same
// state blobs, the same play/stop sequence, the same reopened-session path - and every one of them now
// asserts that the reported figure does NOT move.
//   (1) the reported latency at prepareToPlay with nothing wanted (the baseline);
//   (2) a capable-Link listing mid-play moves nothing;
//   (3) ...and neither does a STOPPED block, which is where the budget used to commit;
//   (4) a REOPENED session whose state says a capable Link was seen reports the same figure (this is
//       the leg that would fail if the persisted flag ever started reserving a budget again);
//   (5) control: a session that never saw one reports the same figure as (4), i.e. the figure does not
//       depend on which Links the project has - which is the whole claim;
//   (6) and capableLinkSeen() is still PERSISTED, because the flag itself is not what went: other
//       things read it, and a guard that let it rot would hide that.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <cstdio>
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
    std::printf ("borrow_budget_guard: addressing a Link costs the host NO latency (the 16384-sample alignment budget was removed with the audio detour, 11 Oct 2026)\n");
    juce::MemoryBlock seenBlob, unseenBlob;
    int latBaseline = -1;
    {
        EchoJayProcessor proc; proc.prepareToPlay (48000.0, 512);
        TestPlayHead ph; proc.setPlayHead (&ph);
        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
        auto block = [&] (int n) { for (int i = 0; i < n; ++i) { buf.clear(); proc.processBlock (buf, midi); ph.pos += 512; } };
        ph.playing = true; block (10);
        latBaseline = proc.getLatencySamples();
        check (latBaseline < 16384, "(1) baseline: an empty chain reports its own latency, not a 16384-sample budget", juce::String (latBaseline));
        proc.getStateInformation (unseenBlob);   // a session that never saw a capable Link
        // The capable-Link listing used to flip WANTED ON here. The flag it set is all that is left of it.
        proc.markCapableLinkSeen();
        block (50);                              // ~0.5 s of PLAYING blocks
        check (proc.capableLinkSeen(), "(2) a capable-Link listing still RECORDS that it was seen");
        check (proc.getLatencySamples() == latBaseline,
               "(2) ...and reports NO added latency for it - a Link in the session is not a latency cost",
               "before=" + juce::String (latBaseline) + " after=" + juce::String (proc.getLatencySamples()));
        ph.playing = false; block (1); ph.playing = true;
        check (proc.getLatencySamples() == latBaseline,
               "(3) a STOPPED block - where the budget used to commit - moves the reported figure by nothing",
               juce::String (proc.getLatencySamples()));
        proc.getStateInformation (seenBlob);     // this session SAW a capable Link
    }
    std::printf ("== (4) a REOPENED session whose state says a capable Link was seen ==\n");
    int latSeen = -1, latUnseen = -1;
    {
        EchoJayProcessor proc;
        proc.setStateInformation (seenBlob.getData(), (int) seenBlob.getSize());
        proc.prepareToPlay (48000.0, 512);
        TestPlayHead ph; proc.setPlayHead (&ph); ph.playing = true;
        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
        for (int i = 0; i < 20; ++i) { buf.clear(); proc.processBlock (buf, midi); ph.pos += 512; }
        latSeen = proc.getLatencySamples();
        check (proc.capableLinkSeen(), "(6) capableLinkSeen is still PERSISTED across a save and reopen - the flag is not what was removed");
        check (latSeen == latBaseline, "(4) ...and reserves NO budget at prepareToPlay", juce::String (latSeen));
    }
    std::printf ("== (5) control: a session that never saw a capable Link ==\n");
    {
        EchoJayProcessor proc;
        proc.setStateInformation (unseenBlob.getData(), (int) unseenBlob.getSize());
        proc.prepareToPlay (48000.0, 512);
        latUnseen = proc.getLatencySamples();
        check (! proc.capableLinkSeen(), "(5) no capable Link ever seen -> the flag is false (the control direction)");
        check (latUnseen == latSeen,
               "(5) THE CLAIM: the reported latency is the SAME whether or not this project has Links",
               "seen=" + juce::String (latSeen) + " unseen=" + juce::String (latUnseen));
    }
    std::printf ("\n==== borrow_budget_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
