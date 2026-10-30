#pragma once

#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <optional>
#include <vector>
#include "EJBandScheme.h"   // macroBandName, B1's six bands in order

// =============================================================================
//  context.match v1: THE BLOCK THE PLUGIN SENDS, AND THE RULES IT IS HELD TO.
// =============================================================================
//
// C3 (CONTRACT_MATCH_WIRE_2026-10-27, revision 3) section 2. The plugin
// computes the comparison and the server never recomputes it; the server checks
// every bound and drops what fails. So this header does two things:
//   - buildMatchBlock writes the block from plain values, with C3's field names;
//   - readMatchBlock applies C3's rules to any block, and buildMatchBlock runs
//     its own output through it. The plugin therefore never sends a field the
//     server would drop, and a dropped field is logged here first, by name.
//
// PURE. No JUCE audio type, no global, no I/O. juce::var and juce::String only.
//
// C3'S RULES, AS WRITTEN (section 2):
//   - a malformed block (not an object, v isn't 1, bandsRel isn't 6 numbers)
//     drops match entirely: the draft answers 409 match_unreadable;
//   - an out-of-bounds field drops its move or finding, with a logged reason;
//   - reference.name is trimmed to 80 characters;
//   - psr and plr are sent only when whole-file on both sides, else null on
//     both, and a one-sided PSR is dropped;
//   - corrSub is never sent, and refused if present;
//   - moves, findings and refusals at most 8 each; links at most 4; share 0..1
//     (rev 3: the plugin routes a share over 1 to the bus, C4 section 4 rule 3);
//   - a track or pair move past X1.trackMoveMaxDb (12 dB) is dropped, never
//     re-routed (rev 3, Sean 28 Oct): the plugin sends those as bus moves.
//
// CREST IS NEVER SENT (C4 rev 4 section 6). The writer puts bandCrest null on
// both sides, always, and writes no crest finding. The reader still applies R7
// to a bandCrest that arrives, because that is the server's rule, and a crest
// finding that arrives falls under R9 (crest is not a SIDE figure) and is
// dropped.
//
// WHERE C3 IS SILENT, AND WHAT THIS DOES (each is K's reading, marked so):
//   R1. A SIDE figure out of bounds. C3 names a move or a finding as what a bad
//       field drops; a side figure has neither, so the figure itself becomes
//       null, logged. A comparison is still readable without one figure.
//   R2. A bandsRel entry outside -30..30. Six numbers, but not six usable
//       bands; C3 refuses "any band list that isn't exactly B1's six", so the
//       block is unreadable.
//   R3. corrSub present: "refused" is read as the whole block (409), the same
//       answer as the other refused shapes.
//   R4. More than 8 moves, findings or refusals: the block is refused (B's MB9
//       refuses 50 moves), not silently cut to 8.
//   R5. window other than "whole" or "loudest30": unreadable. Absent: "whole".
//   R6. "loudest30" without a loudest30 figure on both sides: unreadable,
//       because C3 calls it required there and the gain target needs it.
//   R7. bandCrest of the wrong length or out of 0..40: that side's bandCrest
//       becomes null (C3 allows null "on a side without it").
//   R8. A bad attribution (more than 4 links, a share outside 0..1, a target
//       not in C3's three, or combinedShare on anything but a pair) drops the
//       attribution and keeps the move, as MB9 does for an unknown uid.
//       combinedShare has no bound in C3 rev 3; it is held to 0..2, two shares
//       of at most 1 each. K's reading first; B's staging bounds it the same
//       way (b43c4a3, per the round 5 brief).
//
// combinedShare IS NULL, NOT 0, FOR A TRACK OR A BUS MOVE (round 5c). C3 says
// "null for one track or the bus", and a 0 there is a figure: the reader above
// drops a whole attribution for it. So the WRITER decides by the target, not
// by what it was handed: a track or bus move goes out with null whatever its
// input carried, and only a pair carries the figure.
//   R9. A finding's mix and ref are held to the bounds of the side figure of
//       the same name; a figure C3's SIDE does not name drops the finding. A
//       psr or plr finding is dropped whenever psr or plr is not sent.
// The tonal-diff split C3 also refuses has no field name in C3, so nothing
// here can detect it; nothing here writes it either.
// =============================================================================

