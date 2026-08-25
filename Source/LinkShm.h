#pragma once
// =============================================================================
//  EchoJay shared-memory audio transport  (stage 2 — file-backed mmap)
//
//  Backing store changed from POSIX shm_open (blocked by AU sandbox) to
//  ordinary file mmap under a directory both sandboxed AUs can reach.
//
//  Directory resolution (message thread, off audio thread):
//    1. ~/Library/Application Support/EchoJay/link/
//    2. $TMPDIR/echojay_link/          (always accessible in the same process)
//  Both AUv2 plugins run in-process in Logic, so they share TMPDIR.
//
//  Files:
//    registry.bin       — fixed-size registry (same RegistrySlot layout)
//    audio_<name>.bin   — per-Link audio ring buffer (same SPSC layout)
//
//  Everything else (struct layouts, atomics, ring logic, slot lifecycle)
//  is unchanged from stage 2.
// =============================================================================

#if JUCE_MAC || JUCE_LINUX
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <fcntl.h>
  #include <unistd.h>
  #include <cerrno>
  #define LINK_FILE_SUPPORTED 1
#else
  #define LINK_FILE_SUPPORTED 0
#endif

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <cstddef>   // offsetof (LinkMeterFrame layout freeze)
#include <cmath>     // log/exp (the EQ curve's log-spaced x axis)
#include <vector>
#include <JuceHeader.h>

// =============================================================================
//  Audio ring buffer structs  (layout unchanged)
// =============================================================================

static constexpr uint32_t kLinkMagic      = 0xEC4A1001u;
static constexpr uint32_t kLinkVersion    = 1u;
static constexpr uint32_t kLinkRingFrames = 65536u;   // must be power-of-2
static constexpr uint32_t kLinkMaxCh      = 2u;
static constexpr size_t   kLinkHdrSize    = 192u;
static constexpr size_t   kLinkDataSize   = (size_t)kLinkRingFrames * kLinkMaxCh * sizeof(float);
static constexpr size_t   kLinkShmSize    = kLinkHdrSize + kLinkDataSize;

struct alignas(64) LinkShmHeader
{
    uint32_t magic;
    uint32_t version;
    float    sampleRate;
    uint32_t numChannels;
    uint32_t capacityFrames;
    uint8_t  _pad0[44];

    alignas(64) uint32_t writeIdx;
    // ---- Position stamp (stage 0 of remote editing, 8 Aug 2026) -----------
    // {stampWriteIdx, stampHostPos} pairs a ring frame index with the host
    // sample position of the frame that will be written AT that index, so a
    // consumer can compute its exact timeline margin per block instead of
    // inferring it from backlog. Published by the producer under a seqlock
    // (stampSeq odd while writing, even when stable), CARVED FROM _pad1 so
    // sizeof and every existing offset are unchanged and the asserts below
    // prove it.
    //
    // ABSENCE IS DISTINGUISHABLE FROM ZERO, and it has to be, because a
    // hostSamplePos of 0 is a legitimate value at the start of a timeline.
    // An old-writer Link leaves this region zeroed, so stampSeq == 0. A new
    // writer's FIRST publish takes stampSeq 0 -> 1 -> 2, and it only ever
    // climbs, so stampSeq >= 2 (and even) is the only state in which the
    // fields carry data. stampSeq == 0 means NO DATA -- an old Link, or a
    // new Link whose host never supplied a playhead position -- and
    // ringStampRead returns false rather than fabricating a position. These
    // fields sit on the writeIdx cache line ON PURPOSE: only the producer
    // writes either.
    uint32_t stampSeq;         // 68  seqlock; 0 = never published (absent)
    uint32_t stampWriteIdx;    // 72  ring index the stamp pairs with
    uint32_t _stampPad;        // 76  keeps stampHostPos 8-aligned
    int64_t  stampHostPos;     // 80  host sample position of frame stampWriteIdx
    uint8_t  _pad1[40];        // 88..127

    alignas(64) uint32_t readIdx;
    uint8_t  _pad2[60];
};
static_assert(sizeof(LinkShmHeader) == 192, "");
static_assert(kLinkHdrSize == sizeof(LinkShmHeader), "");
// The layout freeze, field by field. The stamp was carved from pad space and
// these prove no existing offset moved; kLinkVersion stays 1 because the
// change is additive (and version is never exact-checked, but the sidecar's
// v-field lesson stands: additive means additive).
static_assert(offsetof(LinkShmHeader, magic)          ==   0, "");
static_assert(offsetof(LinkShmHeader, version)        ==   4, "");
static_assert(offsetof(LinkShmHeader, sampleRate)     ==   8, "");
static_assert(offsetof(LinkShmHeader, numChannels)    ==  12, "");
static_assert(offsetof(LinkShmHeader, capacityFrames) ==  16, "");
static_assert(offsetof(LinkShmHeader, writeIdx)       ==  64, "");
static_assert(offsetof(LinkShmHeader, stampSeq)       ==  68, "");
static_assert(offsetof(LinkShmHeader, stampWriteIdx)  ==  72, "");
static_assert(offsetof(LinkShmHeader, stampHostPos)   ==  80, "");
static_assert(offsetof(LinkShmHeader, readIdx)        == 128, "");

// =============================================================================
//  Registry structs  (layout unchanged)
// =============================================================================

static constexpr uint32_t kRegMagic      = 0xEC4A2002u;

/// Placement values as they cross the registry and the ctrl protocol:
/// 0 unset, 1 bus, 2 channel insert, 3 send return.
/// POST-FADER placements measure their real contribution to the mix, so
/// comparative claims about them are legitimate; pre-fader ones cannot be
/// compared because the channel fader is invisible to a plugin. ONE
/// predicate, consumed by both plugins, so no "is it bus, otherwise
/// channel" branch can quietly mis-sort a value added later.
inline bool placementIsPostFader(int p) { return p == 1 || p == 3; }
static constexpr int      kRegMaxSlots   = 16;
static constexpr int      kRegStaleCycles = 60;   // ~30 s at 2 Hz probing

// 128 bytes per slot (2 cache lines)
struct alignas(128) RegistrySlot
{
    uint32_t inUse;            //  4  atomic: 0=free, 1=registered
    char     displayName[40];  // 40  user-visible name, null-terminated
    char     audioFile  [48];  // 48  filename only, e.g. "audio_drums.bin"
    float    sampleRate;       //  4
    uint32_t numChannels;      //  4
    uint32_t heartbeat;        //  4  bumped by producer timer ~1 Hz
    uint32_t activeFlag;       //  4  1 = Active (capture/meter role on); carved
                               //     from _pad so existing field offsets and the
                               //     128-byte layout are unchanged
    char     instanceUid[12];  // 12  per-INSTANCE identity (v0.5.6) — commands,
                               //     acks and row identity key on this, never
                               //     the display name (unnamed/duplicate names
                               //     collided). Carved from _pad; old writers
                               //     leave it zeroed -> readers fall back.
    float    gainDb;           //  4  Link's built-in gain stage, dB (v0.5.7).
                               //     Mirrored here so the monitor shows it
                               //     regardless of Active. Carved from _pad;
                               //     old writers leave it 0.0 = unity (benign).
    uint8_t  placement;        //  1  Link placement (v0.6.0): 0=unset/unknown,
                               //     1=bus (post-fader), 2=insert (pre-fader),
                               //     3=send return (post-fader, v0.8.6).
                               //     Old writers leave 0 = unknown (treated as
                               //     pre-fader for level gating). Carved from _pad.
    uint8_t  dialCapable;      //  1  Link reads settings_structured and applies
                               //     it (v0.9.0). CAPABILITY, NOT VERSION, and
                               //     the fallback is INCAPABLE rather than
                               //     unknown: an old Link cannot announce
                               //     anything, so 0 must mean "do not send
                               //     controls" forever, never "ask the version".
                               //     Carved from _pad on the existing
                               //     convention -- old writers leave it zeroed,
                               //     readers fall back -- which is how uid,
                               //     gainDb and placement were each added.
                               //     WHOEVER CARVES THE NEXT BYTE: keep the
                               //     fallback the SAFE answer, not the
                               //     convenient one. A zero here withholds a
                               //     feature; a zero that meant "capable" would
                               //     hand payloads to binaries that drop them.
    uint8_t  _pad[2];          //  2  → total 128
};
static_assert(sizeof(RegistrySlot) == 128, "RegistrySlot must be 128 bytes");

struct alignas(64) RegistryHeader
{
    uint32_t magic;
    uint32_t version;
    uint32_t maxSlots;
    uint8_t  _pad[52];
};
static_assert(sizeof(RegistryHeader) == 64, "");

// -----------------------------------------------------------------------------
//  Per-Link meter frame (registry v2) — one per slot, appended after the slot
//  array. Published by the Link at ~10Hz while ACTIVE; seqlock'd (seq is odd
//  during a write). Readers detect staleness by seq not advancing — no
//  cross-process clocks needed.
// -----------------------------------------------------------------------------
struct alignas(64) LinkMeterFrame
{
    uint32_t seq = 0;              // seqlock; odd = write in progress
    float momentary   = -100.0f;   // LUFS
    float shortTerm   = -100.0f;   // LUFS
    float integrated  = -100.0f;   // LUFS
    float rmsL        = -100.0f;   // dBFS
    float rmsR        = -100.0f;
    float peakL       = -100.0f;
    float peakR       = -100.0f;
    float truePeakMax = -100.0f;   // dBTP, max of L/R
    float crest       = 0.0f;      // dB
    float correlation = 0.0f;      // -1..+1
    float width       = 0.0f;      // 0..1
    float bandRel[6]  = {};        // macroBand dB rel to band mean (sub..air)
    // CURRENT true peak (this frame), not the max-hold above: the overs
    // markers need per-slice truth — truePeakMax latches after one over and
    // painted a continuous coral line. Placed in the old pad space so all
    // prior field offsets are unchanged (old writers leave it 0 = benign).
    float truePeakCur = -100.0f;
    // Loudness-suite readouts (v0.5.4): LRA + short-term true peak (PSR =
    // shortTermTP - shortTerm, PLR = truePeakMax - integrated, computed
    // receiver-side exactly like the Meters tab). Appended in pad space —
    // prior offsets unchanged; old writers leave them 0.
    float lra         = 0.0f;      // LU
    float shortTermTP = -100.0f;   // dBTP
    // Audio liveness (v0.5.5): processBlock counter + publisher-side stale
    // flag. Logic stops calling processBlock on idle channels — the engine
    // freezes mid-song but heartbeats/publishes continue, so the receiver
    // cannot detect it. The PUBLISHER owns this truth.
    uint32_t audioBlocks = 0;      // processBlock call counter at publish time
    uint32_t audioStale  = 0;      // 1 = no audio blocks for ~1s
    // Fast-ballistics sample peak (v0.8.5): ~13.3 dB/s release (20 dB in
    // 1.5s), instant attack, for the mixer's Logic-style bar. The 3s-hold
    // peakL/peakR above stay: they feed the numbers view and the hold tick.
    // Appended in pad space; prior offsets unchanged (static_asserts below).
    float peakFastL = -100.0f;     // dBFS
    float peakFastR = -100.0f;
    // CROSS-VERSION GATE, per the standing spec rule: 0 legitimately means
    // "old writer, none of the fields above this line's group present".
    // Old Links memcpy their whole zero pad on every publish, so a recycled
    // slot can never serve a newer writer's stale mask. Every new field's
    // DISPLAY gates on its bit: an ungated dB field reading 0.0f would
    // render as a real measurement. The in-struct defaults (-100 / 0) are
    // what claimSlot's blank writes, so a claimed-but-never-published slot
    // reads as ABSENT VALUES through the ordinary value gates before the
    // mask is ever consulted.
    uint32_t fieldsMask = 0;
    // Detected key (KEY_DETECTOR_SPEC.md §9): the Link runs EedKeyEngine on
    // its channel and publishes the committed reading, so a main plugin on a
    // VOCAL can know the key of the MUSIC. Appended in pad space — prior
    // offsets unchanged (static_asserts below). keyRoot's -1 default matters:
    // an old writer memcpys its whole zero pad on every publish, and a zero
    // here would read as "C" — which is why every consumer must go through
    // frameHasKey() (mask bit AND a real root), never the raw fields.
    int16_t  keyRoot       = -1;      // 0..11 pitch class (C..B), -1 = none
    uint8_t  keyIsMinor    = 0;       // 0 = major, 1 = minor
    uint8_t  _keyPad       = 0;
    float    keyConfidence = 0.0f;    // 0..1, margin-normalised
    float    keyTuningHz   = 0.0f;    // detected reference pitch, 0 = unknown
    uint32_t keyAgeMs      = 0;       // reading age at publish time
    uint8_t _pad[128 - 4 - 11 * 4 - 6 * 4 - 4 - 8 - 8 - 8 - 4 - 16];   // -> 128 (2 cache lines)
};
static_assert(sizeof(LinkMeterFrame) == 128, "LinkMeterFrame must be 128 bytes");
// Layout freeze: the cross-version story above is only true while these hold.
static_assert(offsetof(LinkMeterFrame, audioStale) == 88,
              "pre-0.8.5 field moved: old readers would misread every frame");
