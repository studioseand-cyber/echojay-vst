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
#include "EJStateRoot.h"
#include "LinkProcessor.h"
#include "LinkShm.h"
#include <cstdio>
#include <unistd.h>
#include <set>
#include <vector>

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
    auto* sl = LinkShm::regSlots (reg);
    auto rowOf = [&] (int idx) { Row r; if (idx >= 0) { r.name = juce::String::fromUTF8 (sl[idx].displayName); r.file = juce::String::fromUTF8 (sl[idx].audioFile); r.uid = juce::String::fromUTF8 (sl[idx].instanceUid); } return r; };
    const Row ra = rowOf (a->diag.slotIdx), rb = rowOf (b->diag.slotIdx);
    std::printf ("  A slot %d uid %s name \"%s\" file %s\n  B slot %d uid %s name \"%s\" file %s\n", a->diag.slotIdx, ra.uid.toRawUTF8(), ra.name.toRawUTF8(), ra.file.toRawUTF8(), b->diag.slotIdx, rb.uid.toRawUTF8(), rb.name.toRawUTF8(), rb.file.toRawUTF8());
    int bad = 0;
    if (a->diag.slotIdx < 0 || b->diag.slotIdx < 0 || a->diag.slotIdx == b->diag.slotIdx) { std::printf ("  ONE SLOT (or none) FOR TWO INSTANCES\n"); bad = 1; }
    else if (ra.uid == rb.uid) { std::printf ("  TWO SLOTS, SAME UID (the gate did not re-mint)\n"); bad = 1; }
    else if (ra.file == rb.file) { std::printf ("  distinct uids, ONE RING FILE (name-keyed file)\n"); bad = 2; }
    // THE INVARIANT (6 Sep 2026): neither instance originated this chunk (its
    // author, the seed instance, is gone): A re-mints under the no-holder rule
    // (L5) and B against live A - both publish EMPTY, never the seeded "Vox".
    if (bad == 0 && (ra.name == "Vox" || rb.name == "Vox")) { std::printf ("  a duplicate published the seeded name (the invariant)\n"); bad = 1; }
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
    std::printf ("  after the burst: A slot %d   B slot %d\n", a->diag.slotIdx, b->diag.slotIdx);
    pump (100);   // then let both timers run ~2 s, as the host would after loading
    auto* sl = LinkShm::regSlots (reg);
    auto uidAt = [&] (int idx) { return idx >= 0 ? juce::String::fromUTF8 (sl[idx].instanceUid) : juce::String ("(none)"); };
    std::printf ("  2 s later:       A slot %d uid %s   B slot %d uid %s\n", a->diag.slotIdx, uidAt (a->diag.slotIdx).toRawUTF8(), b->diag.slotIdx, uidAt (b->diag.slotIdx).toRawUTF8());
    int bad = 0;
    if (a->diag.slotIdx < 0 || b->diag.slotIdx < 0 || a->diag.slotIdx == b->diag.slotIdx) { std::printf ("  ONE SLOT FOR TWO INSTANCES (the gate adopted a live holder as a ghost)\n"); bad = 1; }
    else if (uidAt (a->diag.slotIdx) == uidAt (b->diag.slotIdx)) { std::printf ("  TWO SLOTS, ONE UID\n"); bad = 1; }
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

// THE SEEDED-CHUNK LEGS (6 Sep 2026 ruling, C1/C2/C3). Pro Tools seeds a fresh
// insert with the plugin's last chunk. A chunk that belongs to another LIVE
// instance must contribute NO field that answers "which Link is this":
// neither its host track name nor its typed name. The seeding stays for the
// two cases it was built for: AdoptGhost (a replacement incarnation on the
// same track) and a plain restore (session reopen).
static juce::String slotName (void* reg, int idx) { return idx >= 0 ? juce::String::fromUTF8 (LinkShm::regSlots (reg)[idx].displayName) : juce::String ("(no slot)"); }
static juce::String slotUid  (void* reg, int idx) { return idx >= 0 ? juce::String::fromUTF8 (LinkShm::regSlots (reg)[idx].instanceUid) : juce::String ("(no slot)"); }
static void nameFromHost (LinkProcessor& l, const char* name) { juce::AudioProcessor::TrackProperties tp; tp.name = std::make_optional (juce::String (name)); l.updateTrackProperties (tp); }

