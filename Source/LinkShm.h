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
    uint8_t  _pad1[60];

    alignas(64) uint32_t readIdx;
    uint8_t  _pad2[60];
};
static_assert(sizeof(LinkShmHeader) == 192, "");
static_assert(kLinkHdrSize == sizeof(LinkShmHeader), "");

// =============================================================================
//  Registry structs  (layout unchanged)
// =============================================================================

static constexpr uint32_t kRegMagic      = 0xEC4A2002u;
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
    uint8_t  _pad[20];         // 20  → total 128
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
    uint8_t _pad[128 - 4 - 11 * 4 - 6 * 4 - 4 - 8];   // -> 128 (2 cache lines)
};
static_assert(sizeof(LinkMeterFrame) == 128, "LinkMeterFrame must be 128 bytes");

static constexpr size_t kRegSize =
    sizeof(RegistryHeader) + (size_t)kRegMaxSlots * sizeof(RegistrySlot)
                           + (size_t)kRegMaxSlots * sizeof(LinkMeterFrame);

// =============================================================================
//  Atomic helpers  (GCC/Clang builtins — safe on mmap'd memory, no placement-new)
// =============================================================================
namespace LinkShm {

inline uint32_t loadAcquire (const uint32_t* p) { return __atomic_load_n (p, __ATOMIC_ACQUIRE); }
inline uint32_t loadRelaxed (const uint32_t* p) { return __atomic_load_n (p, __ATOMIC_RELAXED); }
inline void     storeRelease(uint32_t* p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
inline bool casStrong(uint32_t* p, uint32_t expected, uint32_t desired)
{
    return __atomic_compare_exchange_n(p, &expected, desired, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
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
//  Low-level file mmap helpers
// =============================================================================

/// Open (or create) a file, truncate to `size`, mmap RW.
/// Returns mapped pointer or nullptr; errno_out receives errno on failure (0 ok).
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
    __atomic_store_n(&hdr->magic, kLinkMagic, __ATOMIC_RELEASE);
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

/// Claim a free slot.  Returns slot index [0..15] or -1 if full.
/// `audioFilename` = makeAudioFilename(linkName), stored so consumer can open it.
inline int claimSlot(void* regMap,
                     const juce::String& displayName,
                     const juce::String& audioFilename,
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
            slots[i].sampleRate  = sr;
            slots[i].numChannels = ch;
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

struct SlotSnapshot {
    int          idx;
    juce::String displayName;
    juce::String audioFilename;  // filename only, e.g. "audio_drums.bin"
    float        sampleRate;
    uint32_t     numChannels;
    uint32_t     heartbeat;
    bool         active = true;  // capture/meter role on
};

inline bool readSlot(void* regMap, int i, SlotSnapshot& out)
{
    if (!regMap || i < 0 || i >= kRegMaxSlots) return false;
    RegistrySlot* slot = regSlots(regMap) + i;
    if (loadAcquire(&slot->inUse) == 0) return false;
    out.idx           = i;
    out.displayName   = juce::String::fromUTF8(slot->displayName);
    out.audioFilename = juce::String::fromUTF8(slot->audioFile);
    out.sampleRate    = slot->sampleRate;
    out.numChannels   = slot->numChannels;
    out.heartbeat     = loadRelaxed(&slot->heartbeat);
    out.active        = loadAcquire(&slot->activeFlag) != 0;
    return true;
}

inline void reapSlot(void* regMap, int i)
{
    if (!regMap || i < 0 || i >= kRegMaxSlots) return;
    storeRelease(&regSlots(regMap)[i].inUse, 0u);
}

// =============================================================================
//  Meter frames (registry v2)
// =============================================================================

inline LinkMeterFrame* meterFrames(void* regMap)
{
    return reinterpret_cast<LinkMeterFrame*>(
        static_cast<uint8_t*>(regMap) + sizeof(RegistryHeader)
        + (size_t) kRegMaxSlots * sizeof(RegistrySlot));
}

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
