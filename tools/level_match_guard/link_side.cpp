// level_match_guard / LINK SIDE (21t-j, 28 Sep 2026). THREE real LinkProcessors against the Link's shipping
// archive, each with audio through it, each with its own trim. The V2 side presses Apply on a level-match card
// carrying three deltas; this side asserts what Sean's 10:10:34 log could not:
//   (1) each Link's OWN TRIM MOVED by its member's delta - the op used to reach ChainHost::applyChainEdits, miss
//       every rack-op name in the dry run, and abort with "count=0->0 ... ABORTED applied=0/1";
//   (2) a genuinely refused RACK edit acks "not_applied" with a reason, never "stale" - the log said stale one
//       line after "staleness guards passed";
//   (4) the frame each Link publishes is the one V2's record must convert: this side prints its trim and its
//       integrated figure so the V2 side can check the record against the strip.
// RED on the pre-21t-j tree: the trims do not move and the ack reads "stale".
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "LinkProcessor.h"
#include "LinkShm.h"
#include <cstdio>
#include <memory>
struct EchoJayLinkSyncTestAccess { static juce::String uid (LinkProcessor& p) { return p.instanceUid_; } };
using TA = EchoJayLinkSyncTestAccess;
namespace {
int failures = 0;
void check (bool ok, const juce::String& w, const juce::String& d = {})
{ std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
void pumpMs (int ms)
{ const double end = juce::Time::getMillisecondCounterHiRes() + ms;
  while (juce::Time::getMillisecondCounterHiRes() < end) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false); } }
bool waitFile (const juce::File& f, int ms)
{ const double end = juce::Time::getMillisecondCounterHiRes() + ms;
  while (juce::Time::getMillisecondCounterHiRes() < end) { if (f.existsAsFile()) return true; pumpMs (50); } return f.existsAsFile(); }