namespace echojay
{

// -----------------------------------------------------------------------------
// C3's BOUNDS, by field. Each is C3 section 2's figure.
// -----------------------------------------------------------------------------

inline constexpr int    kMatchBlockVersion     = 1;
inline constexpr int    kMatchBlockNameMax     = 80;
inline constexpr int    kMatchBlockListMax     = 8;    ///< moves, findings, refusals
inline constexpr int    kMatchBlockLinksMax    = 4;
inline constexpr double kMatchBlockShareMax    = 1.0;  ///< C3 rev 3: a sent share is 0..1
inline constexpr double kMatchBlockCombinedShareMax = 2.0;  ///< R8: two shares of at most 1
inline constexpr double kMatchBlockTrackMoveMaxDb = 12.0;  ///< X1.trackMoveMaxDb: past it a track or pair move drops
inline constexpr double kMatchBlockMoveDbMax   = 30.0; ///< valueDb and measuredDb, +-
inline constexpr double kMatchBlockBandRelMax  = 30.0; ///< each bandsRel entry, +-
inline constexpr double kMatchBlockCrestMax    = 40.0; ///< each bandCrest entry, 0..
inline constexpr int    kMatchBlockOversMax    = 100000;

struct MatchBlockBound { const char* field; double lo, hi; };

/** SIDE's scalar figures and their ranges, in C3's order. overs is an integer
    as well as in range. */
inline const std::array<MatchBlockBound, 10>& matchBlockSideBounds()
{
    static const std::array<MatchBlockBound, 10> b { {
        { "integrated", -70.0, 0.0 }, { "loudest30", -70.0, 0.0 }, { "truePeak", -70.0, 6.0 },
        { "overs", 0.0, (double) kMatchBlockOversMax }, { "lra", 0.0, 40.0 },
        { "psr", 0.0, 40.0 }, { "plr", 0.0, 40.0 }, { "width", 0.0, 200.0 }, { "corr", -1.0, 1.0 },
        { "bandsRel", -kMatchBlockBandRelMax, kMatchBlockBandRelMax } } };
    return b;
}

inline const MatchBlockBound* matchBlockBoundFor (const juce::String& field)
{
    for (const auto& b : matchBlockSideBounds())
        if (field == b.field) return &b;
    return nullptr;
}

// -----------------------------------------------------------------------------
// INPUT: plain values, absences as std::optional
// -----------------------------------------------------------------------------

struct MatchBlockSide
{
    std::optional<double> integrated, loudest30, truePeak;
    std::optional<int>    overs;
    std::optional<double> lra, psr, plr, width, corr;
    std::array<double, 6> bandsRel {};
    // No bandCrest: it is null on the wire, always (C4 rev 4 section 6).
};

struct MatchBlockLink { juce::String uid, name; double share = 0.0; };

struct MatchBlockAttribution
{
    juce::String target;                  ///< "track", "pair" or "bus"
    std::vector<MatchBlockLink> links;
    std::optional<double> combinedShare;  ///< a pair only: written as null for any other target
    juce::String reason;
};

struct MatchBlockMove
{
    juce::String kind;                    ///< "gain", "ceiling" or "band"
    int    band = -1;                     ///< 0..5, kind "band" only
    double valueDb = 0.0, measuredDb = 0.0;
    std::optional<MatchBlockAttribution> attribution;   ///< band moves only
};

struct MatchBlockFinding { juce::String figure; double mix = 0.0, ref = 0.0, difference = 0.0; };
struct MatchBlockRefusal { juce::String kind, side, message; };

struct MatchBlockIn
{
    juce::String referenceId, referenceName;
    juce::String window = "whole";
    MatchBlockSide mix, ref;
    std::vector<MatchBlockMove>    moves;
    std::vector<MatchBlockFinding> findings;
    std::vector<MatchBlockRefusal> refusals;
};

/** What the reader found. readable false is C3's 409 match_unreadable, and
    then block is void. dropped holds one line per field, move, finding or
    attribution removed, naming it. */
struct MatchBlockRead
{
    bool readable = false;
    juce::String unreadableWhy;
    juce::var block;
    juce::StringArray dropped;
};

inline juce::String matchUnreadableMessage()
{
    return "I couldn't read that comparison; press Match again.";
}

// -----------------------------------------------------------------------------
// THE READER: C3's rules on any block
// -----------------------------------------------------------------------------

inline bool matchBlockIsNumber (const juce::var& v)
{
    return v.isInt() || v.isInt64() || v.isDouble();
}

inline bool matchBlockInRange (const juce::var& v, double lo, double hi)
{
    if (! matchBlockIsNumber (v)) return false;
    const double d = (double) v;
    return d >= lo && d <= hi;
}

inline int matchBlockBandIndex (const juce::String& name)
{
    for (int i = 0; i < 6; ++i)
        if (name == macroBandName (i)) return i;
    return -1;
}

namespace detail
{
    inline MatchBlockRead unreadable (const juce::String& why)
    {
        MatchBlockRead r;
        r.unreadableWhy = why;
        return r;
    }