static int l4Leg (LinkProcessor& a, const juce::MemoryBlock& chunkA, void* reg)
{
    std::printf ("== L4: THREE fresh inserts beside live Link A, each seeded with A's chunk -> three DISTINCT identities and names ==\n");
    std::vector<std::unique_ptr<LinkProcessor>> v;
    for (int i = 0; i < 3; ++i) { v.push_back (std::make_unique<LinkProcessor>()); v.back()->prepareToPlay (48000.0, 512); v.back()->setStateInformation (chunkA.getData(), (int) chunkA.getSize()); pump (60); }
    pump (150);
    std::set<juce::String> uids, names; std::set<int> slotsUsed;
    for (auto& l : v) { uids.insert (slotUid (reg, l->diag.slotIdx)); names.insert (slotName (reg, l->diag.slotIdx)); slotsUsed.insert (l->diag.slotIdx);
                        std::printf ("  insert: slot %d uid %s published \"%s\"\n", l->diag.slotIdx, slotUid (reg, l->diag.slotIdx).toRawUTF8(), slotName (reg, l->diag.slotIdx).toRawUTF8()); }
    const bool distinctIds = uids.size() == 3 && slotsUsed.size() == 3 && ! slotsUsed.count (-1) && ! uids.count (a.getInstanceUidForTest());
    const bool noFalseNames = names.size() == 1 && names.count (juce::String());
    const bool aIntact = a.diag.slotIdx >= 0 && slotUid (reg, a.diag.slotIdx) == a.getInstanceUidForTest();
    std::printf ("  A still holds its slot %d with its uid: %s\n  -> %s (three uids/slots, none A's; every published name EMPTY so the main plugin numbers them Untitled 1..3)\n", a.diag.slotIdx, aIntact ? "yes" : "NO", (distinctIds && noFalseNames && aIntact) ? "PASS" : "FAIL");
    drain(); v.clear(); drain();
    return (distinctIds && noFalseNames && aIntact) ? 0 : 8;
}

