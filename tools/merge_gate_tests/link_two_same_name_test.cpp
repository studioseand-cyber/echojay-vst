/*  SHOOT-DAY DEFECT (6 Sep 2026): two Links in one host with the SAME typed
    name. Rows are uid-keyed and distinct, but the audio ring FILE is keyed on
    the typed name (LinkProcessor::effectiveFilePart), so both instances open
    the same ring and openRingProducer zeroes its header: one file, two
    producers, the second wipes the first. Observable here, from the registry
    both Links publish into: each slot's audioFile string and the ring's
    on-disk identity. BEFORE the fix both slots name one file; AFTER, each
    names its own (uid-keyed) file. Positive control: two DIFFERENT names are
    two files today and must stay so. Rows are released on exit.            */
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "LinkProcessor.h"
#include "LinkShm.h"
#include <cstdio>

// A console process has no application run loop; JUCE's message queue and
// timers dispatch through the main CFRunLoop, so pump it by hand. Drained
// before an instance is destroyed: LinkProcessor queues callAsync([this]) from
// prepareToPlay/setStateInformation and a queued callback outliving its
// instance is a use-after-free (harness ordering; a host pumps continuously).
static void pump (int iters) { for (int t = 0; t < iters; ++t) CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false); }
static void drain() { pump (5); }

struct Row { juce::String name, file, uid; };
static std::vector<Row> rowsNamed (void* reg, const juce::String& name)
{
    std::vector<Row> out; auto* s = LinkShm::regSlots (reg);
    for (int i = 0; i < kRegMaxSlots; ++i)
        if (LinkShm::loadAcquire (&s[i].inUse) != 0 && juce::String::fromUTF8 (s[i].displayName) == name)
            out.push_back ({ name, juce::String::fromUTF8 (s[i].audioFile), juce::String::fromUTF8 (s[i].instanceUid) });
    return out;
}

static int leg (const char* label, const juce::String& nameA, const juce::String& nameB, bool expectDistinct)
{
    std::printf ("== %s: Link A \"%s\", Link B \"%s\" ==\n", label, nameA.toRawUTF8(), nameB.toRawUTF8());
    auto a = std::make_unique<LinkProcessor>(); auto b = std::make_unique<LinkProcessor>();
    a->linkName = nameA; b->linkName = nameB;
    a->prepareToPlay (48000.0, 512); b->prepareToPlay (48000.0, 512);
    a->updateShmState(); b->updateShmState();          // claim + open ring, synchronously
    int err = 0; const auto dir = LinkShm::resolveDir (err);
    int fd = -1, rerr = 0; void* reg = LinkShm::openRegistry (dir, fd, rerr);
    if (reg == nullptr) { std::printf ("  registry not mappable (%d)\n", rerr); return 99; }
    auto ra = rowsNamed (reg, nameA), rb = rowsNamed (reg, nameB);
    // our two rows are the ones whose uid is not some other live Link's: take the last two claimed
    std::vector<Row> ours;
    for (auto& r : ra) ours.push_back (r);
    if (nameB != nameA) for (auto& r : rb) ours.push_back (r);
    std::printf ("  rows named as ours in the registry: %d\n", (int) ours.size());
    for (auto& r : ours) std::printf ("    row uid %-12s file %-28s exists %s\n", r.uid.toRawUTF8(), r.file.toRawUTF8(), juce::File (dir + r.file).existsAsFile() ? "yes" : "NO");
    int bad = 0;
    if (ours.size() < 2) { std::printf ("  FEWER THAN TWO ROWS - one Link did not register\n"); bad = 1; }
    else
    {
        const bool distinct = ours[0].file != ours[1].file;
        const bool sameInode = ! distinct || LinkShm::pathIdentity (dir + ours[0].file) == LinkShm::pathIdentity (dir + ours[1].file);
        std::printf ("  ring files: %s (%s)\n", distinct ? "DISTINCT" : "THE SAME FILE", sameInode ? "same inode" : "different inodes");
        if (distinct != expectDistinct) bad = 1;
    }
    std::printf ("  -> %s\n", bad ? "FAIL" : "PASS");
    drain(); a.reset(); b.reset(); drain();   // fire the instances' deferred publishes while they live, then release the rows
    return bad;
}

