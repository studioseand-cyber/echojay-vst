#pragma once

#include <JuceHeader.h>
#include "LinkShm.h"
#include <cstddef>   // offsetof - the layout freeze
#include <cstdint>

// ===========================================================================
// THE VALUE RING (remote control stage 3, 10 Oct 2026)
// ===========================================================================
//
// WHY A SECOND CHANNEL AT ALL. The command channel is a FILE, polled, and that is
// right for "apply this edit" and hopeless for a knob drag: the lease gate polls on
// the Link's timer with a 3.5 s floor before the rack arm accepts a command, and the
// apply ack is waited for with a 5 s budget. Change-to-audible under ~30 ms cannot
// come from that channel. docs/LINK_REMOTE_CONTROL_PLAN.md section 3 sets the rule
// that decides which channel a command belongs on, and it is one question:
//
//     DOES THE NEXT ONE SUPERSEDE THIS ONE?
//
// Yes -> the ring. No -> a file. A drag has no meaningful ack (the next frame
// supersedes it); an `add` has nothing but its ack. Two channels with different
// jobs: a ring for values, a file for decisions.
//
// MODELLED ON LinkShm, deliberately and in detail, because that layout is already
// proven byte-for-byte across two processes by shm_layout_guard:
//   - plain uint32_t members, so the byte layout is identical on both platforms;
//   - atomics OVERLAID rather than declared (LinkShm's own note on why: a
//     std::atomic member's size and alignment are not guaranteed to match the
//     plain type's, and this block is mapped by two processes built separately);
//   - every offset and the total size pinned by static_assert, so a field added in
//     the middle is a compile error and not a silent disagreement between a V2 and
//     a Link of different vintages.
//
// SINGLE PRODUCER, SINGLE CONSUMER: the V2 writes, the Link drains in processBlock.
// No allocation, no locks, no logging anywhere near the drain.

