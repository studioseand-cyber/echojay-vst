#pragma once

// ===========================================================================
// THE REFERENCE INDEX (12 Sep 2026)
//
// The on-disk library of reference tracks: what they are, where they live, and
// what was measured about them. The format is specified in
// HANDOVER/Claude outputs/REFERENCE_INDEX_SCHEMA.md, which is the contract;
// this header is that contract expressed as types, and where the two disagree
// the schema document is right and this is a bug.
//
// NO CALLERS YET, DELIBERATELY. Commit 1 of Phase 1 makes the format real and
// pinnable before anything depends on it, the same shape as EJMisdialReport.h.
// Commit 2 starts writing it; commit 3 starts trusting it.
//
// WHY THIS EXISTS AT ALL. Today a reference is a path in the plugin's state
// blob, re-analysed from audio on every load, and its measurements are written
// into preset JSON and read by nothing. A file that moves takes its numbers
// with it and vanishes from the list without a word. The index is what lets a
// measurement outlive the file it came from.
//
// Header-only inline in the manner of EJCaptureGuard.h and EJSpectralEvidence.h,
// so tools/mapfps_test drives the SHIPPED parser rather than a copy of it.
// ===========================================================================

#include <JuceHeader.h>
#include <array>
#include <mutex>
#include <chrono>
#include <vector>

namespace echojay
{

// ---------------------------------------------------------------------------
// VERSIONS. Two numbers that move for different reasons and must not be
// conflated: schema is the SHAPE of the document, measurementEpoch is the
// MEANING of the numbers inside it.
// ---------------------------------------------------------------------------

/** The document shape this build writes and fully understands. */
inline constexpr int kRefIndexSchema = 1;

/** The meaning of a stored measurement. Bumped when a number stops meaning what
    it meant, never when a field is merely renamed or added.

    EPOCH 1: eqCurve is the arithmetic mean of the MeterEngine spectrum across
    every analysis block of the whole file, `reduction` is recorded beside it,
    and macroBandDb is ABSENT because nothing accumulates it yet.

    Phase 0b would have bumped this: before it, the reference side of a
    comparison used the meter's ballistic reading after the final block, so a
    stored number from then would be a 150 ms fade out wearing the same key as a
    whole-file average. Same field, different meaning. There was no index then,
    which is the only reason epoch 1 is the first rather than the second.

    Phase 1b WILL bump this to 2, when macroBandDb becomes an accumulated
    whole-window measurement rather than absent. */
inline constexpr int kRefMeasurementEpoch = 1;

/** The library cap. An add beyond it REFUSES and names what to remove; nothing
    is ever evicted, because an eviction is a thing the user put there
    disappearing without their deciding it should. */
inline constexpr int kRefIndexMaxEntries = 200;

// ---------------------------------------------------------------------------
// AVAILABILITY. A missing file is a STATE, not a deletion: the entry keeps its
// name and its measurements and stays usable for comparison, losing only
// playback. That is the whole of why indexing in place is safe.
// ---------------------------------------------------------------------------

enum class RefAvailability { Present = 0, Missing, Unreadable };

inline const char* refAvailabilityKey (RefAvailability a) noexcept
{
    switch (a)
    {
        case RefAvailability::Present:    return "present";
        case RefAvailability::Missing:    return "missing";
        case RefAvailability::Unreadable: return "unreadable";
    }
    return "missing";
}

/** Unknown states read as Missing rather than Present: the safe direction is to
    decline playback, never to offer a file we cannot vouch for. */
inline RefAvailability refAvailabilityFromKey (const juce::String& k) noexcept
{
    if (k == "present")    return RefAvailability::Present;
    if (k == "unreadable") return RefAvailability::Unreadable;
    return RefAvailability::Missing;
}

/** True when the file can be read right now: the only state that may be played
    or re-analysed. */
inline bool refIsPlayable (RefAvailability a) noexcept
{
    return a == RefAvailability::Present;
}

// ---------------------------------------------------------------------------
// THE PIECES OF AN ENTRY
// ---------------------------------------------------------------------------

/** What the file was when it was last analysed. bytes + modifiedAt detect a
    file changed underneath the entry, so stale numbers can be noticed rather
    than trusted. */
struct RefSource
{
    juce::int64  bytes = 0;
    juce::String modifiedAt;          // ISO 8601 UTC, "" = unknown
    double       sampleRate = 0.0;
    int          channels = 0;
    float        durationSeconds = 0.0f;
};

/** The numbers. Every one carries the reduction and window that produced it,
    because a figure without its provenance cannot be checked, which is the
    lesson Phase 0b was spent on.

    hasMacroBands is FALSE at epoch 1 and distinguishes "this build did not
    measure it" from "measured as silence". The absent-key convention alone
    cannot say that, which is why the flag exists rather than a sentinel. */
struct RefMeasurements
{
    bool                  valid = false;        // false = never analysed
    juce::String          reduction;            // "wholeFileAverage"
    float                 windowSeconds = 0.0f;

