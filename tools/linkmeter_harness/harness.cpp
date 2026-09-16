// Real-assembly harness for the Link meter-context defect (11 Sep 2026).
// It does NOT hand-build a request. It seeds a LinkMeterFrame into an isolated
// registry, constructs the REAL EchoJayProcessor + EchoJayEditor, and calls the
// REAL EchoJayEditor::standardChainInjections with targetLinkUid set - the exact
// path a Link chain build takes. It asserts the request carries the Link
// [METER SNAPSHOT v2] block AND a Link-attributed [CHAIN LEVELS] pre-gain line.
//
// On the CURRENT binary (meter emission sits in the else-of-targetLinkUid branch,
// unreachable for a Link target) this MUST FAIL - that is the red the guard has
// to be observed producing before it is trusted. After the wiring it must pass.
//
// REPAIRED 17 Sep 2026 (ruling: harness stale, product unchanged - RED at 078b130
// and HEAD identically, Link injection byte-identical): (1) the integrated token
// follows the 15 Sep CATCH D/E wording ("Input -20.0 LUFS ... overall since
// playback began"), matched case-insensitively; the latch is driven the way the
// plugin does (updateLinkAudioRecency after every seed). (2) the applied pre-gain
// is read from the ctrl-cmd file the Build site writes {preGainDb, preGainUserSet:
// false} (16 Sep: V2-only pre-gain to the LINK's ChainHost), not the main
// ChainHost. (3) the Lapse marker is "[LINK NEVER HEARD" (CATCH E rename).
// THREE WAYS on one binary via EJ_LM_MODE: healthy (default) -> GREEN;
// silent (decayed seed: audioStale=1, momentary floor, no bands -> no latch) ->
// RED with Lapse; absent (no frame, nothing claimed) -> RED with NoTarget.
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "LinkShm.h"
#include <cstdio>
#include <cstring>

using namespace juce;

static const char* kUid   = "TLINK01";   // short: fits RegistrySlot.instanceUid without truncation
static const char* kName  = "Nafe Lead Vocal";
static String mode() { const char* m = std::getenv("EJ_LM_MODE"); return m ? String(m) : String("healthy"); }

static void* g_rmap = nullptr;   // seeded registry, kept open to bump the heartbeat
static int   g_slot = -1;
static uint32_t g_hb = 100;

// A live publisher's heartbeat climbs; refreshLinkRegistry marks a Link connected
// only when it sees that climb. Bump it so linkUidLive() is true - the harness
// must reproduce a LIVE Link (the block's absence must be the call-site defect,
// not an offline short-circuit).
static void bumpHeartbeat()
{
    if (! g_rmap || g_slot < 0) return;
    auto* slots = LinkShm::regSlots(g_rmap);
    LinkShm::storeRelease(&slots[g_slot].heartbeat, ++g_hb);
}

static void writeFrame(void* rmap, int slot, const LinkMeterFrame& f)
{
    LinkMeterFrame* dst = LinkShm::meterFrames(rmap) + slot;
    const uint32_t s0 = LinkShm::loadRelaxed(&dst->seq) & ~1u;
    LinkShm::storeRelease(&dst->seq, s0 + 1);
    std::memcpy(reinterpret_cast<uint8_t*>(dst) + sizeof(uint32_t),
                reinterpret_cast<const uint8_t*>(&f) + sizeof(uint32_t),
                sizeof(LinkMeterFrame) - sizeof(uint32_t));
    LinkShm::storeRelease(&dst->seq, s0 + 2);
}

// Rewrite slot 0's frame with a new integrated level (gain-path (d): a materially
// different level must yield a correspondingly different applied gain).
static void reseedIntegrated(float integ)
{
    if (! g_rmap || g_slot < 0) return;
    LinkMeterFrame f {};
    f.momentary = integ; f.shortTerm = integ; f.integrated = integ;
    f.truePeakMax = -1.3f; f.truePeakCur = -1.6f; f.crest = 8.4f;
    f.correlation = 0.55f; f.width = 0.72f; f.lra = 6.1f; f.shortTermTP = -2.4f;
    f.bandRel[0]=1.4f; f.bandRel[5]=3.4f; f.audioBlocks = 2000; f.audioStale = 0;
    if (mode() == "silent")   // the decayed shape persists across the gain-path reseeds (never latched)
    { f.audioStale = 1; f.momentary = -100.0f; f.shortTerm = -100.0f; f.bandRel[0] = 0.0f; f.bandRel[5] = 0.0f; }
    writeFrame(g_rmap, g_slot, f);
}

