// link_build_level_guard (item 11, Sean's 13 Oct round): DOES A BUILD ON A LINK ARM THAT LINK'S LEVEL MATCH?
//
// Sean: after the vocal build the rack was LOUDER than its input, every slot trim at +0.0, and the Kramer PIE's
// own output gain was the cause. He expects a build to leave the rack at the same loudness.
//
// A post-build volume match exists and is fully wired on the Link side - resolveStage points at the Link's own
// OUT gain, aimIsTarget says a Link never hits a target, isPlaying and logLine are bound in the constructor.
// And `LoudnessLoop::armFromChain()` had exactly ONE caller in the whole product: the V2 editor's
// armLoudnessLoopIfTargeted(), which runs when the V2 builds ITS OWN rack. So a chain built onto a Link armed
// nothing at all. The loop was not failing to land the level; it was never asked.
//
// One process, a real LinkProcessor against the Link's shipping archive, a real chain-cmd on disk - because the
// claim is about the Link's own build path, and a stand-in calling armFromChain() directly would prove only
// that the function works, which was never in doubt.
//
//   (a) a BUILD command arms the loop, and it arms as a VOLUME MATCH driving the rack's OUT gain
//   (b) ...with nothing landed yet - arming is not a gain move, and a loop that moved a fader before hearing
//       audio would be a worse bug than the one this fixes
//   (c) a RESTORE does not arm: reopening a project must not re-level a rack the user has since set by hand
//
// RED on the tree before item 11: (a) fails - everArmed() is false after a build, because nothing called it.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "LinkProcessor.h"
#include "LinkShm.h"
#include "EedCompressorProcessor.h"   // force-link a built-in registrar so the spec can name one
#include "EedDeviceRegistry.h"
#include <cstdio>
#include <memory>

struct EchoJayLinkSyncTestAccess
{
    static juce::String uid (LinkProcessor& p) { return p.instanceUid_; }
    // leg (d): the Link's OWN output gain, read from the member the mixer strip uses - so the leg can say the
    // loop moved THAT control and not a number of its own. getGainDb() is public, but reading it through the
    // same access struct keeps every private read in this file in one place.
    static float gainDbOf (LinkProcessor& p) { return p.gainDb_.load (std::memory_order_relaxed); }
};
using TA = EchoJayLinkSyncTestAccess;

namespace {
int failures = 0;
void check (bool ok, const juce::String& w, const juce::String& d = {})
{
    std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(),
                 d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : "");
    if (! ok) ++failures;
}
void pumpMs (int ms)
{
    const double end = juce::Time::getMillisecondCounterHiRes() + ms;
    while (juce::Time::getMillisecondCounterHiRes() < end)
    { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false); }
}

// A v1 build command, exactly as the Build button writes one.
int writeBuild (const juce::String& dir, const juce::String& uid, const juce::StringArray& names)
{
    static int seq = 100;
    auto* cmd = new juce::DynamicObject();
    cmd->setProperty ("v", 1);
    cmd->setProperty ("seq", ++seq);
    juce::Array<juce::var> chain;
    for (const auto& n : names)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("name", n);
        chain.add (juce::var (o));
    }
    cmd->setProperty ("chain", juce::var (chain));
    juce::File (dir + "chain-ack-" + uid + ".json").deleteFile();
    juce::File (dir + "chain-cmd-" + uid + ".json").replaceWithText (juce::JSON::toString (juce::var (cmd), true));
    return seq;
}