static_assert(offsetof(LinkMeterFrame, peakFastL)  == 92,  "peakFast offset");
static_assert(offsetof(LinkMeterFrame, fieldsMask) == 100, "fieldsMask offset");
static_assert(offsetof(LinkMeterFrame, keyRoot)       == 104, "key group offset");
static_assert(offsetof(LinkMeterFrame, keyConfidence) == 108, "key group offset");
static_assert(offsetof(LinkMeterFrame, keyAgeMs)      == 116, "key group offset");

/// fieldsMask bits. A bit promises ONLY that the writer populates the
/// field group; values still carry their own absent conventions (-100).
static constexpr uint32_t kFrameHasFastPeak = 1u << 0;
static constexpr uint32_t kFrameHasKey      = 1u << 1;

/// THE gate every fast-peak consumer goes through. Pure, testable.
inline bool frameHasFastPeak(const LinkMeterFrame& f)
{
    return (f.fieldsMask & kFrameHasFastPeak) != 0;
}

/// THE gate every detected-key consumer goes through. The mask bit promises a
/// key-capable writer; the root range rejects both the "no reading yet"
/// sentinel (-1) and an old writer's zeroed pad served through a recycled
/// slot's stale mask (belt and braces — claimSlot blanks the frame anyway).
inline bool frameHasKey(const LinkMeterFrame& f)
{
    return (f.fieldsMask & kFrameHasKey) != 0
        && f.keyRoot >= 0 && f.keyRoot < 12
        && f.keyConfidence > 0.0f;
}

static constexpr size_t kRegSize =
    sizeof(RegistryHeader) + (size_t)kRegMaxSlots * sizeof(RegistrySlot)
                           + (size_t)kRegMaxSlots * sizeof(LinkMeterFrame);

// =============================================================================
//  Atomic helpers  (portable std::atomic — safe on mmap'd memory, no placement-new)
//
//  The shared structs keep plain uint32_t members so the mapped layout and byte
//  offsets are identical on every platform (Mac and Windows map the same block).
//  These helpers overlay std::atomic<uint32_t> onto those fields instead of
//  declaring them atomic, which keeps the layout untouched while giving the same
//  orderings the GCC __atomic_* builtins provided. std::atomic_ref would express
//  this more directly, but it is C++20 and this project builds as C++17.
//
//  The overlay is sound only if std::atomic<uint32_t> is layout-identical to a
//  bare uint32_t and never falls back to a lock — asserted below. On a locked
//  implementation the lock would live in one process's address space and the
//  cross-process handshake would silently break, so this must stay a hard error.
// =============================================================================
namespace LinkShm {

static_assert(sizeof(std::atomic<uint32_t>)  == sizeof(uint32_t),
              "std::atomic<uint32_t> must not change the shared-memory layout");
static_assert(alignof(std::atomic<uint32_t>) == alignof(uint32_t),
              "std::atomic<uint32_t> must not change the shared-memory alignment");
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "cross-process atomics require lock-free std::atomic<uint32_t>");

/// Overlay an atomic view on a shared uint32_t field. The field is a plain
/// uint32_t in the mapping; only the access is atomic.
inline std::atomic<uint32_t>* asAtomic (uint32_t* p)
{
    return reinterpret_cast<std::atomic<uint32_t>*>(p);
}
inline const std::atomic<uint32_t>* asAtomic (const uint32_t* p)
{
    return reinterpret_cast<const std::atomic<uint32_t>*>(p);
}

inline uint32_t loadAcquire (const uint32_t* p) { return asAtomic(p)->load(std::memory_order_acquire); }
inline uint32_t loadRelaxed (const uint32_t* p) { return asAtomic(p)->load(std::memory_order_relaxed); }
inline void     storeRelease(uint32_t* p, uint32_t v) { asAtomic(p)->store(v, std::memory_order_release); }
inline bool casStrong(uint32_t* p, uint32_t expected, uint32_t desired)
{
    // expected is by-value: like the __atomic_compare_exchange_n call this
    // replaces, the witnessed value on failure is intentionally discarded.
    return asAtomic(p)->compare_exchange_strong(expected, desired,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire);
}

// =============================================================================
//  Directory resolution  (call once, off the audio thread)
// =============================================================================

/// Resolve the shared directory both plugins can write to.
/// Tries ~/Library/Application Support/EchoJay/link/ first;
/// falls back to $TMPDIR/echojay_link/ if that fails.
/// Returns the path with a trailing slash, or "" on total failure.
/// errno_out: 0 = success, otherwise the errno from the failing mkdir/stat.
inline juce::String resolveDir(int& errno_out)
{
    errno_out = 0;
    // Helper: try to create a directory and verify it's writable
    auto tryDir = [&](const juce::File& d) -> bool {
        if (!d.exists())
        {
            const juce::Result r = d.createDirectory();
            if (r.failed()) { errno_out = EACCES; return false; }
        }
        // Quick write-access probe
        const juce::File probe = d.getChildFile(".probe");
        if (!probe.replaceWithText("ok")) { errno_out = EACCES; return false; }
        probe.deleteFile();
        return true;
    };

    // 1. Preferred: persistent across sessions
#if JUCE_MAC
    {
        juce::File appSupport = juce::File::getSpecialLocation(
            juce::File::userApplicationDataDirectory)
            .getChildFile("Application Support/EchoJay/link");
        if (tryDir(appSupport))
            return appSupport.getFullPathName() + "/";
    }
#endif

    // 2. Fallback: per-user temp dir (always accessible, same value within one process)
    {
        const char* td = ::getenv("TMPDIR");
        juce::File base = td ? juce::File(juce::String::fromUTF8(td))
                             : juce::File::getSpecialLocation(juce::File::tempDirectory);
        juce::File fallback = base.getChildFile("echojay_link");
        if (tryDir(fallback))
            return fallback.getFullPathName() + "/";
    }

    errno_out = EACCES;
    return {};
}

// =============================================================================
//  File name helpers
// =============================================================================

/// Sanitise a user link name into a safe filename component (alphanumeric + _).
/// Returns "" if empty after sanitisation. Max 16 chars.
inline juce::String makeSafeFilePart(const juce::String& linkName)
{
    juce::String safe;
    for (auto c : linkName.trim().toStdString())
        safe += (std::isalnum((unsigned char)c) ? c : '_');
    return safe.substring(0, 16);
}

/// Returns the audio ring filename for a given link name, e.g. "audio_drums.bin".
inline juce::String makeAudioFilename(const juce::String& linkName)
{
    const juce::String safe = makeSafeFilePart(linkName);
    if (safe.isEmpty()) return {};
    return "audio_" + safe + ".bin";
}

/// Registry filename. v2 appends the LinkMeterFrame array; the name is
/// bumped because openFileMapped ftruncates to the opener's size — an old
/// build opening a v2 file would shrink it under a live v2 mapping (SIGBUS).
/// Separate files mean old and new builds simply don't see each other.
static inline const char* kRegistryFilename = "registry_v2.bin";

// =============================================================================
//  Registry liveness (25 Aug 2026): a slot is LISTED only after its heartbeat
//  has been observed to CLIMB. inUse alone lists ghosts: a killed Link's slot
//  keeps inUse=1 with a frozen heartbeat until the ~30s reaper, and the
//  selector showed five dead rows settling to two. Pure so the decision is
//  gateable without a processor; the same observation feeds the reaper.
// =============================================================================
struct RegLiveness
{
    uint32_t lastHb = 0;
    bool     seen   = false;    // at least one observation exists
    bool     proven = false;    // the heartbeat has climbed since first seen
    bool observe (uint32_t hb)  // returns proven as of this observation
    {
        if (seen && hb != lastHb) proven = true;
        lastHb = hb;
        seen   = true;
        return proven;
    }
};

// =============================================================================
//  Uid claim gate (25 Aug 2026 ruling): a Link restoring its SAVED uid may
//  find another registry slot already carrying it. Three cases, decided by
//  the holder's heartbeat, never by inUse alone:
//    - PROVEN LIVE (heartbeat observed climbing): a genuine duplicate
//      (copy/paste clones the saved state, uid included) -> THIS instance
//      re-mints; first to live on the uid keeps it.
//    - OBSERVED FROZEN through the threshold: a ghost of a dead launch ->
//      reap the slot and ADOPT the uid. This is the churn fix: the old
//      guard re-minted against ghosts, so every unclean kill burned an
//      identity and orphaned its files (62 sidecars on one machine).
//    - NOT YET DECIDED (just-launched holder, too few observations): WAIT.
//      An unproven holder is never adopted -- reaping a possibly-live slot
//      is the one unrecoverable mistake here.
//  Pure so all three arms gate functionally; the caller re-observes on its
//  claim-retry tick (~1s; producers bump ~1Hz, so live proves in 1-2 ticks).
// =============================================================================
struct UidClaimGate
{
    enum class Decision { Wait, Remint, AdoptGhost };
    RegLiveness live;
    int ticks = 0;
    Decision observe (uint32_t holderHb, int frozenTicksNeeded = 5)
    {
        ++ticks;
        if (live.observe (holderHb)) return Decision::Remint;
        if (ticks >= frozenTicksNeeded) return Decision::AdoptGhost;
        return Decision::Wait;
    }
};