// Seed one live Link with a fresh, real-programme frame into the isolated registry.
static bool seedLink()
{
    if (mode() == "absent") { std::fprintf(stderr, "seed: MODE absent - no frame, nothing claimed (NoTarget expected)\n"); return true; }
    int err = 0;
    const String dir = LinkShm::resolveDir(err);
    if (dir.isEmpty()) { std::fprintf(stderr, "seed: resolveDir failed err=%d\n", err); return false; }
    int fd = -1, oerr = 0;
    void* rmap = LinkShm::openRegistry(dir, fd, oerr);
    if (! rmap) { std::fprintf(stderr, "seed: openRegistry failed err=%d\n", oerr); return false; }

    const int slot = LinkShm::claimSlot(rmap, kName, "", kUid, 48000.0f, 2u);
    if (slot < 0) { std::fprintf(stderr, "seed: claimSlot failed\n"); return false; }

    auto* slots = LinkShm::regSlots(rmap);
    slots[slot].placement = 1;                       // bus (post-fader)
    slots[slot].dialCapable = 1;
    LinkShm::storeRelease(&slots[slot].heartbeat, 100u);   // live
    LinkShm::storeRelease(&slots[slot].inUse, 1u);

    // A fresh, real-programme frame the way a live Link publishes it - every
    // field a DISTINCT realistic value so a per-field check verifies the VALUE
    // landed, not merely presence, and so a constant would be caught.
    LinkMeterFrame f {};
    f.momentary = -18.5f; f.shortTerm = -19.2f; f.integrated = -20.0f;
    f.rmsL = -23.0f; f.rmsR = -23.4f; f.peakL = -6.0f; f.peakR = -6.3f;
    f.truePeakMax = -1.3f; f.truePeakCur = -1.6f;
    f.crest = 8.4f; f.correlation = 0.55f; f.width = 0.72f;
    f.lra = 6.1f; f.shortTermTP = -2.4f;
    f.bandRel[0] = 1.4f; f.bandRel[1] = 0.3f; f.bandRel[2] = -0.6f;
    f.bandRel[3] = -0.9f; f.bandRel[4] = 0.7f; f.bandRel[5] = 3.4f;
    f.audioBlocks = 1000; f.audioStale = 0;
    f.peakFastL = -6.0f; f.peakFastR = -6.0f;
    f.fieldsMask = 0;   // loudness suite carries no mask bit; -100 sentinels gate the rest
    if (mode() == "silent")
    {   // a DECAYED post-playback frame: stale, momentary at the floor, no bands -> never latched
        f.audioStale = 1; f.momentary = -100.0f; f.shortTerm = -100.0f;
        for (int i = 0; i < 6; ++i) f.bandRel[i] = 0.0f;
        std::fprintf(stderr, "seed: MODE silent - decayed frame (audioStale=1, momentary -100, no bands): Lapse expected\n");
    }

    LinkMeterFrame* dst = LinkShm::meterFrames(rmap) + slot;
    const uint32_t s0 = LinkShm::loadRelaxed(&dst->seq) & ~1u;
    LinkShm::storeRelease(&dst->seq, s0 + 1);
    std::memcpy(reinterpret_cast<uint8_t*>(dst) + sizeof(uint32_t),
                reinterpret_cast<const uint8_t*>(&f) + sizeof(uint32_t),
                sizeof(LinkMeterFrame) - sizeof(uint32_t));
    LinkShm::storeRelease(&dst->seq, s0 + 2);

    std::fprintf(stderr, "seed: Link \"%s\" uid=%s in slot %d, integrated -20 LUFS, audioStale=0\n", kName, kUid, slot);
    g_rmap = rmap; g_slot = slot;   // keep open for heartbeat bumps
    return true;
}