namespace echojay::cmdring
{

static constexpr uint32_t kMagic         = 0xEC4A3003u;
static constexpr uint32_t kLayoutVersion = 1u;
static constexpr uint32_t kCapacity      = 256u;      // power of two, masked - never modulo
static_assert ((kCapacity & (kCapacity - 1u)) == 0u, "capacity must be a power of two");

/** The command, as an enum and NEVER a string: strings do not go in the ring.
    The numbers are the WIRE and may never be reordered - a V2 and a Link are built
    separately and can be of different vintages, so a renumbering is a silent
    mis-application, not a build error. Append only. */
enum Kind : uint32_t
{
    kNone       = 0,
    kSetParam   = 1,   // one parameter on one slot's device, in the device's own units (HUMAN GESTURE only)
    kSlotIn     = 2,   // the slot's pre-trim
    kSlotOut    = 3,   // the slot's output gain
    kSlotWet    = 4,   // the slot's wet/dry, 0..100
    kMasterWet  = 5,   // the rack's master wet, 0..100
    kPreGain    = 6,   // the Link's pre-chain gain
    kLinkGain   = 7    // the Link's own output gain
};

/** 0 begin | 1 stream | 2 end. The boundaries are what make ONE UNDO STEP PER
    GESTURE possible, which is why the coalescer may never discard them. */
enum Phase : uint32_t { kBegin = 0, kStream = 1, kEnd = 2 };

struct alignas(64) CmdFrame
{
    uint32_t seq      = 0;   // strictly increasing, LinkShm::nextCtrlSeq's discipline
    uint32_t kind     = kNone;
    uint32_t phase    = kStream;
    uint32_t gesture  = 0;   // every frame of one drag shares it
    int32_t  slot     = -1;  // -1 where the command has no slot
    uint32_t paramId  = 0;   // index into the slot's published schema-id list (3.2)
    double   value    = 0.0;
    uint32_t flags    = 0;   // low 16 bits: the slot's structureRevision (3.2)
    uint32_t reserved[3] { 0, 0, 0 };
};

static_assert (sizeof (CmdFrame) == 64, "CmdFrame is a 64-byte wire struct");
static_assert (offsetof (CmdFrame, seq)      ==  0, "");
static_assert (offsetof (CmdFrame, kind)     ==  4, "");
static_assert (offsetof (CmdFrame, phase)    ==  8, "");
static_assert (offsetof (CmdFrame, gesture)  == 12, "");
static_assert (offsetof (CmdFrame, slot)     == 16, "");
static_assert (offsetof (CmdFrame, paramId)  == 20, "");
static_assert (offsetof (CmdFrame, value)    == 24, "");
static_assert (offsetof (CmdFrame, flags)    == 32, "");
static_assert (offsetof (CmdFrame, reserved) == 36, "");

struct CmdRing
{
    uint32_t magic         = 0;
    uint32_t layoutVersion = 0;
    uint32_t capacity      = 0;
    uint32_t writeIdx      = 0;   // producer only
    uint32_t readIdx       = 0;   // consumer only
    uint32_t overflowCount = 0;   // producer increments when it laps the reader
    uint32_t pad[10] { };         // frames start 64-byte aligned, like LinkShm's header does it
    CmdFrame frames[kCapacity];
};

static_assert (offsetof (CmdRing, magic)         ==  0, "");
static_assert (offsetof (CmdRing, layoutVersion) ==  4, "");
static_assert (offsetof (CmdRing, capacity)      ==  8, "");
static_assert (offsetof (CmdRing, writeIdx)      == 12, "");
static_assert (offsetof (CmdRing, readIdx)       == 16, "");
static_assert (offsetof (CmdRing, overflowCount) == 20, "");
static_assert (offsetof (CmdRing, frames)        == 64, "the frames are 64-byte aligned");
static_assert (sizeof (CmdRing) == 64 + 64 * kCapacity, "");

/** Lay a fresh ring down in a mapped block. Producer side, once per rack. */
inline void initRing (CmdRing* r)
{
    if (r == nullptr) return;
    r->layoutVersion = kLayoutVersion;
    r->capacity      = kCapacity;
    LinkShm::storeRelease (&r->writeIdx, 0u);
    LinkShm::storeRelease (&r->readIdx, 0u);
    LinkShm::storeRelease (&r->overflowCount, 0u);
    // The magic LAST, and released: a consumer that maps this block mid-init must
    // either see a ring that is entirely ready or one it refuses. LinkShm does the
    // same thing for the same reason.
    LinkShm::storeRelease (&r->magic, kMagic);
}

/** Does this block hold a ring THIS build understands? Checked by every reader
    before it touches an index, because a mapped block is shared memory written by
    another process and a wrong layoutVersion read as frames is a segfault. */
inline bool ringUsable (const CmdRing* r)
{
    return r != nullptr
        && LinkShm::loadAcquire (&r->magic) == kMagic
        && LinkShm::loadAcquire (&r->layoutVersion) == kLayoutVersion
        && LinkShm::loadAcquire (&r->capacity) == kCapacity;
}

/** PRODUCER (the V2). Writes one frame. Returns false only if the ring is unusable.
    Lapping the reader is NOT a failure: the newest frame is the one that matters, so
    an overflow is counted and reported, never a reason to drop the newest value.
    See the overflow note below - a dropped `end` is the one case that is not
    harmless, and the control channel covers it. */
inline bool push (CmdRing* r, const CmdFrame& f)
{
    if (! ringUsable (r)) return false;
    const uint32_t w = LinkShm::loadRelaxed (&r->writeIdx);
    const uint32_t rd = LinkShm::loadAcquire (&r->readIdx);
    if (w - rd >= kCapacity)
        LinkShm::storeRelease (&r->overflowCount, LinkShm::loadRelaxed (&r->overflowCount) + 1u);
    r->frames[w & (kCapacity - 1u)] = f;
    LinkShm::storeRelease (&r->writeIdx, w + 1u);   // the frame is visible BEFORE the index that publishes it
    return true;
}

/** How many frames are waiting. Consumer side. */
inline uint32_t pending (const CmdRing* r)
{
    if (! ringUsable (r)) return 0;
    return LinkShm::loadAcquire (&r->writeIdx) - LinkShm::loadAcquire (&r->readIdx);
}

/** The result of one drain: the frames that SURVIVED coalescing, newest-wins.
    A fixed array, because this is used on the audio thread and nothing there may
    allocate. kMaxKept is the number of DISTINCT controls one buffer can carry, not
    the number of frames - a 400-frame drag collapses to one entry. */
static constexpr int kMaxKept = 64;
struct Drain
{
    CmdFrame kept[kMaxKept];
    int   count     = 0;
    int   discarded = 0;   // coalesced away (newest won) - information, not loss
    int   dropped   = 0;   // refused: a structureRevision mismatch, or kept[] full
    uint32_t overflow = 0; // the producer's count, as it stood at this drain
};

/** CONSUMER (the Link, in processBlock). Drains everything available and keeps, per
    (kind, slot, paramId), ONLY THE NEWEST frame - except that a `begin` or an `end`
    frame is never discarded, because the gesture boundaries are what make one undo
    step per gesture possible. A drag that writes 400 frames between two buffers
    costs the audio thread one write per distinct control, not 400.

    `liveStructureRevision` is the rack's own revision. A frame whose low-16 flags
    disagree is DISCARDED WITH A COUNT and never applied: a V2 holding a stale
    sidecar cannot address a parameter the Link does not have, but it CAN address
    the wrong one, which is the same class of mistake baseSlots guards on the chain
    channel and gets the same treatment. Pass 0 to accept any revision (a producer
    that does not stamp one yet).

    No allocation, no locks, no logging. */
inline Drain drain (CmdRing* r, uint32_t liveStructureRevision)
{
    Drain out;
    if (! ringUsable (r)) return out;
    const uint32_t w = LinkShm::loadAcquire (&r->writeIdx);
    uint32_t rd = LinkShm::loadRelaxed (&r->readIdx);
    // A producer that lapped us has overwritten frames we never read. Start from the
    // oldest frame still actually present rather than from a read index that points
    // at a slot the producer has since reused - reading that would hand the audio
    // thread a frame from the FUTURE mixed with ones from the past.
    if (w - rd > kCapacity) rd = w - kCapacity;
    for (; rd != w; ++rd)
    {
        const CmdFrame f = r->frames[rd & (kCapacity - 1u)];
        if (f.kind == kNone) continue;
        if (liveStructureRevision != 0u)
        {
            const uint32_t stamped = f.flags & 0xFFFFu;
            if (stamped != 0u && stamped != (liveStructureRevision & 0xFFFFu)) { ++out.dropped; continue; }
        }
        const bool boundary = (f.phase == kBegin || f.phase == kEnd);
        if (! boundary)
        {
            // Supersede an earlier STREAM frame for the same control. A boundary
            // already kept is left alone: it is the gesture's edge, not a value.
            bool replaced = false;
            for (int i = 0; i < out.count; ++i)
                if (out.kept[i].phase == kStream
                    && out.kept[i].kind == f.kind
                    && out.kept[i].slot == f.slot
                    && out.kept[i].paramId == f.paramId)
                { out.kept[i] = f; ++out.discarded; replaced = true; break; }
            if (replaced) continue;
        }
        if (out.count < kMaxKept) out.kept[out.count++] = f;
        else ++out.dropped;
    }
    LinkShm::storeRelease (&r->readIdx, w);
    out.overflow = LinkShm::loadAcquire (&r->overflowCount);
    return out;
}

// ---- MAPPING ONE RACK'S RING ----------------------------------------------------
// The same directory, the same openFileMapped and the same naming shape LinkShm uses
// for its lease and sidecar files, so there is one answer to "where does a rack's
// shared state live" and not two.

/** `cmd-ring-<uid>.bin` beside the rack's lease and sidecar. */
inline juce::String ringPath (const juce::String& dir, const juce::String& uid)
{
    return dir + "cmd-ring-" + uid + ".bin";
}

/** PRODUCER side (the V2): map a rack's ring, creating and laying it down if this is
    the first time. Returns nullptr on any failure, with errno out, and never throws:
    a rack whose ring will not map must fall back to the file channel, not die.
    `laidDown` says whether this call initialised the block, so a caller can log a
    first-open once instead of every open. */
inline CmdRing* openRingRW (const juce::String& dir, const juce::String& uid,
                            int& fd_out, int& errno_out, bool& laidDown)
{
    laidDown = false;
    void* m = LinkShm::openFileMapped (ringPath (dir, uid), sizeof (CmdRing),
                                       /*readOnly*/ false, fd_out, errno_out);
    if (m == nullptr) return nullptr;
    auto* r = reinterpret_cast<CmdRing*> (m);
    // A block that is already a ring THIS build understands is adopted as it stands -
    // re-initialising it would reset the indices under a live consumer and replay
    // frames it has already applied. Anything else (a fresh zeroed file, or a ring
    // from a layout this build does not know) is laid down afresh.
    if (! ringUsable (r)) { initRing (r); laidDown = true; }
    return r;
}

/** CONSUMER side (the Link): map a rack's ring READ-WRITE, because the consumer owns
    readIdx and must publish it. It does NOT lay a ring down: a Link that finds no
    usable ring simply has no value channel yet and keeps using the file channel, and
    laying one down here would race the producer's own init. */
inline CmdRing* openRingForDrain (const juce::String& dir, const juce::String& uid,
                                  int& fd_out, int& errno_out)
{
    void* m = LinkShm::openFileMapped (ringPath (dir, uid), sizeof (CmdRing),
                                       /*readOnly*/ false, fd_out, errno_out);
    if (m == nullptr) return nullptr;
    auto* r = reinterpret_cast<CmdRing*> (m);
    return ringUsable (r) ? r : nullptr;
}

} // namespace echojay::cmdring