// =============================================================================
//  Dead-uid file reaper, NARROWED to transient classes (25 Aug 2026 ruling).
//  The original premise — "a dead uid can never be addressed again" — is
//  FALSE: the uid is saved in the Link's state and restored on project
//  reopen, so uids RETURN (proven by a sidecar whose revision went
//  backwards under one uid). What remains safe to reap:
//    - PROTOCOL TRANSIENTS (lease, lock, ctrl/chain cmd+ack): consumed or
//      recency-gated by protocol; a returning Link recreates them from
//      nothing.
//    - UNREFERENCED AUDIO RINGS: recreated at openRing by any returning
//      producer; reaped against the ring FILENAMES referenced by registry
//      slots — never by parsing a uid out of a name.
//  SPARED: rack-*.json (the sidecar — a returning uid's rack description)
//  and structplan-*.json ENTIRELY (a journal is someone's rollback; no
//  grace window makes deleting one a safe judgement).
// =============================================================================
inline int reapDeadUidFiles (const juce::String& dir,
                             const juce::StringArray& liveUids,
                             const juce::StringArray& liveAudioFiles,
                             juce::int64 nowMs, juce::int64 graceMs)
{
    int reaped = 0;
    static const char* uidPatterns[] = { "racklock-*.json",
        "lease-*.json", "ctrl-cmd-*.json", "ctrl-ack-*.json",
        "chain-cmd-*.json", "chain-ack-*.json" };
    for (auto* pat : uidPatterns)
        for (auto& f : juce::File (dir).findChildFiles (juce::File::findFiles,
                                                        false, pat))
        {
            const auto uid = f.getFileName()
                                 .fromLastOccurrenceOf ("-", false, false)
                                 .upToLastOccurrenceOf (".", false, false);
            if (uid.isEmpty() || liveUids.contains (uid)) continue;
            if (nowMs - f.getLastModificationTime().toMilliseconds() < graceMs)
                continue;
            if (f.deleteFile()) ++reaped;
        }
    for (auto& f : juce::File (dir).findChildFiles (juce::File::findFiles,
                                                    false, "audio_*.bin"))
    {
        if (liveAudioFiles.contains (f.getFileName())) continue;
        if (nowMs - f.getLastModificationTime().toMilliseconds() < graceMs)
            continue;
        if (f.deleteFile()) ++reaped;
    }
    return reaped;
}

// =============================================================================
//  Low-level file mmap helpers
// =============================================================================

/// Open (or create) a file, truncate to `size`, mmap RW.
/// Returns mapped pointer or nullptr; errno_out receives errno on failure (0 ok).
// File IDENTITY (device+inode) — the consumer binds a ring by identity, not
// by path, so a producer that reopens the ring at the SAME filename (new
// inode after unlink) is detected: the consumer sees the path now points at
// a different inode than the one it mapped and treats its held mapping as
// STALE (no data) rather than reading the dead inode's old audio. Purely
// consumer-side: no shared-memory layout change, so any main/Link version
// mix behaves as before except the stale case degrades to no-data.
struct FileIdentity
{
    uint64_t dev = 0, ino = 0;
    bool     valid = false;
    bool operator==(const FileIdentity& o) const
    { return valid && o.valid && dev == o.dev && ino == o.ino; }
    bool operator!=(const FileIdentity& o) const { return !(*this == o); }
};

inline FileIdentity fdIdentity(int fd)
{
#if LINK_FILE_SUPPORTED
    struct stat st{};
    if (fd >= 0 && ::fstat(fd, &st) == 0)
        return { (uint64_t)st.st_dev, (uint64_t)st.st_ino, true };
#else
    juce::ignoreUnused(fd);
#endif
    return {};
}

inline FileIdentity pathIdentity(const juce::String& path)
{
#if LINK_FILE_SUPPORTED
    struct stat st{};
    if (::stat(path.toStdString().c_str(), &st) == 0)
        return { (uint64_t)st.st_dev, (uint64_t)st.st_ino, true };
#else
    juce::ignoreUnused(path);
#endif
    return {};
}

inline void* openFileMapped(const juce::String& path, size_t size,
                             bool readOnly, int& fd_out, int& errno_out)
{
#if LINK_FILE_SUPPORTED
    const std::string p = path.toStdString();
    const int flags = readOnly ? O_RDONLY : (O_RDWR | O_CREAT);
    const int fd    = ::open(p.c_str(), flags, 0600);
    if (fd < 0) { errno_out = errno; fd_out = -1; return nullptr; }

    if (!readOnly)
    {
        struct stat st{};
        ::fstat(fd, &st);
        if ((size_t)st.st_size != size)
        {
            if (::ftruncate(fd, (off_t)size) != 0)
                { errno_out = errno; ::close(fd); fd_out = -1; return nullptr; }
        }
    }
    else
    {
        struct stat st{};
        if (::fstat(fd, &st) != 0 || (size_t)st.st_size < size)
            { ::close(fd); fd_out = -1; errno_out = EINVAL; return nullptr; }
    }

    const int prot  = readOnly ? PROT_READ : (PROT_READ | PROT_WRITE);
    void* map = ::mmap(nullptr, size, prot, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { errno_out = errno; ::close(fd); fd_out = -1; return nullptr; }

    errno_out = 0;
    fd_out = fd;
    return map;
#else
    juce::ignoreUnused(path, size, readOnly);
    errno_out = ENOSYS; fd_out = -1; return nullptr;
#endif
}

inline void closeMapped(void* map, size_t size, int fd,
                         const juce::String& path, bool doUnlink)
{
#if LINK_FILE_SUPPORTED
    if (map && map != MAP_FAILED) ::munmap(map, size);
    if (fd  >= 0)                 ::close(fd);
    if (doUnlink && path.isNotEmpty()) ::unlink(path.toStdString().c_str());
#else
    juce::ignoreUnused(map, size, fd, path, doUnlink);
#endif
}

// =============================================================================
//  Audio ring  (producer + consumer)
// =============================================================================

inline LinkShmHeader* ringHeader (void* map) { return static_cast<LinkShmHeader*>(map); }
inline float*         ringSamples(void* map)
{
    return reinterpret_cast<float*>(static_cast<uint8_t*>(map) + kLinkHdrSize);
}

inline void ringProduce(void* map, const float* const* chPtrs, int numCh, int numFrames)
{
    auto*          hdr  = ringHeader(map);
    float*         buf  = ringSamples(map);
    const uint32_t mask = kLinkRingFrames - 1u;
    const uint32_t w    = loadRelaxed(&hdr->writeIdx);
    const uint32_t r    = loadAcquire(&hdr->readIdx);
    const uint32_t n    = (uint32_t)std::min((uint32_t)numFrames, kLinkRingFrames - (w - r));
    for (uint32_t i = 0; i < n; ++i)
    {
        uint32_t pos = (w + i) & mask;
        buf[pos * 2 + 0] = (numCh >= 1 && chPtrs && chPtrs[0]) ? chPtrs[0][i] : 0.f;
        buf[pos * 2 + 1] = (numCh >= 2 && chPtrs && chPtrs[1]) ? chPtrs[1][i] : 0.f;
    }
    storeRelease(&hdr->writeIdx, w + n);
}

inline uint32_t ringConsume(void* map, float* outL, float* outR, int numFrames)
{
    auto*          hdr  = ringHeader(map);
    float*         buf  = ringSamples(map);
    const uint32_t mask = kLinkRingFrames - 1u;
    const uint32_t r    = loadRelaxed(&hdr->readIdx);
    const uint32_t w    = loadAcquire(&hdr->writeIdx);
    const uint32_t n    = (uint32_t)std::min((uint32_t)numFrames, w - r);
    for (uint32_t i = 0; i < n; ++i)
    {
        uint32_t pos = (r + i) & mask;
        if (outL) outL[i] = buf[pos * 2 + 0];
        if (outR) outR[i] = buf[pos * 2 + 1];
    }
    storeRelease(&hdr->readIdx, r + n);
    return n;
}

// =============================================================================
//  Stage 0 of remote editing: seek + position stamps (8 Aug 2026)
//
//  ringConsume above is UNTOUCHED, deliberately: capture is its consumer and
//  capture wants every sample, not the latest ones. Seeking is a separate
//  call a consumer opts into per use, never a property of the ring. Nothing
//  in production calls ringSeekForward this pass; stage 1's alignment FIFO
//  will. The two modes coexist on the single-reader ring only in the sense
//  that they cannot run at once: readIdx is one cursor, so a seeking
//  consumer and a capture on the same Link are mutually exclusive. That
//  exclusion is stage 1's problem to enforce (it introduces the second
//  consumer); stage 0 introduces no caller, so today there is nothing to
//  exclude.
// =============================================================================

/// Jump readIdx FORWARD so at most `targetBacklog` frames remain unread,
/// dropping the oldest. Returns how many frames were dropped (0 when already
/// at or under target).
///
/// POLICY: seek-to-target, with seek-to-latest as the target==0 special
/// case. A real-time consumer wants a small cushion so producer/consumer
/// scheduling jitter does not starve it into a gap every other block, and
/// only the consumer knows its cushion, so the target is a parameter rather
/// than a policy baked in here. The seek only ever moves FORWARD (backward
/// would re-read frames the producer may already be overwriting) and never
/// past writeIdx.
inline uint32_t ringSeekForward(void* map, uint32_t targetBacklog)
{
    auto*          hdr = ringHeader(map);
    const uint32_t r   = loadRelaxed(&hdr->readIdx);
    const uint32_t w   = loadAcquire(&hdr->writeIdx);
    const uint32_t backlog = w - r;
    if (backlog <= targetBacklog) return 0;
    const uint32_t drop = backlog - targetBacklog;
    storeRelease(&hdr->readIdx, r + drop);
    return drop;
}

/// Producer: pair ring index `writeIdxAtBlockStart` with the host sample
/// position of the frame about to be written there. Call ONCE per block,
/// BEFORE ringProduce, and only when the host actually supplied a position:
/// no position, no stamp, and stampSeq == 0 keeps meaning "no data" to every
/// reader. Audio thread; two relaxed stores and two release stores, no
/// locks, same discipline as the meter frame seqlock.
///
/// After a partial produce (ring full drops frames) the linear mapping
/// below is wrong until the NEXT stamp re-pairs it, one block later. A
/// consumer that needs drop-awareness compares consecutive stamps.
inline void ringStampPublish(void* map, uint32_t writeIdxAtBlockStart, int64_t hostPos)
{
    auto* hdr = ringHeader(map);
    const uint32_t s = loadRelaxed(&hdr->stampSeq) & ~1u;   // last stable seq
    storeRelease(&hdr->stampSeq, s + 1);                    // odd: writing
    hdr->stampWriteIdx = writeIdxAtBlockStart;
    hdr->stampHostPos  = hostPos;
    storeRelease(&hdr->stampSeq, s + 2);                    // even: stable
}

/// Consumer: read the newest stamp. Returns false for NO DATA -- an old
/// Link that never wrote one (region zeroed, seq 0), a new Link whose host
/// gave no position (never published, seq 0), or a torn read that stayed
/// torn. On true, the host position of any unread frame index f is
///     stampHostPos + (int32_t)(f - stampWriteIdx)
/// (signed diff so uint32 wrap is handled). A returned stampHostPos of 0 is
/// a REAL position at the timeline start, which is exactly why absence is
/// carried by the seq and never by the value.
inline bool ringStampRead(void* map, uint32_t& stampWriteIdxOut, int64_t& hostPosOut)
{
    auto* hdr = ringHeader(map);
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const uint32_t s1 = loadAcquire(&hdr->stampSeq);
        if (s1 == 0u) return false;            // never published: absent
        if (s1 & 1u) continue;                 // mid-write, retry
        const uint32_t w  = hdr->stampWriteIdx;
        const int64_t  p  = hdr->stampHostPos;
        const uint32_t s2 = loadAcquire(&hdr->stampSeq);
        if (s1 == s2) { stampWriteIdxOut = w; hostPosOut = p; return true; }
    }
    return false;                              // stayed torn: no data
}