static int seededLegs()
{
    int err = 0; const auto dir = LinkShm::resolveDir (err);
    int fd = -1, rerr = 0; void* reg = LinkShm::openRegistry (dir, fd, rerr);
    if (reg == nullptr) { std::printf ("== SEEDED LEGS: registry not mappable (%d)\n", rerr); return 99; }
    int bad = 0;
    // Link A: live, typed name "Vox", host track name "Track A"
    auto a = std::make_unique<LinkProcessor>(); a->linkName = "Vox"; a->prepareToPlay (48000.0, 512); nameFromHost (*a, "Track A"); a->updateShmState(); pump (60);
    juce::MemoryBlock chunkA; a->getStateInformation (chunkA);
    std::printf ("== L1: a chunk from PROVEN-LIVE Link A (typed \"Vox\", host \"Track A\", uid %s) seeded into fresh Link B ==\n", a->getInstanceUidForTest().toRawUTF8());
    {
        auto b = std::make_unique<LinkProcessor>(); b->prepareToPlay (48000.0, 512);
        b->setStateInformation (chunkA.getData(), (int) chunkA.getSize());
        pump (150);   // the gate: A's heartbeat climbs -> re-mint
        const auto disp = b->effectiveDisplayName(); const auto host = b->getHostTrackName(); const auto typed = b->linkName;
        std::printf ("  B: slot %d uid %s  typed \"%s\"  host \"%s\"  published \"%s\"\n", b->diag.slotIdx, slotUid (reg, b->diag.slotIdx).toRawUTF8(), typed.toRawUTF8(), host.toRawUTF8(), slotName (reg, b->diag.slotIdx).toRawUTF8());
        const bool ok = b->diag.slotIdx >= 0 && slotUid (reg, b->diag.slotIdx) != a->getInstanceUidForTest() && typed.isEmpty() && host.isEmpty() && disp.isEmpty() && slotName (reg, b->diag.slotIdx).isEmpty();
        std::printf ("  -> %s (B must publish NEITHER \"Vox\" NOR \"Track A\")\n", ok ? "PASS" : "FAIL"); bad |= ok ? 0 : 1;
        drain(); b.reset(); drain();
    }
    bad |= l4Leg (*a, chunkA, reg);
    if (false) {
        std::vector<std::unique_ptr<LinkProcessor>> v;
        for (int i = 0; i < 3; ++i) { v.push_back (std::make_unique<LinkProcessor>()); v.back()->prepareToPlay (48000.0, 512); v.back()->setStateInformation (chunkA.getData(), (int) chunkA.getSize()); pump (60); }
        pump (150);
        std::set<juce::String> uids, names; std::set<int> slotsUsed;
        for (auto& l : v) { uids.insert (slotUid (reg, l->diag.slotIdx)); names.insert (slotName (reg, l->diag.slotIdx)); slotsUsed.insert (l->diag.slotIdx);
                            std::printf ("  insert: slot %d uid %s published \"%s\"\n", l->diag.slotIdx, slotUid (reg, l->diag.slotIdx).toRawUTF8(), slotName (reg, l->diag.slotIdx).toRawUTF8()); }
        const bool distinctIds = uids.size() == 3 && slotsUsed.size() == 3 && ! slotsUsed.count (-1) && ! uids.count (a->getInstanceUidForTest());
        const bool noFalseNames = names.size() == 1 && names.count (juce::String());   // all EMPTY: the main plugin numbers them (C3, proven in the main-archive leg)
        std::printf ("  -> %s (three uids/slots, none A's; every published name EMPTY so the main plugin numbers them Untitled 1..3)\n", (distinctIds && noFalseNames) ? "PASS" : "FAIL"); bad |= (distinctIds && noFalseNames) ? 0 : 8;
        drain(); v.clear(); drain();
    }
    drain(); a.reset(); drain();
    auto* slots = LinkShm::regSlots (reg);
    const char* U = "deadbeef02";
    auto plant = [&]() -> int { for (int i = kRegMaxSlots - 1; i >= 0; --i) if (LinkShm::loadAcquire (&slots[i].inUse) == 0) { std::memset (&slots[i], 0, sizeof (RegistrySlot)); std::strncpy (slots[i].displayName, "Vox", 39); std::strncpy (slots[i].audioFile, "audio_x.bin", 47); std::strncpy (slots[i].instanceUid, U, 10); slots[i].sampleRate = 48000.0f; slots[i].numChannels = 2; LinkShm::storeRelease (&slots[i].heartbeat, 7u); LinkShm::storeRelease (&slots[i].inUse, 1u); return i; } return -1; };
    const juce::String seeded = "{\"linkName\":\"Vox\",\"linkOn\":true,\"instanceUid\":\"" + juce::String (U) + "\",\"hostTrackName\":\"Track A\"}";
    std::printf ("== L2: AdoptGhost (a frozen slot carries the chunk's uid) still KEEPS the seeded names ==\n");
    {
        const int g = plant();
        auto c = std::make_unique<LinkProcessor>(); c->prepareToPlay (48000.0, 512); c->setStateInformation (seeded.toRawUTF8(), (int) seeded.getNumBytesAsUTF8()); pump (200);
        std::printf ("  C: slot %d (ghost was %d) uid %s typed \"%s\" host \"%s\" published \"%s\"\n", c->diag.slotIdx, g, slotUid (reg, c->diag.slotIdx).toRawUTF8(), c->linkName.toRawUTF8(), c->getHostTrackName().toRawUTF8(), slotName (reg, c->diag.slotIdx).toRawUTF8());
        const bool ok = c->diag.slotIdx >= 0 && slotUid (reg, c->diag.slotIdx) == U && c->linkName == "Vox" && c->getHostTrackName() == "Track A" && slotName (reg, c->diag.slotIdx) == "Vox";
        std::printf ("  -> %s\n", ok ? "PASS" : "FAIL"); bad |= ok ? 0 : 2;
        drain(); c.reset(); drain(); for (int i = 0; i < kRegMaxSlots; ++i) if (LinkShm::loadAcquire (&slots[i].inUse) && juce::String::fromUTF8 (slots[i].instanceUid) == U) LinkShm::releaseSlot (reg, i);
    }
    std::printf ("== L3: a plain restore (no holder anywhere) keeps both names ==\n");
    {
        auto d = std::make_unique<LinkProcessor>(); d->prepareToPlay (48000.0, 512); d->setStateInformation (seeded.toRawUTF8(), (int) seeded.getNumBytesAsUTF8()); pump (100);
        std::printf ("  D: slot %d uid %s typed \"%s\" host \"%s\" published \"%s\"\n", d->diag.slotIdx, slotUid (reg, d->diag.slotIdx).toRawUTF8(), d->linkName.toRawUTF8(), d->getHostTrackName().toRawUTF8(), slotName (reg, d->diag.slotIdx).toRawUTF8());
        const bool ok = d->diag.slotIdx >= 0 && slotUid (reg, d->diag.slotIdx) == U && d->linkName == "Vox" && d->getHostTrackName() == "Track A" && slotName (reg, d->diag.slotIdx) == "Vox";
        std::printf ("  -> %s\n", ok ? "PASS" : "FAIL"); bad |= ok ? 0 : 4;
        drain(); d.reset(); drain();
    }
    return bad;
}