    float integrated = -100.0f, loudnessRange = 0.0f;
    float truePeakL = -100.0f, truePeakR = -100.0f;
    float crestFactor = 0.0f, width = 0.0f, correlation = 0.0f, dcOffset = 0.0f;
    float rmsL = -100.0f, rmsR = -100.0f, peakL = -100.0f, peakR = -100.0f;

    std::array<float, 64> eqCurve {};
    bool                  hasEqCurve = false;

    std::array<float, 6>  macroBandDb { -120, -120, -120, -120, -120, -120 };
    bool                  hasMacroBands = false;   // epoch 2 onwards
};

struct RefWaveform
{
    int                stride = 4;
    std::vector<float> points;
};

struct RefEntry
{
    juce::String id;                  // "r_" + 16 hex, minted once, never reused
    juce::String name;                // display name, from the filename at add
    juce::String path;                // absolute, where the file LIVES
    juce::String bookmark;            // reserved: security-scoped bookmark, "" = none

    juce::String addedAt, analysedAt, analysedBy;
    int          measurementEpoch = 0;   // the epoch THESE numbers were made under

    RefSource       source;
    RefMeasurements measurements;
    RefWaveform     waveform;

    RefAvailability availability = RefAvailability::Missing;
    juce::String    checkedAt, lastSeenAt;

    /** The entry as it was parsed, so unknown keys written by a newer build
        survive a rewrite by this one. Empty for entries built in memory. */
    juce::var raw;
};

/** A DELETION, RECORDED AT THE DOCUMENT LEVEL RATHER THAN ON THE ENTRY.

    The obvious form, a "deletedAt" flag on the entry, is a silent resurrection
    and OUR OWN RULE 6 IS THE MECHANISM: an older build preserves unknown keys,
    so it keeps the flag, understands nothing by it, lists the entry as LIVE and
    rewrites the file with the tombstone still attached. A field an old build
    DROPPED would be safer than one it keeps.

    Removing the entry from `entries` and recording its id here means an old
    build sees it simply ABSENT and cannot resurrect it by ignorance, while
    `tombstones` survives as an unknown document key. Its behaviour is correct by
    accident, which is the property worth engineering for.

    WHAT IT IS FOR: a peer whose in-memory list still holds the deleted entry
    would union it straight back in. The merge drops any id named here.

    RESIDUAL RISK, STATED: an OLD build merging does not know to drop tombstoned
    ids, so a mixed-version pair can still resurrect one. Accepted rather than
    bumping the schema, which would turn the library read-only for every older
    build to protect a mixed-version case. */
struct RefTombstone
{
    juce::String id;
    juce::String at;          // ISO 8601 UTC
};

/** Collected at 90 days, or at 500 oldest-first. BOTH bounds, because each alone
    fails: age alone lets a delete-and-re-add loop grow the file without limit,
    and a count alone would collect a RECENT tombstone on a busy library, which
    is the one case where it is still doing work. Never collected merely because
    no entry matches the id: that is a tombstone's normal state, not evidence it
    is spent. Collection runs inside the lock, so it never races. */
inline constexpr int kRefTombstoneMaxDays = 90;
inline constexpr int kRefTombstoneMaxCount = 500;

struct ReferenceIndex
{
    int          schema = kRefIndexSchema;
    int          measurementEpoch = kRefMeasurementEpoch;
    juce::String written, writtenBy;
    std::vector<RefEntry> entries;

    /** Deletions. NOTHING EMITS ONE UNTIL COMMIT 4: parse reads them, write
        emits them and merge honours them as of commit 2, so the format is
        settled and exercised before the first real deletion exists. That is the
        difference between a format change and a behaviour change. */
    std::vector<RefTombstone> tombstones;

    /** Set when the file on disk was written by a NEWER build than this one.
        The document is used read only and NOTHING may write back to it: not a
        migration, not a repair, not a touch of checkedAt. Refusing outright
        would strand a user who opened the newer build once; writing anyway
        would destroy fields this build cannot see. */
    bool readOnly = false;

    /** The schema actually found on disk, which may exceed kRefIndexSchema. */
    int diskSchema = kRefIndexSchema;

