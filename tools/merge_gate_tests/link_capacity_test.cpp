/*  CAPACITY + PROVENANCE LEGS (6 Sep 2026 ruling). Link archive. Modes:
      legs   - host name BEFORE the gate kept; host name AFTER the gate kept;
               dead-publisher rows reclaimed and the slot reused; registry FULL
               reports (diag.regFull + log) rather than vanishing;
               old-layout region rejected cleanly (scratch dir)
      scan50 - the Link's own 30 Hz scan with 50 live rows: CPU per second      */
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "EJStateRoot.h"
#include "LinkProcessor.h"
#include "LinkShm.h"
#include <cstdio>
#include <unistd.h>
#include <sys/resource.h>
#include <set>

static void pump (int iters) { for (int t = 0; t < iters; ++t) CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false); }
static void drain() { pump (5); }
// MILLISECOND pump (8 Sep 2026): pump(n) above is n ITERATIONS of ~20-30 ms, not n ms - the solo legs
// were first written as if it were milliseconds, so their "1,130 / 2,474 ms" were the harness's own step
// size, not the product. Every timed leg uses pumpMs.
static void pumpMs (int ms) { const double end = juce::Time::getMillisecondCounterHiRes() + ms; while (juce::Time::getMillisecondCounterHiRes() < end) CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.002, false); }
static juce::String slotName (void* reg, int i) { return i >= 0 ? juce::String::fromUTF8 (LinkShm::regSlots (reg)[i].displayName) : juce::String ("(no slot)"); }
static void nameFromHost (LinkProcessor& l, const char* n) { juce::AudioProcessor::TrackProperties tp; tp.name = std::make_optional (juce::String (n)); l.updateTrackProperties (tp); }

// plant `count` rows whose sidecars name `pid`; returns the slot indices
static std::vector<int> plantRows (void* reg, const juce::String& dir, int count, int pid, const char* tag)
{
    auto* slots = LinkShm::regSlots (reg); std::vector<int> out;
    for (int k = 0; k < count; ++k)
        for (int i = 0; i < kRegMaxSlots; ++i)
            if (LinkShm::loadAcquire (&slots[i].inUse) == 0)
            {
                const juce::String uid = juce::String (tag) + juce::String::toHexString (0x100000 + k).paddedLeft ('0', 8).substring (0, 8);
                std::memset (&slots[i], 0, sizeof (RegistrySlot));
                std::strncpy (slots[i].audioFile, ("audio_" + uid + ".bin").toRawUTF8(), 47); std::strncpy (slots[i].instanceUid, uid.toRawUTF8(), 10);
                slots[i].sampleRate = 48000.0f; slots[i].numChannels = 2; LinkShm::storeRelease (&slots[i].heartbeat, 1u); LinkShm::storeRelease (&slots[i].inUse, 1u);
                LinkShm::RackSidecar rc; rc.valid = true; rc.uid = uid; rc.name = ""; rc.revision = 1; rc.publisherPid = pid; rc.hostPid = pid; LinkShm::writeRackSidecar (dir, rc);
                out.push_back (i); break;
            }
    return out;
}
static void unplant (void* reg, const juce::String& dir, const std::vector<int>& rows)
{
    auto* slots = LinkShm::regSlots (reg);
    for (int i : rows) { const juce::String uid = juce::String::fromUTF8 (slots[i].instanceUid); juce::File (LinkShm::rackSidecarPath (dir, uid)).deleteFile(); LinkShm::releaseSlot (reg, i); }
}

// THE INVARIANT (6 Sep 2026 ruling): no instance ever publishes a name that
// arrived in a chunk it did not originate. P1/P2 are its two orderings.
static bool publishesForeignName (void* reg, const LinkProcessor& l, const juce::String& chunkTyped, const juce::String& chunkHost)
{
    const auto pub = slotName (reg, l.diag.slotIdx);
    return pub.isNotEmpty() && (pub == chunkTyped || pub == chunkHost);
}
static int provenancePair (void* reg, const juce::MemoryBlock& chunkA, bool verbose)
{
    int bad = 0;
    {   // P1: host delivers BEFORE the gate's clear
        auto b = std::make_unique<LinkProcessor>(); b->prepareToPlay (48000.0, 512);
        b->setStateInformation (chunkA.getData(), (int) chunkA.getSize());
        nameFromHost (*b, "Track B");
        pump (150);
        const bool ok = b->diag.slotIdx >= 0 && b->getHostTrackName() == "Track B" && slotName (reg, b->diag.slotIdx) == "Track B" && ! publishesForeignName (reg, *b, "Vox", "Track A");
        if (verbose) std::printf ("  P1 B: slot %d host \"%s\" typed \"%s\" published \"%s\" -> %s\n", b->diag.slotIdx, b->getHostTrackName().toRawUTF8(), b->linkName.toRawUTF8(), slotName (reg, b->diag.slotIdx).toRawUTF8(), ok ? "PASS" : "FAIL");
        bad |= ok ? 0 : 1; drain(); b.reset(); drain();
    }
    {   // P2: host delivers AFTER the gate's clear
        auto c = std::make_unique<LinkProcessor>(); c->prepareToPlay (48000.0, 512);
        c->setStateInformation (chunkA.getData(), (int) chunkA.getSize()); pump (150);
        const auto afterClear = slotName (reg, c->diag.slotIdx);
        const bool noForeignAfterClear = ! publishesForeignName (reg, *c, "Vox", "Track A");
        nameFromHost (*c, "Track C"); pump (40);
        const bool ok = c->diag.slotIdx >= 0 && noForeignAfterClear && c->getHostTrackName() == "Track C" && slotName (reg, c->diag.slotIdx) == "Track C";
        if (verbose) std::printf ("  P2 C: after the clear published \"%s\"; after the host delivered: host \"%s\" published \"%s\" -> %s\n", afterClear.toRawUTF8(), c->getHostTrackName().toRawUTF8(), slotName (reg, c->diag.slotIdx).toRawUTF8(), ok ? "PASS" : "FAIL");
        bad |= ok ? 0 : 2; drain(); c.reset(); drain();
    }
    return bad;
}