/// Open (create) a ring producer file in `dir`.
/// `filename` = result of makeAudioFilename(). Full path = dir + filename.
/// Returns nullptr on failure.
inline void* openRingProducer(const juce::String& dir, const juce::String& filename,
                               float sr, uint32_t ch,
                               int& fd_out, int& errno_out)
{
    const juce::String path = dir + filename;
    void* map = openFileMapped(path, kLinkShmSize, /*readOnly=*/false, fd_out, errno_out);
    if (!map) return nullptr;

    // Initialise header (zero ring indices, set magic last)
    std::memset(map, 0, kLinkHdrSize);
    auto* hdr = ringHeader(map);
    hdr->version        = kLinkVersion;
    hdr->sampleRate     = sr;
    hdr->numChannels    = ch;
    hdr->capacityFrames = kLinkRingFrames;
    // Write magic last (acts as a "ready" flag for the consumer)
    storeRelease(&hdr->magic, kLinkMagic);
    return map;
}

/// Open an existing ring consumer file. Full path = dir + filename.
/// Returns nullptr if file doesn't exist or magic is wrong.
/// Must be RW: ringConsume() writes readIdx back into the mapping.
inline void* openRingConsumer(const juce::String& dir, const juce::String& filename,
                               int& fd_out)
{
    const juce::String path = dir + filename;
    int err = 0;
    void* map = openFileMapped(path, kLinkShmSize, /*readOnly=*/false, fd_out, err);
    if (!map) return nullptr;
    // Verify magic (producer may not have finished initialising yet)
    if (loadAcquire(&ringHeader(map)->magic) != kLinkMagic)
    {
        closeMapped(map, kLinkShmSize, fd_out, {}, false);
        fd_out = -1;
        return nullptr;
    }
    return map;
}

inline void closeRing(void* map, int fd, const juce::String& fullPath, bool doUnlink)
{
    closeMapped(map, kLinkShmSize, fd, fullPath, doUnlink);
}

// =============================================================================
//  Registry
// =============================================================================

inline RegistryHeader* regHeader(void* map) { return static_cast<RegistryHeader*>(map); }
inline RegistrySlot*   regSlots (void* map)
{
    return reinterpret_cast<RegistrySlot*>(static_cast<uint8_t*>(map) + sizeof(RegistryHeader));
}

/// Open (or create) the registry file at dir/registry.bin.
/// Returns nullptr on failure; errno_out receives errno from open() (0 = success).
inline void* openRegistry(const juce::String& dir, int& fd_out, int& errno_out)
{
    const juce::String path = dir + kRegistryFilename;
    void* map = openFileMapped(path, kRegSize, /*readOnly=*/false, fd_out, errno_out);
    if (!map) return nullptr;

    // Initialise if magic not yet set (atomic CAS — safe for concurrent openers)
    if (casStrong(&regHeader(map)->magic, 0u, kRegMagic))
    {
        std::memset(regSlots(map), 0, (size_t)kRegMaxSlots * sizeof(RegistrySlot));
        regHeader(map)->version  = 1;
        regHeader(map)->maxSlots = (uint32_t)kRegMaxSlots;
    }
    errno_out = 0;
    return map;
}

inline void closeRegistry(void* map, int fd)
{
    // Do NOT delete the file — other instances may still be using it.
    closeMapped(map, kRegSize, fd, {}, /*doUnlink=*/false);
}

// =============================================================================
//  Registry slot operations  (unchanged from stage 2)
// =============================================================================

inline LinkMeterFrame* meterFrames(void* regMap)
{
    return reinterpret_cast<LinkMeterFrame*>(
        static_cast<uint8_t*>(regMap) + sizeof(RegistryHeader)
        + (size_t) kRegMaxSlots * sizeof(RegistrySlot));
}

/// Claim a free slot.  Returns slot index [0..15] or -1 if full.
/// `audioFilename` = makeAudioFilename(linkName), stored so consumer can open it.
inline int claimSlot(void* regMap,
                     const juce::String& displayName,
                     const juce::String& audioFilename,
                     const juce::String& instanceUid,
                     float sr, uint32_t ch)
{
    if (!regMap) return -1;
    RegistrySlot* slots = regSlots(regMap);
    for (int i = 0; i < kRegMaxSlots; ++i)
    {
        if (casStrong(&slots[i].inUse, 0u, 1u))
        {
            std::strncpy(slots[i].displayName, displayName.toRawUTF8(),
                         sizeof(slots[i].displayName) - 1);
            slots[i].displayName[sizeof(slots[i].displayName) - 1] = 0;
            std::strncpy(slots[i].audioFile, audioFilename.toRawUTF8(),
                         sizeof(slots[i].audioFile) - 1);
            slots[i].audioFile[sizeof(slots[i].audioFile) - 1] = 0;
            std::strncpy(slots[i].instanceUid, instanceUid.toRawUTF8(),
                         sizeof(slots[i].instanceUid) - 1);
            slots[i].instanceUid[sizeof(slots[i].instanceUid) - 1] = 0;
            slots[i].sampleRate  = sr;
            slots[i].numChannels = ch;
            slots[i].gainDb      = 0.0f;   // recycled slot: never serve the
                                           // previous owner's gain (the owner
                                           // re-publishes its real gain at once)
            slots[i].placement   = 0;      // recycled slot: unknown until owner republishes
            slots[i].dialCapable = 0;      // and INCAPABLE until the new owner says otherwise
            // Reset the slot's meter frame: a recycled slot must NEVER serve
            // the previous owner's last values — the receiver would see an
            // unfamiliar seq, treat it as a fresh frame, and style frozen
            // mid-song numbers as live. Defaults (-100s) read as dashes.
            {
                LinkMeterFrame* dst = meterFrames(regMap) + i;
                const LinkMeterFrame blank {};
                const uint32_t s0 = loadRelaxed(&dst->seq) & ~1u;
                storeRelease(&dst->seq, s0 + 1);
                std::memcpy(reinterpret_cast<uint8_t*>(dst) + sizeof(uint32_t),
                            reinterpret_cast<const uint8_t*>(&blank) + sizeof(uint32_t),
                            sizeof(LinkMeterFrame) - sizeof(uint32_t));
                storeRelease(&dst->seq, s0 + 2);
            }
            storeRelease(&slots[i].activeFlag, 1u);   // default Active; setSlotActive() refines
            storeRelease(&slots[i].heartbeat, 1u);
            return i;
        }
    }
    return -1;
}

inline void releaseSlot(void* regMap, int slotIdx)
{
    if (!regMap || slotIdx < 0 || slotIdx >= kRegMaxSlots) return;
    RegistrySlot* slot = regSlots(regMap) + slotIdx;
    std::memset(slot->displayName, 0, sizeof(slot->displayName));
    std::memset(slot->audioFile,   0, sizeof(slot->audioFile));
    std::memset(slot->instanceUid, 0, sizeof(slot->instanceUid));
    slot->sampleRate  = 0;
    slot->numChannels = 0;
    storeRelease(&slot->activeFlag, 0u);
    storeRelease(&slot->heartbeat,  0u);
    storeRelease(&slot->inUse,      0u);
}

/// Publish the Link's Active state (capture/meter role) without releasing
/// the slot — a named-but-inactive Link stays visible to the main plugin so
/// it can be re-activated remotely.
inline void setSlotActive(void* regMap, int slotIdx, bool active)
{
    if (!regMap || slotIdx < 0 || slotIdx >= kRegMaxSlots) return;
    storeRelease(&regSlots(regMap)[slotIdx].activeFlag, active ? 1u : 0u);
}

inline void bumpHeartbeat(void* regMap, int slotIdx)
{
    if (!regMap || slotIdx < 0 || slotIdx >= kRegMaxSlots) return;
    RegistrySlot* slot = regSlots(regMap) + slotIdx;
    storeRelease(&slot->heartbeat, loadRelaxed(&slot->heartbeat) + 1u);
}

// Mirror the Link's current gain (dB) into its slot so the monitor can show
// it whether or not the Link is Active. Plain float store — display value,
// no ordering requirement against other fields.
inline void setSlotGain(void* regMap, int slotIdx, float gainDb)
{
    if (!regMap || slotIdx < 0 || slotIdx >= kRegMaxSlots) return;
    regSlots(regMap)[slotIdx].gainDb = gainDb;
}

// Mirror the Link's placement (0 unset, 1 bus, 2 insert) for the monitor.
inline void setSlotPlacement(void* regMap, int slotIdx, uint8_t placement)
{
    if (!regMap || slotIdx < 0 || slotIdx >= kRegMaxSlots) return;
    regSlots(regMap)[slotIdx].placement = placement;
}

// Publish the dial capability. Called by the Link that can READ
// settings_structured, so presence of a 1 here is proof by the writer rather
// than inference by the reader.
inline void setSlotDialCapable(void* regMap, int slotIdx, bool capable)
{
    if (!regMap || slotIdx < 0 || slotIdx >= kRegMaxSlots) return;
    regSlots(regMap)[slotIdx].dialCapable = capable ? 1 : 0;
}