    juce::var raw;                    // document-level unknown keys
};

// ---------------------------------------------------------------------------
// IDENTITY
// ---------------------------------------------------------------------------

/** "r_" + 16 hex from a random 64-bit value. NOT derived from the path: under
    the index-in-place decision a path is a thing the user changes with a drag,
    and an id that moved with it would break every slot holding one. Not derived
    from content either: hashing costs a full read per add and would fuse two
    copies of one master into a single entry, which is not what was asked for. */
inline juce::String newReferenceId()
{
    auto& rng = juce::Random::getSystemRandom();
    const juce::uint64 hi = (juce::uint64) (juce::uint32) rng.nextInt();
    const juce::uint64 lo = (juce::uint64) (juce::uint32) rng.nextInt();
    return "r_" + juce::String::toHexString ((juce::int64) ((hi << 32) ^ lo))
                      .paddedLeft ('0', 16);
}

/** The dedupe key: the normalised absolute path. Two locations are two entries,
    deliberately, because the index records files WHERE THEY LIVE and two paths
    are two things the user can rename and lose independently. Collapsing them
    would mean silently discarding one, which is the copy-and-overwrite failure
    in a new place. */
inline juce::String refPathKey (const juce::String& path)
{
    return juce::File::isAbsolutePath (path)
             ? juce::File (path).getFullPathName() : path;
}

/** Index of the entry holding this path, or -1. */
inline int refFindByPath (const ReferenceIndex& ix, const juce::String& path)
{
    const auto key = refPathKey (path);
    for (int i = 0; i < (int) ix.entries.size(); ++i)
        if (refPathKey (ix.entries[(size_t) i].path) == key) return i;
    return -1;
}

inline int refFindById (const ReferenceIndex& ix, const juce::String& id)
{
    for (int i = 0; i < (int) ix.entries.size(); ++i)
        if (ix.entries[(size_t) i].id == id) return i;
    return -1;
}

// ---------------------------------------------------------------------------
// EPOCH POLICY
// ---------------------------------------------------------------------------

/** An entry below the running epoch whose file is PRESENT is re-analysed: the
    audio is there, the cost is one pass, and a current number beats an old one. */
inline bool refNeedsReanalysis (const RefEntry& e, int runningEpoch = kRefMeasurementEpoch)
{
    if (! refIsPlayable (e.availability)) return false;
    return ! e.measurements.valid || e.measurementEpoch < runningEpoch;
}

/** An entry below the running epoch whose file is GONE is stale and STILL
    USABLE. An epoch-1 eqCurve is a whole-file average whatever came later, and
    it still answers a tonal question; what it cannot do is answer one only the
    new epoch's fields support. Discarding it because the definition moved would
    lose the user a reference they can legitimately still compare against. */
inline bool refIsStale (const RefEntry& e, int runningEpoch = kRefMeasurementEpoch)
{
    return e.measurements.valid
        && e.measurementEpoch < runningEpoch
        && ! refIsPlayable (e.availability);
}

/** Distinct from stale: no numbers at all, at the current epoch. What a path
    that resolved to nothing becomes on first open of an old project. */
inline bool refIsUnmeasured (const RefEntry& e)
{
    return ! e.measurements.valid;
}

/** The cap refuses; it never evicts. Empty string = the add may proceed. */
inline juce::String refAddRefusal (const ReferenceIndex& ix)
{
    if ((int) ix.entries.size() < kRefIndexMaxEntries) return {};
    return "The reference library is full at " + juce::String (kRefIndexMaxEntries)
         + " references. Remove one before adding another; nothing is removed "
           "automatically.";
}

// ---------------------------------------------------------------------------
// READ
// ---------------------------------------------------------------------------

namespace detail
{
    inline float  rdF (const juce::var& o, const char* k, float d)
    { auto* p = o.getDynamicObject(); return p && p->hasProperty (k) ? (float) (double) p->getProperty (k) : d; }
    inline double rdD (const juce::var& o, const char* k, double d)
    { auto* p = o.getDynamicObject(); return p && p->hasProperty (k) ? (double) p->getProperty (k) : d; }
    inline int    rdI (const juce::var& o, const char* k, int d)
    { auto* p = o.getDynamicObject(); return p && p->hasProperty (k) ? (int) p->getProperty (k) : d; }
    inline juce::String rdS (const juce::var& o, const char* k)
    { auto* p = o.getDynamicObject(); return p && p->hasProperty (k) ? p->getProperty (k).toString() : juce::String(); }
    inline juce::var rdV (const juce::var& o, const char* k)
    { auto* p = o.getDynamicObject(); return p ? p->getProperty (k) : juce::var(); }
}

inline RefEntry refEntryFromVar (const juce::var& v)
{
    using namespace detail;
    RefEntry e;
    e.raw = v;
    e.id         = rdS (v, "id");
    e.name       = rdS (v, "name");
    e.path       = rdS (v, "path");
    e.bookmark   = rdS (v, "bookmark");
    e.addedAt    = rdS (v, "addedAt");
    e.analysedAt = rdS (v, "analysedAt");
    e.analysedBy = rdS (v, "analysedBy");
    e.measurementEpoch = rdI (v, "measurementEpoch", 0);

    const auto sv = rdV (v, "source");
    e.source.bytes           = (juce::int64) rdD (sv, "bytes", 0.0);
    e.source.modifiedAt      = rdS (sv, "modifiedAt");
    e.source.sampleRate      = rdD (sv, "sampleRate", 0.0);
    e.source.channels        = rdI (sv, "channels", 0);
    e.source.durationSeconds = rdF (sv, "durationSeconds", 0.0f);

    const auto mv = rdV (v, "measurements");
    if (mv.getDynamicObject() != nullptr)
    {
        auto& m = e.measurements;
        m.reduction     = rdS (mv, "reduction");
        m.windowSeconds = rdF (mv, "windowSeconds", 0.0f);

        const auto tv = rdV (mv, "meters");
        m.integrated    = rdF (tv, "integrated",    -100.0f);
        m.loudnessRange = rdF (tv, "loudnessRange",    0.0f);
        m.truePeakL     = rdF (tv, "truePeakL",     -100.0f);
        m.truePeakR     = rdF (tv, "truePeakR",     -100.0f);
        m.crestFactor   = rdF (tv, "crestFactor",      0.0f);
        m.width         = rdF (tv, "width",            0.0f);
        m.correlation   = rdF (tv, "correlation",      0.0f);
        m.dcOffset      = rdF (tv, "dcOffset",         0.0f);
        m.rmsL          = rdF (tv, "rmsL",          -100.0f);
        m.rmsR          = rdF (tv, "rmsR",          -100.0f);
        m.peakL         = rdF (tv, "peakL",         -100.0f);
        m.peakR         = rdF (tv, "peakR",         -100.0f);

        if (auto* a = rdV (mv, "eqCurve").getArray())
        {
            const int n = juce::jmin (64, a->size());
            for (int i = 0; i < n; ++i) m.eqCurve[(size_t) i] = (float) (double) (*a)[i];
            m.hasEqCurve = (n == 64);
        }
        // null, absent, or the wrong length all read as "not measured", which is
        // what epoch 1 writes and what a partial write would leave.
        if (auto* a = rdV (mv, "macroBandDb").getArray())
            if (a->size() == 6)
            {
                for (int i = 0; i < 6; ++i) m.macroBandDb[(size_t) i] = (float) (double) (*a)[i];
                m.hasMacroBands = true;
            }

        m.valid = m.hasEqCurve && m.reduction.isNotEmpty();
    }

    const auto wv = rdV (v, "waveform");
    if (wv.getDynamicObject() != nullptr)
    {
        e.waveform.stride = juce::jmax (1, rdI (wv, "stride", 4));
        if (auto* a = rdV (wv, "points").getArray())
            for (auto& p : *a) e.waveform.points.push_back ((float) (double) p);
    }

    const auto av = rdV (v, "availability");
    e.availability = refAvailabilityFromKey (rdS (av, "state"));
    e.checkedAt    = rdS (av, "checkedAt");
    e.lastSeenAt   = rdS (av, "lastSeenAt");
    return e;
}

/** Parse. A document whose schema EXCEEDS this build's comes back readOnly:
    every field this build understands is used, and nothing may write to it. */
inline ReferenceIndex parseReferenceIndex (const juce::String& json)
{
    ReferenceIndex ix;
    const auto root = juce::JSON::parse (json);
    if (root.getDynamicObject() == nullptr) return ix;   // empty, schema current

    ix.raw              = root;
    ix.diskSchema       = detail::rdI (root, "schema", kRefIndexSchema);
    ix.schema           = ix.diskSchema;
    ix.measurementEpoch = detail::rdI (root, "measurementEpoch", 0);
    ix.written          = detail::rdS (root, "written");
    ix.writtenBy        = detail::rdS (root, "writtenBy");
    ix.readOnly         = ix.diskSchema > kRefIndexSchema;

    if (auto* arr = detail::rdV (root, "entries").getArray())
        for (auto& ev : *arr) ix.entries.push_back (refEntryFromVar (ev));

    if (auto* arr = detail::rdV (root, "tombstones").getArray())
        for (auto& tv : *arr)
        {
            RefTombstone t;
            t.id = detail::rdS (tv, "id");
            t.at = detail::rdS (tv, "at");
            if (t.id.isNotEmpty()) ix.tombstones.push_back (t);
        }
    return ix;
}

/** What the user is told when the file is from a newer build. Said once, plainly,
    and it says what will NOT happen rather than only what did. */
inline juce::String refReadOnlyNotice (const ReferenceIndex& ix)
{
    if (! ix.readOnly) return {};
    return "This reference library was written by a newer version of EchoJay ("
         + (ix.writtenBy.isNotEmpty() ? ix.writtenBy : juce::String ("unknown version"))
         + "). It is being used read only, and changes made here will not be saved.";
}

// ---------------------------------------------------------------------------
// WRITE
// ---------------------------------------------------------------------------

/** Serialise one entry, starting from its parsed form so that keys written by a
    newer build at the SAME schema survive a rewrite by this one. */
inline juce::var refEntryToVar (const RefEntry& e)
{
    // RELEASE, NOT GET. DynamicObject::clone() returns std::unique_ptr, while
    // Ptr is refcounted: Ptr(clone().get()) gives the object TWO owners, the Ptr
    // takes the count 0 to 1, and then the unique_ptr temporary deletes it at the
    // end of the full expression. The Ptr is left dangling and the next write
    // segfaults. Caught by the gate on this header's first run.
    juce::DynamicObject::Ptr o = e.raw.getDynamicObject() != nullptr
        ? juce::DynamicObject::Ptr (e.raw.getDynamicObject()->clone().release())
        : juce::DynamicObject::Ptr (new juce::DynamicObject());

    o->setProperty ("id",   e.id);
    o->setProperty ("name", e.name);
    o->setProperty ("path", e.path);
    o->setProperty ("bookmark", e.bookmark.isEmpty() ? juce::var() : juce::var (e.bookmark));
    o->setProperty ("addedAt",    e.addedAt);
    o->setProperty ("analysedAt", e.analysedAt);
    o->setProperty ("analysedBy", e.analysedBy);
    o->setProperty ("measurementEpoch", e.measurementEpoch);

    juce::DynamicObject::Ptr s (new juce::DynamicObject());
    s->setProperty ("bytes",           (double) e.source.bytes);
    s->setProperty ("modifiedAt",      e.source.modifiedAt);
    s->setProperty ("sampleRate",      e.source.sampleRate);
    s->setProperty ("channels",        e.source.channels);
    s->setProperty ("durationSeconds", e.source.durationSeconds);
    o->setProperty ("source", juce::var (s.get()));

    juce::DynamicObject::Ptr m (new juce::DynamicObject());
    const auto& mm = e.measurements;
    m->setProperty ("reduction",     mm.reduction);
    m->setProperty ("windowSeconds", mm.windowSeconds);
    juce::DynamicObject::Ptr t (new juce::DynamicObject());
    t->setProperty ("integrated", mm.integrated);       t->setProperty ("loudnessRange", mm.loudnessRange);
    t->setProperty ("truePeakL",  mm.truePeakL);        t->setProperty ("truePeakR",     mm.truePeakR);
    t->setProperty ("crestFactor", mm.crestFactor);     t->setProperty ("width",         mm.width);
    t->setProperty ("correlation", mm.correlation);     t->setProperty ("dcOffset",      mm.dcOffset);
    t->setProperty ("rmsL", mm.rmsL);                   t->setProperty ("rmsR",          mm.rmsR);
    t->setProperty ("peakL", mm.peakL);                 t->setProperty ("peakR",         mm.peakR);
    m->setProperty ("meters", juce::var (t.get()));

    if (mm.hasEqCurve)
    {
        juce::Array<juce::var> a;
        for (int i = 0; i < 64; ++i) a.add (mm.eqCurve[(size_t) i]);
        m->setProperty ("eqCurve", a);
    }
    else
        m->setProperty ("eqCurve", juce::var());

    // null until epoch 2, and null is the honest value: it says this build did
    // not measure it, which a floor of -120 would not.
    if (mm.hasMacroBands)
    {
        juce::Array<juce::var> a;
        for (int i = 0; i < 6; ++i) a.add (mm.macroBandDb[(size_t) i]);
        m->setProperty ("macroBandDb", a);
    }
    else
        m->setProperty ("macroBandDb", juce::var());
    o->setProperty ("measurements", juce::var (m.get()));

    juce::DynamicObject::Ptr w (new juce::DynamicObject());
    w->setProperty ("stride", e.waveform.stride);
    juce::Array<juce::var> pts;
    for (auto p : e.waveform.points) pts.add (p);
    w->setProperty ("points", pts);
    o->setProperty ("waveform", juce::var (w.get()));

    juce::DynamicObject::Ptr av (new juce::DynamicObject());
    av->setProperty ("state",     refAvailabilityKey (e.availability));
    av->setProperty ("checkedAt", e.checkedAt);
    if (e.lastSeenAt.isNotEmpty()) av->setProperty ("lastSeenAt", e.lastSeenAt);
    o->setProperty ("availability", juce::var (av.get()));

    return juce::var (o.get());
}

/** Serialise the document. A lower schema on disk is migrated forward by being
    written at the current one; a HIGHER one must never reach here, which
    refIndexMayWrite is the guard for. */
inline juce::String writeReferenceIndex (const ReferenceIndex& ix,
                                         const juce::String& nowIso,
                                         const juce::String& version)
{
    juce::DynamicObject::Ptr o = ix.raw.getDynamicObject() != nullptr
        ? juce::DynamicObject::Ptr (ix.raw.getDynamicObject()->clone().release())
        : juce::DynamicObject::Ptr (new juce::DynamicObject());

    o->setProperty ("schema",           kRefIndexSchema);
    o->setProperty ("measurementEpoch", kRefMeasurementEpoch);
    o->setProperty ("written",          nowIso);
    o->setProperty ("writtenBy",        version);

    juce::Array<juce::var> arr;
    for (const auto& e : ix.entries) arr.add (refEntryToVar (e));
    o->setProperty ("entries", arr);

    juce::Array<juce::var> ts;
    for (const auto& t : ix.tombstones)
    {
        juce::DynamicObject::Ptr d (new juce::DynamicObject());
        d->setProperty ("id", t.id);
        d->setProperty ("at", t.at);
        ts.add (juce::var (d.get()));
    }
    o->setProperty ("tombstones", ts);

    return juce::JSON::toString (juce::var (o.get()), false);
}

/** The write guard. False for a document from a newer build: not a migration,
    not a repair, not a touch of checkedAt. Every write path asks this first. */
inline bool refIndexMayWrite (const ReferenceIndex& ix) noexcept
{
    return ! ix.readOnly;
}

/** Migrate a parsed document forward in memory. Nothing to do at schema 1; the
    function exists so that the CALL SITE is written now, while there is one
    schema, rather than added later by someone who has to guess where it goes. */
inline void migrateReferenceIndex (ReferenceIndex& ix)
{
    if (ix.readOnly) return;              // never touch a newer document
    if (ix.diskSchema >= kRefIndexSchema) { ix.schema = kRefIndexSchema; return; }
    // schema 0 (absent) or a future older version lands here.
    ix.schema = kRefIndexSchema;
}

/** The atomic write, as two paths: caller serialises, writes tmp, renames.
    Named here so every writer spells it the same way. A half written index
    loses every entry rather than one, at exactly the moment the machine was
    least healthy. */
inline juce::File refIndexFile (const juce::File& appDataEchoJayDir)
{
    return appDataEchoJayDir.getChildFile ("reference_index.json");
}

inline juce::File refIndexTempFile (const juce::File& appDataEchoJayDir)
{
    return appDataEchoJayDir.getChildFile ("reference_index.json.tmp");
}

// ===========================================================================
// LOAD, MERGE, COMMIT (commit 2 of Phase 1: writing only, nothing reads yet)
//
// THE DIRECTORY IS ALWAYS A PARAMETER AND IS NEVER RESOLVED HERE.
// EJStateRoot.h and echojay::userAppData() are on the parked merge, not on this
// branch, so ECHOJAY_STATE_HOME isolates nothing here. A function that resolved
// its own path would make every behavioural pin write to the user's real
// library. The shipping caller passes the real directory; the pins pass a
// temporary one. That is what makes the concurrency pins safe to run, and it is
// stronger than relying on an environment variable to keep tests off live data.
// When the merge lands, ONE line at the caller changes and nothing here does.
// ===========================================================================

/** Absent and Unreadable must never collapse into each other. Absent is an
    empty library and is writable. Unreadable means the file EXISTS and could not
    be read or parsed, and it REFUSES to be written over: the cause is usually
    recoverable (a permission, a half-restored backup, a failing disk) and
    destroying it on the next write would make a transient fault permanent. */
enum class RefLoadState { Absent = 0, Loaded, Unreadable };

struct RefLoadResult
{
    RefLoadState   state = RefLoadState::Absent;
    ReferenceIndex index;
    juce::String   message;      // "" unless the user needs telling
};

/** True when a commit may proceed at all. Unreadable and newer-schema both say
    no, for different reasons, and both preserve the file. */
inline bool refMayCommit (const RefLoadResult& r) noexcept
{
    return r.state != RefLoadState::Unreadable && refIndexMayWrite (r.index);
}

inline RefLoadResult loadReferenceIndex (const juce::File& dir)
{
    RefLoadResult out;
    const auto f = refIndexFile (dir);

    if (! f.existsAsFile())
        return out;                                    // Absent, writable, silent

    const auto text = f.loadFileAsString();
    if (text.trim().isEmpty())
    {
        // Exists and is empty: NOT the same as absent. A zero-length file is
        // what a truncating write leaves behind, so it is treated as damage.
        out.state   = RefLoadState::Unreadable;
        out.message = "The reference library file is empty and will not be "
                      "written over. Move it aside to start a new one.";
        return out;
    }

    out.index = parseReferenceIndex (text);
    if (out.index.raw.getDynamicObject() == nullptr)
    {
        out.state   = RefLoadState::Unreadable;
        out.message = "The reference library could not be read. Nothing will be "
                      "saved over it.";
        return out;
    }

    out.state = RefLoadState::Loaded;
    if (out.index.readOnly)
        out.message = refReadOnlyNotice (out.index);
    return out;
}

/** Union by id, then dedupe by path.

    UNION BY id IS SOUND ONLY BECAUSE ids ARE MINTED ONCE AND NEVER DERIVED.
    Reconciling two independently-read lists means deciding whether two entries
    are the same entry; a path-derived id would answer that with mutable data, so
    a file the user moved would read as a different entry and duplicate.

    THE ONE CASE UNION GETS WRONG ALONE: two instances adding the SAME PATH mint
    DIFFERENT ids, so a union keeps both and breaks the dedupe-on-path rule. The
    second stage fixes it deterministically, keeping the earlier addedAt and
    breaking a tie on the smaller id, so two writers racing converge on one file
    instead of alternating.

    IT CANNOT EXPRESS A DELETION, and that is why commit 2 writes on add and on
    analysis but NOT on removal: a union has no way to say "I deleted this", so a
    removal merged against a peer's copy would be resurrected. Tombstones belong
    with the commit that makes the index authoritative, and until then a removal
    is session-only, which is safe because nothing reads the file yet. */
inline void mergeReferenceIndex (ReferenceIndex& onDisk, const ReferenceIndex& mine)
{
    for (const auto& m : mine.entries)
    {
        const int at = m.id.isNotEmpty() ? refFindById (onDisk, m.id) : -1;
        if (at >= 0) onDisk.entries[(size_t) at] = m;   // mine is the later intent
        else         onDisk.entries.push_back (m);
    }

    // Tombstones from BOTH sides, then the drop. Without this a peer holding a
    // deleted entry in memory unions it straight back in, and the deletion
    // survives only until the next peer writes.
    for (const auto& t : mine.tombstones)
    {
        bool have = false;
        for (const auto& e : onDisk.tombstones) if (e.id == t.id) { have = true; break; }
        if (! have) onDisk.tombstones.push_back (t);
    }
    if (! onDisk.tombstones.empty())
    {
        std::vector<RefEntry> live;
        for (const auto& e : onDisk.entries)
        {
            bool dead = false;
            for (const auto& t : onDisk.tombstones) if (t.id == e.id) { dead = true; break; }
            if (! dead) live.push_back (e);
        }
        onDisk.entries = std::move (live);
    }

    std::vector<RefEntry> kept;
    for (const auto& e : onDisk.entries)
    {
        bool placed = false;
        for (auto& k : kept)
        {
            if (refPathKey (k.path) != refPathKey (e.path)) continue;
            // Same path, two ids: earlier addedAt wins, smaller id breaks a tie.
            const bool eWins = e.addedAt.isNotEmpty() && k.addedAt.isNotEmpty()
                                 ? (e.addedAt < k.addedAt
                                      || (e.addedAt == k.addedAt && e.id < k.id))
                                 : (e.id < k.id);
            if (eWins) k = e;
            placed = true;
            break;
        }
        if (! placed) kept.push_back (e);
    }
    onDisk.entries = std::move (kept);
}

struct RefCommitResult
{
    bool         ok = false;
    juce::String message;        // "" on success
    int          entriesWritten = 0;
};

/** The process-wide half of the lock. Several plugin instances in one DAW
    usually share ONE process, so the inter-process lock alone would not
    serialise them. */
/** TIMED, not a plain mutex. std::mutex has no bounded acquire, so the first
    version of this could block the MESSAGE THREAD indefinitely behind a stuck
    writer. The defect surfaced from asking how to pin lock failure: POSIX fcntl
    locks are per-process, so two threads in one process do not contend through
    the inter-process lock, and the only lock a same-process pin can hold is this
    one. Making it observable made it correct. */
inline std::timed_mutex& refIndexProcessMutex()
{
    static std::timed_mutex m;
    return m;
}

// TWO TIMEOUTS, DIFFERENT NUMBERS, BECAUSE THEY GUARD DIFFERENT THINGS. Both are
// taken ON THE MESSAGE THREAD, so the question is not "is it bounded" but "how
// long does the UI stall". Ten seconds is bounded and is a beachball; 3000 ms was
// bounded and is a three second freeze on a save the user did not ask for.
//
//   process mutex   contended only by another thread in THIS process doing the
//                   same JSON serialise and file write, which is single-digit
//                   milliseconds. 50 ms is an order of magnitude of headroom and
//                   is below the threshold where a stall is perceived at all.
//   IPC lock        contended by another PROCESS, which may be a separate DAW
//                   with a cold page cache on a slower volume. Failing at 50 ms
//                   would make contention the NORMAL outcome between instances;
//                   a retry loop that usually fails is worse than a slightly
//                   longer wait that usually succeeds.
//
// Worst case stall, once: 300 ms, then the debounce retries.
inline constexpr int kRefIndexProcessLockMs = 50;
inline constexpr int kRefIndexIpcLockMs     = 250;

/** Kept for the bounded-ness pin, which asserts the property rather than the
    values; it is the larger of the two. */
inline constexpr int kRefIndexLockTimeoutMs = kRefIndexIpcLockMs;

/** The coalescing rule, named so every caller uses one number. A change marks
    dirty; one write follows however many changes arrived. Restoring a session
    with forty references marks dirty forty times and writes once. 2 seconds
    matches the workspace cache's debounce, so there is one answer to "why 2"
    rather than two. A flush on teardown is required: a debounce that never
    fires loses the last change. */
inline constexpr int kRefIndexDebounceMs = 2000;

// ---------------------------------------------------------------------------
// REPEATED FAILURE IS A DIFFERENT EVENT FROM FAILURE.
//
// One refusal with a successful retry two seconds later is not worth telling
// anyone. The same refusal every two seconds forever is a library that never
// saves while nothing says so, which is precisely the shape of defect this
// project keeps paying for: a feature declining to act, correctly, in silence.
//
// What distinguishes them is CONSECUTIVE failure. Five at a 2 second debounce is
// about ten seconds of failing, far longer than healthy contention and short
// enough that the user has not walked away. The elapsed clause catches the case
// where the debounce itself is starved so the count never reaches five.
// ---------------------------------------------------------------------------
inline constexpr int kRefIndexEscalateAfterFailures = 5;
inline constexpr int kRefIndexEscalateAfterMs       = 60000;

/** The retry backoff: 2, 4, 8, 16, capped at 30 seconds. A genuinely stuck peer
    is not helped by being asked twice a second, and the cap means recovery is
    still noticed within half a minute. */
inline int refIndexBackoffMs (int consecutiveFailures) noexcept
{
    if (consecutiveFailures <= 0) return kRefIndexDebounceMs;
    int ms = kRefIndexDebounceMs;
    for (int i = 1; i < consecutiveFailures && ms < 30000; ++i) ms *= 2;
    return juce::jmin (ms, 30000);
}

/** True when the caller should stop promising and start stating. */
inline bool refIndexShouldEscalate (int consecutiveFailures, int msSinceLastSuccess) noexcept
{
    return consecutiveFailures >= kRefIndexEscalateAfterFailures
        || msSinceLastSuccess  >= kRefIndexEscalateAfterMs;
}

/** THE MESSAGE STOPS PROMISING. "Will be saved shortly" that keeps not coming
    true is worse than a refusal, so at escalation it becomes a statement of fact
    with a duration the user can act on. */
inline juce::String refIndexEscalatedMessage (int msSinceLastSuccess)
{
    const int mins = juce::jmax (1, msSinceLastSuccess / 60000);
    return "The reference library has not been saved for "
         + juce::String (mins) + (mins == 1 ? " minute." : " minutes.")
         + " Your references work for this session only.";
}

/** Serialise, re-read, merge, write temp, rename.

    THE EXISTING FILE IS NEVER OPENED FOR WRITING, which is what guarantees it
    survives a failure byte-identical. A failure at any step before the rename
    leaves the original untouched and deletes the temp; a rename does not
    truncate its destination, so a failed rename leaves it untouched too. Same
    directory, so one volume, so the rename is atomic.

    On failure the caller KEEPS its in-memory state: the reference the user just
    added still works this session, only its persistence failed. */
inline RefCommitResult commitReferenceIndex (const juce::File& dir,
                                             const ReferenceIndex& mine,
                                             const juce::String& nowIso,
                                             const juce::String& version)
{
    RefCommitResult r;

    std::unique_lock<std::timed_mutex> processLock (
        refIndexProcessMutex(), std::chrono::milliseconds (kRefIndexProcessLockMs));
    if (! processLock.owns_lock())
    {
        r.message = "Another part of EchoJay is saving the reference library. "
                    "This change will be saved shortly.";
        return r;                       // never falls through into a write
    }

    juce::InterProcessLock ipc ("EchoJayReferenceIndex");
    if (! ipc.enter (kRefIndexIpcLockMs))
    {
        r.message = "Another EchoJay is saving the reference library. "
                    "This change will be saved shortly.";
        return r;                       // the debounce retries; nothing is lost
    }
    struct IpcExit { juce::InterProcessLock& l; ~IpcExit() { l.exit(); } } ipcExit { ipc };

    if (! dir.createDirectory())
    {
        r.message = "The reference library folder could not be created, so the "
                    "library was not saved. Your references work for this "
                    "session only.";
        return r;
    }

    auto fresh = loadReferenceIndex (dir);
    if (! refMayCommit (fresh))
    {
        r.message = fresh.message.isNotEmpty() ? fresh.message
                  : juce::String ("The reference library was not saved.");
        return r;                       // Unreadable or newer schema: preserved
    }

    mergeReferenceIndex (fresh.index, mine);

    const auto json = writeReferenceIndex (fresh.index, nowIso, version);
    auto tmp = refIndexTempFile (dir);
    tmp.deleteFile();
    if (! tmp.replaceWithText (json))
    {
        tmp.deleteFile();
        r.message = "The reference library could not be written, so it was not "
                    "saved. Your references work for this session only.";
        return r;
    }
    if (! tmp.moveFileTo (refIndexFile (dir)))
    {
        tmp.deleteFile();
        r.message = "The reference library could not be replaced, so it was not "
                    "saved. Your references work for this session only.";
        return r;
    }

    r.ok = true;
    r.entriesWritten = (int) fresh.index.entries.size();
    return r;
}

} // namespace echojay