static int p20()
{
    int err = 0; const auto dir = LinkShm::resolveDir (err);
    int fd = -1, rerr = 0; void* reg = LinkShm::openRegistry (dir, fd, rerr);
    if (reg == nullptr) { std::printf ("registry not mappable (%d)\n", rerr); return 99; }
    auto a = std::make_unique<LinkProcessor>(); a->linkName = "Vox"; a->prepareToPlay (48000.0, 512); nameFromHost (*a, "Track A"); a->updateShmState(); pump (40);
    juce::MemoryBlock chunkA; a->getStateInformation (chunkA);
    int p1 = 0, p2 = 0;
    for (int r = 0; r < 20; ++r) { const int b = provenancePair (reg, chunkA, r == 0); if (! (b & 1)) ++p1; if (! (b & 2)) ++p2; }
    std::printf ("P20: host name BEFORE the clear kept %d/20   host name AFTER the clear kept %d/20   (invariant checked in every run)\n", p1, p2);
    drain(); a.reset(); drain();
    return (p1 == 20 && p2 == 20) ? 0 : 1;
}


// ---- THE FOREIGN-CHUNK LEG (6 Sep 2026, from the v7 Pro Tools log): the host DELIVERS
// the track name to the fresh instance (ctor uid), and ~85 ms LATER Pro Tools applies a
// FOREIGN chunk (the seed / a gone sibling's) carrying a DIFFERENT name; the re-mint
// follows. The delivered name must survive. P20 never modelled this ordering: it
// applied the seed BEFORE the delivery (P1) or after the re-mint (P2), never a foreign
// chunk with its own name AFTER the delivery - which is exactly why P20 went green on v7.
//   F1  host delivered "Kick"  -> foreign chunk (host "Track A", typed "Vox") -> re-mint : host "Kick", row "Kick"
//   F2  user typed "MyBus"     -> foreign chunk (typed "Vox")                 -> re-mint : typed "MyBus", row "MyBus"
//   F3  nothing delivered      -> foreign chunk fills the name (provisional) and the re-mint drops it (P2 unchanged)
static int foreignLegs (void* reg, const juce::MemoryBlock& chunkA, bool verbose)
{
    int bad = 0;
    {   // F1
        auto b = std::make_unique<LinkProcessor>(); b->prepareToPlay (48000.0, 512);
        nameFromHost (*b, "Kick"); pump (30);                                     // the host delivers FIRST (ctor uid)
        b->setStateInformation (chunkA.getData(), (int) chunkA.getSize());      // THEN a foreign chunk with its own name
        pump (150);                                                                // re-mint (no holder) + claim
        const auto row = b->diag.slotIdx >= 0 ? slotName (reg, b->diag.slotIdx) : juce::String ("(no slot)");
        const bool ok = b->diag.slotIdx >= 0 && b->getHostTrackName() == "Kick" && row == "Kick";
        if (verbose) std::printf ("  F1: host \"%s\" typed \"%s\" published \"%s\" -> %s\n", b->getHostTrackName().toRawUTF8(), b->linkName.toRawUTF8(), row.toRawUTF8(), ok ? "PASS" : "FAIL");
        bad |= ok ? 0 : 1; drain(); b.reset(); drain();
    }
    {   // F2
        auto c = std::make_unique<LinkProcessor>(); c->prepareToPlay (48000.0, 512);
        c->linkName = "MyBus"; c->markTypedNameAuthoritative(); pump (30);        // the user typed FIRST
        c->setStateInformation (chunkA.getData(), (int) chunkA.getSize());      // THEN a foreign chunk (typed "Vox")
        pump (150);
        const auto row = c->diag.slotIdx >= 0 ? slotName (reg, c->diag.slotIdx) : juce::String ("(no slot)");
        const bool ok = c->diag.slotIdx >= 0 && c->linkName == "MyBus" && row == "MyBus";
        if (verbose) std::printf ("  F2: typed \"%s\" host \"%s\" published \"%s\" -> %s\n", c->linkName.toRawUTF8(), c->getHostTrackName().toRawUTF8(), row.toRawUTF8(), ok ? "PASS" : "FAIL");
        bad |= ok ? 0 : 2; drain(); c.reset(); drain();
    }
    {   // F3
        auto d = std::make_unique<LinkProcessor>(); d->prepareToPlay (48000.0, 512);
        d->setStateInformation (chunkA.getData(), (int) chunkA.getSize());      // nothing delivered: the chunk FILLS
        const bool filled = d->getHostTrackName() == "Track A" && d->linkName == "Vox";
        pump (150);                                                                // the re-mint then drops the seeded names (P2)
        const auto row = d->diag.slotIdx >= 0 ? slotName (reg, d->diag.slotIdx) : juce::String ("(no slot)");
        const bool ok = filled && d->diag.slotIdx >= 0 && row.isEmpty() && d->getHostTrackName().isEmpty() && d->linkName.isEmpty();
        if (verbose) std::printf ("  F3: filled-before-claim=%d; after the re-mint host \"%s\" typed \"%s\" published \"%s\" -> %s\n", (int) filled, d->getHostTrackName().toRawUTF8(), d->linkName.toRawUTF8(), row.toRawUTF8(), ok ? "PASS" : "FAIL");
        bad |= ok ? 0 : 4; drain(); d.reset(); drain();
    }
    return bad;
}

