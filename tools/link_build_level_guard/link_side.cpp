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

struct EchoJayLinkSyncTestAccess { static juce::String uid (LinkProcessor& p) { return p.instanceUid_; } };
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
