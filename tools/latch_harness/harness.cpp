// Four-state last-good meter LATCH harness (15 Sep 2026).
// Seeds real LinkMeterFrames into an isolated registry, constructs the REAL
// EchoJayProcessor + EchoJayEditor, drives the 1 Hz recency poll to populate the
// latch, and asserts the assembled Link block for the four states the latch must
// handle. It calls no new method - it drives frames + the poll + the real
// assembly and reads the emitted text, so it goes RED on the PRE-LATCH binary
// (live decayed frame served, or the 5-min Lapse) and GREEN with the latch.
//
//   1 healthy-live                  : good frame; momentary from latch, "overall since playback"
//   2 decayed-live-with-latch       : latch=good, live decayed; momentary PRESENT (latched), crest latched not 29.8
//   3 live-cumulative-invalid (E)   : latch=good, live integrated floored; input from latch, "all while-flowing", NOT never-heard
//   4 no-latch-ever                 : short-window never valid; Lapse "LINK NEVER HEARD"
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "LinkShm.h"
#include <cstdio>
#include <cstring>
using namespace juce;

static void*    g_rmap = nullptr;
static uint32_t g_hb   = 100;
static int      g_fails = 0;

static void writeFrameAt(int slot, const LinkMeterFrame& f)
{
    LinkMeterFrame* dst = LinkShm::meterFrames(g_rmap) + slot;
    const uint32_t s0 = LinkShm::loadRelaxed(&dst->seq) & ~1u;
    LinkShm::storeRelease(&dst->seq, s0 + 1);
    std::memcpy(reinterpret_cast<uint8_t*>(dst) + sizeof(uint32_t),
                reinterpret_cast<const uint8_t*>(&f) + sizeof(uint32_t),
                sizeof(LinkMeterFrame) - sizeof(uint32_t));
    LinkShm::storeRelease(&dst->seq, s0 + 2);
}

// A good, while-flowing frame: short-window fields valid, distinct values.
static LinkMeterFrame goodFrame()
{
    LinkMeterFrame f {};
    f.momentary=-18.5f; f.shortTerm=-19.2f; f.integrated=-20.0f;
    f.truePeakMax=-1.3f; f.truePeakCur=-1.6f; f.crest=8.4f;
    f.correlation=0.55f; f.width=0.72f; f.lra=6.1f; f.shortTermTP=-2.4f;
    f.bandRel[0]=1.4f; f.bandRel[1]=0.3f; f.bandRel[2]=-0.6f;
    f.bandRel[3]=-0.9f; f.bandRel[4]=0.7f; f.bandRel[5]=3.4f;
    f.audioBlocks=1000; f.audioStale=0;
    return f;
}
// Decayed live frame: short-window collapsed (momentary floored, bands zero,
// crest inflated), CUMULATIVE still valid - the 10:46-vs-10:10 signature.
static LinkMeterFrame decayedFrame()
{
    LinkMeterFrame f {};
    f.momentary=-100.0f; f.shortTerm=-37.5f; f.integrated=-20.0f;  // integrated still valid
    f.truePeakMax=-1.3f; f.crest=29.8f;                            // inflated decay crest
    f.correlation=1.0f; f.width=0.0f; f.lra=6.1f; f.shortTermTP=-2.4f;
    for (int i=0;i<6;++i) f.bandRel[i]=0.0f;                       // bands collapsed
    f.audioBlocks=2000; f.audioStale=0;
    return f;
}
// Link-restart frame: live CUMULATIVE also invalid (integrated floored).
static LinkMeterFrame restartFrame()
{
    LinkMeterFrame f {};
    f.momentary=-100.0f; f.shortTerm=-100.0f; f.integrated=-100.0f;
    f.truePeakMax=-100.0f; f.crest=0.0f;
    for (int i=0;i<6;++i) f.bandRel[i]=0.0f;
    f.audioBlocks=3000; f.audioStale=0;
    return f;
}
// Never-good frame: short-window never valid from the start (integrated valid,
// so a pre-latch binary would still emit measurements - which is the RED).
// NEVER HEARD, as of 21t-i (27 Sep 2026 ruling). This frame used to carry integrated -20 with no valid short
// window: the LATCH refused it (momentary floored, no bands) and the block said "LINK NEVER HEARD". Every chat
// block is now composed from the stored level RECORD, and an integrated LUFS figure only exists if audio was
// gated in - so a frame carrying INT -20 is a channel that HAS been heard, and claiming otherwise beside that
// figure is the exact contradiction this round exists to remove (a strip holding a figure while the block said
// no signal). "Never heard" therefore means a frame with NO figure at all, which is what this now is.
static LinkMeterFrame neverGoodFrame()
{
    LinkMeterFrame f {};
    f.momentary=-100.0f; f.shortTerm=-100.0f; f.integrated=-100.0f;
    f.truePeakMax=-100.0f; f.shortTermTP=-100.0f;
    for (int i=0;i<6;++i) f.bandRel[i]=0.0f;
    f.audioBlocks=100; f.audioStale=0;
    return f;
}