// THE CLONE (Pro Tools "duplicate track" / copy-paste of the insert restores the
// SAME saved state into two instances: same typed name AND same instanceUid).
// The uid claim gate must give the second instance its own uid; both timers
// run, as they do in a host.
static int cloneLeg()
{
    std::printf ("== THE CLONE: two Links restored from one saved state (same name, same uid) ==\n");
    juce::MemoryBlock st;
    { LinkProcessor seed; seed.linkName = "Vox"; seed.prepareToPlay (48000.0, 512); seed.getStateInformation (st); }
    auto a = std::make_unique<LinkProcessor>(); auto b = std::make_unique<LinkProcessor>();
    a->prepareToPlay (48000.0, 512); b->prepareToPlay (48000.0, 512);
    a->setStateInformation (st.getData(), (int) st.getSize());
    b->setStateInformation (st.getData(), (int) st.getSize());
    // Let both instances' own 30 Hz timers run for ~4 s, as they do in a host:
    // a console process has no application run loop, so pump the main CFRunLoop
    // (JUCE's timers and message queue dispatch through it).
    pump (200);
    int err = 0; const auto dir = LinkShm::resolveDir (err);
    int fd = -1, rerr = 0; void* reg = LinkShm::openRegistry (dir, fd, rerr);
    auto rows = rowsNamed (reg, "Vox");
    std::printf ("  registry rows named Vox: %d   A slot %d   B slot %d\n", (int) rows.size(), a->diag.slotIdx, b->diag.slotIdx);
    for (auto& r : rows) std::printf ("    row uid %-12s file %-28s\n", r.uid.toRawUTF8(), r.file.toRawUTF8());
    int bad = 0;
    if (rows.size() != 2) { std::printf ("  ONE ROW FOR TWO INSTANCES\n"); bad = 1; }
    else if (rows[0].uid == rows[1].uid) { std::printf ("  TWO ROWS, SAME UID (the gate did not re-mint)\n"); bad = 1; }
    else if (rows[0].file == rows[1].file) { std::printf ("  two rows, distinct uids, ONE RING FILE (name-keyed file)\n"); bad = 2; }
    if (a->diag.slotIdx == b->diag.slotIdx) { std::printf ("  BOTH INSTANCES HOLD THE SAME SLOT %d\n", a->diag.slotIdx); bad = 1; }
    std::printf ("  -> %s\n", bad ? "FAIL" : "PASS");
    drain(); a.reset(); b.reset(); drain();
    return bad;
}

// THE BURST: the same clone, but the second instance publishes several times
// SYNCHRONOUSLY before the first instance's timer has ticked once - what a
// host's session load looks like when it calls prepareToPlay/setState in a
// row with the message loop blocked. The gate's AdoptGhost fires after 5
// observations of a non-climbing heartbeat; observations are calls, not time.
static int burstLeg()
{
    std::printf ("== THE BURST: clone, second instance publishes 6x before the first's timer ticks ==\n");
    juce::MemoryBlock st;
    { LinkProcessor seed; seed.linkName = "Vox"; seed.prepareToPlay (48000.0, 512); seed.getStateInformation (st); }
    auto a = std::make_unique<LinkProcessor>(); auto b = std::make_unique<LinkProcessor>();
    a->prepareToPlay (48000.0, 512); b->prepareToPlay (48000.0, 512);
    a->setStateInformation (st.getData(), (int) st.getSize());
    b->setStateInformation (st.getData(), (int) st.getSize());
    for (int i = 0; i < 6; ++i) b->updateShmState();   // synchronous burst, no timer tick in between
    int err = 0; const auto dir = LinkShm::resolveDir (err);
    int fd = -1, rerr = 0; void* reg = LinkShm::openRegistry (dir, fd, rerr);
    auto rows = rowsNamed (reg, "Vox");
    std::printf ("  after the burst: rows named Vox: %d   A slot %d   B slot %d\n", (int) rows.size(), a->diag.slotIdx, b->diag.slotIdx);
    for (auto& r : rows) std::printf ("    row uid %-12s file %-28s\n", r.uid.toRawUTF8(), r.file.toRawUTF8());
    pump (100);   // then let both timers run ~2 s, as the host would after loading
    rows = rowsNamed (reg, "Vox");
    std::printf ("  2 s later:       rows named Vox: %d   A slot %d   B slot %d\n", (int) rows.size(), a->diag.slotIdx, b->diag.slotIdx);
    for (auto& r : rows) std::printf ("    row uid %-12s file %-28s\n", r.uid.toRawUTF8(), r.file.toRawUTF8());
    int bad = 0;
    if (rows.size() != 2 || a->diag.slotIdx == b->diag.slotIdx) { std::printf ("  ONE SLOT FOR TWO INSTANCES (the gate adopted a live holder as a ghost)\n"); bad = 1; }
    else if (rows[0].uid == rows[1].uid) { std::printf ("  TWO ROWS, ONE UID\n"); bad = 1; }
    std::printf ("  -> %s\n", bad ? "FAIL" : "PASS");
    drain(); a.reset(); b.reset(); drain();
    return bad;
}