// C4c: a race leg that passes once proves nothing. Twenty consecutive runs each.
static int race20()
{
    int err = 0; const auto dir = LinkShm::resolveDir (err);
    int fd = -1, rerr = 0; void* reg = LinkShm::openRegistry (dir, fd, rerr);
    int l4pass = 0, burstpass = 0;
    for (int r = 0; r < 20; ++r)
    {
        auto a = std::make_unique<LinkProcessor>(); a->linkName = "Vox"; a->prepareToPlay (48000.0, 512); nameFromHost (*a, "Track A"); a->updateShmState(); pump (40);
        juce::MemoryBlock chunkA; a->getStateInformation (chunkA);
        if (l4Leg (*a, chunkA, reg) == 0) ++l4pass;
        drain(); a.reset(); drain();
        if (burstLeg() == 0) ++burstpass;
    }
    std::printf ("RACE20: L4 %d/20   burst %d/20\n", l4pass, burstpass);
    return (l4pass == 20 && burstpass == 20) ? 0 : 1;
}

// THE STORM MEASUREMENT (6 Sep 2026 ruling): Pro Tools tore the Link down and
// re-created it ~30 times in 30 s, each incarnation restoring the same chunk,
// each finding its predecessor's slot frozen (never released). Simulated with
// the frozen slot planted by hand each cycle (its sidecar naming THIS process
// as publisher - the same-pid case the floor exists for), an incarnation that
// lives ~lifeMs, then goes. Counted: identities burned (re-mints), sidecars and
// ring files left behind for uids other than the seed, adoptions, and how long
// after the storm the first surviving incarnation registers.
static int storm (int cycles, int lifeMs, int deadPid)
{
    int err = 0; const auto dir = LinkShm::resolveDir (err);
    int fd = -1, rerr = 0; void* reg = LinkShm::openRegistry (dir, fd, rerr);
    auto* slots = LinkShm::regSlots (reg);
    const juce::String U = "5707aaaaaa";
    const juce::String chunk = "{\"linkName\":\"\",\"linkOn\":true,\"instanceUid\":\"" + U + "\",\"hostTrackName\":\"bass 2\"}";
    auto plant = [&]() -> int { for (int i = kRegMaxSlots - 1; i >= 0; --i) if (LinkShm::loadAcquire (&slots[i].inUse) == 0) { std::memset (&slots[i], 0, sizeof (RegistrySlot)); std::strncpy (slots[i].displayName, "bass 2", 39); std::strncpy (slots[i].audioFile, ("audio_" + U + ".bin").toRawUTF8(), 47); std::strncpy (slots[i].instanceUid, U.toRawUTF8(), 10); slots[i].sampleRate = 48000.0f; slots[i].numChannels = 2; LinkShm::storeRelease (&slots[i].heartbeat, 7u); LinkShm::storeRelease (&slots[i].inUse, 1u); return i; } return -1; };
    auto writeSide = [&]() { LinkShm::RackSidecar rc; rc.valid = true; rc.uid = U; rc.name = "bass 2"; rc.revision = 1; rc.publisherPid = deadPid > 0 ? deadPid : (int) getpid(); rc.hostPid = rc.publisherPid; LinkShm::writeRackSidecar (dir, rc); };
    writeSide();
    { const auto rc = LinkShm::readRackSidecar (dir, U); std::printf ("== STORM: %d incarnations, ~%d ms each, seeded uid %s; predecessor's sidecar reads valid=%d publisherPid=%d (%s) ==\n", cycles, lifeMs, U.toRawUTF8(), (int) rc.valid, rc.publisherPid, (rc.publisherPid > 0 && ::kill ((pid_t) rc.publisherPid, 0) == 0) ? "ALIVE" : "DEAD"); }
    // Each incarnation: restored from the chunk, lives ~lifeMs, then is DESTROYED
    // (Pro Tools tore it down). What Pro Tools left behind - the predecessor's
    // slot still claimed with a frozen heartbeat, and its sidecar - is
    // re-planted after each death, because the destructor here releases the
    // slot and deletes the files that a killed process would have left.
    std::set<juce::String> burned; int adoptions = 0, unregistered = 0, sidecarOverwrittenByNewcomer = 0;
    const auto t0 = juce::Time::currentTimeMillis();
    for (int c = 0; c < cycles; ++c)
    {
        bool ghost = false; for (int i = 0; i < kRegMaxSlots; ++i) if (LinkShm::loadAcquire (&slots[i].inUse) && juce::String::fromUTF8 (slots[i].instanceUid) == U) ghost = true;
        if (! ghost) plant();
        auto inc = std::make_unique<LinkProcessor>(); inc->prepareToPlay (48000.0, 512);
        inc->setStateInformation (chunk.toRawUTF8(), (int) chunk.getNumBytesAsUTF8());
        pump (lifeMs / 20);
        const juce::String myUid = inc->getInstanceUidForTest();
        if (inc->diag.slotIdx >= 0 && myUid == U) ++adoptions;
        else if (inc->diag.slotIdx >= 0) burned.insert (myUid);
        else ++unregistered;
        { const auto rc = LinkShm::readRackSidecar (dir, U); if (rc.valid && rc.publisherPid == (int) getpid() && deadPid > 0) ++sidecarOverwrittenByNewcomer; }
        drain(); inc.reset(); drain();          // torn down; a killed process would not release - re-plant what it left
        if (adoptions > 0 || true) { bool g2 = false; for (int i = 0; i < kRegMaxSlots; ++i) if (LinkShm::loadAcquire (&slots[i].inUse) && juce::String::fromUTF8 (slots[i].instanceUid) == U) g2 = true; if (! g2) plant(); }
        if (deadPid > 0) writeSide();          // a dead predecessor's sidecar stays as it was
    }
    const auto stormMs = juce::Time::currentTimeMillis() - t0;
    auto survivor = std::make_unique<LinkProcessor>(); survivor->prepareToPlay (48000.0, 512);
    survivor->setStateInformation (chunk.toRawUTF8(), (int) chunk.getNumBytesAsUTF8());
    const auto s0 = juce::Time::currentTimeMillis(); juce::int64 regAt = -1;
    for (int t = 0; t < 400 && regAt < 0; ++t) { pump (1); if (survivor->diag.slotIdx >= 0) regAt = juce::Time::currentTimeMillis() - s0; }
    std::printf ("  storm ran %lld ms: identities burned (re-minted AND claimed) %d, adoptions during the storm %d, incarnations that died unregistered %d\n", (long long) stormMs, (int) burned.size(), adoptions, unregistered);
    std::printf ("  orphans a killed process would leave = one sidecar + one ring per burned identity = %d + %d\n", (int) burned.size(), (int) burned.size());
    if (deadPid > 0) std::printf ("  the predecessor's DEAD-pid sidecar was overwritten by a newcomer's own live pid before its claim in %d of %d cycles\n", sidecarOverwrittenByNewcomer, cycles);
    std::printf ("  survivor registered %lld ms after the storm (slot %d, uid %s) -> %s\n", (long long) regAt, survivor->diag.slotIdx, survivor->getInstanceUidForTest().toRawUTF8(), survivor->getInstanceUidForTest() == U ? "ADOPTED the seed uid" : (regAt < 0 ? "STILL UNREGISTERED after 8 s" : "re-minted"));
    drain(); survivor.reset(); drain();
    for (int i = 0; i < kRegMaxSlots; ++i) if (LinkShm::loadAcquire (&slots[i].inUse) && juce::String::fromUTF8 (slots[i].instanceUid) == U) LinkShm::releaseSlot (reg, i);
    juce::File (LinkShm::rackSidecarPath (dir, U)).deleteFile();
    return (int) burned.size();
}

