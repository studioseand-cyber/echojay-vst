#pragma once
// ===========================================================================
// LevelRecord: THE STORED LEVEL RECORD, one per Link (27 Sep 2026 ruling).
//
// WHY THIS EXISTS. Until this round every chat block - [TRACK LEVELS], the
// [GROUP LEVELS] member lines, the bus [CHAIN LEVELS] header - was composed
// from the LIVE LinkMeterFrame, with a "last good frame" latch as the only
// memory. The strips were not: a strip keeps its own smoothed per-strip state
// (LinkStripState::smInt and its neighbours) which is advanced only on a fresh
// frame and otherwise HOLDS its last value indefinitely. So the two surfaces
// could not agree, and on 27 Sep 15:30 they did not: every strip showed an INT
// (v3_2 -18.9, Main vocal 6 -19.4) while the [GROUP LEVELS] block sent on
// "balance these vocal channels" carried those same two members as having no
// signal. The strip had a hold; the block had a latch whose conditions
// (audioStale == 0, momentary above -70, at least one macro band) describe
// AUDIO PLAYING NOW, not audio heard earlier.
//
// A record is a record of what was heard. It is written while audio flows and
// kept afterwards, across transport stop and start, across the editor closing,
// and across a session being saved and reopened (it rides the owning plugin's
// state and the rack sidecar, so V2 can read it while the Link is quiet or
// parked). It is cleared by ONE thing: an explicit reset by the user.
//
// CONSEQUENCE, ruled: "no signal" means HEARD 0 and nothing else. A quiet Link
// with a record is a member WITH data.
//
// The live frame still feeds the strips. Nothing here changes a meter.
//
// House rules: no em-dashes anywhere in this file.
// ===========================================================================
#include <juce_core/juce_core.h>
#include "EchoJayLevelTally.h"
#include <cmath>
#include <limits>

namespace echojay {

struct LevelRecord
{
    // THE RULED FIELDS: INT, SHORTMAX, SHORT90, PEAK, PSR (derived), HEARD, last-updated.
    // PLUS the fields the existing wire lines already carry, because the token set on those lines is a CONTRACT
    // the server parses and this round changes where the numbers come from, not what they are called. MOM and
    // SHORT are the last LIVE values seen while audio was flowing: they describe a moment, and the line's AGE
    // token is how a reader knows which moment. p10/p50/p90 and crest serve the bus [CHAIN LEVELS] header.
    static constexpr float kNone = -200.0f;   // "not in this record", never a level
    static bool has (float v) noexcept { return v > -99.0f; }

    bool  valid = false;
    float momDb    = kNone;
    float shortDb  = kNone;
    float shortMaxDb = kNone;
    float short90Db  = kNone;
    float intLufs    = kNone;
    float peakDbTp   = kNone;
    float heardSeconds = 0.0f;
    // Whether HEARD is a MEASUREMENT or simply absent. An older Link publishes figures and no heard time at all,
    // and printing "HEARD 0" for it would say "nothing was heard" about a channel we have an INT for. The ruling
    // that "no signal means HEARD 0" is a statement about a record that KNOWS its heard time.
    bool  heardKnown = false;
    float p10 = kNone, p50 = kNone, p90 = kNone, crestDb = kNone;
    // The 3 s true peak, kept for ONE reason: PSR's fallback. Ruled 25 Sep 2026 - PSR is PEAK minus SHORTMAX where
    // there is a SHORTMAX, and the 3 s pair (shortTermTP minus SHORT) for a Link that does not publish one. The
    // record has to carry both terms or the fallback would disappear with the frame it used to be computed from.
    float shortTpDb = kNone;
    // 21t-j (28 Sep 2026): RENAMED, because what it means changed. It used to say "these figures were measured
    // BEFORE the owner's gain stage", and its only downstream use was the "(POST-TRIM...)" warning on a block line
    // for an older Link that metered after its gain. Since the record is now taken AFTER the trim in every case
    // (through frameLoudnessAsHeard, the same conversion the strip uses), the honest name is what it now asserts:
    // the figures are as-heard, the conversion has been applied, and no reader has to think about the trim again.
    bool  asHeard = true;
    juce::int64 updatedMs = 0;         // juce::Time::currentTimeMillis() of the last update

