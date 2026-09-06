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
    if (argc > 1 && juce::String (argv[1]) == "churn")  { const int r = churnLegs(); std::printf ("churn legs: L8 %s   L9 %s\n", (r & 1) ? "FAIL" : "PASS", (r & 2) ? "FAIL" : "PASS"); return r; }
    if (argc > 1 && juce::String (argv[1]) == "churn20") { int p8 = 0, p9 = 0; for (int r = 0; r < 20; ++r) { const int x = churnLegs(); if (! (x & 1)) ++p8; if (! (x & 2)) ++p9; } std::printf ("CHURN20: L8 %d/20   L9 %d/20\n", p8, p9); return (p8 == 20 && p9 == 20) ? 0 : 1; }
    const int r = legs();
    std::printf ("capacity legs: P1 %s  P2 %s  R1 %s  R2 %s  V1 %s\n", (r & 1) ? "FAIL" : "PASS", (r & 2) ? "FAIL" : "PASS", (r & 4) ? "FAIL" : "PASS", (r & 8) ? "FAIL" : "PASS", (r & 16) ? "FAIL" : "PASS");
    return r;
}