// ---- THE HOST, MODELLED (14 Oct 2026) ---------------------------------------------------------------
//
// Sean: "Your guard proves the loop ARMS; prove it LANDS." A landing needs three things the old legs did
// not supply: a transport that says it is PLAYING (the Link reads that from the host's playhead inside
// processBlock, and the loop will not act without it), audio actually going THROUGH the rack, and enough
// of it for a measurement window to close.
//
// So the guard models the host rather than reaching past it: a real AudioPlayHead that reports playing and
// advances time, exactly as Logic's does. The memory rule is "legs model the host" - a leg's fidelity is a
// claim until the host's own behaviour backs it, and bypassing the playhead to poke transportPlaying_ would
// have proved the loop works when told to work, which is not the question.
struct PlayingHead : juce::AudioPlayHead
{
    double timeS = 0.0;
    // getCurrentPosition is GONE in this JUCE (it was the pre-7 API); getPosition is the one virtual to
    // implement, and the Link reads the transport through it - `pos->getIsPlaying()` in processBlock.
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setIsPlaying (true);
        p.setTimeInSeconds (timeS);
        p.setTimeInSamples ((juce::int64) (timeS * 48000.0));
        p.setBpm (120.0);
        return p;
    }
    bool canControlTransport() override { return false; }
};

/** RMS of one channel, in dBFS. */
float rmsDb (const juce::AudioBuffer<float>& b, int ch)
{
    double sum = 0.0;
    const int n = b.getNumSamples();
    for (int i = 0; i < n; ++i) { const double v = b.getSample (ch, i); sum += v * v; }
    const double r = std::sqrt (sum / juce::jmax (1, n));
    return r > 1.0e-9 ? (float) (20.0 * std::log10 (r)) : -200.0f;
}

/** Feed `seconds` of a steady -12 dBFS tone through the Link, in 512-sample blocks, ticking the loop and
    pumping the message thread as a host would. Returns the mean output RMS over the LAST quarter of the
    run, which is the part that reflects whatever the loop has landed by then. */
float feedTone (LinkProcessor& l, PlayingHead& ph, double seconds, double& phaseIo, float& inDbOut)
{
    const double sr = 48000.0;
    const int    block = 512;
    const int    blocks = (int) (seconds * sr / block);
    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;
    double outSum = 0.0; int outN = 0;
    double inSum = 0.0;  int inN = 0;
    const int tailFrom = (blocks * 3) / 4;
    for (int b = 0; b < blocks; ++b)
    {
        for (int i = 0; i < block; ++i)
        {
            const float v = 0.25f * (float) std::sin (phaseIo);       // -12 dBFS
            phaseIo += 2.0 * juce::MathConstants<double>::pi * 220.0 / sr;
            if (phaseIo > 2.0 * juce::MathConstants<double>::pi) phaseIo -= 2.0 * juce::MathConstants<double>::pi;
            buf.setSample (0, i, v); buf.setSample (1, i, v);
        }
        if (b >= tailFrom) { const double r = std::pow (10.0, rmsDb (buf, 0) / 20.0); inSum += r; ++inN; }
        midi.clear();
        l.processBlock (buf, midi);
        ph.timeS += (double) block / sr;
        if (b >= tailFrom) { const double r = std::pow (10.0, rmsDb (buf, 0) / 20.0); outSum += r; ++outN; }
        // The loop's own timer runs on the message thread; pump it every few blocks rather than once at the
        // end, so its windows close while audio is still arriving - which is how it happens in a host.
        if ((b % 8) == 0) { l.loudnessLoop().tickNow(); pumpMs (2); }
    }
    inDbOut  = inN  > 0 ? (float) (20.0 * std::log10 (inSum  / inN))  : -200.0f;
    return    outN > 0 ? (float) (20.0 * std::log10 (outSum / outN)) : -200.0f;
}