static int foreign (int runs)
{
    int err = 0; const auto dir = LinkShm::resolveDir (err);
    int fd = -1, rerr = 0; void* reg = LinkShm::openRegistry (dir, fd, rerr);
    if (reg == nullptr) { std::printf ("registry not mappable (%d)\n", rerr); return 99; }
    auto a = std::make_unique<LinkProcessor>(); a->linkName = "Vox"; a->prepareToPlay (48000.0, 512); nameFromHost (*a, "Track A"); a->updateShmState(); pump (150);
    juce::MemoryBlock chunkA; a->getStateInformation (chunkA);
    drain(); a.reset(); drain();   // A is GONE: its chunk is a seed from a gone instance (Pro Tools' case), so the newcomer re-mints
    int f1 = 0, f2 = 0, f3 = 0, p1 = 0, p2 = 0;
    for (int r = 0; r < runs; ++r)
    {
        const int b = foreignLegs (reg, chunkA, r == 0); if (! (b & 1)) ++f1; if (! (b & 2)) ++f2; if (! (b & 4)) ++f3;
        const int q = provenancePair (reg, chunkA, false); if (! (q & 1)) ++p1; if (! (q & 2)) ++p2;
    }
    std::printf ("FOREIGN x%d: F1 delivered-then-foreign kept %d/%d   F2 typed-then-foreign kept %d/%d   F3 fill-when-none %d/%d   (P20 arms alongside: P1 %d/%d  P2 %d/%d)\n",
                 runs, f1, runs, f2, runs, f3, runs, p1, runs, p2, runs);
    return (f1 == runs && f2 == runs && f3 == runs && p1 == runs && p2 == runs) ? 0 : 1;
}


// ---- THE SOLO FABRIC LEG (8 Sep 2026 ruling, item 4): three synthetic Links in a private root.
// Solo A by the exact file the main writes (ctrl-cmd-<uid>.json {v,seq,soloOn}); assert the three
// steps that were dark in the live log, individually:
//   (i)   A republishes its sidecar with soloOn=true
//   (ii)  B's and C's fabric scan read it: linkMuteWanted() becomes true on B and C, stays false on A
//   (iii) B's processBlock actually silences its output (sine in, ~0 out after the ramp)
// then soloOn=false: B and C unmute.
static float rmsOfBlock (LinkProcessor& l, int blocks)
{
    juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi; double ph = 0; float acc = 0; int cnt = 0;
    for (int b = 0; b < blocks; ++b)
    {
        for (int i = 0; i < 512; ++i) { const float v = std::sin ((float) ph) * 0.5f; ph += 2.0 * juce::MathConstants<double>::pi * 440.0 / 48000.0; buf.setSample (0, i, v); buf.setSample (1, i, v); }
        l.processBlock (buf, midi);
        if (b >= blocks - 4) { for (int i = 0; i < 512; ++i) { acc += buf.getSample (0, i) * buf.getSample (0, i); ++cnt; } }
    }
    return cnt > 0 ? std::sqrt (acc / (float) cnt) : 0.0f;
}
static int soloFabric()
{
    int err = 0; const auto dir = LinkShm::resolveDir (err);
    int fd = -1, rerr = 0; void* reg = LinkShm::openRegistry (dir, fd, rerr);
    if (reg == nullptr) { std::printf ("registry not mappable (%d)\n", rerr); return 99; }
    auto mk = [&](const char* nm) { auto l = std::make_unique<LinkProcessor>(); l->linkName = nm; l->markTypedNameAuthoritative(); l->prepareToPlay (48000.0, 512); l->updateShmState(); return l; };
    auto A = mk ("A"), B = mk ("B"), C = mk ("C");
    // heartbeats climb once per second; the fabric scan needs one climb since first seen (RegLiveness) and freshness
    for (int t = 0; t < 4; ++t) { pumpMs (1000); rmsOfBlock (*A, 2); rmsOfBlock (*B, 2); rmsOfBlock (*C, 2); }
    std::printf ("  claimed: A slot %d uid %s, B slot %d, C slot %d\n", A->diag.slotIdx, A->getInstanceUidForTest().toRawUTF8(), B->diag.slotIdx, C->diag.slotIdx);
    const float before = rmsOfBlock (*B, 8);
    // the main's command, byte for byte the same shape (sendLinkMuteSoloCommand)
    auto sendSolo = [&](bool on, int seq) { auto* cmd = new juce::DynamicObject(); cmd->setProperty ("v", 1); cmd->setProperty ("seq", seq); cmd->setProperty ("soloOn", on);
        juce::File (dir + "ctrl-cmd-" + A->getInstanceUidForTest() + ".json").replaceWithText (juce::JSON::toString (juce::var (cmd), true)); };
    sendSolo (true, 1);
    // (i) sidecar
    bool sideSolo = false; double tSide = -1; const double t0 = juce::Time::getMillisecondCounterHiRes();
    for (int t = 0; t < 3000 && ! sideSolo; ++t) { pumpMs (2); const auto rc = LinkShm::readRackSidecar (dir, A->getInstanceUidForTest()); sideSolo = (rc.uid == A->getInstanceUidForTest()) && rc.soloOn; if (sideSolo) tSide = juce::Time::getMillisecondCounterHiRes() - t0; }
    std::printf ("  (i)   A's sidecar carries soloOn=true: %s (%.0f ms after the command; A soloIsOn=%d)\n", sideSolo ? "YES" : "NO", tSide, (int) A->soloIsOn());
    // (ii) the others' scans
    bool bMute = false, cMute = false, aMute = false; double tScan = -1;
    for (int t = 0; t < 3000 && ! (bMute && cMute); ++t) { pumpMs (2); rmsOfBlock (*A, 1); rmsOfBlock (*B, 1); rmsOfBlock (*C, 1); bMute = B->linkMuteWanted(); cMute = C->linkMuteWanted(); aMute = A->linkMuteWanted(); if (bMute && cMute) tScan = juce::Time::getMillisecondCounterHiRes() - t0; }
    std::printf ("  (ii)  B muteWanted=%d C muteWanted=%d A muteWanted=%d (%.0f ms after the command)\n", (int) bMute, (int) cMute, (int) aMute, tScan);
    // (iii) audio
    const float during = rmsOfBlock (*B, 40);
    const float aDuring = rmsOfBlock (*A, 40);
    std::printf ("  (iii) B output rms before %.3f, while A is soloed %.4f; A's own output while soloed %.3f\n", before, during, aDuring);
    sendSolo (false, 2);
    bool bUn = true; for (int t = 0; t < 3000 && bUn; ++t) { pumpMs (2); rmsOfBlock (*B, 1); bUn = B->linkMuteWanted(); }
    const float after = rmsOfBlock (*B, 40);
    std::printf ("  unsolo: B muteWanted=%d, B output rms %.3f\n", (int) bUn, after);
    // THE BAR (8 Sep 2026 ruling, replacing "correct end state"): solo is a control a person operates in real
    // time; it must be AUDIBLE within 100 ms of the command. The harness asserts the bound, not just the state.
    constexpr double kSoloBoundMs = 100.0;
    const bool inTime = tScan >= 0 && tScan <= kSoloBoundMs;
    const bool ok = sideSolo && bMute && cMute && ! aMute && before > 0.3f && during < 0.01f && aDuring > 0.3f && ! bUn && after > 0.3f && inTime;
    std::printf ("SOLO FABRIC: (i) %s  (ii) %s  (iii) %s  unsolo %s  TIME %.0f ms vs %.0f ms bound %s -> %s\n", sideSolo ? "PASS" : "FAIL", (bMute && cMute && ! aMute) ? "PASS" : "FAIL", (during < 0.01f && aDuring > 0.3f) ? "PASS" : "FAIL", (! bUn && after > 0.3f) ? "PASS" : "FAIL", tScan, kSoloBoundMs, inTime ? "PASS" : "FAIL", ok ? "PASS" : "FAIL");
    drain(); A.reset(); B.reset(); C.reset(); drain();
    return ok ? 0 : 1;
}