    /** PSR is DERIVED, never stored twice: PEAK minus SHORTMAX, the whole-programme figure the server's transient
        rule asks about. Absent whenever either side is absent - there is no half of a ratio. */
    float psrDb() const noexcept
    {
        if (has (peakDbTp) && has (shortMaxDb)) return peakDbTp - shortMaxDb;     // whole programme, ruled first
        if (has (shortTpDb) && has (shortDb))   return shortTpDb - shortDb;       // the 3 s pair, the ruled fallback
        return kNone;
    }

    /** How old the record is. Negative clocks (a session file from a machine whose clock has moved) read as 0
        rather than as a record from the future. */
    int ageSeconds (juce::int64 nowMs) const noexcept
    { return (updatedMs <= 0) ? 0 : (int) juce::jmax ((juce::int64) 0, (nowMs - updatedMs) / 1000); }

    /** THE RULE, in one place: a record with no gated audio behind it is no signal. Not quiet, not settling. */
    bool hasFigures() const noexcept
    { return valid && (has (intLufs) || has (shortDb) || has (shortMaxDb) || has (short90Db) || has (peakDbTp)); }
    bool heardAnything() const noexcept
    { return heardKnown ? (valid && heardSeconds > 0.0f) : hasFigures(); }

    /** ONE UPDATE PATH from a tally snapshot, used by whoever owns the tally (the Link for its own channel, V2 for
        its own bus). A field is taken only when the snapshot HAS it, so a window that has not closed cannot erase
        a figure measured a minute ago: this is a record, and forgetting is what the live frame is for. */
    void updateFromTally (const LevelTally::Snapshot& s, juce::int64 nowMs, bool measuredPreTrim = true)
    {
        bool any = false;
        auto take = [&any] (float& dst, float v) { if (v > -99.0f && v == v) { dst = v; any = true; } };
        take (shortDb,    s.shortTermDb);
        take (shortMaxDb, s.maxShortTermDb);
        take (short90Db,  s.shortTermP90Db);
        take (intLufs,    s.levelDb);
        take (peakDbTp,   s.truePeakDb);
        take (p10, s.p10); take (p50, s.p50); take (p90, s.p90);
        take (crestDb, s.crestDb);
        if (s.heardSeconds > heardSeconds) { heardSeconds = s.heardSeconds; any = true; }
        heardKnown = true;                     // a tally always knows how long it has heard
        asHeard = measuredPreTrim;   // a tally-fed record is as-heard when its owner says the tap is post-trim
        if (any) { valid = true; updatedMs = nowMs; }
    }