struct SlotSnapshot {
    int          idx;
    bool         dialCapable = false;   // 0 from every existing Link; see the field
    juce::String instanceUid;    // per-instance address ("" from old writers)
    juce::String displayName;
    juce::String audioFilename;  // filename only, e.g. "audio_drums.bin"
    float        sampleRate;
    uint32_t     numChannels;
    uint32_t     heartbeat;
    bool         active = true;  // capture/meter role on
    float        gainDb = 0.0f;  // Link's built-in gain stage (0 from old writers)
    uint8_t      placement = 0;  // 0 unset/unknown, 1 bus, 2 insert
};

inline bool readSlot(void* regMap, int i, SlotSnapshot& out)
{
    if (!regMap || i < 0 || i >= kRegMaxSlots) return false;
    RegistrySlot* slot = regSlots(regMap) + i;
    if (loadAcquire(&slot->inUse) == 0) return false;
    out.idx           = i;
    out.displayName   = juce::String::fromUTF8(slot->displayName);
    out.instanceUid   = juce::String::fromUTF8(slot->instanceUid);
    out.audioFilename = juce::String::fromUTF8(slot->audioFile);
    out.sampleRate    = slot->sampleRate;
    out.numChannels   = slot->numChannels;
    out.heartbeat     = loadRelaxed(&slot->heartbeat);
    out.active        = loadAcquire(&slot->activeFlag) != 0;
    out.gainDb        = slot->gainDb;
    out.placement     = slot->placement;
    out.dialCapable   = slot->dialCapable != 0;   // 0 from every old Link
    return true;
}

inline void reapSlot(void* regMap, int i)
{
    if (!regMap || i < 0 || i >= kRegMaxSlots) return;
    storeRelease(&regSlots(regMap)[i].inUse, 0u);
}

// ============================================================================
//  Rack sidecar (Phase R) — rack-<uid>.json in the shared dir.
//
//  The Link publishes its live rack whenever its ChainHost revision changes;
//  the main plugin reads it at compose/apply time to build a targeted
//  [CURRENT CHAIN] and to staleness-guard v:2 edit sends. Deliberately a
//  JSON sidecar, NOT a registry extension: RegistrySlot is a fixed 128-byte
//  layout with 3 spare bytes, rack data is variable-length, and a
//  registry_v3 would break cross-version compatibility. Old Links simply
//  never write one — readers treat absence/parse failure as "rack unknown"
//  (valid=false) and fall back to build-only, never an error.
//
//  `revision` is the LINK's own ChainHost counter. It shares a numbering
//  space with NOTHING else — never compare it to the main plugin's
//  chainRevision. `name` is a write-time convenience for debugging; user-
//  facing labels come from the registry displayName, which renames faster.
//
//  Writes are plain replaceWithText (same non-atomic exposure as cmd/ack
//  files); a rare torn read parses as invalid → "rack unknown" → safe.
// ---- The built-in EQ's magnitude curve -------------------------------------
//
//  PRECOMPUTED RESPONSE, NOT BAND PARAMETERS, and the reason is stronger than
//  "the reader might replicate the maths wrongly". Replicating it correctly is
//  already hard (six band types, high/low pass at 6..96 dB per octave, which is
//  cascaded sections rather than one biquad, plus per-band Stereo/Mid/Side/
//  Left/Right routing, all exact with respect to the SVF at the ENGINE's sample
//  rate). But a DYNAMIC band's contribution is signal dependent and is simply
//  absent from any parameter snapshot: a reader drawing from parameters would
//  show a static curve while the audio did something else. That is not a
//  divergence risk to be managed, it is a guaranteed lie for any dynamic band.
//  So the writer calls EqEngine::getMagnitudeResponse, which is the same call
//  the EQ's own editor draws from, and ships the answer.
//
//  Integer deci-dB: the payload is predictable (see kEqCurvePoints) and the
//  0.05 dB quantisation error is orders of magnitude below one pixel at strip
//  size.
static constexpr int   kEqCurvePoints = 64;
static constexpr float kEqCurveLoHz   = 20.0f;
static constexpr float kEqCurveHiHz   = 20000.0f;
//  The PUBLISHED clamp, deliberately generous. A steep high-pass runs toward
//  minus infinity at 20 Hz, so an unclamped curve has no bottom at all. Plus
//  or minus 30 dB captures everything a bell or shelf does and pins the rest.
//  A reader may DRAW a tighter range (the mixer's strip thumbnail does, so
//  that ordinary moves are visible at 20-odd pixels tall) but must not assume
//  the published data is inside one.
static constexpr int   kEqCurveClampDeciDb = 300;

/// The curve's x axis, defined ONCE so writer and reader cannot disagree about
/// which frequency a sample belongs to. Log spaced, endpoints inclusive.
inline void eqCurveFreqs (float* out, int n) noexcept
{
    if (out == nullptr || n <= 0) return;
    if (n == 1) { out[0] = kEqCurveLoHz; return; }
    const double lo = std::log ((double) kEqCurveLoHz);
    const double hi = std::log ((double) kEqCurveHiHz);
    for (int i = 0; i < n; ++i)
        out[i] = (float) std::exp (lo + (hi - lo) * (double) i / (double) (n - 1));
}

struct RackSidecarSlot {
    juce::String name, format, settings;
    bool  bypassed = false;
    float wet = 1.0f;
    // Stage 1 remote editing: TRUE while this slot is externally controlled
    // (leased to the main plugin, bypassed here, edited there). Additive at
    // v:1 exactly like the curve: the key is written only when true, an old
    // reader never asks for it, and absent means not controlled.
    bool  controlled = false;
    // kEqCurvePoints integer deci-dB samples on the eqCurveFreqs grid.
    // EMPTY MEANS NO CURVE WAS PUBLISHED: this slot is not the built-in EQ,
    // or the writer predates the field. Empty is NOT a flat response and must
    // never be drawn as one, because a flat line is a positive claim that the
    // EQ is doing nothing. Absent key = unavailable, the same convention the
    // meter fields keep.
    std::vector<int16_t> curveDeciDb;
    // Identity, so a Link-racked slot can enter the server's fingerprint union
    // and be dialled (19 Aug 2026). Without these a Link slot reaches the
    // server as a name with no fp and cannot be resolved to a map at all.
    // Additive at v:1: written only when known, an old reader never asks. fp is
    // the exact binary fingerprint; uid/version form the ik for the tiered
    // fallback when a vendor update has moved the fp. AT THE END of the struct
    // ON PURPOSE: RackSidecarSlot is positionally brace-initialised
    // (LinkProcessor publishRackSidecar), so a new field goes last and is set
    // by assignment, never spliced into the middle.
    juce::String fp, uid, version;
    // Rack lock recency transport (21 Aug 2026): wallclock ms of the Link's
    // last LOCAL rack edit — the six LinkProcessor UI mutations, never a
    // remote op, or the main would wait out its own edits after releasing.
    // 0 = never / written by a build that predates the field, which reads as
    // "quiet" (racklock_test asserts the field genuinely round-trips, so a
    // systemic zero cannot masquerade as a pass). LAST, after the identity
    // strings, same positional-init rule as above.
    double lastEditMs = 0.0;
};
struct RackSidecar {
    bool  valid = false;
    juce::String uid, name;
    int   revision = -1;
    float masterWet = 1.0f;
    // Pre-chain gain (18 Aug 2026): mirrored so the mixer can show and drive
    // each channel's pre-gain in Pre mode. Additive keys at v:1; an old
    // reader ignores them and preGainDb stays 0 (unity, benign). userSet says
    // the value was set by hand (a build will not overwrite it) so the strip
    // can mark it; inputKnown gates the "no level, no confident 0" display.
    float preGainDb = 0.0f;
    bool  preGainUserSet = false;
    bool  preGainInputKnown = false;
    // Whole-rack borrow capability (step 2, 21 Aug 2026). Additive at v:1:
    // written true by a binary that honors the rack-scoped lease; absent
    // reads false, and a main NEVER offers borrow against false — an old
    // Link must not half-engage (see LeaseScope below for the wire-level
    // belt to this braces).
    bool  borrowCapable = false;
    // Structure editing capability (phase 2): a Link that can journal and
    // apply a structure plan announces it; absent reads false and a main
    // never sends a plan — the never-half-see pattern, again.
    bool  structureEditCapable = false;
    bool  inContextCapable = false;
    bool  muteEngaged = false;         // LIVE state: the mute is actually
                                       // applied this instant — the closed
                                       // loop that replaces "impossible by
                                       // construction" with CONFIRMED    // §8: can mute its output on lease
                                       // command (muteOut) — in-context
                                       // monitoring is OFFERED only when
                                       // announced, never detected
    std::vector<RackSidecarSlot> slots;
};

// =============================================================================
//  Whole-rack borrow: the third state-transfer tier and the lease scope
//  (RACK_BORROW_IMPLEMENTATION_SPEC §4/§5, step 2)
// =============================================================================
// The Link pull is a LOCAL file between two processes — document-class, not
// request-class. Its own named pair (spec §5/3c, decided): initialized to the
// session tier's values, free to diverge, and NEVER aligned with the API tier
// (anything that leaves the machine keeps kApiState*). Slot cap enforced
// Link-side before encoding; the TOTAL is a whole-chain budget enforced
// main-side across the rack pull (over → the borrow REFUSES with a named
// list, §5e — no partial borrow).
static constexpr int kLinkTransferMaxSlotBytes  = 4 * 1024 * 1024;
static constexpr int kLinkTransferMaxTotalBytes = 16 * 1024 * 1024;

// The lease's rack scope rides the EXISTING lease file additively: a rack
// lease writes scope:"rack" and slot:0. The pure decision below is shared by
// the Link's engage arm and the test; its old-binary arm is not hypothetical
// — an old binary never parses `scope`, sees slot1 == 0, and its own
// slot-validity check refuses the engage. Same outcome, proven both ways.
// The borrow's ring binding is re-resolved every renew tick rather than
// trusted from engage (hands-on finding #3: a sample-rate change can remap
// the Link slots and a stale binding goes SILENT while the lease still holds
// the Link dry). Pure decision, pinned in borrowhost_test: found again →
// stay/rebind; not found → tolerate kBorrowRingMaxLostTicks consecutive
// ticks (the Link may be re-registering), then RELEASE WITH WORDS — a
// borrow must produce sound or a stated release, never silence.
static constexpr int kBorrowRingMaxLostTicks = 3;   // ~3s at the renew cadence

// Step 3 (22 Aug 2026): WHAT APPLY WRITES, decided per slot by one pure
// gate. The hard rule (spec §5c, decided): a WITHHELD slot — one whose
// state never arrived because the identity match refused it — is NEVER
// written back, edited or not: its local instance runs defaults plus
// whatever the user did to defaults, and committing that would stomp the
// Link's real settings. An UNEDITED slot (current state byte-equal to the
// post-seed baseline) is left untouched too: Apply writes exactly what the
// user changed, nothing else. The Link's state can only change through a
// commit payload, so proving the plan is proving the Link untouched.
// The sidecar's slot uid is HEX (getSlotIdentity's toHexString — built for
// the server's fp union), but stateFitsPlugin compares DECIMAL
// (String(found.uniqueId)). Feeding one into the other withheld EVERY
// slot with a known uid — seed and Apply alike (22 Aug 2026, the
// zero-commits round). ONE converter, used by every consumer that hands a
// sidecar uid to the state-match policy; empty stays empty (no opinion).
inline juce::String sidecarUidToStateUid (const juce::String& hexUid)
{
    if (hexUid.isEmpty()) return {};
    return juce::String (hexUid.getHexValue32());
}