    /** A JSON array of exactly n numbers, each within [lo, hi]. */
    inline bool numberArray (const juce::var& v, int n, double lo, double hi)
    {
        if (! v.isArray() || v.size() != n) return false;
        for (int i = 0; i < n; ++i)
        {
            const juce::var e = v[i];
            if (! matchBlockInRange (e, lo, hi)) return false;
        }
        return true;
    }

    inline juce::var copyObject (const juce::var& v)
    {
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        if (auto* src = v.getDynamicObject())
            for (const auto& p : src->getProperties())
                o->setProperty (p.name, p.value);
        return juce::var (o.get());
    }
}

/** Applies C3 section 2 to a block, as the server will. Never throws; a block
    it cannot read comes back readable false with the reason. */
inline MatchBlockRead readMatchBlock (const juce::var& in)
{
    using detail::unreadable;
    if (! in.isObject()) return unreadable ("the block is not an object");
    const juce::var v = in.getProperty ("v", juce::var());
    if (! matchBlockIsNumber (v) || (double) v != (double) kMatchBlockVersion)
        return unreadable ("v is not 1");
    if (in.hasProperty ("corrSub")) return unreadable ("corrSub is never sent");   // R3

    MatchBlockRead out;
    juce::var block = detail::copyObject (in);
    auto* bo = block.getDynamicObject();

    // window (R5)
    const juce::var wv = in.getProperty ("window", juce::var());
    juce::String window = "whole";
    if (! wv.isVoid())
    {
        window = wv.toString();
        if (! wv.isString() || (window != "whole" && window != "loudest30"))
            return unreadable ("window is neither \"whole\" nor \"loudest30\"");
    }
    bo->setProperty ("window", window);

    // reference: the name trimmed to 80
    const juce::var refv = in.getProperty ("reference", juce::var());
    if (refv.isObject())
    {
        juce::var r2 = detail::copyObject (refv);
        const juce::String name = refv.getProperty ("name", juce::var()).toString();
        if (name.length() > kMatchBlockNameMax)
        {
            r2.getDynamicObject()->setProperty ("name", name.substring (0, kMatchBlockNameMax));
            out.dropped.add ("reference.name trimmed to " + juce::String (kMatchBlockNameMax) + " characters");
        }
        bo->setProperty ("reference", r2);
    }

    // the two sides
    const char* sideNames[2] = { "mix", "ref" };
    juce::var sides[2];
    for (int s = 0; s < 2; ++s)
    {
        const juce::var sv = in.getProperty (sideNames[s], juce::var());
        if (! sv.isObject()) return unreadable (juce::String (sideNames[s]) + " is not an object");
        if (sv.hasProperty ("corrSub")) return unreadable ("corrSub is never sent");   // R3
        const juce::var rel = sv.getProperty ("bandsRel", juce::var());
        if (! rel.isArray() || rel.size() != 6)
            return unreadable (juce::String (sideNames[s]) + ".bandsRel isn't 6 numbers");
        for (int i = 0; i < 6; ++i)
        {
            const juce::var e = rel[i];
            if (! matchBlockIsNumber (e))
                return unreadable (juce::String (sideNames[s]) + ".bandsRel isn't 6 numbers");
            if (std::abs ((double) e) > kMatchBlockBandRelMax)   // R2
                return unreadable (juce::String (sideNames[s]) + ".bandsRel has a band outside -30..30");
        }

        juce::var side = detail::copyObject (sv);
        auto* so = side.getDynamicObject();
        for (const auto& b : matchBlockSideBounds())
        {
            const juce::Identifier id (b.field);
            if (id == juce::Identifier ("bandsRel") || ! sv.hasProperty (id)) continue;
            const juce::var f = sv.getProperty (id, juce::var());
            if (f.isVoid()) continue;
            bool ok = matchBlockInRange (f, b.lo, b.hi);
            if (ok && id == juce::Identifier ("overs"))
                ok = (double) f == std::floor ((double) f);
            if (! ok)   // R1
            {
                so->setProperty (id, juce::var());
                out.dropped.add (juce::String (sideNames[s]) + "." + b.field + " out of bounds, sent as null");
            }
        }
        const juce::var crest = sv.getProperty ("bandCrest", juce::var());
        if (! crest.isVoid() && ! detail::numberArray (crest, 6, 0.0, kMatchBlockCrestMax))   // R7
        {
            so->setProperty ("bandCrest", juce::var());
            out.dropped.add (juce::String (sideNames[s]) + ".bandCrest isn't 6 figures in 0..40, sent as null");
        }
        sides[s] = side;
    }

    // loudest30 is required in the loudest30 window (R6)
    if (window == "loudest30")
        for (int s = 0; s < 2; ++s)
            if (! matchBlockIsNumber (sides[s].getProperty ("loudest30", juce::var())))
                return unreadable (juce::String (sideNames[s]) + ".loudest30 is required in \"loudest30\"");

    // psr and plr: whole-file on both sides, else null on both
    bool sent[2] = { false, false };   // psr, plr
    const char* pp[2] = { "psr", "plr" };
    for (int k = 0; k < 2; ++k)
    {
        const juce::var a = sides[0].getProperty (pp[k], juce::var());
        const juce::var b = sides[1].getProperty (pp[k], juce::var());
        sent[k] = window == "whole" && matchBlockIsNumber (a) && matchBlockIsNumber (b);
        if (! sent[k])
        {
            if (matchBlockIsNumber (a) || matchBlockIsNumber (b))
                out.dropped.add (juce::String (pp[k]) + " is not whole-file on both sides, sent as null on both");
            for (auto& sd : sides) sd.getDynamicObject()->setProperty (pp[k], juce::var());
        }
    }
    bo->setProperty ("mix", sides[0]);
    bo->setProperty ("ref", sides[1]);

    // the three lists: at most 8 each (R4)
    const char* lists[3] = { "moves", "findings", "refusals" };
    for (const char* l : lists)
    {
        const juce::var lv = in.getProperty (l, juce::var());
        if (lv.isArray() && lv.size() > kMatchBlockListMax)
            return unreadable (juce::String (l) + " holds more than " + juce::String (kMatchBlockListMax));
    }

    // moves
    juce::Array<juce::var> moves;
    {
        const juce::var mv = in.getProperty ("moves", juce::var());
        for (int i = 0; mv.isArray() && i < mv.size(); ++i)
        {
            const juce::var m = mv[i];
            const juce::String kind = m.getProperty ("kind", juce::var()).toString();
            const juce::var band = m.getProperty ("band", juce::var());
            const juce::String what = "move " + juce::String (i);
            if (kind != "gain" && kind != "ceiling" && kind != "band")
            { out.dropped.add (what + ": kind is not gain, ceiling or band"); continue; }
            if (kind == "band" ? matchBlockBandIndex (band.toString()) < 0 || ! band.isString()
                               : ! band.isVoid())
            { out.dropped.add (what + ": band is only for kind band, and must be one of B1's six"); continue; }
            if (! matchBlockInRange (m.getProperty ("valueDb", juce::var()), -kMatchBlockMoveDbMax, kMatchBlockMoveDbMax))
            { out.dropped.add (what + ": valueDb out of -30..30"); continue; }
            if (! matchBlockInRange (m.getProperty ("measuredDb", juce::var()), -kMatchBlockMoveDbMax, kMatchBlockMoveDbMax))
            { out.dropped.add (what + ": measuredDb out of -30..30"); continue; }
            {
                const juce::String t = m.getProperty ("attribution", juce::var()).getProperty ("target", juce::var()).toString();
                if ((t == "track" || t == "pair")
                    && std::abs ((double) m.getProperty ("valueDb", juce::var())) > kMatchBlockTrackMoveMaxDb)
                { out.dropped.add (what + ": a " + t + " move past 12 dB is dropped, never re-routed"); continue; }
            }

            juce::var m2 = detail::copyObject (m);
            const juce::var at = m.getProperty ("attribution", juce::var());
            if (! at.isVoid())
            {
                bool ok = kind == "band" && at.isObject();
                const juce::String target = at.getProperty ("target", juce::var()).toString();
                ok = ok && (target == "track" || target == "pair" || target == "bus");
                const juce::var links = at.getProperty ("links", juce::var());
                ok = ok && links.isArray() && links.size() <= kMatchBlockLinksMax;
                for (int k = 0; ok && k < links.size(); ++k)
                {
                    const juce::var lk = links[k];
                    ok = matchBlockInRange (lk.getProperty ("share", juce::var()), 0.0, kMatchBlockShareMax)
                      && lk.getProperty ("uid", juce::var()).isString();
                }
                const juce::var cs = at.getProperty ("combinedShare", juce::var());
                ok = ok && (target == "pair" ? matchBlockInRange (cs, 0.0, kMatchBlockCombinedShareMax) : cs.isVoid());
                if (! ok)   // R8
                {
                    m2.getDynamicObject()->setProperty ("attribution", juce::var());
                    out.dropped.add (what + ": attribution out of bounds, dropped; the move stays");
                }
            }
            moves.add (m2);
        }
    }
    bo->setProperty ("moves", moves);

    // findings (R9)
    juce::Array<juce::var> findings;
    {
        const juce::var fv = in.getProperty ("findings", juce::var());
        for (int i = 0; fv.isArray() && i < fv.size(); ++i)
        {
            const juce::var f = fv[i];
            const juce::String fig = f.getProperty ("figure", juce::var()).toString();
            const auto* b = matchBlockBoundFor (fig);
            const bool unsentFigure = (fig == "psr" && ! sent[0]) || (fig == "plr" && ! sent[1]);
            if (b == nullptr || fig == "bandsRel" || unsentFigure
                || ! matchBlockInRange (f.getProperty ("mix", juce::var()), b->lo, b->hi)
                || ! matchBlockInRange (f.getProperty ("ref", juce::var()), b->lo, b->hi)
                || ! matchBlockIsNumber (f.getProperty ("difference", juce::var())))
            { out.dropped.add ("finding " + juce::String (i) + " (" + fig + ") out of bounds, dropped"); continue; }
            findings.add (f);
        }
    }
    bo->setProperty ("findings", findings);

    {
        const juce::var rv = in.getProperty ("refusals", juce::var());
        juce::Array<juce::var> refusals;
        for (int i = 0; rv.isArray() && i < rv.size(); ++i) refusals.add (rv[i]);
        bo->setProperty ("refusals", refusals);
    }

    out.readable = true;
    out.block = block;
    return out;
}

// -----------------------------------------------------------------------------
// THE WRITER
// -----------------------------------------------------------------------------

namespace detail
{
    inline juce::var opt (const std::optional<double>& v) { return v ? juce::var (*v) : juce::var(); }