// ...and the frame that used to be "never good": no short window, but an INTEGRATED figure. It is a MEASUREMENT
// now, and state 4b asserts the block reports it instead of denying it.
static LinkMeterFrame intOnlyFrame()
{
    LinkMeterFrame f {};
    f.momentary=-100.0f; f.shortTerm=-100.0f; f.integrated=-20.0f;
    for (int i=0;i<6;++i) f.bandRel[i]=0.0f;
    f.audioBlocks=100; f.audioStale=0;
    return f;
}

static int seedLink(const char* uid, const char* name)
{
    int err=0; const String dir=LinkShm::resolveDir(err);
    if (dir.isEmpty()) { std::fprintf(stderr, "seed: resolveDir failed\n"); return -1; }
    if (!g_rmap) { int fd=-1,oerr=0; g_rmap=LinkShm::openRegistry(dir, fd, oerr); }
    if (!g_rmap) { std::fprintf(stderr, "seed: openRegistry failed\n"); return -1; }
    const int slot=LinkShm::claimSlot(g_rmap, name, "", uid, 48000.0f, 2u);
    if (slot<0) { std::fprintf(stderr, "seed: claimSlot failed\n"); return -1; }
    auto* slots=LinkShm::regSlots(g_rmap);
    slots[slot].placement=1; slots[slot].dialCapable=1;
    LinkShm::storeRelease(&slots[slot].heartbeat,100u);
    LinkShm::storeRelease(&slots[slot].inUse,1u);
    return slot;
}

// Climb every claimed slot's heartbeat and drive the registry + recency poll, so
// each seeded Link reads LIVE and the poll latches any good short-window frame.
static void pump(EchoJayProcessor& proc)
{
    auto* slots=LinkShm::regSlots(g_rmap);
    for (int k=0;k<8;++k)
    {
        ++g_hb;
        for (int s=0;s<256;++s)
            if (LinkShm::loadRelaxed(&slots[s].inUse)) LinkShm::storeRelease(&slots[s].heartbeat, g_hb);
        proc.refreshLinkRegistry();
        proc.updateLinkAudioRecency();
    }
}

static void check(const char* label, bool ok)
{
    std::fprintf(stderr, "   %-58s %s\n", label, ok ? "PASS" : "FAIL  <-- RED");
    if (!ok) ++g_fails;
}