// ---------------------------------------------------------------------------
// Ctrl-cmd seq, collision-proof (24 Aug 2026): the seconds-resolution stamp
// meant two commands in one second shared a seq and the second was refused
// as a duplicate — silently, which read as "Apply does nothing". ONE author
// for every command seq this process issues: seeded at wall seconds so a
// restart stays monotonic against a Link's remembered lastApplied, and
// strictly increasing per call so two commands issued in the same
// MILLISECOND still get distinct seqs (gated functionally, not trusted).
// ---------------------------------------------------------------------------
inline int nextCtrlSeq()
{
    static std::atomic<int> last { 0 };
    const int now = (int) (juce::Time::currentTimeMillis() / 1000);
    int prev = last.load();
    int next;
    do { next = juce::jmax (now, prev + 1); }
    while (! last.compare_exchange_weak (prev, next));
    return next;
}

struct BorrowCommit
{
    enum class Action { Commit, LeaveWithheld, LeaveUnedited };
    static Action classify (bool withheld, bool edited)
    {
        if (withheld) return Action::LeaveWithheld;   // beats edited, always
        return edited ? Action::Commit : Action::LeaveUnedited;
    }
    struct Plan
    {
        std::vector<Action> actions;
        int changed = 0;          // slots the user edited (incl. withheld ones)
        int committing = 0;       // what Apply will actually write
        int withheldEdited = 0;   // edited but never written (the asymmetry)
        int untouched = 0;        // unedited, left alone
    };
    static Plan plan (const std::vector<std::pair<bool, bool>>& slots) // {withheld, edited}
    {
        Plan p;
        for (const auto& [withheld, edited] : slots)
        {
            const auto a = classify (withheld, edited);
            p.actions.push_back (a);
            if (edited) ++p.changed;
            if (a == Action::Commit) ++p.committing;
            else if (withheld && edited) ++p.withheldEdited;
            if (! edited) ++p.untouched;
        }
        return p;
    }
};

// =============================================================================
//  Structure editing, phase 1 (RACK_STRUCTURE_EDIT_SPEC, 22 Aug 2026):
//  the PLAN and the JOURNAL — pure computation and file format only. No UI,
//  no Link-side applier, no audio; phase 2 wires these into the transport.
// =============================================================================
namespace StructureEdit
{
    // Per-slot identity for the guard (spec §5): name always compared, uid
    // and fp only when BOTH sides carry them (absent-is-no-opinion — the
    // stateFitsPlugin grammar). uid is DECIMAL here, normalized from the
    // sidecar's hex at construction — the encoding that shipped a feature
    // which couldn't write is not allowed to recur.
    struct SlotIdentity
    {
        juce::String name, uid, fp;
        static SlotIdentity fromSidecar (const RackSidecarSlot& s)
        {
            return { s.name, sidecarUidToStateUid (s.uid), s.fp };
        }
        bool matches (const SlotIdentity& other) const
        {
            if (name.trim() != other.name.trim()) return false;
            if (uid.isNotEmpty() && other.uid.isNotEmpty() && uid != other.uid)
                return false;
            if (fp.isNotEmpty() && other.fp.isNotEmpty() && fp != other.fp)
                return false;
            return true;
        }
    };

    // The identity guard (spec §5). Count first, then per-index identity.
    // §3e's same-name swap is CAUGHT here: names match after a swap, but
    // per-index uid/fp do not.
    enum class BaseCheck { Match, CountMismatch, IdentityMismatch };
    inline BaseCheck verifyBaseIdentity (const std::vector<SlotIdentity>& planBase,
                                         const std::vector<SlotIdentity>& live)
    {
        if (planBase.size() != live.size()) return BaseCheck::CountMismatch;
        for (size_t i = 0; i < planBase.size(); ++i)
            if (! planBase[i].matches (live[i]))
                return BaseCheck::IdentityMismatch;
        return BaseCheck::Match;
    }

    // Capability (spec §0): an old Link never sees a plan. Trivial and pure
    // so the refusal arm is pinnable.
    enum class Accept { Proceed, RefuseIncapable };
    inline Accept accept (bool structureEditCapable)
    { return structureEditCapable ? Accept::Proceed : Accept::RefuseIncapable; }

    // The ordered ops (spec §3). Leave* classes are not ops — they are the
    // absence of one; the plan reports their counts for the confirm.
    enum class OpType { Remove, Move, Create, Commit };
    struct Op
    {
        OpType type {};
        int from = -1;             // Remove/Move/Commit: index at op time
        int to   = -1;             // Move: target index; Create: insert index
        juce::String name;         // display + honesty
        juce::String stateB64;     // Create seed / Commit payload
        SlotIdentity identity;     // phase 2: the applier keys staging/park by this
        bool bypassed = false;     // Create: the bypass state the user gave the
                                   // slot in the main — carried so the lease's
                                   // prior remap has a truth for created slots
                                   // (never a default, never "not restored")
        juce::String stateFormat;  // Create: the format of the instance whose
                                   // state seeded stateB64 (the main may host a
                                   // SUBSTITUTE build) — state is format-
                                   // specific, and the applier seeds only on a
                                   // match; empty = no opinion (seed)
    };

    // What the plan computation is TOLD about each current slot. originIndex
    // is the base index this slot came from, -1 for a slot created in the
    // main. The withheld/edited flags are the RECORDED facts from the borrow
    // session, never recomputed here.
    struct CurrentSlot
    {
        SlotIdentity identity;
        int  originIndex = -1;
        bool edited = false, withheld = false;
        juce::String stateB64;     // current state (Create seed / Commit)
        bool bypassedNow = false;  // live bypass in the main's borrowed host —
                                   // rides the Create op (field added LAST:
                                   // existing positional inits stay valid)
        juce::String stateFormat;  // the live instance's format — rides the
                                   // Create op so a substitute's blob can
                                   // never seed a different format's build
    };

    struct Plan
    {
        juce::String uid;                       // the Link
        std::vector<SlotIdentity> baseIdentity; // the guard's snapshot
        std::vector<Op> ops;                    // ordered: Removes, Moves, Creates, Commits
        int changed = 0, committing = 0, creating = 0, removing = 0,
            moving = 0, withheldEdited = 0, untouched = 0;
    };

    // The plan computation, deterministic by construction (spec §3 order:
    // removes descending, then moves to target order, then creates at their
    // positions, then commits). Withheld beats edited, always — a withheld
    // slot never yields a Commit, moved or not.
    inline Plan computePlan (const juce::String& uid,
                             const std::vector<SlotIdentity>& base,
                             const std::vector<CurrentSlot>& current)
    {
        Plan p;
        p.uid = uid;
        p.baseIdentity = base;

        // Removes: base indices no current slot originates from, descending
        // so earlier removes cannot shift later ones.
        std::vector<bool> survives (base.size(), false);
        for (const auto& c : current)
            if (c.originIndex >= 0 && c.originIndex < (int) base.size())
                survives[(size_t) c.originIndex] = true;
        for (int i = (int) base.size() - 1; i >= 0; --i)
            if (! survives[(size_t) i])
            {
                p.ops.push_back ({ OpType::Remove, i, -1, base[(size_t) i].name, {},
                                   base[(size_t) i] });
                ++p.removing;
            }

        // Moves: bring the survivors into the current order. The working
        // model mirrors what the applier will hold after the removes.
        std::vector<int> work;                     // base origin per position
        for (size_t i = 0; i < base.size(); ++i)
            if (survives[i]) work.push_back ((int) i);
        std::vector<int> target;                   // survivor origins, current order
        for (const auto& c : current)
            if (c.originIndex >= 0) target.push_back (c.originIndex);
        for (int pos = 0; pos < (int) target.size(); ++pos)
        {
            if (work[(size_t) pos] == target[(size_t) pos]) continue;
            int fromPos = pos + 1;
            while (fromPos < (int) work.size()
                   && work[(size_t) fromPos] != target[(size_t) pos]) ++fromPos;
            const int origin = work[(size_t) fromPos];
            work.erase (work.begin() + fromPos);
            work.insert (work.begin() + pos, origin);
            p.ops.push_back ({ OpType::Move, fromPos, pos,
                               base[(size_t) origin].name, {},
                               base[(size_t) origin] });
            ++p.moving;
        }

        // Creates: at their final positions, ascending (earlier inserts make
        // later target indices correct).
        for (int i = 0; i < (int) current.size(); ++i)
            if (current[(size_t) i].originIndex < 0)
            {
                p.ops.push_back ({ OpType::Create, -1, i,
                                   current[(size_t) i].identity.name,
                                   current[(size_t) i].stateB64,
                                   current[(size_t) i].identity,
                                   current[(size_t) i].bypassedNow,
                                   current[(size_t) i].stateFormat });
                ++p.creating;
            }

        // Commits: edited survivors, withheld NEVER (spec §3 / §5c).
        for (int i = 0; i < (int) current.size(); ++i)
        {
            const auto& c = current[(size_t) i];
            if (c.originIndex < 0) continue;       // Create seeds itself
            if (c.edited) ++p.changed;
            if (! c.edited) { ++p.untouched; continue; }
            if (c.withheld) { ++p.withheldEdited; continue; }
            p.ops.push_back ({ OpType::Commit, i, -1,
                               c.identity.name, c.stateB64, c.identity });
            ++p.committing;
        }
        return p;
    }

    // -------------------------------------------------------------------
    //  The journal (spec §1): plan + ABSOLUTE pre-images, written before
    //  the first mutation, deleted only on completion. Pre-images are the
    //  whole shape and every state — restore is wholesale replacement, so
    //  replaying it is idempotent by construction (and gated anyway).
    // -------------------------------------------------------------------
    struct PreImages
    {
        std::vector<SlotIdentity> shape;
        juce::StringArray states;              // parallel, "" = none held
    };

    inline juce::String journalPath (const juce::String& dir, const juce::String& uid)
    { return dir + "structplan-" + uid + ".json"; }