    inline juce::var sideVar (const MatchBlockSide& s)
    {
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("integrated", opt (s.integrated));
        o->setProperty ("loudest30",  opt (s.loudest30));
        o->setProperty ("truePeak",   opt (s.truePeak));
        o->setProperty ("overs",      s.overs ? juce::var (*s.overs) : juce::var());
        o->setProperty ("lra",        opt (s.lra));
        o->setProperty ("psr",        opt (s.psr));
        o->setProperty ("plr",        opt (s.plr));
        o->setProperty ("width",      opt (s.width));
        o->setProperty ("corr",       opt (s.corr));
        juce::Array<juce::var> rel;
        for (double d : s.bandsRel) rel.add (d);
        o->setProperty ("bandsRel", rel);
        o->setProperty ("bandCrest", juce::var());   // always null (C4 rev 4 section 6)
        return juce::var (o.get());
    }
}

/** The block, in C3 section 2's field names, already through readMatchBlock.
    readable false means the plugin should not send it at all. */
inline MatchBlockRead buildMatchBlock (const MatchBlockIn& in)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty ("v", kMatchBlockVersion);

    juce::DynamicObject::Ptr r = new juce::DynamicObject();
    r->setProperty ("id", in.referenceId);
    r->setProperty ("name", in.referenceName);
    o->setProperty ("reference", juce::var (r.get()));