// ---- TWO-PROCESS SOLO LEGS, LINK SIDE (8 Sep 2026 ruling): this process holds A, B, C in the private root
// and MEASURES; the main-plugin harness (solo_legs_test) presses. Handshake by files in the link dir:
//   solo_ready.txt   <- us: the three uids, once claimed and heartbeat-proven
//   solo_t0.txt      <- main: wall-clock ms just before the press (solo A)
//   solo_t1.txt      <- main: wall-clock ms just before the release
//   solo_state.txt   -> us, every 20 ms: A/B/C muteWanted + rms, for the borrow leg's assertions
// Verdict here: B (a non-soloed Link) must be MUTED within 100 ms of t0 and AUDIBLE again within 100 ms of t1.
// A wall-clock playhead on an epoch shared through the state root: both processes report the same host position
// for the same instant, so the Link's ring stamps line up with the main's position the way one host callback does.
struct EpochPlayHead : public juce::AudioPlayHead
{
    double epochMs = 0; bool playing = true;
    juce::Optional<PositionInfo> getPosition() const override
    { PositionInfo q; q.setIsPlaying (playing); const double s = (juce::Time::getMillisecondCounterHiRes() - epochMs) * 48.0; q.setTimeInSamples ((juce::int64) s); q.setTimeInSeconds (s / 48000.0); return q; }
};
static int soloFabric2()
{
    int err = 0; const auto dir = LinkShm::resolveDir (err);
    int fd = -1, rerr = 0; void* reg = LinkShm::openRegistry (dir, fd, rerr);
    if (reg == nullptr) { std::printf ("registry not mappable (%d)\n", rerr); return 99; }
    auto mk = [&](const char* nm) { auto l = std::make_unique<LinkProcessor>(); l->linkName = nm; l->markTypedNameAuthoritative(); l->prepareToPlay (48000.0, 512); l->updateShmState(); return l; };
    auto A = mk ("A"), B = mk ("B"), C = mk ("C");
    EpochPlayHead eph; eph.epochMs = juce::Time::getMillisecondCounterHiRes();
    juce::File (dir + "solo_epoch.txt").replaceWithText (juce::String (eph.epochMs, 3));   // the main reads the same epoch
    A->setPlayHead (&eph); B->setPlayHead (&eph); C->setPlayHead (&eph);
    for (int t = 0; t < 4; ++t)
    {
        const double a = juce::Time::getMillisecondCounterHiRes(); pumpMs (1000);
        const double b = juce::Time::getMillisecondCounterHiRes(); rmsOfBlock (*A, 2); rmsOfBlock (*B, 2); rmsOfBlock (*C, 2);
        std::printf ("  warm-up %d: pump(1000) took %.0f ms, 6 blocks took %.0f ms\n", t, b - a, juce::Time::getMillisecondCounterHiRes() - b); std::fflush (stdout);
    }
    juce::File (dir + "solo_ready.txt").replaceWithText (A->getInstanceUidForTest() + "\n" + B->getInstanceUidForTest() + "\n" + C->getInstanceUidForTest() + "\n");
    std::printf ("  ready: A %s B %s C %s\n", A->getInstanceUidForTest().toRawUTF8(), B->getInstanceUidForTest().toRawUTF8(), C->getInstanceUidForTest().toRawUTF8());
    float rmsA = 0, rmsB = 0, rmsC = 0;
    auto stamp = [&]{ juce::File (dir + "solo_state.txt").replaceWithText (juce::String ((int) A->linkMuteWanted()) + " " + juce::String ((int) B->linkMuteWanted()) + " " + juce::String ((int) C->linkMuteWanted())
                                                                              + " " + juce::String (rmsA, 3) + " " + juce::String (rmsB, 3) + " " + juce::String (rmsC, 3)); };
    auto waitFile = [&](const char* name, juce::int64& tOut, int maxMs) { const double end = juce::Time::getMillisecondCounterHiRes() + maxMs; while (juce::Time::getMillisecondCounterHiRes() < end) { juce::File f (dir + name); if (f.existsAsFile()) { tOut = f.loadFileAsString().trim().getLargeIntValue(); return true; } pumpMs (2); rmsA = rmsOfBlock (*A, 1); rmsB = rmsOfBlock (*B, 1); rmsC = rmsOfBlock (*C, 1); stamp(); } return false; };
    juce::int64 t0 = 0, t1 = 0;
    if (! waitFile ("solo_t0.txt", t0, 90000)) { std::printf ("SOLO2: no press arrived\n"); return 2; }
    // measure: time from t0 until B wants mute and its output is silent
    double tMute = -1, tSilent = -1; float bRms = 1;
    for (int i = 0; i < 3000; ++i) { pumpMs (2); rmsA = rmsOfBlock (*A, 1); rmsC = rmsOfBlock (*C, 1); bRms = rmsB = rmsOfBlock (*B, 1); stamp();
        if (tMute < 0 && B->linkMuteWanted()) tMute = (double) (juce::Time::currentTimeMillis() - t0);
        if (tSilent < 0 && bRms < 0.01f) tSilent = (double) (juce::Time::currentTimeMillis() - t0);
        if (tMute >= 0 && tSilent >= 0) break; }
    std::printf ("  B muteWanted %.0f ms after the press; B silent (rms<0.01) %.0f ms after; A muteWanted=%d C muteWanted=%d\n", tMute, tSilent, (int) A->linkMuteWanted(), (int) C->linkMuteWanted());
    if (! waitFile ("solo_t1.txt", t1, 90000)) { std::printf ("SOLO2: no release arrived\n"); return 2; }
    double tUn = -1, tAud = -1;
    for (int i = 0; i < 3000; ++i) { pumpMs (2); rmsA = rmsOfBlock (*A, 1); rmsC = rmsOfBlock (*C, 1); bRms = rmsB = rmsOfBlock (*B, 1); stamp();
        if (tUn < 0 && ! B->linkMuteWanted()) tUn = (double) (juce::Time::currentTimeMillis() - t1);
        if (tAud < 0 && bRms > 0.3f) tAud = (double) (juce::Time::currentTimeMillis() - t1);
        if (tUn >= 0 && tAud >= 0) break; }
    std::printf ("  B unmuted %.0f ms after the release; audible again %.0f ms after\n", tUn, tAud);
    const bool ok = tMute >= 0 && tMute <= 100 && tSilent >= 0 && tSilent <= 150 && tUn >= 0 && tUn <= 100 && tAud >= 0;
    const bool borrowMode = juce::File (dir + "solo_mode.txt").existsAsFile();
    if (borrowMode)
    {   // keep rendering and stamping until the main says it is done (rendered-audio legs read the state file throughout)
        // REAL-TIME PACED (9 Sep 2026): the host calls each Link once per 512 samples of wall time; a harness that renders
        // faster starves or floods the ring the main injects from. One block per Link every 10.667 ms.
        double nextMs = juce::Time::getMillisecondCounterHiRes();
        for (int i = 0; i < 30000 && ! juce::File (dir + "solo_done.txt").existsAsFile(); ++i)
        {
            pumpMs (1);
            const double now = juce::Time::getMillisecondCounterHiRes();
            if (now >= nextMs) { rmsA = rmsOfBlock (*A, 1); rmsB = rmsOfBlock (*B, 1); rmsC = rmsOfBlock (*C, 1); stamp(); nextMs += 512000.0 / 48000.0; if (now - nextMs > 200) nextMs = now; }
        }   // solo-dominates-borrow: B is the SOLOED Link here; the assertion is that it stays AUDIBLE (at least one channel audible)
        const bool bAud = ! B->linkMuteWanted() && bRms > 0.3f;
        std::printf ("SOLO2 (borrow mode): B audible while soloed = %d (rms %.3f) -> %s\n", (int) bAud, bRms, bAud ? "PASS" : "FAIL");
        juce::File (dir + "solo_done.txt").replaceWithText ("1"); pumpMs (500); drain(); A.reset(); B.reset(); C.reset(); drain();
        return bAud ? 0 : 1;
    }
    std::printf ("SOLO2: mute %s (%.0f ms vs 100)  unmute %s (%.0f ms vs 100) -> %s\n", (tMute >= 0 && tMute <= 100) ? "PASS" : "FAIL", tMute, (tUn >= 0 && tUn <= 100) ? "PASS" : "FAIL", tUn, ok ? "PASS" : "FAIL");
    juce::File (dir + "solo_done.txt").replaceWithText ("1");
    pumpMs (500); drain(); A.reset(); B.reset(); C.reset(); drain();
    return ok ? 0 : 1;
}