    /** THE ONE COMPOSER for every block line (21t-i). The token names, their order and their number format are a
        CONTRACT the server parses, so they are reproduced here character for character from the line this replaces:
        MOM, SHORT, SHORTMAX, SHORT90, INT, PEAK, PSR, HEARD, and then AGE. Absent reads "no reading" and never a
        floor value. AGE is the ONE new token this round: how old the record is, in whole seconds, so a figure kept
        across a transport stop cannot be read as a figure measured a moment ago. It sits last in the machine list,
        additively, which is how short90 was added and why an older parser is unaffected.

        AGE IS RULED (27 Sep 2026): "AGE <seconds>", an integer count of seconds since the record was last updated,
        appended after HEARD on every [GROUP LEVELS] member line and on [TRACK LEVELS]. No unit suffix and no
        minutes or hours form - there is nothing to parse but the integer. B parses it from its next deploy, and
        wire_tokens_guard pins the spelling. It is NOT added to the [CHAIN LEVELS] header, which the ruling does
        not cover and whose tokens B already parses. */
    juce::String tokens (juce::int64 nowMs) const
    {
        auto num = [] (float v) { return v > -99.0f ? juce::String (v, 1) : juce::String ("no reading"); };
        return juce::String ("MOM ") + num (momDb) + ", "
             + "SHORT "    + num (shortDb) + ", "
             + "SHORTMAX " + num (shortMaxDb) + ", "
             + "SHORT90 "  + num (short90Db) + ", "
             + "INT "      + num (intLufs) + ", "
             + "PEAK "     + num (peakDbTp) + ", "
             + "PSR "      + num (psrDb()) + ", "
             + "HEARD "    + (heardKnown ? juce::String ((int) (heardSeconds + 0.5f))
                                            : juce::String ("no reading")) + ", "
             + "AGE "      + juce::String (ageSeconds (nowMs))
             // The only downstream use: a record that has NOT been converted says so on the line, so a reader can
             // see that this one figure is not as-heard. Every record this build writes is converted, so the
             // clause is now the absence-case only - and a guard asserts it never appears.
             + (asHeard ? juce::String() : juce::String (" (NOT AS HEARD: this record was not converted to the "
                                                        "point the strip meters)"));
    }

    juce::var toVar() const
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("v", 1);
        o->setProperty ("mom", (double) momDb);          o->setProperty ("short", (double) shortDb);
        o->setProperty ("shortMax", (double) shortMaxDb); o->setProperty ("short90", (double) short90Db);
        o->setProperty ("int", (double) intLufs);         o->setProperty ("peak", (double) peakDbTp);
        o->setProperty ("heard", (double) heardSeconds);
        o->setProperty ("heardKnown", heardKnown);
        o->setProperty ("p10", (double) p10); o->setProperty ("p50", (double) p50); o->setProperty ("p90", (double) p90);
        o->setProperty ("crest", (double) crestDb);
        o->setProperty ("shortTp", (double) shortTpDb);
        o->setProperty ("asHeard", asHeard);
        o->setProperty ("updatedMs", updatedMs);
        return juce::var (o);
    }
    static LevelRecord fromVar (const juce::var& v)
    {
        LevelRecord r;
        auto* o = v.getDynamicObject();
        if (o == nullptr) return r;
        auto get = [o] (const char* k, float def) -> float
        { return o->hasProperty (k) ? (float) (double) o->getProperty (k) : def; };
        r.momDb = get ("mom", kNone);           r.shortDb = get ("short", kNone);
        r.shortMaxDb = get ("shortMax", kNone); r.short90Db = get ("short90", kNone);
        r.intLufs = get ("int", kNone);         r.peakDbTp = get ("peak", kNone);
        r.heardSeconds = get ("heard", 0.0f);
        r.heardKnown = o->hasProperty ("heardKnown") ? (bool) o->getProperty ("heardKnown") : r.heardSeconds > 0.0f;
        r.p10 = get ("p10", kNone); r.p50 = get ("p50", kNone); r.p90 = get ("p90", kNone);
        r.crestDb = get ("crest", kNone);
        r.shortTpDb = get ("shortTp", kNone);
        // An older sidecar carries "preTrim" and means the opposite question; a record from it is NOT known to be
        // converted, so it is read as not-as-heard and the line says so rather than assuming.
        r.asHeard = o->hasProperty ("asHeard") ? (bool) o->getProperty ("asHeard")
                                               : ! o->hasProperty ("preTrim");
        r.updatedMs = o->hasProperty ("updatedMs") ? (juce::int64) o->getProperty ("updatedMs") : 0;
        // A stored record is valid if it carries ANY figure or any heard time. "valid" is not stored: it is a
        // statement about the contents, and computing it here means a hand-edited or truncated node cannot claim
        // to be a measurement it does not contain.
        r.valid = r.heardSeconds > 0.0f || has (r.intLufs) || has (r.shortMaxDb) || has (r.short90Db)
               || has (r.peakDbTp) || has (r.shortDb);
        return r;
    }
};

} // namespace echojay