    o->setProperty ("window", in.window);
    o->setProperty ("mix", detail::sideVar (in.mix));
    o->setProperty ("ref", detail::sideVar (in.ref));

    juce::Array<juce::var> moves;
    for (const auto& m : in.moves)
    {
        juce::DynamicObject::Ptr mo = new juce::DynamicObject();
        mo->setProperty ("kind", m.kind);
        if (m.kind == "band") mo->setProperty ("band", juce::String (macroBandName (m.band)));
        mo->setProperty ("valueDb", m.valueDb);
        mo->setProperty ("measuredDb", m.measuredDb);
        if (m.attribution)
        {
            juce::DynamicObject::Ptr ao = new juce::DynamicObject();
            ao->setProperty ("target", m.attribution->target);
            juce::Array<juce::var> links;
            for (const auto& l : m.attribution->links)
            {
                juce::DynamicObject::Ptr lo = new juce::DynamicObject();
                lo->setProperty ("uid", l.uid);
                lo->setProperty ("name", l.name);
                lo->setProperty ("share", l.share);
                links.add (juce::var (lo.get()));
            }
            ao->setProperty ("links", links);
            // null for a track or the bus, whatever was handed in (5c)
            ao->setProperty ("combinedShare", m.attribution->target == "pair"
                                                  ? detail::opt (m.attribution->combinedShare)
                                                  : juce::var());
            ao->setProperty ("reason", m.attribution->reason);
            mo->setProperty ("attribution", juce::var (ao.get()));
        }
        else mo->setProperty ("attribution", juce::var());
        moves.add (juce::var (mo.get()));
    }
    o->setProperty ("moves", moves);

    juce::Array<juce::var> findings;
    for (const auto& f : in.findings)
    {
        if (f.figure == "crest") continue;   // never written (C4 rev 4 section 6)
        juce::DynamicObject::Ptr fo = new juce::DynamicObject();
        fo->setProperty ("figure", f.figure);
        fo->setProperty ("mix", f.mix);
        fo->setProperty ("ref", f.ref);
        fo->setProperty ("difference", f.difference);
        findings.add (juce::var (fo.get()));
    }
    o->setProperty ("findings", findings);

    juce::Array<juce::var> refusals;
    for (const auto& x : in.refusals)
    {
        juce::DynamicObject::Ptr ro = new juce::DynamicObject();
        ro->setProperty ("kind", x.kind);
        ro->setProperty ("side", x.side);
        ro->setProperty ("message", x.message);
        refusals.add (juce::var (ro.get()));
    }
    o->setProperty ("refusals", refusals);

    const juce::var block (o.get());
    return readMatchBlock (block);
}

} // namespace echojay