int main()
{
    ScopedJuceInitialiser_GUI gui;
    File tmp = File::getSpecialLocation(File::tempDirectory)
                   .getChildFile("ej_latch_harness_" + String(Time::getMillisecondCounter()));
    tmp.createDirectory();
    setenv("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);

    const int slot01 = seedLink("TLINK01", "Nafe Lead Vocal");
    const int slot02 = seedLink("TLINK02", "Backing Vox");
    if (slot01 < 0 || slot02 < 0) { std::fprintf(stderr, "HARNESS ERROR: seed failed\n"); return 2; }

    EchoJayProcessor proc;
    std::unique_ptr<AudioProcessorEditor> edBase(proc.createEditor());
    auto* ed = dynamic_cast<EchoJayEditor*>(edBase.get());
    if (!ed) { std::fprintf(stderr, "HARNESS ERROR: no editor\n"); return 2; }
    auto assemble = [&](const char* uid) -> String
    { StringArray mf; return ed->testAssembleChainInjections("build me a mastering chain", String(uid), &mf); };

    // ---- STATE 1: healthy-live (good frame, latched, live=good) ----
    writeFrameAt(slot01, goodFrame());
    writeFrameAt(slot02, neverGoodFrame());   // TLINK02 stays never-good throughout
    pump(proc);
    const String s1 = assemble("TLINK01");
    std::fprintf(stderr, "\nSTATE 1  healthy-live:\n");
    check("momentary -18.5 present (latched short-window)", s1.contains("momentary -18.5"));
    check("input -20.0 LUFS present (cumulative)",          s1.contains("input -20.0 LUFS") || s1.contains("Input -20.0 LUFS"));
    check("crest 8.4 (real dynamic, not decay)",            s1.contains("crest 8.4"));
    check("labelled 'overall since playback began'",        s1.contains("overall since playback began"));
    check("NOT 'LINK NEVER HEARD'",                         ! s1.contains("LINK NEVER HEARD"));
    check("NOT 'live meters have since reset'",             ! s1.contains("live meters have since reset"));

    // ---- STATE 2: decayed-live-with-latch (latch=good, live=decayed) ----
    writeFrameAt(slot01, decayedFrame());
    pump(proc);   // decayed is NOT good -> latch stays the good frame
    const String s2 = assemble("TLINK01");
    std::fprintf(stderr, "STATE 2  decayed-live-with-latch:\n");
    check("momentary -18.5 present (served from latch, not floored live)", s2.contains("momentary -18.5"));
    check("crest 8.4 (latched), NOT the 29.8 decay artefact", s2.contains("crest 8.4") && ! s2.contains("crest 29.8"));
    check("input -20.0 LUFS present (cumulative from live)",   s2.contains("input -20.0 LUFS") || s2.contains("Input -20.0 LUFS"));
    check("labelled 'overall since playback began'",          s2.contains("overall since playback began"));
    check("NOT 'LINK NEVER HEARD'",                           ! s2.contains("LINK NEVER HEARD"));

    // ---- STATE 3: live-cumulative-invalid-with-latch (CATCH E) ----
    writeFrameAt(slot01, restartFrame());     // live integrated floored (Link restart)
    pump(proc);
    const String s3 = assemble("TLINK01");
    std::fprintf(stderr, "STATE 3  live-cumulative-invalid-with-latch (CATCH E):\n");
    check("input -20.0 LUFS present (from latch; live invalid)", s3.contains("input -20.0 LUFS") || s3.contains("Input -20.0 LUFS"));
    check("labelled 'all from the while-flowing window'",        s3.contains("all from the while-flowing window"));
    check("says 'live meters have since reset'",                 s3.contains("live meters have since reset"));
    check("NOT 'LINK NEVER HEARD' (a latch exists = heard)",     ! s3.contains("LINK NEVER HEARD"));

    // ---- STATE 4: no-latch-ever (short-window never valid) ----
    const String s4 = assemble("TLINK02");
    std::fprintf(stderr, "STATE 4  no-latch-ever:\n");
    check("says 'LINK NEVER HEARD'",              s4.contains("LINK NEVER HEARD"));
    check("NO momentary figure emitted",          ! s4.contains("momentary "));
    check("NO 'input -20' measurement emitted",   ! s4.contains("input -20") && ! s4.contains("Input -20"));

    // ---- STATE 4b (21t-i): a frame with an INT and no valid short window is a channel that HAS been heard ----
    // The record keeps what was measured, so the block reports the figure rather than denying the channel.
    writeFrameAt(slot02, intOnlyFrame());
    pump(proc);
    const String s4b = assemble("TLINK02");
    std::fprintf(stderr, "STATE 4b  int-only frame (21t-i: the record has a figure):\n");
    check("NOT 'LINK NEVER HEARD' - an integrated figure means audio was gated in",
          ! s4b.contains("LINK NEVER HEARD"));
    check("the figure is reported: input -20", s4b.contains("input -20") || s4b.contains("Input -20"));

    std::fprintf(stderr, "\n==== LATCH HARNESS: %s (%d assertion(s) failed) ====\n",
                 g_fails==0 ? "GREEN" : "RED", g_fails);
    return g_fails==0 ? 0 : 1;
}