static int pumpTest()
{
    int err = 0; const auto dir = LinkShm::resolveDir (err); int fd = -1, rerr = 0; void* reg = LinkShm::openRegistry (dir, fd, rerr);
    if (reg == nullptr) return 99;
    auto t = [](const char* what, std::function<void()> f) { const double a = juce::Time::getMillisecondCounterHiRes(); f(); std::printf ("  %-44s %.0f ms\n", what, juce::Time::getMillisecondCounterHiRes() - a); std::fflush (stdout); };
    t ("pump(1000) with NO Link", [&]{ pump (1000); });
    auto A = std::make_unique<LinkProcessor>(); A->linkName = "A"; A->markTypedNameAuthoritative(); A->prepareToPlay (48000.0, 512); A->updateShmState();
    t ("pump(1000) with 1 Link", [&]{ pump (1000); });
    auto B = std::make_unique<LinkProcessor>(); B->linkName = "B"; B->markTypedNameAuthoritative(); B->prepareToPlay (48000.0, 512); B->updateShmState();
    auto C = std::make_unique<LinkProcessor>(); C->linkName = "C"; C->markTypedNameAuthoritative(); C->prepareToPlay (48000.0, 512); C->updateShmState();
    t ("pump(1000) with 3 Links", [&]{ pump (1000); });
    t ("pump(1000) with 3 Links, no rms", [&]{ pump (1000); });
    t ("CFRunLoopRunInMode x100 (0.01 s) direct", [&]{ for (int i = 0; i < 100; ++i) CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false); });
    t ("sleep 1000 (no pump)", [&]{ juce::Thread::sleep (1000); });
    t ("pump(1000) with 3 Links, 2nd time", [&]{ pump (1000); });
    drain(); A.reset(); B.reset(); C.reset(); drain();
    return 0;
}