    inline juce::var identityToVar (const SlotIdentity& s)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("name", s.name);
        if (s.uid.isNotEmpty()) o->setProperty ("uid", s.uid);
        if (s.fp.isNotEmpty())  o->setProperty ("fp",  s.fp);
        return juce::var (o);
    }
    inline SlotIdentity identityFromVar (const juce::var& v)
    {
        SlotIdentity s;
        if (auto* o = v.getDynamicObject())
        {
            s.name = o->getProperty ("name").toString();
            s.uid  = o->getProperty ("uid").toString();
            s.fp   = o->getProperty ("fp").toString();
        }
        return s;
    }

    inline juce::var planToVar (const Plan& p, const PreImages& pre)
    {
        auto* root = new juce::DynamicObject();
        root->setProperty ("v",   1);
        root->setProperty ("uid", p.uid);
        juce::Array<juce::var> baseArr, opsArr, shapeArr;
        for (const auto& s : p.baseIdentity) baseArr.add (identityToVar (s));
        root->setProperty ("baseIdentity", baseArr);
        for (const auto& op : p.ops)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("type", (int) op.type);
            o->setProperty ("from", op.from);
            o->setProperty ("to",   op.to);
            o->setProperty ("name", op.name);
            if (op.stateB64.isNotEmpty()) o->setProperty ("state", op.stateB64);
            o->setProperty ("identity", identityToVar (op.identity));
            o->setProperty ("byp", op.bypassed);
            if (op.stateFormat.isNotEmpty())
                o->setProperty ("stateFmt", op.stateFormat);
            opsArr.add (juce::var (o));
        }
        root->setProperty ("ops", opsArr);
        for (const auto& s : pre.shape) shapeArr.add (identityToVar (s));
        root->setProperty ("preShape", shapeArr);
        juce::Array<juce::var> preStates;
        for (const auto& st : pre.states) preStates.add (st);
        root->setProperty ("preStates", preStates);
        return juce::var (root);
    }

    inline bool planFromVar (const juce::var& v, Plan& pOut, PreImages& preOut)
    {
        auto* o = v.getDynamicObject();
        if (o == nullptr || (int) o->getProperty ("v") != 1) return false;
        pOut = {};
        preOut = {};
        pOut.uid = o->getProperty ("uid").toString();
        if (auto* arr = o->getProperty ("baseIdentity").getArray())
            for (auto& e : *arr) pOut.baseIdentity.push_back (identityFromVar (e));
        if (auto* arr = o->getProperty ("ops").getArray())
            for (auto& e : *arr)
                if (auto* oo = e.getDynamicObject())
                    pOut.ops.push_back ({ (OpType) (int) oo->getProperty ("type"),
                                          (int) oo->getProperty ("from"),
                                          (int) oo->getProperty ("to"),
                                          oo->getProperty ("name").toString(),
                                          oo->getProperty ("state").toString(),
                                          identityFromVar (oo->getProperty ("identity")),
                                          (bool) oo->getProperty ("byp"),
                                          oo->getProperty ("stateFmt").toString() });
        if (auto* arr = o->getProperty ("preShape").getArray())
            for (auto& e : *arr) preOut.shape.push_back (identityFromVar (e));
        if (auto* arr = o->getProperty ("preStates").getArray())
            for (auto& e : *arr) preOut.states.add (e.toString());
        return true;
    }

    inline void writeJournal (const juce::String& dir, const Plan& p,
                              const PreImages& pre)
    {
        juce::File (journalPath (dir, p.uid))
            .replaceWithText (juce::JSON::toString (planToVar (p, pre), true));
    }
    inline bool readJournal (const juce::String& dir, const juce::String& uid,
                             Plan& pOut, PreImages& preOut)
    {
        juce::File f (journalPath (dir, uid));
        if (! f.existsAsFile()) return false;
        return planFromVar (juce::JSON::parse (f.loadFileAsString()), pOut, preOut);
    }

    // The restore, as a pure model transform: ABSOLUTE pre-images replace
    // the rack wholesale. Feeding the output back in yields itself — the
    // idempotence the journal's crash-retry depends on, gated in
    // structplan_test rather than trusted.
    struct RackModel { std::vector<SlotIdentity> shape; juce::StringArray states; };
    inline RackModel restoreFromJournal (const PreImages& pre, const RackModel&)
    { return { pre.shape, pre.states }; }
}

// Where the borrowed solo lands (hands-on decision, 21 Aug 2026): on a Mix
// Bus or Master Bus main, the borrowed channel feeds INTO the main's own
// chain — soloing a channel on the mix bus must not lose the master
// processing. On every other channel type it REPLACES the output after the
// main's chain — a borrowed vocal must not run through a guitar track's
// chain. Sub-buses (Vocal Bus, Drum Bus, ...) are "other". The user flip
// overrides either default; pure, pinned in borrowhost_test.
struct BorrowRoute
{
    static bool throughMainChain (bool mixOrMasterBus, bool userFlip)
    { return mixOrMasterBus != userFlip; }
};
struct BorrowRing
{
    enum class Verdict { Bound, Rebind, Lost, Release };
    static Verdict poll (int currentSlot, int foundSlot, int lostTicksSoFar)
    {
        if (foundSlot >= 0)
            return foundSlot == currentSlot ? Verdict::Bound : Verdict::Rebind;
        return lostTicksSoFar + 1 >= kBorrowRingMaxLostTicks ? Verdict::Release
                                                             : Verdict::Lost;
    }
};

struct LeaseScope
{
    enum class Engage { Refuse, Slot, Rack };
    static Engage decide (bool scopeIsRack, bool binarySupportsRack,
                          int slot1, int numSlots)
    {
        if (scopeIsRack)
            return binarySupportsRack ? Engage::Rack : Engage::Refuse;
        return (slot1 >= 1 && slot1 <= numSlots) ? Engage::Slot : Engage::Refuse;
    }
};

inline juce::String rackSidecarPath(const juce::String& dir, const juce::String& uid)
{
    return dir + "rack-" + uid + ".json";
}

// =============================================================================
//  Edit lease (stage 1 of remote editing) -- lease-<uid>.json
//
//  THE FILE IS THE LEASE. The main plugin writes {v:1, leaseId, slot, tMs}
//  every kLeaseRenewMs while it holds control of one of a Link's slots and
//  DELETES it on release; the Link polls at its ~100ms tick and derives the
//  lease state from what it finds. Heartbeat-and-expiry, never a release
//  message, because a crash sends nothing: a main plugin that dies simply
//  stops renewing, tMs goes stale, and the Link restores itself at
//  kLeaseExpireMs. tMs is the writer's wallclock (both plugins share one
//  machine and one clock), not file mtime, so filesystem timestamp
//  granularity is not in the protocol.
// =============================================================================
static constexpr double kLeaseRenewMs  = 1000.0;
static constexpr double kLeaseExpireMs = 3000.0;

inline juce::String leasePath(const juce::String& dir, const juce::String& uid)
{
    return dir + "lease-" + uid + ".json";
}

// =============================================================================
//  State blob codec (stage 1) -- ONE pairing, one author.
//
//  The pull shipped encoding with juce::Base64::toBase64 (RFC 4648) and
//  decoding with juce::MemoryBlock::fromBase64Encoding, which is NOT RFC
//  base64: it is MemoryBlock's own alphabet, and it returns false on RFC
//  input. Every third-party pull therefore failed at the decode with the
//  bytes intact on the wire, and the commit direction carried the same
//  mismatch. Both sides now call THESE two functions and nothing else, so
//  the pairing cannot drift again; ringseek_test round-trips both
//  directions against them and proves the old decoder refuses the same
//  input.
// =============================================================================
inline juce::String stateToB64(const juce::MemoryBlock& mb)
{
    return juce::Base64::toBase64(mb.getData(), mb.getSize());
}
inline bool stateFromB64(const juce::String& b64, juce::MemoryBlock& out)
{
    juce::MemoryOutputStream mo;
    if (!juce::Base64::convertFromBase64(mo, b64)) return false;
    out.replaceAll(mo.getData(), mo.getDataSize());
    return true;
}

/// The Link-side lease decision, PURE so the self-test can hold it without a
/// host or a filesystem. Feed it what the poll found (empty fileId = no
/// file); it tells the caller what to do and tracks the one piece of memory
/// that matters: a lease that EXPIRED must not re-engage when its writer
/// thaws and resumes renewing. The Link restored itself and owns the audio
/// again (the Link wins); the dead id is remembered and only a NEW leaseId
/// -- a deliberate new session -- can engage.
struct LeaseGate
{
    enum Action {
        None,      // no lease, nothing to do
        Engage,    // take control: save bypass state, bypass slot, publish controlled
        Hold,      // lease alive, keep holding
        Expire,    // renewals stopped: restore, publish, REMEMBER the dead id
        Release,   // file deleted: restore, publish (clean end, nothing remembered)
    };

    juce::String activeId;     // empty = not engaged
    int          activeSlot1 = 0;   // 1-based, valid while engaged
    juce::String deadId;       // last EXPIRED id; never engages again

    Action poll(const juce::String& fileId, int fileSlot1, double ageMs)
    {
        const bool fresh = fileId.isNotEmpty() && ageMs < kLeaseExpireMs;
        if (activeId.isEmpty())
        {
            // A stale file engages nothing: its writer is already gone.
            if (!fresh || fileId == deadId) return None;
            activeId = fileId; activeSlot1 = fileSlot1;
            return Engage;
        }
        if (fileId.isEmpty())
        { activeId.clear(); activeSlot1 = 0; return Release; }
        if (fileId == activeId && fresh) return Hold;
        // Renewals stopped (stale), or a DIFFERENT id appeared (a new main
        // session started without a clean release, e.g. after a crash and
        // relaunch): either way THIS lease is over. Restore first; a new id
        // engages on the next poll, through the empty-activeId arm above,
        // so the restore and the engage can never interleave.
        deadId = activeId;
        activeId.clear(); activeSlot1 = 0;
        return Expire;
    }
};

// =============================================================================
//  Rack lock — racklock-<uid>.json (21 Aug 2026)
//
//  The UI-only ownership claim from RACK_BORROW_REQUIREMENTS §4: while a main
//  plugin's editor is actively SHOWING a Link's rack in its Chain tab, that
//  Link's own rack UI goes read-only. Same thinking as the edit lease —
//  heartbeat (renew ~1s), expiry (3s), processor-renewed / editor-gated,
//  deleted on clean release — but a SIBLING file, never a field in the
//  sidecar: the clearing path must work independently of the state file,
//  because clearing is what has to work when everything else has gone wrong.
//  NEVER an audio change: no bypass, no Active toggle, nothing in a graph.
// =============================================================================
static constexpr double kRackLockRenewMs   = 1000.0;
static constexpr double kRackLockExpireMs  = 3000.0;
// Reverse contention: a LOCAL rack edit on the Link inside this window makes
// the main wait (and auto-acquire when quiet). Recency, not window state —
// Link windows sit open idle all day.
static constexpr double kRackLockRecencyMs = 10000.0;

inline juce::String racklockPath(const juce::String& dir, const juce::String& uid)
{
    return dir + "racklock-" + uid + ".json";
}