// Real audio, so the frame carries a real integrated figure for the record/strip comparison.
void feed (LinkProcessor& l, double seconds, float amp)
{
    juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
    const int blocks = (int) (seconds * 48000.0 / 512.0);
    juce::Random rng (1234);
    for (int b = 0; b < blocks; ++b)
    {
        for (int ch = 0; ch < 2; ++ch)
        { auto* d = buf.getWritePointer (ch); for (int i = 0; i < 512; ++i) d[i] = (rng.nextFloat() * 2.0f - 1.0f) * amp; }
        l.processBlock (buf, midi);
        if ((b % 20) == 19) pumpMs (1);   // the 30 Hz timer publishes frames and consumes ctrl-cmds
    }
}
}
int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::String H = argc > 1 ? argv[1] : "/tmp";
    juce::File (H).createDirectory();
    {
    struct One { std::unique_ptr<LinkProcessor> l; juce::String uid; float trim0 = 0.0f; float delta = 0.0f; };
    std::vector<One> links (3);
    const char* names[3] = { "v4_1", "v5_2", "V2_2" };
    const float trims[3] = { -2.5f, -2.4f, -2.6f };     // the trims Sean's six members were sitting at
    const float deltas[3] = { 3.6f, -1.5f, 2.0f };      // the card's three deltas (v4_1 owed +3.6)
    for (int i = 0; i < 3; ++i)
    {
        links[(size_t) i].l = std::make_unique<LinkProcessor>();
        links[(size_t) i].l->linkName = names[i];
        links[(size_t) i].l->markTypedNameAuthoritative();
        links[(size_t) i].l->prepareToPlay (48000.0, 512);
        links[(size_t) i].trim0 = trims[i];
        links[(size_t) i].delta = deltas[i];
    }
    for (int k = 0; k < 400; ++k)
    {
        bool all = true;
        for (auto& o : links) { o.uid = TA::uid (*o.l); if (o.uid.isEmpty()) all = false; }
        if (all) break;
        pumpMs (10);
    }
    for (auto& o : links)
        if (o.uid.isEmpty()) { std::printf ("link side: a Link never claimed a uid\n"); return 2; }
    for (auto& o : links) { o.l->setGainDb (o.trim0); }
    for (auto& o : links) feed (*o.l, 4.0, 0.2f);       // 4 s each: an integrated figure and closed 3 s windows
    pumpMs (300);
    check (true, "link side: three real Links, audio through each, trims set",
           juce::String (links[0].trim0, 1) + " / " + juce::String (links[1].trim0, 1) + " / "
           + juce::String (links[2].trim0, 1) + " dB");
    {
        auto* o = new juce::DynamicObject();
        for (int i = 0; i < 3; ++i)
        {
            auto* e = new juce::DynamicObject();
            e->setProperty ("uid", links[(size_t) i].uid);
            e->setProperty ("name", names[i]);
            e->setProperty ("trim", links[(size_t) i].trim0);
            e->setProperty ("delta", links[(size_t) i].delta);
            o->setProperty (juce::Identifier ("link" + juce::String (i + 1)), juce::var (e));
        }
        juce::File (H + "/link_ready.json").replaceWithText (juce::JSON::toString (juce::var (o), true));
    }
    std::printf ("link side: ready, waiting for the V2 side's Apply\n");

    // ---- (1) THE TRIMS MOVE ---------------------------------------------------------------------------------
    const bool applied = waitFile (juce::File (H + "/v2_applied.json"), 30000);
    for (int k = 0; k < 60; ++k) { for (auto& o : links) feed (*o.l, 0.1, 0.2f); pumpMs (50); }
    check (applied, "the V2 side pressed Apply (v2_applied.json)");
    for (int i = 0; i < 3; ++i)
    {
        const auto& o = links[(size_t) i];
        const float want = juce::jlimit (-24.0f, 12.0f, o.trim0 + o.delta);
        const float now  = o.l->getGainDb();
        check (std::abs (now - want) <= 0.1f,
               juce::String ("(1) \"") + names[i] + "\" moved its OWN trim by the member's delta  (RED as it "
               "stood: the op reached the rack sequencer, matched no rack op and ABORTED, applied=0/1)",
               "asked " + juce::String (want, 2) + ", reads " + juce::String (now, 2) + " dB");
    }
    {
        auto* o = new juce::DynamicObject();
        for (int i = 0; i < 3; ++i)
            o->setProperty (juce::Identifier ("trim" + juce::String (i + 1)), (double) links[(size_t) i].l->getGainDb());
        juce::File (H + "/link_trims.json").replaceWithText (juce::JSON::toString (juce::var (o), true));
    }

    // ---- (2) AN ABORTED RACK EDIT ACKS "not_applied", NOT "stale" -------------------------------------------
    const bool sentBad = waitFile (juce::File (H + "/v2_badedit.json"), 20000);
    for (int k = 0; k < 40; ++k) { pumpMs (50); for (auto& o : links) feed (*o.l, 0.05, 0.2f); }
    check (sentBad, "the V2 side sent a rack edit that cannot apply (v2_badedit.json)");
    {
        int e = 0; const juce::String dir = LinkShm::resolveDir (e);
        // THE CHAIN ACK, not the ctrl ack: a rack edit is answered in chain-ack-<uid>.json (writeChainAck), while
        // ctrl-ack-<uid>.json answers the gain/alias transport. The first cut read the wrong file and got an empty
        // object, which is the shape of "no answer" - and would have passed a guard that only checked != "stale".
        const juce::File ack (dir + "chain-ack-" + links[0].uid + ".json");
        juce::var av = juce::JSON::parse (ack.loadFileAsString());
        const auto status = av.getProperty ("status", juce::var()).toString();
        const auto why = av.getProperty ("reason", juce::var()).toString();
        std::printf ("    the ack for the refused edit: status=\"%s\" reason=\"%s\"\n",
                     status.toRawUTF8(), why.substring (0, 90).toRawUTF8());
        check (status == "not_applied",
               "(2) an ABORTED edit acks \"not_applied\"  (RED as it stood: it acked \"stale\", one line after "
               "\"staleness guards passed\")", status);
        check (status.isNotEmpty() && status != "stale",
               "(2) ...and never \"stale\", which means the rack moved under the edit (an EMPTY status is not a "
               "pass: that is the shape of no answer at all)", status.isEmpty() ? juce::String ("(empty)") : status);
        check (why.isNotEmpty(), "(2) ...with the reason the sequencer already had, not an empty detail",
               why.substring (0, 80));
    }
    { auto* o = new juce::DynamicObject(); o->setProperty ("done", true);
      juce::File (H + "/link_done.json").replaceWithText (juce::JSON::toString (juce::var (o), true)); }
    waitFile (juce::File (H + "/v2_done.json"), 20000);
    for (auto& o : links) o.l.reset();
    }
    std::printf ("\n==== level_match_guard (link side): %s (%d assertion(s) failed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
