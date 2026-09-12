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

struct ReferenceIndex
{
    int          schema = kRefIndexSchema;
    int          measurementEpoch = kRefMeasurementEpoch;
    juce::String written, writtenBy;
    std::vector<RefEntry> entries;

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

} // namespace echojay