static int legs()
{
    int err = 0; const auto dir = LinkShm::resolveDir (err);
    int fd = -1, rerr = 0; void* reg = LinkShm::openRegistry (dir, fd, rerr);
    if (reg == nullptr) { std::printf ("registry not mappable (%d): %s\n", rerr, lastRegistryLayoutError().toRawUTF8()); return 99; }
    int bad = 0;
    std::printf ("== layout: %d slots, v%d ==\n", kRegMaxSlots, (int) kRegLayoutVersion);
    // ---- a live Link A whose chunk seeds the newcomers
    auto a = std::make_unique<LinkProcessor>(); a->linkName = "Vox"; a->prepareToPlay (48000.0, 512); nameFromHost (*a, "Track A"); a->updateShmState(); pump (40);
    juce::MemoryBlock chunkA; a->getStateInformation (chunkA);
    std::printf ("== P1: host name arrives BEFORE the gate's clear (seeded chunk, then the host delivers, then the claim) ==\n");
    {
        auto b = std::make_unique<LinkProcessor>(); b->prepareToPlay (48000.0, 512);
        b->setStateInformation (chunkA.getData(), (int) chunkA.getSize());   // seeded: provisional "Track A"
        nameFromHost (*b, "Track B");                                         // the host delivers BEFORE any claim resolved
        pump (150);
        std::printf ("  B: slot %d host \"%s\" typed \"%s\" published \"%s\"\n", b->diag.slotIdx, b->getHostTrackName().toRawUTF8(), b->linkName.toRawUTF8(), slotName (reg, b->diag.slotIdx).toRawUTF8());
        const bool ok = b->diag.slotIdx >= 0 && b->getHostTrackName() == "Track B" && b->linkName.isEmpty() && slotName (reg, b->diag.slotIdx) == "Track B";
        std::printf ("  -> %s\n", ok ? "PASS" : "FAIL"); bad |= ok ? 0 : 1; drain(); b.reset(); drain();
    }
    std::printf ("== P2: host name arrives AFTER the gate's clear ==\n");
    {
        auto c = std::make_unique<LinkProcessor>(); c->prepareToPlay (48000.0, 512);
        c->setStateInformation (chunkA.getData(), (int) chunkA.getSize()); pump (150);   // re-mint + clear happen here
        const auto afterClear = slotName (reg, c->diag.slotIdx);
        nameFromHost (*c, "Track C"); pump (40);                                          // the host delivers AFTER
        std::printf ("  C: after the clear published \"%s\"; after the host delivered: host \"%s\" published \"%s\"\n", afterClear.toRawUTF8(), c->getHostTrackName().toRawUTF8(), slotName (reg, c->diag.slotIdx).toRawUTF8());
        const bool ok = c->diag.slotIdx >= 0 && afterClear.isEmpty() && c->getHostTrackName() == "Track C" && slotName (reg, c->diag.slotIdx) == "Track C";
        std::printf ("  -> %s\n", ok ? "PASS" : "FAIL"); bad |= ok ? 0 : 2; drain(); c.reset(); drain();
    }
    drain(); a.reset(); drain();
    std::printf ("== R1: every free slot filled with rows whose publisher pid is DEAD; a fresh Link must reclaim one ==\n");
    {
        auto rows = plantRows (reg, dir, kRegMaxSlots, 999999, "dd");
        std::printf ("  planted %d dead-publisher rows (registry now full)\n", (int) rows.size());
        auto n = std::make_unique<LinkProcessor>(); n->prepareToPlay (48000.0, 512); n->updateShmState(); pump (20);
        int stillDead = 0; auto* sl = LinkShm::regSlots (reg); for (int i : rows) if (LinkShm::loadAcquire (&sl[i].inUse) && juce::String::fromUTF8 (sl[i].instanceUid).startsWith ("dd")) ++stillDead;
        std::printf ("  fresh Link: slot %d regFull=%d; dead rows left %d of %d\n", n->diag.slotIdx, (int) n->diag.regFull, stillDead, (int) rows.size());
        const bool ok = n->diag.slotIdx >= 0 && ! n->diag.regFull;
        std::printf ("  -> %s\n", ok ? "PASS" : "FAIL"); bad |= ok ? 0 : 4; drain(); n.reset(); drain();
        for (int i : rows) juce::File (LinkShm::rackSidecarPath (dir, juce::String::fromUTF8 (sl[i].instanceUid))).deleteFile();
        for (int i = 0; i < kRegMaxSlots; ++i) if (LinkShm::loadAcquire (&sl[i].inUse) && juce::String::fromUTF8 (sl[i].instanceUid).startsWith ("dd")) LinkShm::releaseSlot (reg, i);
    }
    std::printf ("== R2: every free slot filled with rows whose publisher is ALIVE (this process); a fresh Link must REPORT full ==\n");
    {
        auto rows = plantRows (reg, dir, kRegMaxSlots, (int) getpid(), "aa");
        std::printf ("  planted %d live-publisher rows (registry now full)\n", (int) rows.size());
        auto n = std::make_unique<LinkProcessor>(); n->prepareToPlay (48000.0, 512); n->updateShmState(); pump (20);
        std::printf ("  fresh Link: slot %d regFull=%d\n", n->diag.slotIdx, (int) n->diag.regFull);
        const bool ok = n->diag.slotIdx < 0 && n->diag.regFull;
        std::printf ("  -> %s (reported, not silent)\n", ok ? "PASS" : "FAIL"); bad |= ok ? 0 : 8; drain(); n.reset(); drain();
        unplant (reg, dir, rows);
    }
    std::printf ("== V1: a region of the OLD layout (v1, 16 slots) must be refused with a clear message ==\n");
    {
        const juce::File scratch = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ej_layout_test"); scratch.deleteRecursively(); scratch.createDirectory();
        const juce::String sdir = scratch.getFullPathName() + "/";
        // write a header claiming v1 / 16 slots
        juce::MemoryBlock hdr (kRegSize, true); auto* h = static_cast<RegistryHeader*> (hdr.getData()); h->magic = kRegMagic; h->version = 1; h->maxSlots = 16;
        juce::File (sdir + juce::String (LinkShm::kRegistryFilename)).replaceWithData (hdr.getData(), hdr.getSize());
        int fd2 = -1, e2 = 0; void* m = LinkShm::openRegistry (sdir, fd2, e2);
        std::printf ("  openRegistry -> %s, errno %d, message: %s\n", m ? "MAPPED (wrong)" : "refused", e2, lastRegistryLayoutError().toRawUTF8());
        const bool ok = m == nullptr && e2 == EPROTO && lastRegistryLayoutError().contains ("REFUSING");
        std::printf ("  -> %s\n", ok ? "PASS" : "FAIL"); bad |= ok ? 0 : 16;
        if (m) LinkShm::closeRegistry (m, fd2); scratch.deleteRecursively();
    }
    return bad;
}

