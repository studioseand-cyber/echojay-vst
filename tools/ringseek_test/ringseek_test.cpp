/*
    Link audio ring: seek + position-stamp self-test (stage 0 of remote
    editing).

    WHY THIS EXISTS. The ring had no resync: ringConsume advances readIdx by
    at most numFrames, nothing ever jumps it forward, and consumes are
    skipped on a tryEnter failure, so backlog was monotonic and latency
    ratcheted toward the ring's 1.49s capacity with no recovery. Stage 0
    adds a seek and a {ring index, host sample position} stamp; this suite
    holds both, plus the claim that matters most: CAPTURE IS UNCHANGED. The
    capture pattern (consume everything, never seek) must deliver every
    sample, bit-exact and in order, exactly as before.

    The ring functions are header-inline in LinkShm.h, so this TU's
    instantiation IS the shipping arithmetic: there is no separate copy to
    drift. The buffer under test is a plain heap allocation initialised the
    way openRingProducer initialises its mapping; no shared memory and no
    host are involved, which is what makes stage 0 testable at all.

    WHAT IT CHECKS:
      - capture pattern: N blocks produced, consumed in mismatched block
        sizes, every sample arrives once, in order, bit-exact
      - backlog is monotonic when consumes are skipped (the disease), and
        ringSeekForward is the cure: seeks to an exact target, drops the
        OLDEST frames, returns the count, never moves past writeIdx, never
        backward, no-op at/under target
      - stamps: a zeroed header reads ABSENT (false), not position 0; a
        published stamp of position 0 reads TRUE with 0, which is the
        absence-vs-zero distinction the header comment promises; the
        index->position mapping holds across the uint32 wrap; a mid-write
        (odd) seq reads as no-data rather than as torn fields
*/

#include "LinkShm.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int failures = 0;
static void check (bool ok, const char* what)
{
    if (!ok) { std::printf ("  FAIL: %s\n", what); ++failures; }
}
static void checkEq (long long got, long long want, const char* what)
{
    if (got != want)
    { std::printf ("  FAIL: %s (got %lld, want %lld)\n", what, got, want); ++failures; }
}

// A ring the way openRingProducer makes one, minus the file: zeroed, header
// fields set, magic last. Callers that want the OLD-WRITER state simply skip
// init and keep the zeroed block.
static void* makeRing (bool initHeader)
{
    void* map = std::calloc (1, kLinkShmSize);
    if (initHeader)
    {
        auto* hdr = LinkShm::ringHeader (map);
        hdr->version        = kLinkVersion;
        hdr->sampleRate     = 44100.0f;
        hdr->numChannels    = 2;
        hdr->capacityFrames = kLinkRingFrames;
        LinkShm::storeRelease (&hdr->magic, kLinkMagic);
    }
    return map;
}

static void produceRamp (void* map, int start, int n)
{
    // Sample value == its global index, so order and identity are the same
    // check. L carries the index, R carries the negative, so a channel swap
    // cannot pass either.
    std::vector<float> L ((size_t) n), R ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        L[(size_t) i] = (float) (start + i);
        R[(size_t) i] = (float) -(start + i);
    }
    const float* ch[2] = { L.data(), R.data() };
    LinkShm::ringProduce (map, ch, 2, n);
}

static void testCapturePattern()
{
    std::printf ("capture pattern: every sample, in order, bit-exact\n");
    void* map = makeRing (true);

    // Produce in 512s, consume in 480s: mismatched sizes so the test cannot
    // pass by accident of alignment. 100 blocks = 51200 samples.
    int produced = 0, consumed = 0;
    std::vector<float> outL (4096), outR (4096);
    bool ordered = true;
    while (consumed < 51200)
    {
        if (produced < 51200) { produceRamp (map, produced, 512); produced += 512; }
        const uint32_t n = LinkShm::ringConsume (map, outL.data(), outR.data(), 480);
        for (uint32_t i = 0; i < n; ++i)
            if (outL[i] != (float) (consumed + (int) i)
                || outR[i] != (float) -(consumed + (int) i))
                ordered = false;
        consumed += (int) n;
    }
    check (ordered, "every sample arrived once, in order, on both channels");
    checkEq (consumed, 51200, "no sample lost and none invented");
    std::free (map);
}

static void testBacklogAndSeek()
{
    std::printf ("seek: the monotonic backlog and its cure\n");
    void* map = makeRing (true);
    auto* hdr = LinkShm::ringHeader (map);

    // THE DISEASE, reproduced: produce 10 blocks, consume only 5. Backlog
    // sticks at 5 blocks and ordinary consumption can never shrink it below
    // the per-call amount again.
    for (int b = 0; b < 10; ++b) produceRamp (map, b * 512, 512);
    std::vector<float> outL (4096), outR (4096);
    for (int b = 0; b < 5; ++b) LinkShm::ringConsume (map, outL.data(), outR.data(), 512);
    checkEq ((long long) (hdr->writeIdx - hdr->readIdx), 5 * 512, "skipped consumes accumulate");

    // THE CURE: seek to a 512-frame cushion. 2048 dropped, oldest first.
    const uint32_t dropped = LinkShm::ringSeekForward (map, 512);
    checkEq (dropped, 4 * 512, "seek reports exactly what it dropped");
    checkEq ((long long) (hdr->writeIdx - hdr->readIdx), 512, "backlog lands ON the target");
    // The next sample proves the drop took the OLDEST frames: 10 blocks
    // produced = samples 0..5119; a 512 cushion means 4608 is next.
    LinkShm::ringConsume (map, outL.data(), outR.data(), 1);
    checkEq ((long long) outL[0], 4608, "the oldest frames were dropped, not the newest");

    // No-op at/under target, and never past writeIdx.
    checkEq (LinkShm::ringSeekForward (map, 511), 0, "at/under target is a no-op");
    checkEq (LinkShm::ringSeekForward (map, 1u << 30), 0, "a huge target never moves readIdx");
    const uint32_t rBefore = hdr->readIdx;
    LinkShm::ringSeekForward (map, 0);
    check (hdr->readIdx == hdr->writeIdx && hdr->readIdx >= rBefore,
           "seek-to-latest stops AT writeIdx and never goes backward");
    std::free (map);
}