// THE GHOST LEGS (6 Sep 2026, Pro Tools storm): a FROZEN slot exists in the
// registry (a dead incarnation: inUse=1, heartbeat never climbs) carrying
// uid U and name "Ghost". (a) a FRESH Link (no state) must mint its own uid
// and take its own slot; (b) a Link RESTORING state that carries U must adopt
// the frozen slot (the 25 Aug 2026 case: session reopen after an unclean
// kill). The frozen slot is written by hand, borrowhost_test's pattern.
static int ghostLegs()
{
    int err = 0; const auto dir = LinkShm::resolveDir (err);
    int fd = -1, rerr = 0; void* reg = LinkShm::openRegistry (dir, fd, rerr);
    if (reg == nullptr) { std::printf ("== GHOST LEGS: registry not mappable (%d)\n", rerr); return 99; }
    auto* slots = LinkShm::regSlots (reg);
    const char* U = "deadbeef01";
    auto plantGhost = [&]() -> int
    {
        for (int i = kRegMaxSlots - 1; i >= 0; --i)
            if (LinkShm::loadAcquire (&slots[i].inUse) == 0)
            {
                std::memset (&slots[i], 0, sizeof (RegistrySlot));
                std::strncpy (slots[i].displayName, "Ghost", 39);
                std::strncpy (slots[i].audioFile, "audio_Ghost.bin", 47);
                std::strncpy (slots[i].instanceUid, U, 10);
                slots[i].sampleRate = 48000.0f; slots[i].numChannels = 2;
                LinkShm::storeRelease (&slots[i].heartbeat, 7u);   // frozen: never bumped again
                LinkShm::storeRelease (&slots[i].inUse, 1u);
                return i;
            }
        return -1;
    };
    int bad = 0;
    {
        std::printf ("== GHOST (a): a FRESH Link, no state, beside a frozen slot carrying uid %s ==\n", U);
        const int g = plantGhost();
        auto x = std::make_unique<LinkProcessor>(); x->prepareToPlay (48000.0, 512); x->updateShmState();
        pump (100);   // ~2 s of its own timer (claim retries, if any)
        const bool ghostStillThere = LinkShm::loadAcquire (&slots[g].inUse) != 0 && juce::String::fromUTF8 (slots[g].instanceUid) == U;
        const juce::String mine = x->diag.slotIdx >= 0 ? juce::String::fromUTF8 (slots[x->diag.slotIdx].instanceUid) : juce::String ("(no slot)");
        std::printf ("  fresh Link: slot %d uid %s   ghost slot %d still frozen-in-use: %s\n", x->diag.slotIdx, mine.toRawUTF8(), g, ghostStillThere ? "yes" : "NO (reaped/adopted)");
        const bool ok = x->diag.slotIdx >= 0 && x->diag.slotIdx != g && mine != U && ghostStillThere;
        std::printf ("  -> %s (a fresh instance must mint its OWN uid and leave the ghost alone)\n", ok ? "PASS" : "FAIL");
        bad |= ! ok;
        drain(); x.reset(); drain();
        if (ghostStillThere) LinkShm::releaseSlot (reg, g);
    }
    {
        std::printf ("== GHOST (b): a Link RESTORING state that carries uid %s must ADOPT the frozen slot ==\n", U);
        const int g = plantGhost();
        juce::String json = "{\"linkName\":\"\",\"linkOn\":true,\"instanceUid\":\"" + juce::String (U) + "\"}";
        auto y = std::make_unique<LinkProcessor>(); y->prepareToPlay (48000.0, 512);
        y->setStateInformation (json.toRawUTF8(), (int) json.getNumBytesAsUTF8());
        pump (200);   // the gate needs 5 frozen observations on its ~1 s retry tick
        const juce::String mine = y->diag.slotIdx >= 0 ? juce::String::fromUTF8 (slots[y->diag.slotIdx].instanceUid) : juce::String ("(no slot)");
        int uSlots = 0; for (int i = 0; i < kRegMaxSlots; ++i) if (LinkShm::loadAcquire (&slots[i].inUse) && juce::String::fromUTF8 (slots[i].instanceUid) == U) ++uSlots;
        std::printf ("  restoring Link: slot %d uid %s   slots carrying %s now: %d\n", y->diag.slotIdx, mine.toRawUTF8(), U, uSlots);
        const bool ok = y->diag.slotIdx >= 0 && mine == U && uSlots == 1;
        std::printf ("  -> %s (restore-of-own-uid adopts the ghost, exactly one slot carries the uid)\n", ok ? "PASS" : "FAIL");
        bad |= (ok ? 0 : 2);
        drain(); y.reset(); drain();
        for (int i = 0; i < kRegMaxSlots; ++i) if (LinkShm::loadAcquire (&slots[i].inUse) && juce::String::fromUTF8 (slots[i].instanceUid) == U) LinkShm::releaseSlot (reg, i);
    }
    return bad;
}

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI init;
    const int ctl  = leg ("POSITIVE CONTROL, two different names", "VoxA", "VoxB", true);
    const int same = leg ("THE DEFECT, the same typed name",      "Vox",  "Vox",  true);
    const int clone = cloneLeg();
    const int burst = burstLeg();
    const int ghost = ghostLegs();
    std::printf ("ghost legs: fresh-beside-ghost %s   restore-adopts %s\n", (ghost & 1) ? "FAIL" : "PASS", (ghost & 2) ? "FAIL" : "PASS");
    std::printf ("control: %s   same-name: %s   clone: %s   burst: %s\n", ctl == 0 ? "PASS" : "FAIL", same == 0 ? "PASS (two files)" : "FAIL (one file shared by two Links)",
                 clone == 0 ? "PASS" : (clone == 2 ? "FAIL (one ring file)" : "FAIL (one slot / one uid)"), burst == 0 ? "PASS" : "FAIL (one slot)");
    return ctl != 0 ? 2 : (same | clone | burst | ghost);
}