// THE CHURN LEGS (6 Sep 2026, v6 regressions). Pro Tools re-applies an
// instance's OWN chunk repeatedly (212 setState lines for ~40 instances in one
// live session). L8: after the host delivered the track name, the instance's
// own chunk re-applied N times must change NOTHING - same uid, same published
// name, no re-mint. L9: a soloed Link whose own chunk is re-applied must stay
// visible as soloed to another Link's fabric scan (sidecar under the SAME uid).
struct EchoJayLinkSyncTestAccess
{
    static void setSolo (LinkProcessor& p, bool s) { p.soloOn_.store (s, std::memory_order_relaxed); }
    static bool soloMuteWant (LinkProcessor& p) { return p.soloMuteWant_.load (std::memory_order_relaxed); }
    static void fabricScan (LinkProcessor& p) { p.soloFabricScan(); }
    static void publish (LinkProcessor& p) { p.publishRackSidecar(); }
};
static int churnLegs()
{
    int err = 0; const auto dir = LinkShm::resolveDir (err);
    int fd = -1, rerr = 0; void* reg = LinkShm::openRegistry (dir, fd, rerr);
    if (reg == nullptr) { std::printf ("registry not mappable (%d)\n", rerr); return 99; }
    int bad = 0;
    std::printf ("== L8: the host delivered \"Kick\"; then the instance's OWN chunk is re-applied 5x (as Pro Tools does) ==\n");
    {
        auto a = std::make_unique<LinkProcessor>(); a->prepareToPlay (48000.0, 512); a->updateShmState(); pump (30);
        nameFromHost (*a, "Kick"); pump (40);
        const auto uid0 = a->getInstanceUidForTest(); const auto slot0 = a->diag.slotIdx; const auto name0 = slotName (reg, a->diag.slotIdx);
        std::printf ("  after delivery: slot %d uid %s published \"%s\"\n", slot0, uid0.toRawUTF8(), name0.toRawUTF8());
        for (int r = 0; r < 5; ++r) { juce::MemoryBlock own; a->getStateInformation (own); a->setStateInformation (own.getData(), (int) own.getSize()); pump (30); nameFromHost (*a, "Kick"); pump (30); }
        const auto uid1 = a->getInstanceUidForTest(); const auto name1 = slotName (reg, a->diag.slotIdx);
        std::printf ("  after 5 re-applies: slot %d uid %s published \"%s\"\n", a->diag.slotIdx, uid1.toRawUTF8(), name1.toRawUTF8());
        const bool ok = name0 == "Kick" && uid1 == uid0 && name1 == "Kick" && a->diag.slotIdx >= 0;
        std::printf ("  -> %s (uid unchanged, \"Kick\" still published)\n", ok ? "PASS" : "FAIL"); bad |= ok ? 0 : 1;
        drain(); a.reset(); drain();
    }
    std::printf ("== L9: Link A soloed; Link B's fabric scan sees it; A's OWN chunk re-applied 3x; B must STILL see it ==\n");
    {
        auto a = std::make_unique<LinkProcessor>(); a->prepareToPlay (48000.0, 512); a->updateShmState(); pump (30);
        auto b = std::make_unique<LinkProcessor>(); b->prepareToPlay (48000.0, 512); b->updateShmState(); pump (30);
        EchoJayLinkSyncTestAccess::setSolo (*a, true); EchoJayLinkSyncTestAccess::publish (*a); pump (10);
        for (int t = 0; t < 3; ++t) { pump (10); EchoJayLinkSyncTestAccess::fabricScan (*b); }
        const bool seen0 = EchoJayLinkSyncTestAccess::soloMuteWant (*b);
        const auto uidA0 = a->getInstanceUidForTest();
        for (int r = 0; r < 3; ++r) { juce::MemoryBlock own; a->getStateInformation (own); a->setStateInformation (own.getData(), (int) own.getSize()); pump (40); EchoJayLinkSyncTestAccess::publish (*a); }
        for (int t = 0; t < 6; ++t) { pump (10); EchoJayLinkSyncTestAccess::fabricScan (*b); }
        const bool seen1 = EchoJayLinkSyncTestAccess::soloMuteWant (*b);
        const bool sidecarUnderUid = juce::File (LinkShm::rackSidecarPath (dir, a->getInstanceUidForTest())).existsAsFile();
        std::printf ("  before: B sees A's solo = %d (A uid %s)   after 3 re-applies: A uid %s, sidecar under A's uid: %s, B sees A's solo = %d\n", (int) seen0, uidA0.toRawUTF8(), a->getInstanceUidForTest().toRawUTF8(), sidecarUnderUid ? "yes" : "NO", (int) seen1);
        const bool ok = seen0 && seen1 && a->getInstanceUidForTest() == uidA0 && sidecarUnderUid;
        std::printf ("  -> %s\n", ok ? "PASS" : "FAIL"); bad |= ok ? 0 : 2;
        EchoJayLinkSyncTestAccess::setSolo (*a, false); drain(); b.reset(); a.reset(); drain();
    }
    return bad;
}