// Wait for the LINK's own ack, bounded, so no leg reads a loop state from before the build applied.
// A fixed sleep here would be the 12 Oct S5 mistake again: a guessed wait standing in for a wait.
bool waitAck (const juce::String& dir, const juce::String& uid, int seq, int maxMs, juce::String& statusOut)
{
    const juce::File ack (dir + "chain-ack-" + uid + ".json");
    const double end = juce::Time::getMillisecondCounterHiRes() + maxMs;
    while (juce::Time::getMillisecondCounterHiRes() < end)
    {
        if (ack.existsAsFile())
        {
            const auto v = juce::JSON::parse (ack.loadFileAsString());
            if ((int) v.getProperty ("seq", -1) == seq)
            { statusOut = v.getProperty ("status", juce::var()).toString(); return true; }
        }
        pumpMs (50);
    }
    return false;
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::ignoreUnused (argc, argv);
    std::printf ("==== link_build_level_guard (link side) ====\n");

    int err = 0;
    const juce::String dir = LinkShm::resolveDir (err);
    if (dir.isEmpty()) { std::printf ("  FAIL  no shared dir resolved\n"); std::printf ("==== link_build_level_guard (link side): RED ====\n"); return 1; }

    {
        auto l = std::make_unique<LinkProcessor>();
        l->prepareToPlay (48000.0, 512);
        pumpMs (400);                       // registry slot + the uid
        const auto uid = TA::uid (*l);
        std::printf ("  link uid %s\n", uid.toRawUTF8());
        if (uid.isEmpty()) { std::printf ("  FAIL  the Link never took a uid\n"); std::printf ("==== link_build_level_guard (link side): RED ====\n"); return 1; }

        auto& loop = l->loudnessLoop();
        check (! loop.everArmed(), "a fresh Link has not armed its levelling loop");

        // THE SCAN FIRST, AND WAITED FOR. The Link resolves a built-in's name against the shared entries
        // cache, and the first build in a fresh state root arrives before that cache exists: my first run of
        // this guard built against "list=0 entries", one slot of two loaded, the ack came back "partial", and
        // the arm - correctly gated on failures == 0 - did not run. That would have been a RED that said
        // nothing about item 11. A warm-up build does the scan; the measured build follows it.
        {
            const int warm = writeBuild (dir, uid, { "EchoJay EQ" });
            juce::String ws; waitAck (dir, uid, warm, 30000, ws);
            std::printf ("  warm-up build acked \"%s\" (it exists to populate the entries cache)\n", ws.toRawUTF8());
            pumpMs (300);
        }

        // ---- (a) A BUILD ARMS IT --------------------------------------------------------------------------
        // ONE built-in, so a plugin this machine happens not to have cannot turn the ack partial and take the
        // leg's subject with it. EchoJay EQ is compiled into the Link.
        const int seq = writeBuild (dir, uid, { "EchoJay EQ" });
        juce::String status;
        const bool acked = waitAck (dir, uid, seq, 30000, status);
        check (acked && status == "ok", "the Link acked the build \"ok\"", acked ? status : juce::String ("no ack"));
        pumpMs (300);   // the arm runs in the apply callback, after the ack is written

        check (loop.everArmed(),
               "(a) THE BUILD ARMED THE LINK'S LEVELLING LOOP  (RED as it stood: armFromChain had one caller, "
               "the V2's own-rack build, so a chain built onto a Link armed nothing)",
               loop.everArmed() ? juce::String ("armed") : juce::String ("NOT armed"));
        check (loop.aimWords() == "matched to input",
               "(a) ...as a VOLUME MATCH - a Link is a channel and never hits a target (Sean, 9 Oct)",
               loop.aimWords());
        check (loop.stageName() == "rack_out",
               "(a) ...driving the rack's OUT gain, which is the control the OUT caption describes",
               loop.stageName());

        // ---- (b) ARMING IS NOT A GAIN MOVE ---------------------------------------------------------------
        check (std::abs (loop.currentGainDb()) < 0.001f,
               "(b) nothing has been landed yet - the loop is listening, not guessing",
               juce::String (loop.currentGainDb(), 3) + " dB");

        // ---- (d) IT LANDS: Sean's own case, measured (14 Oct 2026) ---------------------------------------
        //
        // "Your guard proves the loop ARMS; prove it LANDS." His vocal build came out 5-6 dB out with the
        // rack OUT still reading 0.0 dB, so the question is not whether a flag flipped - it is whether the
        // OUTPUT comes back to the INPUT with audio running.
        //
        // The rack is made deliberately LOUD, the way Sean's was: +6 dB on the built slot's own OUT gain,
        // which is what a plugin arriving with its output up does to the rack (his Kramer PIE came back
        // from its stored state that way). Then real audio runs through it and the measurement is of the
        // buffers, not of a field.
        {
            PlayingHead ph;
            l->setPlayHead (&ph);
            l->getChainHost().setSlotOutGainDb (0, 6.0f);     // the rack is now +6 dB on its input
            pumpMs (100);

            double phase = 0.0;
            float inDb = 0.0f;
            const float before = loop.currentGainDb();

            // Bounded: up to 40 s of audio in 4 s passes, stopping as soon as the loop has written. A fixed
            // run would either be too short to let a window close or spend the time whatever happened.
            float outDb = 0.0f;
            int passes = 0;
            for (; passes < 10; ++passes)
            {
                outDb = feedTone (*l, ph, 4.0, phase, inDb);
                if (std::abs (loop.currentGainDb() - before) > 0.05f && std::abs (outDb - inDb) < 1.5f) break;
            }

            const float moved = loop.currentGainDb() - before;
            check (moved < -0.5f,
                   "(d) THE LOOP LANDED A CUT on the rack's OUT  (RED as it stood: the loop was never armed on "
                   "a Link at all, so nothing was ever written)",
                   "moved " + juce::String (moved, 2) + " dB after " + juce::String (passes + 1) + " pass(es)");
            check (std::abs (outDb - inDb) < 1.5f,
                   "(d) ...and the rack's OUTPUT now matches its INPUT, measured from the audio  (Sean's test: "
                   "the vocal came out 5-6 dB out)",
                   "in " + juce::String (inDb, 2) + " dBFS vs out " + juce::String (outDb, 2) + " dBFS");
            check (std::abs (moved + 6.0f) < 2.0f,
                   "(d) ...by about the 6 dB the rack was adding, not an arbitrary amount",
                   juce::String (moved, 2) + " dB against -6.0 expected");
            check (std::abs (TA::gainDbOf (*l) - loop.currentGainDb()) < 0.01f,
                   "(d) ...and what the loop reports IS the Link's own OUT gain, not a private number",
                   juce::String (TA::gainDbOf (*l), 2) + " vs " + juce::String (loop.currentGainDb(), 2));
            l->setPlayHead (nullptr);
        }
    }

    // ---- (c) A RESTORE DOES NOT ARM ---------------------------------------------------------------------
    // The control that stops the fix becoming a new bug: a project reopening must not re-level a rack whose
    // gain the user has since set by hand. Restore and Assistant are different LoadOrigins and only one arms.
    {
        auto l = std::make_unique<LinkProcessor>();
        l->prepareToPlay (48000.0, 512);
        pumpMs (400);
        const auto uid = TA::uid (*l);

        // Build, save the Link's own chunk, then restore it into a FRESH Link: the restore path.
        const int seq = writeBuild (dir, uid, { "EchoJay EQ" });
        juce::String status; waitAck (dir, uid, seq, 30000, status);
        pumpMs (300);
        juce::MemoryBlock chunk;
        l->getStateInformation (chunk);
        check (chunk.getSize() > 0, "the built Link saved a chunk", juce::String ((int) chunk.getSize()) + " bytes");

        auto fresh = std::make_unique<LinkProcessor>();
        fresh->prepareToPlay (48000.0, 512);
        pumpMs (400);
        fresh->setStateInformation (chunk.getData(), (int) chunk.getSize());
        pumpMs (1500);   // the restore hosts its slots asynchronously
        check (! fresh->loudnessLoop().everArmed(),
               "(c) a RESTORE does not arm - reopening a project must not re-level a rack the user has since set",
               fresh->loudnessLoop().everArmed() ? juce::String ("ARMED (wrong)") : juce::String ("not armed"));
    }

    std::printf ("==== link_build_level_guard (link side): %s ====\n", failures == 0 ? "GREEN" : "RED");
    return failures == 0 ? 0 : 1;
}