// L5/L6/L7 (6 Sep 2026, "third and beyond"): the invariant is that seeded
// names survive ONLY when the instance genuinely continues the chunk's identity.
static int l5l6l7()
{
    int err = 0; const auto dir = LinkShm::resolveDir (err);
    int fd = -1, rerr = 0; void* reg = LinkShm::openRegistry (dir, fd, rerr);
    int bad = 0;
    std::printf ("== L5: a chunk whose uid is held by NO slot (its author is gone), seeded into a fresh instance ==\n");
    {
        juce::MemoryBlock chunk; juce::String authorUid;
        { auto z = std::make_unique<LinkProcessor>(); z->linkName = "Vox"; z->prepareToPlay (48000.0, 512); nameFromHost (*z, "Track Z"); z->updateShmState(); pump (40); z->getStateInformation (chunk); authorUid = z->getInstanceUidForTest(); drain(); z.reset(); drain(); }
        const auto legStart = juce::Time::getCurrentTime();
        auto n = std::make_unique<LinkProcessor>(); n->prepareToPlay (48000.0, 512); n->setStateInformation (chunk.getData(), (int) chunk.getSize()); pump (120);
        n->updateShmState(); n->updateShmState(); pump (40);   // re-publishes (a rename, a host name) must NOT re-mint again
        const auto nUid = slotUid (reg, n->diag.slotIdx);
        int transientSidecars = 0;   // sidecars written during this leg under a uid that is neither the author's nor the newcomer's
        for (auto& f : juce::File (dir).findChildFiles (juce::File::findFiles, false, "rack-*.json"))
            if (f.getLastModificationTime() >= legStart) { const auto u = f.getFileNameWithoutExtension().fromFirstOccurrenceOf ("rack-", false, false); if (u != authorUid && u != nUid && u != n->getInstanceUidForTest()) ++transientSidecars; }
        std::printf ("  author uid %s (gone)   newcomer: slot %d uid %s (instance uid %s) typed \"%s\" host \"%s\" published \"%s\"   transient sidecars: %d\n", authorUid.toRawUTF8(), n->diag.slotIdx, nUid.toRawUTF8(), n->getInstanceUidForTest().toRawUTF8(), n->linkName.toRawUTF8(), n->getHostTrackName().toRawUTF8(), slotName (reg, n->diag.slotIdx).toRawUTF8(), transientSidecars);
        const bool ok = n->diag.slotIdx >= 0 && nUid != authorUid && nUid == n->getInstanceUidForTest() && n->linkName.isEmpty() && n->getHostTrackName().isEmpty() && transientSidecars == 0;
        std::printf ("  -> %s (as ruled: mints its own uid, publishes neither seeded name)\n", ok ? "PASS" : "FAIL"); bad |= ok ? 0 : 1;
        drain(); n.reset(); drain();
    }
    std::printf ("== L6: FIVE fresh inserts in sequence, each seeded with the PREVIOUS instance's current chunk ==\n");
    {
        std::vector<std::unique_ptr<LinkProcessor>> v; std::set<juce::String> uids, names;
        v.push_back (std::make_unique<LinkProcessor>()); v.back()->linkName = "Vox"; v.back()->prepareToPlay (48000.0, 512); nameFromHost (*v.back(), "Track 1"); v.back()->updateShmState(); pump (40);
        for (int i = 1; i < 5; ++i)
        {
            juce::MemoryBlock chunk; v.back()->getStateInformation (chunk);
            v.push_back (std::make_unique<LinkProcessor>()); v.back()->prepareToPlay (48000.0, 512); v.back()->setStateInformation (chunk.getData(), (int) chunk.getSize()); pump (120);
        }
        for (auto& l : v) { const auto u = slotUid (reg, l->diag.slotIdx), nm = slotName (reg, l->diag.slotIdx); uids.insert (u); names.insert (nm + "|" + u);
                            std::printf ("  insert: slot %d uid %s published \"%s\"\n", l->diag.slotIdx, u.toRawUTF8(), nm.toRawUTF8()); }
        std::set<juce::String> pubNames; for (auto& l : v) pubNames.insert (slotName (reg, l->diag.slotIdx));
        // five identities; the first keeps "Vox"; every later one publishes EMPTY (numbered by the main plugin) - so no two published names collide except empties, which the main plugin numbers
        int nonEmptyDupes = 0; { std::map<juce::String,int> c; for (auto& l : v) { auto nm = slotName (reg, l->diag.slotIdx); if (nm.isNotEmpty()) ++c[nm]; } for (auto& kv : c) if (kv.second > 1) nonEmptyDupes += kv.second - 1; }
        const bool ok = uids.size() == 5 && ! uids.count (juce::String()) && nonEmptyDupes == 0;
        std::printf ("  -> %s (five distinct uids: %d; duplicated non-empty names: %d)\n", ok ? "PASS" : "FAIL", (int) uids.size(), nonEmptyDupes); bad |= ok ? 0 : 2;
        drain(); v.clear(); drain();
    }
    std::printf ("== L7: AdoptGhost and a plain restore still keep their names (uid unchanged) ==\n");
    { const int g = ghostLegs(); (void) g; }
    return bad;
}