static int scan50()
{
    int err = 0; const auto dir = LinkShm::resolveDir (err);
    int fd = -1, rerr = 0; void* reg = LinkShm::openRegistry (dir, fd, rerr);
    auto cpuMs = [] { rusage r; getrusage (RUSAGE_SELF, &r); return (r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1000.0 + (r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1000.0; };
    auto measure = [&] (int rows) {
        auto planted = plantRows (reg, dir, rows, (int) getpid(), "sc");
        auto l = std::make_unique<LinkProcessor>(); l->prepareToPlay (48000.0, 512); l->updateShmState(); pump (25);
        auto* sl = LinkShm::regSlots (reg);
        const double c0 = cpuMs(); const auto t0 = juce::Time::getMillisecondCounterHiRes();
        for (int t = 0; t < 150; ++t) { for (int i : planted) LinkShm::storeRelease (&sl[i].heartbeat, (uint32_t) (2 + t)); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false); }
        const double cpu = cpuMs() - c0, wall = juce::Time::getMillisecondCounterHiRes() - t0;
        std::printf ("  one Link, %3d other live rows: %.1f ms CPU over %.0f ms wall = %.2f%% of one core\n", rows, cpu, wall, 100.0 * cpu / wall);
        drain(); l.reset(); drain(); unplant (reg, dir, planted);
    };
    std::printf ("== the Link's own 30 Hz timer (solo scan over the registry) ==\n");
    measure (0); measure (16); measure (50); measure (200);
    return 0;
}

int main (int argc, char** argv)
{
    echojay::requireIsolationOrDie ("link_capacity_test.cpp");
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI init;
    if (argc > 1 && juce::String (argv[1]) == "scan50") return scan50();
    if (argc > 1 && juce::String (argv[1]) == "p20")    return p20();
    if (argc > 1 && juce::String (argv[1]) == "foreign")   return foreign (1);
    if (argc > 1 && juce::String (argv[1]) == "solofabric") return soloFabric();
    if (argc > 1 && juce::String (argv[1]) == "solofabric2") return soloFabric2();
    if (argc > 1 && juce::String (argv[1]) == "pumptest") return pumpTest();
    if (argc > 1 && juce::String (argv[1]) == "foreign20") return foreign (20);
    if (argc > 1 && juce::String (argv[1]) == "churn")  { const int r = churnLegs(); std::printf ("churn legs: L8 %s   L9 %s\n", (r & 1) ? "FAIL" : "PASS", (r & 2) ? "FAIL" : "PASS"); return r; }
    if (argc > 1 && juce::String (argv[1]) == "churn20") { int p8 = 0, p9 = 0; for (int r = 0; r < 20; ++r) { const int x = churnLegs(); if (! (x & 1)) ++p8; if (! (x & 2)) ++p9; } std::printf ("CHURN20: L8 %d/20   L9 %d/20\n", p8, p9); return (p8 == 20 && p9 == 20) ? 0 : 1; }
    const int r = legs();
    std::printf ("capacity legs: P1 %s  P2 %s  R1 %s  R2 %s  V1 %s\n", (r & 1) ? "FAIL" : "PASS", (r & 2) ? "FAIL" : "PASS", (r & 4) ? "FAIL" : "PASS", (r & 8) ? "FAIL" : "PASS", (r & 16) ? "FAIL" : "PASS");
    return r;
}