static void testStamps()
{
    std::printf ("stamps: absence vs zero, mapping, wrap, torn\n");

    // OLD WRITER: an all-zero header region must read ABSENT, not pos 0.
    void* oldMap = makeRing (false);
    uint32_t sw = 999; int64_t sp = 999;
    check (! LinkShm::ringStampRead (oldMap, sw, sp),
           "a zeroed (old-writer) stamp reads ABSENT");
    std::free (oldMap);

    void* map = makeRing (true);
    check (! LinkShm::ringStampRead (map, sw, sp),
           "a new ring with no publish yet also reads ABSENT");

    // THE ADVERSARIAL VALUE: host position 0 is real data at the timeline
    // start. After a publish of 0 the read must return TRUE with 0.
    LinkShm::ringStampPublish (map, 0, 0);
    check (LinkShm::ringStampRead (map, sw, sp), "a published stamp reads back");
    checkEq (sp, 0, "hostSamplePos 0 is DATA, not absence");
    checkEq (sw, 0, "the paired ring index reads back");

    // Mapping: frame f sits at stampHostPos + (int32)(f - stampWriteIdx).
    LinkShm::ringStampPublish (map, 100, 44100);
    LinkShm::ringStampRead (map, sw, sp);
    checkEq (sp + (int32_t) (137u - sw), 44137, "index->position mapping");

    // Wrap: a stamp published just under UINT32_MAX still maps frames past
    // the wrap through the signed diff.
    const uint32_t nearWrap = 0xFFFFFFF0u;
    LinkShm::ringStampPublish (map, nearWrap, 1000000);
    LinkShm::ringStampRead (map, sw, sp);
    checkEq (sp + (int32_t) ((nearWrap + 0x20u) - sw), 1000032,
             "mapping survives the uint32 wrap");

    // Torn: an odd seq is a write in progress and must read as no-data.
    auto* hdr = LinkShm::ringHeader (map);
    hdr->stampSeq |= 1u;
    check (! LinkShm::ringStampRead (map, sw, sp), "an odd (mid-write) seq reads as no-data");
    std::free (map);
}

static void testLeaseGate()
{
    // Stage 1's lease decision, pure. The one behaviour that must never
    // regress: an EXPIRED lease id can never re-engage. The main plugin that
    // froze for four seconds and thawed still renews the old id; the Link
    // already restored itself and owns the audio again (the Link wins), so
    // that id is dead and only a NEW session may engage.
    std::printf ("lease gate: engage, hold, expire, the dead id, release\n");
    using G = LinkShm::LeaseGate;
    G g;

    checkEq (g.poll ({}, 0, 1.0e12), G::None, "no file, no lease, nothing to do");
    checkEq (g.poll ("A", 2, 100.0), G::Engage, "a fresh lease engages");
    checkEq (g.activeSlot1, 2, "the engaged slot is carried");
    checkEq (g.poll ("A", 2, 900.0), G::Hold, "renewals hold");
    checkEq (g.poll ("A", 2, 3500.0), G::Expire, "stale renewals expire");
    checkEq (g.poll ("A", 2, 100.0), G::None,
             "THE DEAD ID NEVER RE-ENGAGES, even fresh again");
    checkEq (g.poll ("B", 3, 100.0), G::Engage, "a new session id engages");
    checkEq (g.poll ({}, 0, 1.0e12), G::Release, "a deleted file releases cleanly");
    checkEq (g.poll ("B", 3, 100.0), G::Engage,
             "a RELEASED id may engage again (only expiry kills an id)");
    // A different id appearing while engaged = the old session died without
    // cleanup and a new one started: restore FIRST, engage on the next poll.
    checkEq (g.poll ("C", 1, 100.0), G::Expire, "a superseding id expires the old lease");
    checkEq (g.poll ("C", 1, 100.0), G::Engage, "and engages on the NEXT poll");
    // A stale file engages nothing: its writer is already gone.
    G g2;
    checkEq (g2.poll ("X", 1, 9000.0), G::None, "a stale file never engages");
}

int main()
{
    testCapturePattern();
    testBacklogAndSeek();
    testStamps();
    testLeaseGate();
    std::printf (failures == 0 ? "ringseek_test: PASS\n"
                               : "ringseek_test: FAIL (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}