// ONE author for the file format, both sides and the test read/write through
// these. owner is a display name for the overlay ("Selected in the rack on
// <owner>"), lockId is the identity FCFS compares.
inline void writeRackLockFile(const juce::String& dir, const juce::String& uid,
                              const juce::String& lockId, const juce::String& owner)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("v",      1);
    o->setProperty("lockId", lockId);
    o->setProperty("owner",  owner);
    o->setProperty("tMs",    juce::Time::currentTimeMillis());
    juce::File(racklockPath(dir, uid))
        .replaceWithText(juce::JSON::toString(juce::var(o), true));
}

inline bool readRackLockFile(const juce::String& dir, const juce::String& uid,
                             juce::String& lockIdOut, juce::String& ownerOut,
                             double& ageMsOut)
{
    lockIdOut.clear(); ownerOut.clear(); ageMsOut = 1.0e12;   // absent = infinitely stale
    juce::File f(racklockPath(dir, uid));
    if (!f.existsAsFile()) return false;
    auto v = juce::JSON::parse(f.loadFileAsString());
    auto* o = v.getDynamicObject();
    if (o == nullptr || (int) o->getProperty("v") != 1) return false;
    lockIdOut = o->getProperty("lockId").toString();
    ownerOut  = o->getProperty("owner").toString();
    ageMsOut  = (double) juce::Time::currentTimeMillis()
                  - (double) (juce::int64) o->getProperty("tMs");
    return true;
}

// The PURE decisions, LeaseGate-style, so racklock_test can pin them with no
// processor in the room.
struct RackLock
{
    // What the file says, from any reader's point of view. myId empty (the
    // Link side never owns) makes any fresh claim Other.
    enum class Claim { Free, Mine, Other };
    static Claim read(const juce::String& fileId, double ageMs, const juce::String& myId)
    {
        if (fileId.isEmpty() || ageMs >= kRackLockExpireMs) return Claim::Free;
        return (myId.isNotEmpty() && fileId == myId) ? Claim::Mine : Claim::Other;
    }

    // The main's acquire decision each renew tick. lastEditMs is the newest
    // slot stamp from the sidecar (0 = no local edit ever recorded).
    enum class Acquire {
        Take,          // write/renew the file: we hold it
        WaitRecency,   // the Link was edited moments ago; clears by waiting
        HeldByOther,   // FCFS: another main holds it; wait for release/expiry
    };
    static Acquire decide(Claim claim, bool alreadyHolding,
                          double nowMs, double lastEditMs)
    {
        // A fresh FOREIGN id always wins, even over a holder — it can only
        // appear if our renewals lapsed past expiry, and two writers is the
        // one state this file exists to prevent. Yield; FCFS re-runs.
        if (claim == Claim::Other) return Acquire::HeldByOther;
        if (alreadyHolding)        return Acquire::Take;   // renewal
        if (lastEditMs > 0.0 && nowMs - lastEditMs < kRackLockRecencyMs)
            return Acquire::WaitRecency;
        return Acquire::Take;
    }
};

inline void writeRackSidecar(const juce::String& dir, const RackSidecar& rc)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty("v",         1);
    obj->setProperty("uid",       rc.uid);
    obj->setProperty("name",      rc.name);
    obj->setProperty("revision",  rc.revision);
    obj->setProperty("masterWet", rc.masterWet);
    obj->setProperty("preGainDb",        rc.preGainDb);
    obj->setProperty("preGainUserSet",   rc.preGainUserSet);
    obj->setProperty("preGainInputKnown", rc.preGainInputKnown);
    if (rc.borrowCapable) obj->setProperty("borrowCapable", true);
    if (rc.structureEditCapable) obj->setProperty("structureEditCapable", true);
    if (rc.inContextCapable) obj->setProperty("inContextCapable", true);
    if (rc.muteEngaged) obj->setProperty("muteEngaged", true);
    juce::Array<juce::var> slots;
    for (const auto& s : rc.slots)
    {
        auto* so = new juce::DynamicObject();
        so->setProperty("name",     s.name);
        so->setProperty("format",   s.format);
        so->setProperty("settings", s.settings);
        so->setProperty("bypassed", s.bypassed);
        so->setProperty("wet",      s.wet);
        // Identity for the fingerprint union (additive at v:1, written only
        // when known). See RackSidecarSlot.
        if (s.fp.isNotEmpty())      so->setProperty("fp",      s.fp);
        if (s.uid.isNotEmpty())     so->setProperty("uid",     s.uid);
        if (s.version.isNotEmpty()) so->setProperty("version", s.version);
        // ADDITIVE AT v:1, and it must stay that way. `v` is an EXACT-match
        // reject in readRackSidecar below, not a minimum, so publishing v:2
        // would make an older main plugin discard the WHOLE sidecar and lose
        // this rack's names, bypass and wet as well as the curve it does not
        // understand. A new key at v:1 is simply never read by an old reader.
        // KEY NAME: deliberately NOT "eqCurve", which this codebase already
        // uses as a JSON key for something else entirely (ReferenceAnalyser's
        // reference-track curve, in a different document). Two unrelated keys
        // sharing a name is a trap for whoever greps next. "eqMagDb" also
        // says what the numbers ARE: magnitudes in dB, not band settings.
        if ((int) s.curveDeciDb.size() == kEqCurvePoints)
        {
            juce::Array<juce::var> pts;
            for (auto d : s.curveDeciDb) pts.add ((int) d);
            so->setProperty("eqMagDb", pts);
        }
        if (s.controlled)
            so->setProperty("controlled", true);
        // Additive at v:1, written only when a local edit has ever happened.
        if (s.lastEditMs > 0.0)
            so->setProperty("lastEditMs", s.lastEditMs);
        slots.add(juce::var(so));
    }
    obj->setProperty("slots", slots);
    juce::File(rackSidecarPath(dir, rc.uid))
        .replaceWithText(juce::JSON::toString(juce::var(obj), true));
}

inline RackSidecar readRackSidecar(const juce::String& dir, const juce::String& uid)
{
    RackSidecar rc;
    juce::File f(rackSidecarPath(dir, uid));
    if (!f.existsAsFile()) return rc;                      // rack unknown
    auto v = juce::JSON::parse(f.loadFileAsString());
    auto* obj = v.getDynamicObject();
    if (obj == nullptr || (int)obj->getProperty("v") != 1) return rc;
    rc.uid       = obj->getProperty("uid").toString();
    rc.name      = obj->getProperty("name").toString();
    rc.revision  = (int)obj->getProperty("revision");
    rc.masterWet = obj->hasProperty("masterWet")
                     ? (float)(double)obj->getProperty("masterWet") : 1.0f;
    rc.preGainDb        = obj->hasProperty("preGainDb") ? (float)(double)obj->getProperty("preGainDb") : 0.0f;
    rc.preGainUserSet   = obj->hasProperty("preGainUserSet") && (bool)obj->getProperty("preGainUserSet");
    rc.preGainInputKnown = obj->hasProperty("preGainInputKnown") && (bool)obj->getProperty("preGainInputKnown");
    rc.borrowCapable     = obj->hasProperty("borrowCapable") && (bool)obj->getProperty("borrowCapable");
    rc.structureEditCapable = obj->hasProperty("structureEditCapable") && (bool)obj->getProperty("structureEditCapable");
    rc.inContextCapable = obj->hasProperty("inContextCapable") && (bool)obj->getProperty("inContextCapable");
    rc.muteEngaged = obj->hasProperty("muteEngaged") && (bool)obj->getProperty("muteEngaged");
    if (auto* arr = obj->getProperty("slots").getArray())
        for (auto& sv : *arr)
            if (auto* so = sv.getDynamicObject())
            {
                RackSidecarSlot s;
                s.name     = so->getProperty("name").toString();
                s.format   = so->getProperty("format").toString();
                s.settings = so->getProperty("settings").toString();
                s.bypassed = (bool)so->getProperty("bypassed");
                s.wet      = so->hasProperty("wet")
                               ? (float)(double)so->getProperty("wet") : 1.0f;
                s.fp       = so->getProperty("fp").toString();
                s.uid      = so->getProperty("uid").toString();
                s.version  = so->getProperty("version").toString();
                // Accepted ONLY at the exact published length. A short or long
                // array is a version this reader does not understand, and half
                // a curve drawn as a whole one would misplace every frequency
                // on it. Anything else leaves curveDeciDb empty, which the
                // mixer renders as NO SLOT rather than as a flat response.
                if (auto* ca = so->getProperty("eqMagDb").getArray())
                    if (ca->size() == kEqCurvePoints)
                    {
                        s.curveDeciDb.reserve ((size_t) kEqCurvePoints);
                        for (auto& pv : *ca)
                            s.curveDeciDb.push_back ((int16_t) juce::jlimit (
                                -kEqCurveClampDeciDb, kEqCurveClampDeciDb, (int) pv));
                    }
                s.controlled = so->hasProperty("controlled")
                                 && (bool) so->getProperty("controlled");
                s.lastEditMs = so->hasProperty("lastEditMs")
                                 ? (double) so->getProperty("lastEditMs") : 0.0;
                if (s.name.isNotEmpty()) rc.slots.push_back(std::move(s));
            }
    rc.valid = rc.uid == uid && rc.revision >= 0;
    return rc;
}

// =============================================================================
//  Meter frames (registry v2)
// =============================================================================

/// Publish a frame (Link side, ~10Hz, message thread). Seqlock: seq goes odd
/// during the payload write, even when stable.
inline void publishMeterFrame(void* regMap, int slotIdx, const LinkMeterFrame& f)
{
    if (!regMap || slotIdx < 0 || slotIdx >= kRegMaxSlots) return;
    LinkMeterFrame* dst = meterFrames(regMap) + slotIdx;
    const uint32_t s = loadRelaxed(&dst->seq) & ~1u;   // last stable seq
    storeRelease(&dst->seq, s + 1);                    // odd: writing
    std::memcpy(reinterpret_cast<uint8_t*>(dst) + sizeof(uint32_t),
                reinterpret_cast<const uint8_t*>(&f) + sizeof(uint32_t),
                sizeof(LinkMeterFrame) - sizeof(uint32_t));
    storeRelease(&dst->seq, s + 2);                    // even: stable
}

/// Read a frame (main plugin side). Returns false only if a torn read could
/// not be resolved in a few tries (writer mid-flight) — callers just keep
/// their previous copy. out.seq is the stable sequence number: an unchanged
/// seq across reads means NO new frame (staleness detection).
inline bool readMeterFrame(void* regMap, int slotIdx, LinkMeterFrame& out)
{
    if (!regMap || slotIdx < 0 || slotIdx >= kRegMaxSlots) return false;
    const LinkMeterFrame* src = meterFrames(regMap) + slotIdx;
    for (int tries = 0; tries < 4; ++tries)
    {
        const uint32_t s1 = loadAcquire(&src->seq);
        if (s1 & 1u) continue;
        LinkMeterFrame tmp;
        std::memcpy(&tmp, src, sizeof(LinkMeterFrame));
        const uint32_t s2 = loadAcquire(&src->seq);
        if (s1 == s2) { out = tmp; out.seq = s1; return true; }
    }
    return false;
}

} // namespace LinkShm