int main (int argc, char** argv)
{
    echojay::requireIsolationOrDie ("link_two_same_name_test.cpp");
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI init;
    if (argc > 1 && juce::String (argv[1]) == "race20") return race20();
    if (argc > 1 && juce::String (argv[1]) == "l567")   return l5l6l7();
    if (argc > 1 && juce::String (argv[1]) == "l6x20")  { int pass = 0; for (int r = 0; r < 20; ++r) { int rr = l5l6l7(); if ((rr & 2) == 0) ++pass; } std::printf ("L6 x20: %d/20\n", pass); return pass == 20 ? 0 : 1; }
    if (argc > 1 && juce::String (argv[1]) == "storm")  { storm (30, 1000, 0); storm (5, 1000, 999999); return 0; }
    const int ctl  = leg ("POSITIVE CONTROL, two different names", "VoxA", "VoxB", true);
    const int same = leg ("THE DEFECT, the same typed name",      "Vox",  "Vox",  true);
    const int clone = cloneLeg();
    const int burst = burstLeg();
    const int ghost = ghostLegs();
    const int seeded = seededLegs();
    std::printf ("seeded legs: L1 %s   L2 %s   L3 %s   L4 %s\n", (seeded & 1) ? "FAIL" : "PASS", (seeded & 2) ? "FAIL" : "PASS", (seeded & 4) ? "FAIL" : "PASS", (seeded & 8) ? "FAIL" : "PASS");
    std::printf ("ghost legs: fresh-beside-ghost %s   restore-adopts %s\n", (ghost & 1) ? "FAIL" : "PASS", (ghost & 2) ? "FAIL" : "PASS");
    std::printf ("control: %s   same-name: %s   clone: %s   burst: %s\n", ctl == 0 ? "PASS" : "FAIL", same == 0 ? "PASS (two files)" : "FAIL (one file shared by two Links)",
                 clone == 0 ? "PASS" : (clone == 2 ? "FAIL (one ring file)" : "FAIL (one slot / one uid)"), burst == 0 ? "PASS" : "FAIL (one slot)");
    return ctl != 0 ? 2 : (same | clone | burst | ghost | (seeded ? 16 : 0));
}