int main()
{
    ScopedJuceInitialiser_GUI gui;

    // Isolation: a private state root, never the user's live registry.
    File tmp = File::getSpecialLocation(File::tempDirectory).getChildFile("ej_linkmeter_harness_" + String(Time::getMillisecondCounter()));
    tmp.createDirectory();
    setenv("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);

    if (! seedLink()) { std::fprintf(stderr, "HARNESS ERROR: could not seed the Link\n"); return 2; }

    EchoJayProcessor proc;
    // Several refreshes with a climbing heartbeat so the Link reads as LIVE
    // (connected), and recency stamps so its fresh frame is 'recent'.
    for (int k = 0; k < 8; ++k)
    {
        bumpHeartbeat();
        proc.refreshLinkRegistry();
        proc.updateLinkAudioRecency();
    }

    // Prove the reading IS available to the processor, so a red can only be the
    // call site, never missing data: find the seeded Link and read its frame.
    { int derr = 0; std::fprintf(stderr, "debug: seed registry dir = %s\n", LinkShm::resolveDir(derr).toRawUTF8()); }
    { auto dl = proc.getLinkDisplayList();
      std::fprintf(stderr, "debug: processor display list size = %d\n", (int) dl.size());
      for (const auto& e : dl) std::fprintf(stderr, "   slot uid=%s name=%s regIdx=%d connected=%d active=%d\n",
                 e.info.uid.toRawUTF8(), e.displayName.toRawUTF8(), e.info.regIdx, (int)e.info.connected, (int)e.info.active); }
    int seededRegIdx = -1; bool seededConnected = false;
    for (const auto& e : proc.getLinkDisplayList())
        if (e.info.uid == String(kUid)) { seededRegIdx = e.info.regIdx; seededConnected = e.info.connected; }
    LinkMeterFrame chk {}; bool frameOk = seededRegIdx >= 0 && proc.readLinkMeterFrame(seededRegIdx, chk);
    std::fprintf(stderr, "precondition: Link in display list=%s regIdx=%d connected=%s  frame readable=%s integrated=%.1f\n",
                 seededRegIdx >= 0 ? "yes" : "NO", seededRegIdx, seededConnected ? "yes" : "no",
                 frameOk ? "yes" : "NO", frameOk ? chk.integrated : 0.0f);

    // OWN-CHANNEL CONTROL: feed real noise through the actual meter path so a
    // genuine own-channel reading exists (chain-input tally + meter engine),
    // exactly as playback would. No test seam into the meters - real audio.
    proc.prepareToPlay(48000.0, 512);
    Random rng(1234);
    AudioBuffer<float> buf(2, 512);
    for (int b = 0; b < 600; ++b)   // ~6.4 s at 48k/512
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* d = buf.getWritePointer(ch);
            for (int i = 0; i < 512; ++i) d[i] = (rng.nextFloat() * 2.0f - 1.0f) * 0.1f;  // ~-20 dBFS
        }
        MidiBuffer midi;
        proc.processBlock(buf, midi);
    }

    std::unique_ptr<AudioProcessorEditor> edBase(proc.createEditor());
    auto* ed = dynamic_cast<EchoJayEditor*>(edBase.get());
    if (! ed) { std::fprintf(stderr, "HARNESS ERROR: createEditor did not return an EchoJayEditor\n"); return 2; }

    auto assemble = [&](const String& targetUid) -> String
    { StringArray mf; return ed->testAssembleChainInjections("build me a mastering chain", targetUid, &mf); };
    const String ownOut  = assemble(String());
    const String linkOut  = assemble(String(kUid));
    // Dump the REAL assembled Link injection so the item-4 leg posts THIS, not a
    // hand-built body (the reviewer's methodological requirement).
    if (const char* dp = std::getenv("EJ_DUMP_LINK"))
        File(String(dp)).replaceWithText(linkOut, false, false, "\n");

    // REQUIRED FIELD SET, hardcoded (item 0c): every measured field a live Link
    // publishes, minus the three agreed gaps. Each row carries the token that
    // proves the SEEDED VALUE landed - presence of the value, not just the key,
    // so a constant or a dropped field turns the column red.
    struct Field { const char* name; const char* ownKey; const char* linkValue; bool declared; };
    const Field FIELDS[] = {
        { "integrated LUFS", "input ",        "Input -20.0 LUFS", false },   // 15 Sep wording: "Input -20.0 LUFS and peak ... overall since playback began"
        { "momentary",       "momentary",     "momentary -18.5",  false },
        { "short-term",      "short-term",    "short-term -19.2", false },
        { "true peak",       "peak ",         "peak -1.3 dBFS",   false },
        { "crest",           "crest",         "crest 8.4 dB",     false },
        { "correlation",     "correlation",   "correlation 0.55", false },
        { "width",           "width",         "width 0.72",       false },
        { "LRA",             "LRA",           "LRA 6.1",          false },
        { "PSR",             "\"psr\"",       "PSR 16.8",         false },
        { "PLR",             "\"plr\"",       "PLR 18.7",         false },
        { "macro bands (6)", "macroBands",    "macroBands",       false },
        // the three agreed gaps - DECLARED, not required:
        { "fine spectrum",   "\"spectrum\":[","\"spectrum\":[",   true },
        { "oversCount",      "oversCount",    "oversCount",       true },
        { "bandCrest",       "bandCrest",     "bandCrest",        true },
    };
    std::fprintf(stderr, "\n==== FIELD-BY-FIELD, own vs link, same binary (item 0a) ====\n");
    std::fprintf(stderr, "  %-16s | %-8s | %-8s | %s\n", "field", "own", "link", "status");
    bool allRequired = true;
    for (const auto& F : FIELDS)
    {
        const bool inOwn  = ownOut.contains(F.ownKey);        // KEY presence (own carries its own values)
        const bool inLink = linkOut.containsIgnoreCase(F.linkValue);    // VALUE presence (seeded value landed); case-insensitive (CATCH D/E capitalised "Input")
        const char* status;
        if (F.declared)        status = inLink ? "FILLED(?!)" : "DECLARED";
        else if (inLink)       status = "FILLED";
        else { status = "UNEXPLAINED"; allRequired = false; }
        std::fprintf(stderr, "  %-16s | %-8s | %-8s | %s\n", F.name,
                     inOwn ? "present" : "absent", inLink ? "present" : "absent", status);
    }
    const bool linkPreGain = linkOut.contains("pre-gain");
    std::fprintf(stderr, "  %-16s | %-8s | %-8s | %s\n", "pre-gain line", "(build)", linkPreGain ? "present" : "absent",
                 linkPreGain ? "FILLED" : "UNEXPLAINED");

    // ===== GAIN-PATH ASSERTIONS (a-d) - applied == stated, from the frame =====
    const float stored   = ed->testPendingLinkPreGainDb();                      // the ONE stored value (int=-20)
    const float expected = juce::jlimit(-24.0f, 24.0f, -18.0f - (-20.0f));      // clamp(-18 - integrated) = +2
    const bool  statedTxt = linkOut.contains("pre-gain to +2.0 dB");   // 15 Sep wording: "Set the chain's pre-gain to +2.0 dB"              // printed in [CHAIN LEVELS]
    // 16 Sep 2026: the Build site sends the pre-gain to the LINK as ctrl-cmd {preGainDb, preGainUserSet:false};
    // "applied" is what that file carries (the pregain_readback harness proves the same send half).
    auto sentPreGain = [&](bool* userSetOut) -> float
    {
        int e = 0; const String d = LinkShm::resolveDir(e);
        const File f(d + "ctrl-cmd-" + String(kUid) + ".json");
        if (! f.existsAsFile()) { if (userSetOut) *userSetOut = true; return 0.0f; }
        auto v = JSON::parse(f.loadFileAsString()); auto* o = v.getDynamicObject();
        if (o == nullptr || ! o->hasProperty("preGainDb")) { if (userSetOut) *userSetOut = true; return 0.0f; }
        if (userSetOut) *userSetOut = o->hasProperty("preGainUserSet") ? (bool) o->getProperty("preGainUserSet") : true;
        f.deleteFile();   // consumed, like the Link does
        return (float)(double) o->getProperty("preGainDb");
    };
    ed->applyPendingLinkPreGain();
    bool userSet1 = true;
    const float applied = sentPreGain(&userSet1);
    const bool a = (std::abs(applied - stored) < 0.001f) && statedTxt && ! userSet1;   // sent == stored == stated, AUTO (userSet=false)
    const bool b = (std::abs(stored - expected) < 0.001f) && (std::abs(applied - 2.0f) < 0.001f);
    reseedIntegrated(-8.0f); proc.updateLinkAudioRecency();
    StringArray mfd; const String linkOut2 = ed->testAssembleChainInjections("build me a mastering chain", String(kUid), &mfd);
    const float stored2 = ed->testPendingLinkPreGainDb();
    const float expected2 = juce::jlimit(-24.0f, 24.0f, -18.0f - (-8.0f));      // = -10
    ed->applyPendingLinkPreGain();
    bool userSet2 = true;
    const float applied2 = sentPreGain(&userSet2);
    const bool d = (std::abs(stored2 - expected2) < 0.001f) && (std::abs(applied2 - stored2) < 0.001f)
                 && (std::abs(applied2 - applied) > 1.0f) && linkOut2.contains("pre-gain to -10.0 dB");
    std::fprintf(stderr, "\n==== GAIN-PATH (a-d) ====\n");
    std::fprintf(stderr, "  (a) sent(ctrl-cmd preGainDb) == stored == stated, userSet=false: %s  sent=%.2f stored=%.2f stated-in-text=%s userSet=%s\n", a?"PASS":"FAIL", applied, stored, statedTxt?"yes":"no", userSet1?"true":"false");
    std::fprintf(stderr, "  (b) == clamp(-18 - integrated)  : %s  stored=%.2f expected=%.2f\n", b?"PASS":"FAIL", stored, expected);
    std::fprintf(stderr, "  (c) clamp bounds                : [-24.0, +24.0] dB (ChainHost::kPreGainMin/MaxDb)\n");
    std::fprintf(stderr, "  (d) int=-8 -> sent %.2f (was %.2f), stated -10.0 : %s\n", applied2, applied, d?"PASS":"FAIL");
    reseedIntegrated(-20.0f); proc.updateLinkAudioRecency();

    // ===== FOUR OUTCOMES, never silence =====
    { int e2=0; const String d2=LinkShm::resolveDir(e2); int fd2=-1,e3=0; void* r2=LinkShm::openRegistry(d2,fd2,e3);
      if (r2){ int s2=LinkShm::claimSlot(r2,"Stale Vox","","TLINK02",48000.0f,2u); auto* sl=LinkShm::regSlots(r2);
        sl[s2].placement=1; LinkShm::storeRelease(&sl[s2].inUse,1u);
        LinkMeterFrame sf{}; sf.integrated=-16.0f; sf.audioStale=1; writeFrame(r2,s2,sf);
        for(int k=0;k<8;++k){ LinkShm::storeRelease(&sl[s2].heartbeat,300u+(uint32_t)k); proc.refreshLinkRegistry(); proc.updateLinkAudioRecency(); } } }
    StringArray mm;
    const String outMeas  = ed->testAssembleChainInjections("build me a mastering chain", String(kUid),        &mm);
    const String outLapse = ed->testAssembleChainInjections("build me a mastering chain", String("TLINK02"),   &mm);
    const String outNoTgt = ed->testAssembleChainInjections("build me a mastering chain", String("NOSUCHLINK"),&mm);
    std::fprintf(stderr, "\n==== FOUR OUTCOMES, never silence ====\n");
    std::fprintf(stderr, "  Measurements : %s\n", (outMeas.contains("[METER SNAPSHOT v2") && outMeas.contains("[CHAIN LEVELS")) ? "meter + levels" : "MISSING");
    std::fprintf(stderr, "  Lapse        : %s\n", (outLapse.contains("[LINK NEVER HEARD") || outLapse.contains("[LINK NOT HEARD RECENTLY")) ? "prompt-to-play note" : "MISSING");
    const String tgtOutcome = outMeas.contains("[METER SNAPSHOT v2") ? "Measurements"
                            : outMeas.contains("[LINK NEVER HEARD") ? "Lapse (never heard)"
                            : (outMeas.contains("[LINK NOT ADDRESSABLE") || outMeas.contains("[LINK TARGET UNRESOLVED")) ? "NoTarget"
                            : "SILENT (?!)";
    std::fprintf(stderr, "  TARGET %s (mode %s): %s\n", kUid, mode().toRawUTF8(), tgtOutcome.toRawUTF8());
    std::fprintf(stderr, "  NoTarget     : %s\n", (outNoTgt.contains("[LINK NOT ADDRESSABLE") || outNoTgt.contains("[LINK TARGET UNRESOLVED")) ? "stated no-target note" : "MISSING");
    std::fprintf(stderr, "  OwnEmpty     : %s\n", ownOut.contains("[CHAIN LEVELS") ? "[CHAIN LEVELS] present (never silent)" : "SILENT");

    // ITEM 2 known-good for the pre-gain FIELD (own).
    proc.getChainHost().setPreGainDb(3.0f, /*userSet*/ true);
    const bool pgKnownGood = assemble(String()).contains("pre-gain +3.0 dB");
    std::fprintf(stderr, "\nITEM 2 pre-gain known-good: own channel, +3.0 seeded -> \"pre-gain +3.0 dB\" present: %s\n",
                 pgKnownGood ? "YES" : "NO");

    const bool ownGreen  = ownOut.contains("[CHAIN LEVELS") && ownOut.contains("[METER SNAPSHOT v2");
    const bool linkGreen = allRequired && linkPreGain;    // every REQUIRED field with its value, plus pre-gain
    std::fprintf(stderr, "\nGREEN CRITERION = every required field present with its seeded value, AND the pre-gain line.\n");
    std::fprintf(stderr, "  own-channel : %s   link-target : %s\n",
                 ownGreen ? "GREEN" : "RED", linkGreen ? "GREEN" : "RED (a required field is UNEXPLAINED)");
    // Exit: 0 only if the control is green (proves the guard is real). If the
    // control is red the harness is broken and increment 3 must NOT proceed.
    if (! ownGreen) return 2;     // 2 = control failed: STOP condition
    return linkGreen ? 0 : 1;     // 1 = link red as expected on current binary
}
